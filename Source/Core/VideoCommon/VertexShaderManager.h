// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <span>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/Matrix.h"
#include "VideoCommon/ConstantManager.h"
#include "VideoCommon/NativeVertexFormat.h"

class PointerWrap;
struct PortableVertexDeclaration;
class XFStateManager;

// Head-tracked camera for the libretro core's immersive VR mode.
struct LibretroVRCamera
{
  bool enabled = false;
  // Set when the values below change, so the projection is reloaded.
  bool dirty = false;
  // Whether the projection currently loaded uses the headset camera, or the 2D screen.
  bool applied = false;
  bool screen_applied = false;
  // Head rotation, applied after the game's view like the Free Look camera.
  Common::Matrix44 view = Common::Matrix44::Identity();
  // The headset's eye frustums averaged into one centred projection, in Dolphin's layout.
  float scale_x = 1.0f;
  float offset_x = 0.0f;
  float scale_y = 1.0f;
  float offset_y = 0.0f;
  // How far the left eye's frustum sits from the centred one, in clip-space x per unit of w.
  // The right eye's is the negation.
  float eye_offset_x = 0.0f;
  // Tangents of the game camera's half field of view, from its last projection; 0 until seen.
  float game_tan_x = 0.0f;
  float game_tan_y = 0.0f;
  // The display's shape, for the 2D screen before the game camera is seen.
  float display_aspect = 4.0f / 3.0f;
  // What the draw being flushed samples. 2D drawn from a copy of the frame (bloom, blur, fades
  // of the scene) must cover the scene exactly, so it is left at full size.
  bool draw_textured = false;
  bool draw_samples_copy = false;
};
extern LibretroVRCamera g_libretro_vr_camera;

// The non-API dependent parts.
class alignas(16) VertexShaderManager
{
public:
  void Init();
  void DoState(PointerWrap& p);

  // constant management
  void SetProjectionMatrix(XFStateManager& xf_state_manager);
  void SetConstants(std::span<const std::string> textures, XFStateManager& xf_state_manager);

  // data: 3 floats representing the X, Y and Z vertex model coordinates and the posmatrix index.
  // out:  4 floats which will be initialized with the corresponding clip space coordinates
  // NOTE: m_projection_matrix must be up to date when this is called
  //       (i.e. VertexShaderManager::SetConstants needs to be called before using this!)
  void TransformToClipSpace(const float* data, float* out, u32 mtxIdx);

  static bool UseVertexDepthRange();

  VertexShaderConstants constants{};
  bool dirty = false;

  static DOLPHIN_FORCE_INLINE void UpdateValue(bool* dirty, u32* old_value, u32 new_value)
  {
    if (*old_value == new_value)
      return;
    *old_value = new_value;
    *dirty = true;
  }

  static DOLPHIN_FORCE_INLINE void UpdateOffset(bool* dirty, bool include_components,
                                                u32* old_value, const AttributeFormat& attribute)
  {
    if (!attribute.enable)
      return;
    u32 new_value = attribute.offset / 4;  // GPU uses uint offsets
    if (include_components)
      new_value |= attribute.components << 16;
    UpdateValue(dirty, old_value, new_value);
  }

  template <size_t N>
  static DOLPHIN_FORCE_INLINE void UpdateOffsets(bool* dirty, bool include_components,
                                                 std::array<u32, N>* old_value,
                                                 const std::array<AttributeFormat, N>& attribute)
  {
    for (size_t i = 0; i < N; i++)
      UpdateOffset(dirty, include_components, &(*old_value)[i], attribute[i]);
  }

  DOLPHIN_FORCE_INLINE void SetVertexFormat(u32 components, const PortableVertexDeclaration& format)
  {
    UpdateValue(&dirty, &constants.components, components);
    UpdateValue(&dirty, &constants.vertex_stride, format.stride / 4);
    UpdateOffset(&dirty, true, &constants.vertex_offset_position, format.position);
    UpdateOffset(&dirty, false, &constants.vertex_offset_posmtx, format.posmtx);
    UpdateOffsets(&dirty, true, &constants.vertex_offset_texcoords, format.texcoords);
    UpdateOffsets(&dirty, false, &constants.vertex_offset_colors, format.colors);
    UpdateOffsets(&dirty, false, &constants.vertex_offset_normals, format.normals);
  }

private:
  alignas(16) std::array<float, 16> m_projection_matrix;

  // track changes
  bool m_projection_graphics_mod_change = false;

  Common::Matrix44 LoadProjectionMatrix();
};
