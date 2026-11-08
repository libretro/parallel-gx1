// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/GL/GLExtensions/GLExtensions.h"

namespace OGL
{
class StreamBuffer
{
public:
  // How the buffer is streamed, chosen by Create for the driver (OGLStreamBuffer.cpp)
  enum Kind : u8
  {
    MAP_AND_ORPHAN,
    MAP_AND_SYNC,
    BUFFER_STORAGE,
    PINNED_MEMORY,
    BUFFER_SUB_DATA,
    BUFFER_DATA
  };

  static StreamBuffer* Create(u32 type, u32 size);
  // Mapped for good, in client memory: what the CPU writes in place goes straight into what the
  // GPU reads. nullptr without buffer storage.
  static StreamBuffer* CreatePersistentClient(u32 type, u32 size);
  ~StreamBuffer();
  StreamBuffer(const StreamBuffer&) = delete;
  StreamBuffer& operator=(const StreamBuffer&) = delete;

  u32 GetGLBufferId() const { return m_buffer; }
  u32 GetSize() const { return m_size; }
  u32 GetCurrentOffset() const { return m_iterator; }

  /* This mapping function will return a pair of:
   * - the pointer to the mapped buffer
   * - the offset into the real GPU buffer (always multiple of stride)
   * On mapping, the maximum of size for allocation has to be set.
   * The size really pushed into this fifo only has to be known on Unmapping.
   * Mapping invalidates the current buffer content,
   * so it isn't allowed to access the old content any more.
   */
  std::pair<u8*, u32> Map(u32 size);
  void Unmap(u32 used_size);

  std::pair<u8*, u32> Map(u32 size, u32 stride)
  {
    u32 padding = m_iterator % stride;
    if (padding)
    {
      m_iterator += stride - padding;
    }
    return Map(size);
  }

  const u32 m_buffer;

private:
  StreamBuffer(u32 type, u32 size);
  StreamBuffer(Kind kind, u32 type, u32 size, bool coherent, bool client_storage);
  void CreateFences();
  void DeleteFences();
  void AllocMemory(u32 size);

  const u32 m_buffertype;
  const u32 m_size;

  u32 m_iterator;
  u32 m_used_iterator;
  u32 m_free_iterator;

  u8* m_pointer = nullptr;  // what the CPU writes to, when not mapped each time
  Kind m_kind = MAP_AND_ORPHAN;
  bool m_coherent = true;

  static constexpr int SYNC_POINTS = 16;
  int Slot(u32 x) const { return x >> m_bit_per_slot; }
  const int m_bit_per_slot;

  std::array<GLsync, SYNC_POINTS> m_fences{};
};
}  // namespace OGL
