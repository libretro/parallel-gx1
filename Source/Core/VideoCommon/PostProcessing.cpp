// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/PostProcessing.h"

#include <retro_miscellaneous.h>

#include <array>
#include <cstring>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/MsgHandler.h"

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace VideoCommon
{
namespace
{
constexpr char s_copy_pixel_shader[] = "void main() { SetOutput(Sample()); }\n";

// Resampling, gamut and gamma correction and HDR scaling for the final pass.
constexpr char s_correction_pixel_shader[] =
    R"(/***** COLOR CORRECTION *****/

// Color Space references:
// https://www.unravel.com.au/understanding-color-spaces

// SMPTE 170M - BT.601 (NTSC-M) -> BT.709
mat3 from_NTSCM = transpose(mat3(
	0.939497225737661,		0.0502268452914346,		0.0102759289709032,
	0.0177558637510127,		0.965824605885027,		0.0164195303639603,
	-0.00162163209967010,	-0.00437400622653655,	1.00599563832621));

// ARIB TR-B9 (9300K+27MPCD with chromatic adaptation) (NTSC-J) -> BT.709
mat3 from_NTSCJ = transpose(mat3(
	0.768497526,		-0.210804164,	  0.000297427177,
	0.0397904068,		1.04825413,			0.00555809540,
	0.00147510506,	0.0328789241,		1.36515128));

// EBU - BT.470BG/BT.601 (PAL) -> BT.709
mat3 from_PAL = transpose(mat3(
	1.04408168421813,		-0.0440816842181253,	0.000000000000000,
	0.000000000000000,	1.00000000000000,			0.000000000000000,
	0.000000000000000,	0.0118044782106489,		0.988195521789351));

float3 LinearTosRGBGamma(float3 color)
{
	const float a = 0.055;

	for (int i = 0; i < 3; ++i)
	{
		float x = color[i];
		if (x <= 0.0031308)
			x = x * 12.92;
		else
			x = (1.0 + a) * pow(x, 1.0 / 2.4) - a;
		color[i] = x;
	}

	return color;
}

float3 Rec709_Luminance = float3(0.2126, 0.7152, 0.0722);

float Luminance(float3 color, bool native_color_space)
{
	if (native_color_space && OptionEnabled(correct_color_space))
	{
		if (game_color_space == 0)
			color *= from_NTSCM;
		else if (game_color_space == 1)
			color *= from_NTSCJ;
		else if (game_color_space == 2)
			color *= from_PAL;
	}
	return dot(color, Rec709_Luminance);
}

/***** COLOR SAMPLING *****/

// Non filtered gamma corrected sample (nearest neighbor)
float4 QuickSample(float3 uvw, float gamma)
{
#if 0 // Test sampling range
	const float threshold = 0.00000001;
	float2 xy = uvw.xy * GetResolution();
	// Sampling outside the valid range, draw in yellow
	if (xy.x < (0.0 - threshold) || xy.x > (GetResolution().x + threshold) || xy.y < (0.0 - threshold) || xy.y > (GetResolution().y + threshold))
		return float4(1.0, 1.0, 0.0, 1);
	// Sampling at the edges, draw in purple
	if (xy.x < 1.0 || xy.x > (GetResolution().x - 1.0) || xy.y < 1.0 || xy.y > (GetResolution().y - 1.0))
		return float4(0.5, 0, 0.5, 1);
#endif

	float4 color = texture(samp1, uvw);
	color.rgb = pow(color.rgb, float3(gamma));
	return color;
}
float4 QuickSample(float2 uv, float w, float gamma)
{
	return QuickSample(float3(uv, w), gamma);
}
float4 QuickSampleByPixel(float2 xy, float w, float gamma)
{
	float3 uvw = float3(xy * GetInvResolution(), w);
	return QuickSample(uvw, gamma);
}

/***** Bilinear Interpolation *****/

float4 BilinearSample(float3 uvw, float gamma)
{
	// This emulates the (bi)linear filtering done directly from GPUs HW.
	// Note that GPUs might natively filter red green and blue differently, but we don't do it.
	// They might also use different filtering between upscaling and downscaling.
	float2 source_size = GetResolution();
	float2 pixel = (uvw.xy * source_size) - 0.5; // Try to find the matching pixel top left corner

	// Find the integer and floating point parts
	float2 int_pixel = floor(pixel);
	float2 frac_pixel = fract(pixel);

	// Take 4 samples around the original uvw
	float4 c11 = QuickSampleByPixel(int_pixel + float2(0.5, 0.5), uvw.z, gamma);
	float4 c21 = QuickSampleByPixel(int_pixel + float2(1.5, 0.5), uvw.z, gamma);
	float4 c12 = QuickSampleByPixel(int_pixel + float2(0.5, 1.5), uvw.z, gamma);
	float4 c22 = QuickSampleByPixel(int_pixel + float2(1.5, 1.5), uvw.z, gamma);

	// Blend the 4 samples by their weight
	float4 avg_c = lerp(lerp(c11, c21, frac_pixel.x), lerp(c12, c22, frac_pixel.x), frac_pixel.y);

	// Calculate the average of the gamma space luminance, as that can be used to
	// properly determine the perceptual brightness the color should have.
	float l11 = pow(Luminance(c11.rgb, true), 1.0 / gamma);
	float l21 = pow(Luminance(c21.rgb, true), 1.0 / gamma);
	float l12 = pow(Luminance(c12.rgb, true), 1.0 / gamma);
	float l22 = pow(Luminance(c22.rgb, true), 1.0 / gamma);
	float target_avg_c_lum = lerp(lerp(l11, l21, frac_pixel.x), lerp(l12, l22, frac_pixel.x), frac_pixel.y);

	float avg_c_lum = Luminance(avg_c.rgb, true);
	if (avg_c_lum != 0.0)
	{
		avg_c.rgb *= pow(target_avg_c_lum, gamma) / avg_c_lum;
	}

	return avg_c;
}

void main()
{
	// This tries to fall back on GPU HW sampling if it can (it won't be gamma corrected).
	bool needs_rescaling = GetResolution() != GetWindowResolution();

	bool needs_resampling = needs_rescaling && (OptionEnabled(hdr_output) || OptionEnabled(correct_gamma));

	float4 color;

	if (needs_resampling)
	{
		// Doing linear sampling in "gamma space" on linear texture formats isn't correct.
		// If the source and target resolutions don't match, the GPU will return a color
		// that is the average of 4 gamma space colors, but gamma space colors can't be blended together,
		// gamma needs to be de-applied first. This makes a big difference if colors change
		// drastically between two pixels.

		color = BilinearSample(v_tex0, game_gamma);
	}
	else
	{
		// Default GPU HW sampling. Bilinear is identical to Nearest Neighbor if the input and output resolutions match.
		if (needs_rescaling)
			color = texture(samp0, v_tex0);
		else
			color = texture(samp1, v_tex0);

		// Convert to linear before doing any other of follow up operations.
		color.rgb = pow(color.rgb, float3(game_gamma));
	}

	if (OptionEnabled(correct_color_space))
	{
		if (game_color_space == 0)
			color.rgb = color.rgb * from_NTSCM;
		else if (game_color_space == 1)
			color.rgb = color.rgb * from_NTSCJ;
		else if (game_color_space == 2)
			color.rgb = color.rgb * from_PAL;
	}

	if (OptionEnabled(hdr_output))
	{
		float hdr_paper_white = hdr_paper_white_nits / hdr_sdr_white_nits;
		color.rgb *= hdr_paper_white;
	}

	if (OptionEnabled(linear_space_output))
	{
		// Nothing to do here
	}
	// Correct the SDR gamma for sRGB (PC/Monitor) or ~2.2 (Common TV gamma)
	else if (OptionEnabled(correct_gamma))
	{
		if (OptionEnabled(sdr_display_gamma_sRGB))
			color.rgb = LinearTosRGBGamma(color.rgb);
		else
			color.rgb = pow(color.rgb, float3(1.0 / sdr_display_custom_gamma));
	}
	// Restore the original gamma without changes
	else
	{
		color.rgb = pow(color.rgb, float3(1.0 / game_gamma));
	}

	SetOutput(color);
}


)";

struct Uniforms
{
  // bools are s32
  std::array<float, 4> source_resolution;
  std::array<float, 4> target_resolution;
  std::array<float, 4> window_resolution;
  std::array<float, 4> stereo_views;
  std::array<float, 4> src_rect;
  s32 src_layer;
  u32 time;
  s32 graphics_api;
  s32 intermediary_buffer;
  s32 correct_color_space;
  s32 game_color_space;
  s32 correct_gamma;
  float game_gamma;
  s32 sdr_display_gamma_sRGB;
  float sdr_display_custom_gamma;
  s32 linear_space_output;
  s32 hdr_output;
  float hdr_paper_white_nits;
  float hdr_sdr_white_nits;
};

constexpr char s_uniform_block[] = R"(UBO_BINDING(std140, 1) uniform PSBlock {
  float4 resolution;
  float4 target_resolution;
  float4 window_resolution;
  int2 stereo_views;
  float4 src_rect;
  int src_layer;
  uint time;
  int graphics_api;
  int intermediary_buffer;
  int correct_color_space;
  int game_color_space;
  int correct_gamma;
  float game_gamma;
  int sdr_display_gamma_sRGB;
  float sdr_display_custom_gamma;
  int linear_space_output;
  int hdr_output;
  float hdr_paper_white_nits;
  float hdr_sdr_white_nits;
};

)";

