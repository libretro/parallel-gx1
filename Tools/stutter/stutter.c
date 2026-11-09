/* stutter: the stutter harness. A headless libretro frontend that runs
 * the core on content (the gxgen corpus) and reports what shader and
 * pipeline work happened, where, and what retro_run cost.
 *
 *    stutter <core> <content> [options]
 *
 *    --backend vulkan|software   renderer (vulkan)
 *    --frames N                  retro_run calls at most (300)
 *    --new N                     stop once N new pictures were presented
 *    --boot N                    the first N runs are boot, reported apart (1)
 *    --budget MS                 a run over this is over budget (16.667)
 *    --system DIR, --save DIR    the frontend's directories (.)
 *    --opt key=value             a core option, any number of them
 *    --check                     hash each new picture (no timing then)
 *    --allocs                    count the core's heap allocations after boot
 *                                (allocs.c), and report where they were made
 *    --state-out FILE            save a state after run --state-frame
 *    --state-in FILE             load this state after run --state-frame
 *    --state-frame N             where (the last boot run)
 *    --state-new N               or where the N-th new picture came
 *
 * With the stats core (make STATS=1), the counters
 * (Source/Core/VideoCommon/StutterStats.h) are read after boot and at the
 * end: "draw_*" kinds after boot are compiles and waits on the draw path.
 * Timing runs read nothing back from the GPU. One line per result, key and
 * value, for run.sh.
 *
 * C89. Linux: cc -O2 -I<core>/Externals/libretro-common/include
 *   -I<core>/Externals/Vulkan-Headers/include stutter.c allocs.c -ldl -lpthread
 * Windows (MSYS2 MINGW64): the same, without -ldl -lpthread. The Vulkan
 * loader is opened at run time. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200112L
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#endif

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <libretro.h>
#include <libretro_vulkan.h>

#include "allocs.h"

#define SYNC_COUNT 3
#define MAX_OPTS 64
#define MAX_STATS 64

/* Platform */

/* what a library's symbols are fetched as, then cast to their own type */
typedef void (*func_t)(void);

#ifdef _WIN32
static void *lib_open(const char *path) { return (void *)LoadLibraryA(path); }
static func_t lib_sym(void *lib, const char *name)
{
   return (func_t)GetProcAddress((HMODULE)lib, name);
}
static double now_ms(void)
{
   static LARGE_INTEGER freq;
   LARGE_INTEGER t;
   if (!freq.QuadPart)
      QueryPerformanceFrequency(&freq);
   QueryPerformanceCounter(&t);
   return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}
static CRITICAL_SECTION queue_lock;
static void lock_init(void) { InitializeCriticalSection(&queue_lock); }
static void lock_take(void) { EnterCriticalSection(&queue_lock); }
static void lock_give(void) { LeaveCriticalSection(&queue_lock); }
#define VULKAN_LIB "vulkan-1.dll"
#else
static void *lib_open(const char *path) { return dlopen(path, RTLD_NOW); }
static func_t lib_sym(void *lib, const char *name)
{
   /* dlsym's object pointer, as the function pointer it is */
   void *p = dlsym(lib, name);
   func_t f;
   memcpy(&f, &p, sizeof(f));
   return f;
}
static double now_ms(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock_init(void) {}
static void lock_take(void) { pthread_mutex_lock(&queue_lock); }
static void lock_give(void) { pthread_mutex_unlock(&queue_lock); }
#define VULKAN_LIB "libvulkan.so.1"
#endif

/* Settings */

static const char *backend = "vulkan";
static const char *sysdir = ".", *savedir = ".";
static const char *opt_key[MAX_OPTS], *opt_val[MAX_OPTS];
static unsigned n_opts;
static int check;

/* The frontend's state */

static struct retro_hw_render_callback hw;
static const struct retro_hw_render_context_negotiation_interface_vulkan *neg;
static struct retro_hw_render_interface_vulkan iface;
static const struct retro_vulkan_image *last_image;
static unsigned sync_index, frame_no, frames_new, frames_repeat, refresh_calls, bad_runs;
static int failed;

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice gpu;
static VkDevice dev;
static VkQueue queue;
static unsigned qfam;

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
   va_list ap;
   if (level < RETRO_LOG_ERROR)
      return;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
}

