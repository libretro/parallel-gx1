/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Vulkan device memory: large blocks taken from the driver and handed out
 * as offsets inside them. C89.
 *
 *  - Blocks come from the driver and stay while they hold anything; an
 *    image or buffer made mid-frame is an offset in a VkDeviceMemory that
 *    exists already, not a trip to the driver.
 *  - An allocation is the lowest free span of a block that fits, at the
 *    alignment asked for. A block keeps its free spans sorted by offset, so
 *    a free is a binary search and a merge with its neighbours. Nothing is
 *    allocated per allocation: the handle is a small struct the owner keeps
 *    by value.
 *  - A host-visible block is mapped once, when it is taken, and stays
 *    mapped; an allocation in it carries its address, so writes go straight
 *    into the memory the GPU reads.
 *  - Buffers and linear images never share a block with optimal images,
 *    which keeps bufferImageGranularity without padding anything.
 *  - What the driver requires or prefers dedicated, and anything of half a
 *    block or more, gets a VkDeviceMemory of its own, sized to fit.
 *  - Host-visible blocks are smaller than device-local ones: the host ones
 *    are system memory the process maps, and what is mapped and touched is
 *    resident. One empty block of each kind is kept for the next
 *    allocation; further empty ones go straight back to the driver.
 *
 * One owning thread: the heap takes no locks. The renderer allocates and
 * frees on the thread that records and retires command buffers.
 */

#ifndef VK_HEAP_H
#define VK_HEAP_H

#include <stddef.h>

#ifndef VULKAN_CORE_H_
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The entry points the heap calls, passed in: the loader that has them is
 * the renderer's. The *2 ones are optional (NULL without Vulkan 1.1 or
 * VK_KHR_get_memory_requirements2); without them nothing is dedicated
 * unless it is large. */
typedef struct vk_heap_fns
{
   PFN_vkAllocateMemory               allocate_memory;
   PFN_vkFreeMemory                   free_memory;
   PFN_vkMapMemory                    map_memory;
   PFN_vkUnmapMemory                  unmap_memory;
   PFN_vkFlushMappedMemoryRanges      flush_ranges;
   PFN_vkInvalidateMappedMemoryRanges invalidate_ranges;
   PFN_vkCreateBuffer                 create_buffer;
   PFN_vkDestroyBuffer                destroy_buffer;
   PFN_vkCreateImage                  create_image;
   PFN_vkDestroyImage                 destroy_image;
   PFN_vkGetBufferMemoryRequirements  buffer_requirements;
   PFN_vkGetImageMemoryRequirements   image_requirements;
   PFN_vkGetBufferMemoryRequirements2 buffer_requirements2;
   PFN_vkGetImageMemoryRequirements2  image_requirements2;
   PFN_vkBindBufferMemory             bind_buffer;
   PFN_vkBindImageMemory              bind_image;
} vk_heap_fns_t;

/* Where something ended up. mapped already has offset added; NULL when the
 * memory is not host-visible. A zeroed one is no allocation. */
typedef struct vk_heap_allocation
{
   VkDeviceMemory memory;
   VkDeviceSize   offset;
   VkDeviceSize   size;
   void          *mapped;
   unsigned       block;
   unsigned       type;   /* memory type index */
} vk_heap_alloc_t;

/* What memory is for. */
enum vk_heap_usage
{
   /* device-local; the CPU never touches it */
   VK_HEAP_DEVICE = 0,
   /* written by the CPU, read by the GPU once or a few times: host-visible,
    * coherent where there is such, in system memory rather than the
    * device-local window, which is slower for the CPU to stream into */
   VK_HEAP_UPLOAD,
   /* written by the GPU, read by the CPU: host-visible, cached where there
    * is such */
   VK_HEAP_READBACK,
   /* host-visible and coherent whatever else: for drivers whose cached
    * memory is slow to read back */
   VK_HEAP_HOST_COHERENT
};

typedef struct vk_heap_span
{
   VkDeviceSize offset;
   VkDeviceSize size;
} vk_heap_span_t;