constexpr char s_pixel_header[] = R"(SAMPLER_BINDING(0) uniform sampler2DArray samp0;
SAMPLER_BINDING(1) uniform sampler2DArray samp1;
FRAGMENT_OUTPUT_LOCATION(0) out float4 ocol0;

float4 Sample() { return texture(samp0, v_tex0); }
float2 GetWindowResolution() { return window_resolution.xy; }
float2 GetInvWindowResolution() { return window_resolution.zw; }
float2 GetResolution() { return resolution.xy; }
float2 GetInvResolution() { return resolution.zw; }
void SetOutput(float4 color) { ocol0 = color; }

#define OptionEnabled(x) ((x) != 0)

)";

ShaderCode PixelShaderSource(const char* body)
{
  ShaderCode source;
  source.Puts(s_uniform_block);
  if (g_backend_info.bSupportsGeometryShaders)
    source.Puts("VARYING_LOCATION(0) in VertexData {\n  float3 v_tex0;\n};\n");
  else
    source.Puts("VARYING_LOCATION(0) in float3 v_tex0;\n");
  source.Puts(s_pixel_header);
  source.Puts(body);
  return source;
}

ShaderCode VertexShaderSource()
{
  ShaderCode source;
  source.Puts(s_uniform_block);
  if (g_backend_info.bSupportsGeometryShaders)
    source.Puts("VARYING_LOCATION(0) out VertexData {\n  float3 v_tex0;\n};\n");
  else
    source.Puts("VARYING_LOCATION(0) out float3 v_tex0;\n");
  source.Puts("void main() {\n"
              "  v_tex0 = float3(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2), 0.0f);\n"
              "  gl_Position = float4(v_tex0.xy * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), "
              "0.0f, 1.0f);\n"
              "  v_tex0 = float3(src_rect.xy + (src_rect.zw * v_tex0.xy), float(src_layer));\n");
  // Vulkan's Y axis points down
  if (g_backend_info.api_type == APIType::Vulkan)
    source.Puts("  gl_Position.y = -gl_Position.y;\n");
  source.Puts("}\n");
  return source;
}
}  // namespace