static void set_image(void *h, const struct retro_vulkan_image *image, uint32_t n,
      const VkSemaphore *sem, uint32_t src_queue_family)
{
   (void)h; (void)n; (void)sem; (void)src_queue_family;
   last_image = image;
}
static uint32_t get_sync_index(void *h) { (void)h; return sync_index; }
static uint32_t get_sync_index_mask(void *h) { (void)h; return (1u << SYNC_COUNT) - 1; }
static void set_command_buffers(void *h, uint32_t n, const VkCommandBuffer *c)
{
   (void)h; (void)n; (void)c;
}
static void wait_sync_index(void *h) { (void)h; }
/* one queue, taken in turns with the core, which submits from its GPU thread */
static void lock_queue(void *h) { (void)h; lock_take(); }
static void unlock_queue(void *h) { (void)h; lock_give(); }
static void set_signal_semaphore(void *h, VkSemaphore s) { (void)h; (void)s; }

/* Picture checks: the image copied out and hashed (FNV-1a, 64 bits). */

static unsigned long long hash_bytes(const unsigned char *p, size_t n)
{
   unsigned long long h = 14695981039346656037ULL;
   size_t i;
   for (i = 0; i < n; i++)
   {
      h ^= p[i];
      h *= 1099511628211ULL;
   }
   return h;
}

/* The corpus frame a picture is: the number gxgen draws over its top left
 * corner, 12 squares of 16x16 EFB pixels, white for a one, bit 0 first. */
static unsigned picture_corpus_frame(const unsigned char *px, unsigned w, unsigned h,
      size_t pitch)
{
   unsigned i, k = 0;
   for (i = 0; i < 12; i++)
   {
      const size_t x = ((size_t)i * 16 + 8) * w / 640, y = (size_t)8 * h / 528;
      const unsigned char *p = px + y * pitch + x * 4;
      if (p[0] + p[1] + p[2] > 3 * 128)
         k |= 1u << i;
   }
   return k;
}

#define VKFN(name) PFN_##name p_##name = (PFN_##name)vgdpa(dev, #name)

