// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

/* The video backend in use. One runs at a time: its Gfx sets this as it
 * is made, and the abstract GPU objects (shaders, pipelines, textures,
 * staging textures, framebuffers) call into its classes by it. */
enum VideoBackendKind
{
  VIDEO_BACKEND_NULL,
  VIDEO_BACKEND_SOFTWARE,
  VIDEO_BACKEND_OGL,
  VIDEO_BACKEND_VULKAN,
  VIDEO_BACKEND_D3D11,
  VIDEO_BACKEND_D3D12
};

extern VideoBackendKind g_video_backend_kind;