PostProcessing::PostProcessing() = default;
PostProcessing::~PostProcessing()
{
  DestroyPipelines();
  AbstractShaderDelete(m_correction_pixel_shader);
  AbstractShaderDelete(m_copy_pixel_shader);
  AbstractShaderDelete(m_vertex_shader);
}

void PostProcessing::DestroyPipelines()
{
  AbstractPipelineDelete(m_copy_pipeline);
  m_copy_pipeline = nullptr;
  AbstractPipelineDelete(m_correction_pipeline);
  m_correction_pipeline = nullptr;
  AbstractPipelineDelete(m_screen_pipeline);
  m_screen_pipeline = nullptr;
}

bool PostProcessing::Initialize(AbstractTextureFormat format)
{
  m_framebuffer_format = format;
  m_uniform_staging_buffer.resize(sizeof(Uniforms));
  return CompileShaders() && CompilePipelines();
}

void PostProcessing::RecompilePipeline()
{
  DestroyPipelines();
  CompilePipelines();
}

bool PostProcessing::IsColorCorrectionActive() const
{
  return g_ActiveConfig.color_correction.bCorrectColorSpace ||
         g_ActiveConfig.color_correction.bCorrectGamma ||
         m_framebuffer_format == AbstractTextureFormat::RGBA16F;
}