static void check_image(const struct retro_vulkan_image *img, unsigned w, unsigned h)
{
   PFN_vkGetDeviceProcAddr vgdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
   PFN_vkGetPhysicalDeviceMemoryProperties p_props =
      (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
   VKFN(vkCreateBuffer); VKFN(vkGetBufferMemoryRequirements); VKFN(vkAllocateMemory);
   VKFN(vkBindBufferMemory); VKFN(vkCreateCommandPool); VKFN(vkAllocateCommandBuffers);
   VKFN(vkBeginCommandBuffer); VKFN(vkCmdPipelineBarrier); VKFN(vkCmdCopyImageToBuffer);
   VKFN(vkEndCommandBuffer); VKFN(vkQueueSubmit); VKFN(vkQueueWaitIdle); VKFN(vkMapMemory);
   VKFN(vkUnmapMemory); VKFN(vkDestroyCommandPool); VKFN(vkDestroyBuffer); VKFN(vkFreeMemory);
   VkPhysicalDeviceMemoryProperties props;
   VkBufferCreateInfo bi;
   VkMemoryRequirements mr;
   VkMemoryAllocateInfo ai;
   VkCommandPoolCreateInfo pi;
   VkCommandBufferAllocateInfo cbi;
   VkCommandBufferBeginInfo begin;
   VkImageMemoryBarrier b;
   VkBufferImageCopy copy;
   VkSubmitInfo si;
   VkBuffer buf;
   VkDeviceMemory mem;
   VkCommandPool pool;
   VkCommandBuffer cb;
   void *px;
   uint32_t i;
   const VkMemoryPropertyFlags want =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

   memset(&bi, 0, sizeof(bi));
   bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
   bi.size = (VkDeviceSize)w * h * 4;
   bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   p_vkCreateBuffer(dev, &bi, NULL, &buf);
   p_vkGetBufferMemoryRequirements(dev, buf, &mr);
   p_props(gpu, &props);
   memset(&ai, 0, sizeof(ai));
   ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   ai.allocationSize = mr.size;
   for (i = 0; i < props.memoryTypeCount; i++)
      if ((mr.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want)
         break;
   ai.memoryTypeIndex = i;
   p_vkAllocateMemory(dev, &ai, NULL, &mem);
   p_vkBindBufferMemory(dev, buf, mem, 0);

   memset(&pi, 0, sizeof(pi));
   pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
   pi.queueFamilyIndex = qfam;
   p_vkCreateCommandPool(dev, &pi, NULL, &pool);
   memset(&cbi, 0, sizeof(cbi));
   cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
   cbi.commandPool = pool;
   cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   cbi.commandBufferCount = 1;
   p_vkAllocateCommandBuffers(dev, &cbi, &cb);
   memset(&begin, 0, sizeof(begin));
   begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
   p_vkBeginCommandBuffer(cb, &begin);

   memset(&b, 0, sizeof(b));
   b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
   b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   b.oldLayout = img->image_layout;
   b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   b.image = img->create_info.image;
   b.subresourceRange = img->create_info.subresourceRange;
   p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
         0, 0, NULL, 0, NULL, 1, &b);
   memset(&copy, 0, sizeof(copy));
   copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   copy.imageSubresource.layerCount = 1;
   copy.imageExtent.width = w;
   copy.imageExtent.height = h;
   copy.imageExtent.depth = 1;
   p_vkCmdCopyImageToBuffer(cb, img->create_info.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         buf, 1, &copy);
   b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
   b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   b.newLayout = img->image_layout;
   p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
         0, 0, NULL, 0, NULL, 1, &b);
   p_vkEndCommandBuffer(cb);
   memset(&si, 0, sizeof(si));
   si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
   si.commandBufferCount = 1;
   si.pCommandBuffers = &cb;
   lock_take();
   p_vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
   p_vkQueueWaitIdle(queue);
   lock_give();

   p_vkMapMemory(dev, mem, 0, bi.size, 0, &px);
   printf("picture frame=%u corpus=%u size=%ux%u hash=%016llx\n", frame_no,
         picture_corpus_frame((const unsigned char *)px, w, h, (size_t)w * 4), w, h,
         hash_bytes((const unsigned char *)px, (size_t)w * h * 4));
   p_vkUnmapMemory(dev, mem);
   p_vkDestroyCommandPool(dev, pool, NULL);
   p_vkDestroyBuffer(dev, buf, NULL);
   p_vkFreeMemory(dev, mem, NULL);
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
   refresh_calls++;
   if (!strcmp(backend, "software"))
   {
      /* a new picture is a new pointer or new contents; repeats come as NULL */
      if (!data)
      {
         frames_repeat++;
         return;
      }
      frames_new++;
      if (check)
      {
         unsigned long long hsh = 14695981039346656037ULL;
         unsigned y;
         for (y = 0; y < h; y++)
         {
            const unsigned char *row = (const unsigned char *)data + (size_t)y * pitch;
            unsigned x;
            for (x = 0; x < w * 4; x++)
            {
               hsh ^= row[x];
               hsh *= 1099511628211ULL;
            }
         }
         printf("picture frame=%u corpus=%u size=%ux%u hash=%016llx\n", frame_no,
               picture_corpus_frame((const unsigned char *)data, w, h, pitch), w, h, hsh);
      }
      return;
   }
   if (data != RETRO_HW_FRAME_BUFFER_VALID)
   {
      frames_repeat++;
      sync_index = (sync_index + 1) % SYNC_COUNT;
      return;
   }
   frames_new++;
   if (!last_image)
   {
      fprintf(stderr, "frame %u: a new picture with no image\n", frame_no);
      failed = 1;
      return;
   }
   if (check)
      check_image(last_image, w, h);
   sync_index = (sync_index + 1) % SYNC_COUNT;
}

