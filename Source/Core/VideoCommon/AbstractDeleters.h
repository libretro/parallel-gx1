// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

/* The abstract GPU objects, the Gfx, the vertex manager, the bounding box
 * the queries, the vertex formats and the frame output are deleted as the backend's own classes
 * (VideoBackendKind, BackendObjects.cpp, BackendGfx.cpp,
 * BackendVertexManager.cpp, BackendPixelEngine.cpp): they have no virtual
 * destructor.
 * Declared before any std::unique_ptr of them is, so headers that only
 * name these types include this. */

class AbstractFramebuffer;
class AbstractGfx;
class AbstractStagingTexture;
class AbstractTexture;
class BoundingBox;
class FrameOutput;
class NativeVertexFormat;
class PerfQueryBase;
class VertexManagerBase;

#define ABSTRACT_GPU_OBJECT_DELETER(type)                                                         \
  template <>                                                                                      \
  struct std::default_delete<type>                                                                 \
  {                                                                                                \
    constexpr default_delete() noexcept = default;                                                 \
    template <typename U>                                                                          \
    default_delete(const std::default_delete<U>&) noexcept                                         \
    {                                                                                              \
    }                                                                                              \
    void operator()(type* object) const;                                                           \
  };

ABSTRACT_GPU_OBJECT_DELETER(AbstractFramebuffer)
ABSTRACT_GPU_OBJECT_DELETER(AbstractGfx)
ABSTRACT_GPU_OBJECT_DELETER(AbstractStagingTexture)
ABSTRACT_GPU_OBJECT_DELETER(AbstractTexture)
ABSTRACT_GPU_OBJECT_DELETER(BoundingBox)
ABSTRACT_GPU_OBJECT_DELETER(FrameOutput)
ABSTRACT_GPU_OBJECT_DELETER(NativeVertexFormat)
ABSTRACT_GPU_OBJECT_DELETER(PerfQueryBase)
ABSTRACT_GPU_OBJECT_DELETER(VertexManagerBase)

#undef ABSTRACT_GPU_OBJECT_DELETER