void PostProcessing::BlitFromTexture(const MathUtil::Rectangle<int>& dst,
                                     const MathUtil::Rectangle<int>& src,
                                     const AbstractTexture* src_tex, int src_layer)
{
  if (g_gfx->GetCurrentFramebuffer()->GetColorFormat() != m_framebuffer_format)
  {
    m_framebuffer_format = g_gfx->GetCurrentFramebuffer()->GetColorFormat();
    RecompilePipeline();
  }

  // The bilinear sampler does the scaling; colour correction or HDR takes the correction shader,
  // built the first time either is turned on.
  const bool needs_correction = IsColorCorrectionActive();
  if (needs_correction && !m_correction_shader_tried)
  {
    CompileCorrectionShader();
    RecompilePipeline();
  }
  const AbstractPipeline* pipeline =
      needs_correction && m_correction_pipeline ? m_correction_pipeline :
                                                  m_copy_pipeline;
  if (!pipeline)
    return;

  g_gfx->SetSamplerState(0, RenderState::GetLinearSamplerState());
  g_gfx->SetSamplerState(1, RenderState::GetPointSamplerState());
  g_gfx->SetTexture(0, src_tex);
  g_gfx->SetTexture(1, src_tex);

  FillUniformBuffer(src, src_tex, MAX(src_layer, 0), g_gfx->GetCurrentFramebuffer()->GetRect(),
                    g_presenter->GetTargetRectangle());
  g_vertex_manager->UploadUtilityUniforms(m_uniform_staging_buffer.data(),
                                          static_cast<u32>(m_uniform_staging_buffer.size()));

  g_gfx->SetViewportAndScissor(
      g_gfx->ConvertFramebufferRectangle(dst, g_gfx->GetCurrentFramebuffer()));
  g_gfx->SetPipeline(pipeline);
  g_gfx->Draw(0, 3);
}

void PostProcessing::BlitScreen(const MathUtil::Rectangle<int>& dst,
                                const MathUtil::Rectangle<int>& src,
                                const AbstractTexture* src_tex)
{
  if (g_gfx->GetCurrentFramebuffer()->GetColorFormat() != m_framebuffer_format)
  {
    m_framebuffer_format = g_gfx->GetCurrentFramebuffer()->GetColorFormat();
    RecompilePipeline();
  }
  if (!m_screen_pipeline)
    return;

  g_gfx->SetSamplerState(0, RenderState::GetPointSamplerState());
  g_gfx->SetTexture(0, src_tex);
  FillUniformBuffer(src, src_tex, 0, dst, dst);
  g_vertex_manager->UploadUtilityUniforms(m_uniform_staging_buffer.data(),
                                          static_cast<u32>(m_uniform_staging_buffer.size()));
  g_gfx->SetViewportAndScissor(
      g_gfx->ConvertFramebufferRectangle(dst, g_gfx->GetCurrentFramebuffer()));
  g_gfx->SetPipeline(m_screen_pipeline);
  g_gfx->Draw(0, 3);
}

bool PostProcessing::CompileShaders()
{
  m_vertex_shader = g_gfx->CreateShaderFromSource(ShaderStage::Vertex, VertexShaderSource(), "Present vertex shader");
  m_copy_pixel_shader = g_gfx->CreateShaderFromSource(
      ShaderStage::Pixel, PixelShaderSource(s_copy_pixel_shader),
      "Present copy pixel shader");
  // Only built when color correction or HDR is on: most sessions never use it.
  if (IsColorCorrectionActive())
    CompileCorrectionShader();

  if (!m_vertex_shader || !m_copy_pixel_shader)
  {
    PanicAlert("Failed to compile the presentation shaders");
    return false;
  }
  return true;
}