static bool env_cb(unsigned cmd, void *data)
{
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
         ((struct retro_log_callback *)data)->log = log_cb;
         return true;
      case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
         *(const char **)data = sysdir;
         return true;
      case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
         *(const char **)data = savedir;
         return true;
      case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
         *(unsigned *)data = RETRO_HW_CONTEXT_VULKAN;
         return !strcmp(backend, "vulkan");
      case RETRO_ENVIRONMENT_SET_HW_RENDER:
         if (strcmp(backend, "vulkan"))
            return false;
         hw = *(struct retro_hw_render_callback *)data;
         return hw.context_type == RETRO_HW_CONTEXT_VULKAN;
      case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
         neg = (const struct retro_hw_render_context_negotiation_interface_vulkan *)data;
         return true;
      case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
         *(const struct retro_hw_render_interface **)data =
               (const struct retro_hw_render_interface *)&iface;
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool *)data = false;
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE:
      {
         struct retro_variable *v = (struct retro_variable *)data;
         unsigned i;
         if (!strcmp(v->key, "dolphin_renderer"))
         {
            v->value = strcmp(backend, "software") ? "Hardware" : "Software";
            return true;
         }
         for (i = 0; i < n_opts; i++)
            if (!strcmp(v->key, opt_key[i]))
            {
               v->value = opt_val[i];
               return true;
            }
         return false;
      }
      case RETRO_ENVIRONMENT_SET_GEOMETRY:
      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
         return true;
      default:
         return false;
   }
}

static void audio_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch_cb(const int16_t *d, size_t n) { (void)d; return n; }
static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned p, unsigned d, unsigned i, unsigned id)
{
   (void)p; (void)d; (void)i; (void)id;
   return 0;
}

static int vulkan_bring_up(void)
{
   void *loader = lib_open(VULKAN_LIB);
   PFN_vkCreateInstance p_create_instance;
   PFN_vkEnumeratePhysicalDevices p_enum;
   VkApplicationInfo app;
   VkInstanceCreateInfo ici;
   struct retro_vulkan_context ctx;
   uint32_t n = 1;

   if (!loader)
   {
      fprintf(stderr, "no Vulkan loader (%s)\n", VULKAN_LIB);
      return 0;
   }
   gipa = (PFN_vkGetInstanceProcAddr)lib_sym(loader, "vkGetInstanceProcAddr");
   p_create_instance = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
   memset(&app, 0, sizeof(app));
   app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
   app.apiVersion = VK_API_VERSION_1_1;
   memset(&ici, 0, sizeof(ici));
   ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
   ici.pApplicationInfo = neg->get_application_info ? neg->get_application_info() : &app;
   if (p_create_instance(&ici, NULL, &inst) != VK_SUCCESS)
      return 0;
   p_enum = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
   p_enum(inst, &n, &gpu);

   memset(&ctx, 0, sizeof(ctx));
   if (!neg->create_device(&ctx, inst, gpu, VK_NULL_HANDLE, gipa, NULL, 0, NULL, 0, NULL))
      return 0;
   dev = ctx.device;
   queue = ctx.queue;
   qfam = ctx.queue_family_index;

   iface.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
   iface.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
   iface.handle = &iface;
   iface.instance = inst;
   iface.gpu = ctx.gpu;
   iface.device = dev;
   iface.get_device_proc_addr = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
   iface.get_instance_proc_addr = gipa;
   iface.queue = queue;
   iface.queue_index = qfam;
   iface.set_image = set_image;
   iface.get_sync_index = get_sync_index;
   iface.get_sync_index_mask = get_sync_index_mask;
   iface.set_command_buffers = set_command_buffers;
   iface.wait_sync_index = wait_sync_index;
   iface.lock_queue = lock_queue;
   iface.unlock_queue = unlock_queue;
   iface.set_signal_semaphore = set_signal_semaphore;
   return 1;
}