typedef struct vk_heap_block
{
   VkDeviceMemory  memory;     /* VK_NULL_HANDLE: a free slot */
   VkDeviceSize    size;
   VkDeviceSize    used;
   /* No free span is larger. Exact after a search that found nothing, an
    * upper bound otherwise, so a full block is passed over unread. */
   VkDeviceSize    max_free;
   void           *mapped;     /* the whole block, or NULL */
   vk_heap_span_t *spans;      /* free, sorted by offset, never adjacent */
   unsigned        span_count;
   unsigned        span_capacity;
   unsigned        type;
   unsigned char   linear;     /* buffers and linear images, or optimal images */
   unsigned char   dedicated;  /* one allocation, given back with it */
} vk_heap_block_t;

typedef struct vk_heap
{
   VkPhysicalDeviceMemoryProperties props;
   vk_heap_fns_t                    fns;
   VkDeviceSize                     device_block_size;
   VkDeviceSize                     host_block_size;
   VkDeviceSize                     atom;       /* nonCoherentAtomSize */
   VkDeviceSize                     bytes_reserved;
   VkDeviceSize                     bytes_used;
   VkDevice                         device;
   vk_heap_block_t                 *blocks;
   unsigned                         block_count;
   unsigned                         block_capacity;
   VkResult                         last_error;
} vk_heap_t;

/* atom is VkPhysicalDeviceLimits::nonCoherentAtomSize. Block sizes of 0
 * mean 64 MiB device-local and 16 MiB host-visible; a block is never more
 * than an eighth of the driver heap it comes from. Returns 0 when fns
 * lacks an entry point that is not optional. */
int  vk_heap_init(vk_heap_t *heap, VkDevice device,
      const VkPhysicalDeviceMemoryProperties *props, VkDeviceSize atom,
      const vk_heap_fns_t *fns, VkDeviceSize device_block_size,
      VkDeviceSize host_block_size);

/* Gives every block back to the driver. Nothing may still be allocated. */
void vk_heap_shutdown(vk_heap_t *heap);

#define VK_HEAP_DEDICATED_PREFERRED 1
#define VK_HEAP_DEDICATED_REQUIRED  2

/* Memory for these requirements. linear: a buffer or an image with linear
 * tiling. dedicated, one of the above or 0: a VkDeviceMemory of its own,
 * with for_buffer or for_image, if not VK_NULL_HANDLE, named in it; a
 * preference falls back to a shared block. Returns 0 when there is no
 * memory; heap->last_error says why. */
int  vk_heap_alloc(vk_heap_t *heap, const VkMemoryRequirements *req,
      enum vk_heap_usage usage, int linear, int dedicated,
      VkBuffer for_buffer, VkImage for_image, vk_heap_alloc_t *out);

/* Zeroes *alloc. */
void vk_heap_free(vk_heap_t *heap, vk_heap_alloc_t *alloc);

/* Creates the buffer or image, finds its memory and binds it. On failure
 * nothing is left behind and the result says why. */
VkResult vk_heap_create_buffer(vk_heap_t *heap, const VkBufferCreateInfo *info,
      enum vk_heap_usage usage, VkBuffer *buffer, vk_heap_alloc_t *out);
VkResult vk_heap_create_image(vk_heap_t *heap, const VkImageCreateInfo *info,
      enum vk_heap_usage usage, VkImage *image, vk_heap_alloc_t *out);

/* Destroys the object and frees its memory; both may be null. */
void vk_heap_destroy_buffer(vk_heap_t *heap, VkBuffer buffer, vk_heap_alloc_t *alloc);
void vk_heap_destroy_image(vk_heap_t *heap, VkImage image, vk_heap_alloc_t *alloc);

/* The property flags of the memory an allocation is in. */
VkMemoryPropertyFlags vk_heap_flags(const vk_heap_t *heap, const vk_heap_alloc_t *alloc);

/* Makes CPU writes to [offset, offset + size) of the allocation visible to
 * the device (flush), or device writes visible to the CPU (invalidate).
 * size may be VK_WHOLE_SIZE. Nothing to do, and nothing done, on coherent
 * memory. */
VkResult vk_heap_flush(vk_heap_t *heap, const vk_heap_alloc_t *alloc,
      VkDeviceSize offset, VkDeviceSize size);
VkResult vk_heap_invalidate(vk_heap_t *heap, const vk_heap_alloc_t *alloc,
      VkDeviceSize offset, VkDeviceSize size);

#ifdef __cplusplus
}
#endif

#endif