void PostProcessing::CompileCorrectionShader()
{
  m_correction_shader_tried = true;
  // Optional: without it the output is copied uncorrected.
  m_correction_pixel_shader = g_gfx->CreateShaderFromSource(
      ShaderStage::Pixel, PixelShaderSource(s_correction_pixel_shader),
      "Present correction pixel shader");
}

bool PostProcessing::CompilePipelines()
{
  // Some backends refuse pipelines with no targets; nothing is drawn then anyway.
  if (m_framebuffer_format == AbstractTextureFormat::Undefined)
    return true;

  AbstractPipelineConfig config = {};
  config.vertex_shader = m_vertex_shader;
  config.rasterization_state = RenderState::GetNoCullRasterizationState(PrimitiveType::Triangles);
  config.depth_state = RenderState::GetNoDepthTestingDepthState();
  config.blending_state = RenderState::GetNoBlendingBlendState();
  config.framebuffer_state = RenderState::GetColorFramebufferState(m_framebuffer_format);
  config.usage = AbstractPipelineUsage::Utility;

  if (m_correction_pixel_shader)
  {
    config.pixel_shader = m_correction_pixel_shader;
    m_correction_pipeline = g_gfx->CreatePipeline(config);
  }

  config.pixel_shader = m_copy_pixel_shader;
  m_copy_pipeline = g_gfx->CreatePipeline(config);

  config.blending_state.alpha_update = false;
  m_screen_pipeline = g_gfx->CreatePipeline(config);
  return m_copy_pipeline != nullptr;
}

void PostProcessing::FillUniformBuffer(const MathUtil::Rectangle<int>& src,
                                       const AbstractTexture* src_tex, int src_layer,
                                       const MathUtil::Rectangle<int>& dst,
                                       const MathUtil::Rectangle<int>& wnd)
{
  const float rcp_src_width = 1.0f / src_tex->GetWidth();
  const float rcp_src_height = 1.0f / src_tex->GetHeight();
  const auto& cc = g_ActiveConfig.color_correction;
  const bool hdr = m_framebuffer_format == AbstractTextureFormat::RGBA16F;

  Uniforms u = {};
  u.source_resolution = {static_cast<float>(src_tex->GetWidth()),
                         static_cast<float>(src_tex->GetHeight()), rcp_src_width, rcp_src_height};
  u.target_resolution = {static_cast<float>(dst.GetWidth()), static_cast<float>(dst.GetHeight()),
                         1.0f / static_cast<float>(dst.GetWidth()),
                         1.0f / static_cast<float>(dst.GetHeight())};
  u.window_resolution = {static_cast<float>(wnd.GetWidth()), static_cast<float>(wnd.GetHeight()),
                         1.0f / static_cast<float>(wnd.GetWidth()),
                         1.0f / static_cast<float>(wnd.GetHeight())};
  u.src_rect = {static_cast<float>(src.left) * rcp_src_width,
                static_cast<float>(src.top) * rcp_src_height,
                static_cast<float>(src.GetWidth()) * rcp_src_width,
                static_cast<float>(src.GetHeight()) * rcp_src_height};
  u.src_layer = src_layer;
  u.graphics_api = static_cast<s32>(g_backend_info.api_type);
  u.correct_color_space = cc.bCorrectColorSpace;
  u.game_color_space = static_cast<s32>(cc.game_color_space);
  u.correct_gamma = cc.bCorrectGamma;
  u.game_gamma = cc.fGameGamma;
  u.sdr_display_gamma_sRGB = cc.bSDRDisplayGammaSRGB;
  u.sdr_display_custom_gamma = cc.fSDRDisplayCustomGamma;
  // scRGB (RGBA16F) takes linear values, which may leave the 0-1 range
  u.linear_space_output = hdr;
  u.hdr_output = hdr;
  u.hdr_paper_white_nits = cc.fHDRPaperWhiteNits;
  // 1.0 is about 80 nits in HDR
  u.hdr_sdr_white_nits = 80.f;

  std::memcpy(m_uniform_staging_buffer.data(), &u, sizeof(u));
}
}  // namespace VideoCommon