/* The counters, by their names in enum stutter_stat's order. */
static const char *const stat_names[] = {
   "draw_gx_pipeline", "draw_uber_pipeline", "draw_efb_copy_to_vram", "draw_efb_copy_to_ram",
   "draw_reinterpret", "draw_decode", "draw_queued_compile", "draw_wait", "draw_disk_write",
   "worker_compile", "shader_create", "pipeline_create", "driver_pipeline",
   "draw_driver_pipeline", "shader_generate", "shader_emit", "shader_words"
};
#define NUM_STAT_NAMES (sizeof(stat_names) / sizeof(stat_names[0]))

typedef unsigned (*stats_fn)(unsigned long long *, unsigned long long *, unsigned);

static int cmp_double(const void *a, const void *b)
{
   const double x = *(const double *)a, y = *(const double *)b;
   return x < y ? -1 : x > y;
}

static void *read_file(const char *path, size_t *size)
{
   FILE *f = fopen(path, "rb");
   void *p;
   long n;
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   n = ftell(f);
   fseek(f, 0, SEEK_SET);
   p = malloc(n > 0 ? (size_t)n : 1);
   if (p && fread(p, 1, (size_t)n, f) != (size_t)n)
   {
      free(p);
      p = NULL;
   }
   fclose(f);
   *size = (size_t)n;
   return p;
}

#define SYM(var, type, name) do { var = (type)lib_sym(core, name); \
   if (!var) { fprintf(stderr, "the core has no %s\n", name); return 2; } } while (0)

