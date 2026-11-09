/* allocs: the stutter harness's allocation tracer. The harness defines the
 * C allocator's entry points, so the core's calls land here before glibc's;
 * one made while counting, whose caller is inside the core, counts. The
 * core links its C++ runtime statically, so operator new is inside it too.
 * Calls from the driver, the C library or the harness do not count.
 *
 * Each counted call's site is kept by the two frames above the allocator
 * (the second for the caller of operator new and the like), as offsets in
 * the core, for addr2line -e <core>.
 *
 * C89. glibc on Linux only; elsewhere, nothing is counted. */
#define _GNU_SOURCE
#include <stdio.h> /* glibc says it is glibc here */

#if defined(__linux__) && defined(__GLIBC__)
#include <execinfo.h>
#include <link.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocs.h"

extern void *__libc_malloc(size_t size);
extern void *__libc_calloc(size_t n, size_t size);
extern void *__libc_realloc(void *p, size_t size);
extern void __libc_free(void *p);
extern void *__libc_memalign(size_t align, size_t size);

#define SITES 1024
#define DEPTH 4

struct site
{
   unsigned long a, b; /* offsets in the core, 0 for none */
   unsigned long count;
};

static unsigned long core_lo, core_hi, core_base;
static volatile int counting;
static unsigned long count, bytes, sites_lost;
static struct site sites[SITES];
static pthread_key_t inside; /* set while a site is taken: backtrace may allocate */

static int in_core(unsigned long a) { return a >= core_lo && a < core_hi; }

static void record(void *ret, size_t size)
{
   void *frames[DEPTH];
   unsigned long a, b = 0, h;
   unsigned i;
   int n;

   __sync_fetch_and_add(&count, 1UL);
   __sync_fetch_and_add(&bytes, (unsigned long)size);
   if (pthread_getspecific(inside))
      return;
   pthread_setspecific(inside, (void *)1);
   a = (unsigned long)ret - core_base;
   /* 0: record, 1: the allocator, 2: its caller (ret), 3: that one's */
   n = backtrace(frames, DEPTH);
   if (n == DEPTH && in_core((unsigned long)frames[3]))
      b = (unsigned long)frames[3] - core_base;
   h = (a * 2654435761UL ^ b * 40503UL) % SITES;
   for (i = 0; i < SITES; i++)
   {
      struct site *s = &sites[(h + i) % SITES];
      if (s->a == a && s->b == b)
      {
         __sync_fetch_and_add(&s->count, 1UL);
         break;
      }
      if (!s->a && __sync_bool_compare_and_swap(&s->a, 0UL, a))
      {
         s->b = b;
         __sync_fetch_and_add(&s->count, 1UL);
         break;
      }
   }
   if (i == SITES)
      __sync_fetch_and_add(&sites_lost, 1UL);
   pthread_setspecific(inside, NULL);
}

#define COUNT(size) \
   do \
   { \
      void *ret_ = __builtin_return_address(0); \
      if (counting && in_core((unsigned long)ret_)) \
         record(ret_, (size)); \
   } while (0)

void *malloc(size_t size)
{
   COUNT(size);
   return __libc_malloc(size);
}

void *calloc(size_t n, size_t size)
{
   COUNT(n * size);
   return __libc_calloc(n, size);
}

void *realloc(void *p, size_t size)
{
   COUNT(size);
   return __libc_realloc(p, size);
}

void free(void *p) { __libc_free(p); }

void *memalign(size_t align, size_t size)
{
   COUNT(size);
   return __libc_memalign(align, size);
}

void *aligned_alloc(size_t align, size_t size)
{
   COUNT(size);
   return __libc_memalign(align, size);
}

int posix_memalign(void **out, size_t align, size_t size)
{
   void *p;
   COUNT(size);
   p = __libc_memalign(align, size);
   if (!p)
      return 12; /* ENOMEM */
   *out = p;
   return 0;
}

struct find
{
   unsigned long probe;
   int found;
};

static int find_core(struct dl_phdr_info *info, size_t size, void *data)
{
   struct find *f = (struct find *)data;
   unsigned long lo = ~0UL, hi = 0;
   int i;
   (void)size;
   for (i = 0; i < info->dlpi_phnum; i++)
   {
      const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
      unsigned long a, e;
      if (ph->p_type != PT_LOAD)
         continue;
      a = info->dlpi_addr + ph->p_vaddr;
      e = a + ph->p_memsz;
      if (a < lo)
         lo = a;
      if (e > hi)
         hi = e;
   }
   if (f->probe < lo || f->probe >= hi)
      return 0;
   core_lo = lo;
   core_hi = hi;
   core_base = info->dlpi_addr;
   f->found = 1;
   return 1;
}

int allocs_init(void (*function_in_core)(void))
{
   struct find f;
   void *frames[DEPTH];
   if (pthread_key_create(&inside, NULL))
      return 0;
   /* the unwinder loads on first use, allocating: before counting */
   backtrace(frames, DEPTH);
   f.probe = (unsigned long)function_in_core;
   f.found = 0;
   dl_iterate_phdr(find_core, &f);
   return f.found;
}

void allocs_count(int on) { counting = on; }

static int by_count(const void *x, const void *y)
{
   const struct site *a = (const struct site *)x, *b = (const struct site *)y;
   return a->count < b->count ? 1 : a->count > b->count ? -1 : 0;
}

void allocs_report(unsigned top)
{
   unsigned i, n = 0;
   counting = 0;
   printf("allocs play=%lu bytes=%lu sites_lost=%lu\n", count, bytes, sites_lost);
   for (i = 0; i < SITES; i++)
      if (sites[i].a)
         sites[n++] = sites[i];
   qsort(sites, n, sizeof(sites[0]), by_count);
   for (i = 0; i < n && i < top; i++)
      printf("alloc_site count=%lu at=0x%lx from=0x%lx\n", sites[i].count, sites[i].a,
            sites[i].b);
}

#else

#include "allocs.h"

int allocs_init(void (*function_in_core)(void))
{
   (void)function_in_core;
   return 0;
}
void allocs_count(int on) { (void)on; }
void allocs_report(unsigned top)
{
   (void)top;
   printf("allocs unavailable\n");
}

#endif
