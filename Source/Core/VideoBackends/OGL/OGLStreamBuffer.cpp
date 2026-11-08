// Copyright 2013 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/OGL/OGLStreamBuffer.h"

#include "Common/Align.h"
#include "Common/MathUtil.h"
#include "Common/MemoryUtil.h"

#include "VideoBackends/OGL/OGLConfig.h"

#include "VideoCommon/DriverDetails.h"

namespace OGL
{
// moved out of constructor, so m_buffer is allowed to be const
static u32 GenBuffer()
{
  u32 id;
  glGenBuffers(1, &id);
  return id;
}

StreamBuffer::StreamBuffer(u32 type, u32 size)
    : m_buffer(GenBuffer()), m_buffertype(type), m_size(MathUtil::NextPowerOf2(size)),
      m_bit_per_slot(MathUtil::IntLog2(MathUtil::NextPowerOf2(size) / SYNC_POINTS))
{
  m_iterator = 0;
  m_used_iterator = 0;
  m_free_iterator = 0;
}

/* Shared synchronization code for ring buffers
 *
 * The next three functions are to create/delete/use the OpenGL synchronization.
 * ARB_sync (OpenGL 3.2) is used and required.
 *
 * To reduce overhead, the complete buffer is split up into SYNC_POINTS chunks.
 * For each of this chunks, there is a fence which checks if this chunk is still in use.
 *
 * As our API allows to alloc more memory then it has to use, we have to catch how much is already
 * written.
 *
 * m_iterator      - writing position
 * m_free_iterator - last position checked if free
 * m_used_iterator - last position known to be written
 *
 * So on alloc, we have to wait for all slots between m_free_iterator and m_iterator (and set
 * m_free_iterator to m_iterator afterwards).
 *
 * We also assume that this buffer is accessed by the GPU between the Unmap and Map function,
 * so we may create the fences on the start of mapping.
 * Some here, new fences for the chunks between m_used_iterator and m_iterator (also update
 * m_used_iterator).
 *
 * As ring buffers have an ugly behavior on rollover, have fun to read this code ;)
 */

void StreamBuffer::CreateFences()
{
  for (int i = 0; i < SYNC_POINTS; i++)
  {
    m_fences[i] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  }
}
void StreamBuffer::DeleteFences()
{
  for (int i = Slot(m_free_iterator) + 1; i < SYNC_POINTS; i++)
  {
    glDeleteSync(m_fences[i]);
  }
  for (int i = 0; i < Slot(m_iterator); i++)
  {
    glDeleteSync(m_fences[i]);
  }
}
void StreamBuffer::AllocMemory(u32 size)
{
  // insert waiting slots for used memory
  for (int i = Slot(m_used_iterator); i < Slot(m_iterator); i++)
  {
    m_fences[i] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  }
  m_used_iterator = m_iterator;

  // wait for new slots to end of buffer
  for (int i = Slot(m_free_iterator) + 1; i <= Slot(m_iterator + size) && i < SYNC_POINTS; i++)
  {
    glClientWaitSync(m_fences[i], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
    glDeleteSync(m_fences[i]);
  }

  // If we allocate a large amount of memory (A), commit a smaller amount, then allocate memory
  // smaller than allocation A, we will have already waited for these fences in A, but not used
  // the space. In this case, don't set m_free_iterator to a position before that which we know
  // is safe to use, which would result in waiting on the same fence(s) next time.
  if ((m_iterator + size) > m_free_iterator)
    m_free_iterator = m_iterator + size;

  // if buffer is full
  if (m_iterator + size >= m_size)
  {
    // insert waiting slots in unused space at the end of the buffer
    for (int i = Slot(m_used_iterator); i < SYNC_POINTS; i++)
    {
      m_fences[i] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }

    // move to the start
    m_used_iterator = m_iterator = 0;  // offset 0 is always aligned

    // wait for space at the start
    for (int i = 0; i <= Slot(m_iterator + size); i++)
    {
      glClientWaitSync(m_fences[i], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
      glDeleteSync(m_fences[i]);
    }
    m_free_iterator = m_iterator + size;
  }
}

/* The ways to stream, by kind:
 *
 * MAP_AND_ORPHAN: the usual way to stream data to the GPU.
 * Described here: https://www.opengl.org/wiki/Buffer_Object_Streaming#Unsynchronized_buffer_mapping
 * Just do unsync appends until the buffer is full. When it's full, orphan (alloc a new buffer and
 * free the old one). As reallocation is an overhead, this method isn't as fast as it is known to
 * be.
 *
 * MAP_AND_SYNC: a modified streaming way without reallocation, a ring buffer allocated on
 * initialization. With this limited resource, we have to care about the CPU-GPU distance, else
 * this fifo may overflow. So we had traded orphan vs syncing.
 *
 * BUFFER_STORAGE: streaming fifo without mapping overhead, with ARB_buffer_storage (OpenGL 4.4),
 * usually not available on OpenGL3 GPUs. It allows us to render from a mapped buffer, so we map it
 * persistently in the initialization. Unsync mapping sounds like an easy task, but it isn't for
 * threaded drivers: every mapping on current close-source driver _will_ end in at least a round
 * trip time between two threads. As persistently mapped buffer can't use orphaning, we also have
 * to sync.
 *
 * PINNED_MEMORY (AMD only): another streaming fifo without mapping overhead, with
 * AMD_pinned_memory, available on all AMD GPUs. As we can't orphan without mapping, we have to
 * sync. OpenGL 4.4 drivers should use BUFFER_STORAGE.
 *
 * BUFFER_SUB_DATA: fifo based on the glBufferSubData call. As everything must be copied before
 * glBufferSubData returns, an additional memcpy in the driver will be done. So this is a huge
 * overhead, only use it if required.
 *
 * BUFFER_DATA: fifo based on the glBufferData call. Some trashy drivers stall in BufferSubData.
 * So here we use glBufferData, which realloc this buffer every time. This may avoid stalls, but it
 * is a bigger overhead than BufferSubData.
 */
static constexpr u32 ALIGN_PINNED_MEMORY = 4096;

StreamBuffer::StreamBuffer(Kind kind, u32 type, u32 size, bool coherent, bool client_storage)
    : StreamBuffer(type, size)
{
  m_kind = kind;
  m_coherent = coherent;
  switch (kind)
  {
  case MAP_AND_ORPHAN:
    glBindBuffer(m_buffertype, m_buffer);
    glBufferData(m_buffertype, m_size, nullptr, GL_STREAM_DRAW);
    break;
  case MAP_AND_SYNC:
    CreateFences();
    glBindBuffer(m_buffertype, m_buffer);
    glBufferData(m_buffertype, m_size, nullptr, GL_STREAM_DRAW);
    break;
  case BUFFER_STORAGE:
    CreateFences();
    glBindBuffer(m_buffertype, m_buffer);
    // PERSISTANT_BIT to make sure that the buffer can be used while mapped
    // COHERENT_BIT is set so we don't have to use a MemoryBarrier on write
    // CLIENT_STORAGE_BIT asks for client memory, cached, for what the CPU writes in place
    glBufferStorage(m_buffertype, m_size, nullptr,
                    GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT |
                        (coherent ? GL_MAP_COHERENT_BIT : 0) |
                        (client_storage ? GL_CLIENT_STORAGE_BIT : 0));
    m_pointer =
        (u8*)glMapBufferRange(m_buffertype, 0, m_size,
                              GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT |
                                  (coherent ? GL_MAP_COHERENT_BIT : GL_MAP_FLUSH_EXPLICIT_BIT));
    break;
  case PINNED_MEMORY:
    CreateFences();
    m_pointer = static_cast<u8*>(Common::AllocateAlignedMemory(
        Common::AlignUp(m_size, ALIGN_PINNED_MEMORY), ALIGN_PINNED_MEMORY));
    glBindBuffer(GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD, m_buffer);
    glBufferData(GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD,
                 Common::AlignUp(m_size, ALIGN_PINNED_MEMORY), m_pointer, GL_STREAM_COPY);
    glBindBuffer(GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD, 0);
    glBindBuffer(m_buffertype, m_buffer);
    break;
  case BUFFER_SUB_DATA:
    glBindBuffer(m_buffertype, m_buffer);
    glBufferData(m_buffertype, size, nullptr, GL_STATIC_DRAW);
    m_pointer = new u8[m_size];
    break;
  case BUFFER_DATA:
    glBindBuffer(m_buffertype, m_buffer);
    m_pointer = new u8[m_size];
    break;
  }
}

StreamBuffer::~StreamBuffer()
{
  switch (m_kind)
  {
  case MAP_AND_ORPHAN:
    break;
  case MAP_AND_SYNC:
    DeleteFences();
    break;
  case BUFFER_STORAGE:
    DeleteFences();
    glUnmapBuffer(m_buffertype);
    glBindBuffer(m_buffertype, 0);
    break;
  case PINNED_MEMORY:
    DeleteFences();
    glBindBuffer(m_buffertype, 0);
    glFinish();  // ogl pipeline must be flushed, else this buffer can be in use
    Common::FreeAlignedMemory(m_pointer);
    break;
  case BUFFER_SUB_DATA:
  case BUFFER_DATA:
    delete[] m_pointer;
    break;
  }
  glDeleteBuffers(1, &m_buffer);
}

std::pair<u8*, u32> StreamBuffer::Map(u32 size)
{
  u8* pointer;
  switch (m_kind)
  {
  case MAP_AND_ORPHAN:
    if (m_iterator + size >= m_size)
    {
      glBufferData(m_buffertype, m_size, nullptr, GL_STREAM_DRAW);
      m_iterator = 0;
    }
    pointer = (u8*)glMapBufferRange(m_buffertype, m_iterator, size,
                                    GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT |
                                        GL_MAP_UNSYNCHRONIZED_BIT);
    return std::make_pair(pointer, m_iterator);
  case MAP_AND_SYNC:
    AllocMemory(size);
    pointer = (u8*)glMapBufferRange(m_buffertype, m_iterator, size,
                                    GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT |
                                        GL_MAP_UNSYNCHRONIZED_BIT);
    return std::make_pair(pointer, m_iterator);
  case BUFFER_STORAGE:
  case PINNED_MEMORY:
    AllocMemory(size);
    return std::make_pair(m_pointer + m_iterator, m_iterator);
  default:  // BUFFER_SUB_DATA, BUFFER_DATA
    return std::make_pair(m_pointer, 0u);
  }
}

void StreamBuffer::Unmap(u32 used_size)
{
  switch (m_kind)
  {
  case MAP_AND_ORPHAN:
  case MAP_AND_SYNC:
    glFlushMappedBufferRange(m_buffertype, 0, used_size);
    glUnmapBuffer(m_buffertype);
    m_iterator += used_size;
    break;
  case BUFFER_STORAGE:
    if (!m_coherent)
      glFlushMappedBufferRange(m_buffertype, m_iterator, used_size);
    m_iterator += used_size;
    break;
  case PINNED_MEMORY:
    m_iterator += used_size;
    break;
  case BUFFER_SUB_DATA:
    glBufferSubData(m_buffertype, 0, used_size, m_pointer);
    break;
  case BUFFER_DATA:
    glBufferData(m_buffertype, used_size, m_pointer, GL_STREAM_DRAW);
    break;
  }
}

StreamBuffer* StreamBuffer::CreatePersistentClient(u32 type, u32 size)
{
  if (!g_ogl_config.bSupportsGLSync || !g_ogl_config.bSupportsGLBufferStorage ||
      DriverDetails::HasBug(DriverDetails::BUG_BROKEN_BUFFER_STORAGE))
  {
    return nullptr;
  }
  return new StreamBuffer(BUFFER_STORAGE, type, size, true, true);
}

// Chooses the best streaming method based on the supported extensions and known issues
static StreamBuffer::Kind ChooseKind(u32 type)
{
  // without basevertex support, only streaming methods whith uploads everything to zero works fine:
  if (!g_ogl_config.bSupportsGLBaseVertex)
  {
    if (!DriverDetails::HasBug(DriverDetails::BUG_BROKEN_BUFFER_STREAM))
      return StreamBuffer::BUFFER_SUB_DATA;

    // BufferData is by far the worst way, only use it if needed
    return StreamBuffer::BUFFER_DATA;
  }

  // Prefer the syncing buffers over the orphaning one
  if (g_ogl_config.bSupportsGLSync)
  {
    // pinned memory is much faster than buffer storage on AMD cards
    if (g_ogl_config.bSupportsGLPinnedMemory &&
        !(DriverDetails::HasBug(DriverDetails::BUG_BROKEN_PINNED_MEMORY)))
      return StreamBuffer::PINNED_MEMORY;

    // buffer storage works well in most situations
    if (g_ogl_config.bSupportsGLBufferStorage &&
        !(DriverDetails::HasBug(DriverDetails::BUG_BROKEN_BUFFER_STORAGE) &&
          type == GL_ARRAY_BUFFER) &&
        !(DriverDetails::HasBug(DriverDetails::BUG_INTEL_BROKEN_BUFFER_STORAGE) &&
          type == GL_ELEMENT_ARRAY_BUFFER))
      return StreamBuffer::BUFFER_STORAGE;

    // don't fall back to MapAnd* for Nvidia drivers
    if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_UNSYNC_MAPPING))
    {
      if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_BUFFER_STREAM))
        return StreamBuffer::BUFFER_DATA;
      else
        return StreamBuffer::BUFFER_SUB_DATA;
    }

    // mapping fallback
    return StreamBuffer::MAP_AND_SYNC;
  }

  // default fallback, should work everywhere, but isn't the best way to do this job
  return StreamBuffer::MAP_AND_ORPHAN;
}

StreamBuffer* StreamBuffer::Create(u32 type, u32 size)
{
  return new StreamBuffer(ChooseKind(type), type, size, true, false);
}
}  // namespace OGL