int main(int argc, char **argv)
{
   void *core;
   void (*p_set_environment)(retro_environment_t);
   void (*p_set_video_refresh)(retro_video_refresh_t);
   void (*p_set_audio_sample)(retro_audio_sample_t);
   void (*p_set_audio_sample_batch)(retro_audio_sample_batch_t);
   void (*p_set_input_poll)(retro_input_poll_t);
   void (*p_set_input_state)(retro_input_state_t);
   void (*p_init)(void);
   bool (*p_load_game)(const struct retro_game_info *);
   void (*p_run)(void);
   size_t (*p_serialize_size)(void);
   bool (*p_serialize)(void *, size_t);
   bool (*p_unserialize)(const void *, size_t);
   void (*p_unload_game)(void);
   void (*p_deinit)(void);
   stats_fn p_stats;
   struct retro_game_info game;
   unsigned frames = 300, boot = 1, i, n_stats = 0, over = 0, max_frame = 0;
   double budget = 1000.0 / 60.0, t_load, t_boot = 0.0, t0, *run_ms;
   unsigned long long c_boot[MAX_STATS], y_boot[MAX_STATS], c_end[MAX_STATS], y_end[MAX_STATS];
   const char *state_in = NULL, *state_out = NULL;
   unsigned state_frame = 0, state_new = 0, until_new = 0;
   int state_done = 0, allocs = 0;

   if (argc < 3)
   {
      fprintf(stderr, "usage: stutter <core> <content> [options]\n");
      return 2;
   }
   for (i = 3; i < (unsigned)argc; i++)
   {
      const char *a = argv[i];
      const char *v = i + 1 < (unsigned)argc ? argv[i + 1] : NULL;
      if (!strcmp(a, "--check"))
      {
         check = 1;
         continue;
      }
      if (!strcmp(a, "--allocs"))
      {
         allocs = 1;
         continue;
      }
      if (!v)
      {
         fprintf(stderr, "%s needs a value\n", a);
         return 2;
      }
      i++;
      if (!strcmp(a, "--backend"))
         backend = v;
      else if (!strcmp(a, "--frames"))
         frames = (unsigned)atoi(v);
      else if (!strcmp(a, "--new"))
         until_new = (unsigned)atoi(v);
      else if (!strcmp(a, "--boot"))
         boot = (unsigned)atoi(v);
      else if (!strcmp(a, "--budget"))
         budget = atof(v);
      else if (!strcmp(a, "--system"))
         sysdir = v;
      else if (!strcmp(a, "--save"))
         savedir = v;
      else if (!strcmp(a, "--state-in"))
         state_in = v;
      else if (!strcmp(a, "--state-out"))
         state_out = v;
      else if (!strcmp(a, "--state-frame"))
         state_frame = (unsigned)atoi(v);
      else if (!strcmp(a, "--state-new"))
         state_new = (unsigned)atoi(v);
      else if (!strcmp(a, "--opt") && n_opts < MAX_OPTS && strchr(v, '='))
      {
         /* key=value, split in place */
         char *eq = strchr(argv[i], '=');
         *eq = 0;
         opt_key[n_opts] = argv[i];
         opt_val[n_opts] = eq + 1;
         n_opts++;
      }
      else
      {
         fprintf(stderr, "unknown option %s\n", a);
         return 2;
      }
   }
   if (strcmp(backend, "vulkan") && strcmp(backend, "software"))
   {
      fprintf(stderr, "backend %s: vulkan or software\n", backend);
      return 2;
   }
   if (!boot)
      boot = 1;
   if (frames <= boot)
      frames = boot + 1;
   if (!state_frame)
      state_frame = boot;
   lock_init();

   core = lib_open(argv[1]);
   if (!core)
   {
      fprintf(stderr, "cannot open %s\n", argv[1]);
      return 2;
   }
   SYM(p_set_environment, void (*)(retro_environment_t), "retro_set_environment");
   SYM(p_set_video_refresh, void (*)(retro_video_refresh_t), "retro_set_video_refresh");
   SYM(p_set_audio_sample, void (*)(retro_audio_sample_t), "retro_set_audio_sample");
   SYM(p_set_audio_sample_batch, void (*)(retro_audio_sample_batch_t),
         "retro_set_audio_sample_batch");
   SYM(p_set_input_poll, void (*)(retro_input_poll_t), "retro_set_input_poll");
   SYM(p_set_input_state, void (*)(retro_input_state_t), "retro_set_input_state");
   SYM(p_init, void (*)(void), "retro_init");
   SYM(p_load_game, bool (*)(const struct retro_game_info *), "retro_load_game");
   SYM(p_run, void (*)(void), "retro_run");
   SYM(p_serialize_size, size_t (*)(void), "retro_serialize_size");
   SYM(p_serialize, bool (*)(void *, size_t), "retro_serialize");
   SYM(p_unserialize, bool (*)(const void *, size_t), "retro_unserialize");
   SYM(p_unload_game, void (*)(void), "retro_unload_game");
   SYM(p_deinit, void (*)(void), "retro_deinit");
   p_stats = (stats_fn)lib_sym(core, "parallel_gx1_stutter_stats");
   if (allocs && !allocs_init(p_run))
   {
      fprintf(stderr, "--allocs: cannot count the core's allocations here\n");
      allocs = 0;
   }
   if (!p_stats)
      fprintf(stderr, "no counters: not the stats core (make STATS=1)\n");

   run_ms = (double *)calloc(frames, sizeof(*run_ms));
   if (!run_ms)
      return 1;

   p_set_environment(env_cb);
   p_set_video_refresh(video_cb);
   p_set_audio_sample(audio_cb);
   p_set_audio_sample_batch(audio_batch_cb);
   p_set_input_poll(input_poll_cb);
   p_set_input_state(input_state_cb);
   p_init();

   memset(&game, 0, sizeof(game));
   game.path = argv[2];
   t0 = now_ms();
   if (!p_load_game(&game))
   {
      fprintf(stderr, "the core did not load %s\n", argv[2]);
      return 1;
   }
   if (!strcmp(backend, "vulkan"))
   {
      if (hw.context_type != RETRO_HW_CONTEXT_VULKAN || !neg || !vulkan_bring_up())
      {
         fprintf(stderr, "no Vulkan device\n");
         return 1;
      }
      hw.context_reset();
   }
   t_load = now_ms() - t0;

   for (frame_no = 0; frame_no < frames; frame_no++)
   {
      double t;
      if (until_new && frames_new >= until_new && frame_no > boot)
         break;
      refresh_calls = 0;
      t = now_ms();
      p_run();
      run_ms[frame_no] = now_ms() - t;
      if (refresh_calls != 1)
         bad_runs++;
      if (frame_no < boot)
         t_boot += run_ms[frame_no];
      if (frame_no + 1 == boot && p_stats)
         n_stats = p_stats(c_boot, y_boot, MAX_STATS);
      if (frame_no + 1 == boot && allocs)
         allocs_count(1);
      if (!state_done && (state_new ? frames_new >= state_new : frame_no + 1 == state_frame))
      {
         state_done = 1;
         if (state_out || state_in)
         {
            size_t size = p_serialize_size();
            void *state = NULL;
            if (state_in)
               state = read_file(state_in, &size);
            else if ((state = malloc(size)) && p_serialize(state, size))
            {
               FILE *f = fopen(state_out, "wb");
               if (f)
               {
                  fwrite(state, 1, size, f);
                  fclose(f);
               }
            }
            if (state_in && (!state || !p_unserialize(state, size)))
            {
               fprintf(stderr, "the core did not load the state %s\n", state_in);
               failed = 1;
            }
            free(state);
         }
      }
   }
   if (allocs)
      allocs_count(0);
   if (p_stats)
      p_stats(c_end, y_end, MAX_STATS);
   frames = frame_no;

   /* retro_run after boot: median, p99, max, over budget */
   for (i = boot; i < frames; i++)
      if (run_ms[i] > budget)
         over++;
   for (i = boot; i < frames; i++)
      if (run_ms[i] >= run_ms[max_frame] || max_frame < boot)
         max_frame = i;
   {
      const unsigned n = frames - boot;
      double *sorted = (double *)malloc(n * sizeof(*sorted));
      if (!sorted)
         return 1;
      memcpy(sorted, run_ms + boot, n * sizeof(*sorted));
      qsort(sorted, n, sizeof(*sorted), cmp_double);
      if (!check)
         printf("timing backend=%s load_ms=%.2f boot_ms=%.2f runs=%u run_p50_ms=%.3f "
               "run_p99_ms=%.3f run_max_ms=%.3f run_max_frame=%u over_budget=%u\n",
               backend, t_load, t_boot, n, sorted[n / 2], sorted[(n * 99) / 100],
               sorted[n - 1], max_frame, over);
      free(sorted);
   }
   printf("frames backend=%s new=%u repeat=%u bad_runs=%u\n", backend, frames_new,
         frames_repeat, bad_runs);
   for (i = 0; i < n_stats && i < NUM_STAT_NAMES; i++)
      printf("stat %s boot=%llu boot_mcyc=%.3f play=%llu play_mcyc=%.3f\n", stat_names[i],
            c_boot[i], (double)y_boot[i] / 1e6, c_end[i] - c_boot[i],
            (double)(y_end[i] - y_boot[i]) / 1e6);
   if (allocs)
      allocs_report(16);

   if (hw.context_destroy)
      hw.context_destroy();
   p_unload_game();
   p_deinit();
   free(run_ms);
   return failed || bad_runs;
}
