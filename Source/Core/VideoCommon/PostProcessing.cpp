// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/PostProcessing.h"

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

/***** Bicubic Interpolation *****/

// Formula derived from:
// https://en.wikipedia.org/wiki/Mitchell%E2%80%93Netravali_filters#Definition
// Values from:
// https://guideencodemoe-mkdocs.readthedocs.io/encoding/resampling/#mitchell-netravali-bicubic
// Other references:
// https://www.codeproject.com/Articles/236394/Bi-Cubic-and-Bi-Linear-Interpolation-with-GLSL
// https://github.com/ValveSoftware/gamescope/pull/740
// https://stackoverflow.com/questions/13501081/efficient-bicubic-filtering-code-in-glsl
#define CUBIC_COEFF_GEN(B, C)																						\
	(mat4(/* t^0 */ ((B) / 6.0), (-(B) / 3.0 + 1.0), ((B) / 6.0), (0.0),	\
		/* t^1 */ (-(B) / 2.0 - (C)), (0.0), ((B) / 2.0 + (C)), (0.0),			\
		/* t^2 */ ((B) / 2.0 + 2.0 * (C)), (2.0 * (B) + (C)-3.0),						\
		(-5.0 * (B) / 2.0 - 2.0 * (C) + 3.0), (-(C)),												\
		/* t^3 */ (-(B) / 6.0 - (C)), (-3.0 * (B) / 2.0 - (C) + 2.0),				\
		(3.0 * (B) / 2.0 + (C)-2.0), ((B) / 6.0 + (C))))

float4 CubicCoeffs(float t, mat4 coeffs)
{
	return coeffs * float4(1.0, t, t * t, t * t * t);
}

float4 CubicMix(float4 c0, float4 c1, float4 c2, float4 c3, float4 coeffs)
{
	return c0 * coeffs[0] + c1 * coeffs[1] + c2 * coeffs[2] + c3 * coeffs[3];
}

// By Sam Belliveau. Public Domain license.
// Simple 16 tap, gamma correct, implementation of bicubic filtering.
float4 BicubicSample(float3 uvw, float gamma, mat4 coeffs)
{
	float2 pixel = (uvw.xy * GetResolution()) - 0.5;
	float2 int_pixel = floor(pixel);
	float2 frac_pixel = fract(pixel);

	float4 c00 = QuickSampleByPixel(int_pixel + float2(-0.5, -0.5), uvw.z, gamma);
	float4 c10 = QuickSampleByPixel(int_pixel + float2(+0.5, -0.5), uvw.z, gamma);
	float4 c20 = QuickSampleByPixel(int_pixel + float2(+1.5, -0.5), uvw.z, gamma);
	float4 c30 = QuickSampleByPixel(int_pixel + float2(+2.5, -0.5), uvw.z, gamma);

	float4 c01 = QuickSampleByPixel(int_pixel + float2(-0.5, +0.5), uvw.z, gamma);
	float4 c11 = QuickSampleByPixel(int_pixel + float2(+0.5, +0.5), uvw.z, gamma);
	float4 c21 = QuickSampleByPixel(int_pixel + float2(+1.5, +0.5), uvw.z, gamma);
	float4 c31 = QuickSampleByPixel(int_pixel + float2(+2.5, +0.5), uvw.z, gamma);

	float4 c02 = QuickSampleByPixel(int_pixel + float2(-0.5, +1.5), uvw.z, gamma);
	float4 c12 = QuickSampleByPixel(int_pixel + float2(+0.5, +1.5), uvw.z, gamma);
	float4 c22 = QuickSampleByPixel(int_pixel + float2(+1.5, +1.5), uvw.z, gamma);
	float4 c32 = QuickSampleByPixel(int_pixel + float2(+2.5, +1.5), uvw.z, gamma);

	float4 c03 = QuickSampleByPixel(int_pixel + float2(-0.5, +2.5), uvw.z, gamma);
	float4 c13 = QuickSampleByPixel(int_pixel + float2(+0.5, +2.5), uvw.z, gamma);
	float4 c23 = QuickSampleByPixel(int_pixel + float2(+1.5, +2.5), uvw.z, gamma);
	float4 c33 = QuickSampleByPixel(int_pixel + float2(+2.5, +2.5), uvw.z, gamma);

	float4 cx = CubicCoeffs(frac_pixel.x, coeffs);
	float4 cy = CubicCoeffs(frac_pixel.y, coeffs);

	float4 x0 = CubicMix(c00, c10, c20, c30, cx);
	float4 x1 = CubicMix(c01, c11, c21, c31, cx);
	float4 x2 = CubicMix(c02, c12, c22, c32, cx);
	float4 x3 = CubicMix(c03, c13, c23, c33, cx);

	return CubicMix(x0, x1, x2, x3, cy);
}

/***** Sharp Bilinear Filtering *****/

// Based on https://github.com/libretro/slang-shaders/blob/master/interpolation/shaders/sharp-bilinear.slang
// by Themaister, Public Domain license
// Does a bilinear stretch, with a preapplied Nx nearest-neighbor scale,
// giving a sharper image than plain bilinear.
float4 SharpBilinearSample(float3 uvw, float gamma)
{
	float2 source_size = GetResolution();
	float2 inverted_source_size = GetInvResolution();
	float2 target_size = GetWindowResolution();
	float2 texel = uvw.xy * source_size;
	float2 texel_floored = floor(texel);
	float2 s = fract(texel);
	float scale = max(floor(max(target_size.x * inverted_source_size.x, target_size.y * inverted_source_size.y)), 1.0);
	float region_range = 0.5 - (0.5 / scale);

	// Figure out where in the texel to sample to get correct pre-scaled bilinear.

	float2 center_dist = s - 0.5;
	float2 f = ((center_dist - clamp(center_dist, -region_range, region_range)) * scale) + 0.5;

	float2 mod_texel = texel_floored + f;

	uvw.xy = mod_texel * inverted_source_size;
	return BilinearSample(uvw, gamma);
}

/***** Area Sampling *****/

// By Sam Belliveau and Filippo Tarpini. Public Domain license.
// Effectively a more accurate sharp bilinear filter when upscaling,
// that also works as a mathematically perfect downscale filter.
// https://entropymine.com/imageworsener/pixelmixing/
// https://github.com/obsproject/obs-studio/pull/1715
// https://legacy.imagemagick.org/Usage/filter/
float4 AreaSampling(float3 uvw, float gamma)
{
	// Determine the sizes of the source and target images.
	float2 source_size = GetResolution();
	float2 target_size = GetWindowResolution();
	float2 inverted_target_size = GetInvWindowResolution();

	// Compute the top-left and bottom-right corners of the target pixel box.
	float2 t_beg = floor(uvw.xy * target_size);
	float2 t_end = t_beg + float2(1.0, 1.0);

	// Convert the target pixel box to source pixel box.
	float2 beg = t_beg * inverted_target_size * source_size;
	float2 end = t_end * inverted_target_size * source_size;

	// Compute the top-left and bottom-right corners of the pixel box.
	float2 f_beg = floor(beg);
	float2 f_end = floor(end);

	// Compute how much of the start and end pixels are covered horizontally & vertically.
	float area_w = 1.0 - fract(beg.x);
	float area_n = 1.0 - fract(beg.y);
	float area_e = fract(end.x);
	float area_s = fract(end.y);

	// Compute the areas of the corner pixels in the pixel box.
	float area_nw = area_n * area_w;
	float area_ne = area_n * area_e;
	float area_sw = area_s * area_w;
	float area_se = area_s * area_e;

	// Initialize the color accumulator.
	float4 avg_color = float4(0.0, 0.0, 0.0, 0.0);
	float avg_luminance = 0.0;
	float4 temp_color;

	float luminance_gamma = gamma; // For now, default to the user selected gamma (alternatively we could fix it to anything between 2.2 and 3.0)
	float luminance_inv_gamma = 1.0 / luminance_gamma;

	// Prevents rounding errors due to the coordinates flooring above
	const float2 offset = float2(0.5, 0.5);

	// Accumulate corner pixels.
	temp_color = QuickSampleByPixel(float2(f_beg.x, f_beg.y) + offset, uvw.z, gamma);
	avg_color += area_nw * temp_color;
	avg_luminance += area_nw * pow(Luminance(temp_color.rgb, true), luminance_inv_gamma);
	temp_color = QuickSampleByPixel(float2(f_end.x, f_beg.y) + offset, uvw.z, gamma);
	avg_color += area_ne * temp_color;
	avg_luminance += area_ne * pow(Luminance(temp_color.rgb, true), luminance_inv_gamma);
	temp_color = QuickSampleByPixel(float2(f_beg.x, f_end.y) + offset, uvw.z, gamma);
	avg_color += area_sw * temp_color;
	avg_luminance += area_sw * pow(Luminance(temp_color.rgb, true), luminance_inv_gamma);
	temp_color = QuickSampleByPixel(float2(f_end.x, f_end.y) + offset, uvw.z, gamma);
	avg_color += area_se * temp_color;
	avg_luminance += area_se * pow(Luminance(temp_color.rgb, true), luminance_inv_gamma);

	// Determine the size of the pixel box.
	int x_range = int(f_end.x - f_beg.x - 0.5);
	int y_range = int(f_end.y - f_beg.y - 0.5);

	// Workaround to compile the shader with DX11/12.
	// If this isn't done, it will complain that the loop could have too many iterations.
	// This number should be enough to guarantee downscaling from very high to very small resolutions.
	// Note that this number might be referenced in the UI.
	const int max_iterations = 16;

	// Fix up the average calculations in case we reached the upper limit
	x_range = min(x_range, max_iterations);
	y_range = min(y_range, max_iterations);

)"
R"(	// Accumulate top and bottom edge pixels.
	for (int ix = 0; ix < max_iterations; ++ix)
	{
		if (ix < x_range)
		{
			float x = f_beg.x + 1.0 + float(ix);
			temp_color = QuickSampleByPixel(float2(x, f_beg.y) + offset, uvw.z, gamma);
			avg_color += area_n * temp_color;
			avg_luminance += area_n * pow(Luminance(temp_color.rgb, true), luminance_inv_gamma);
			temp_color = QuickSampleByPixel(float2(x, f_end.y) + offset, uvw.z, gamma);
			avg_color += area_s * temp_color;
			avg_luminance += area_s * pow(Luminance(temp_color.rgb, true), luminance_inv_gamma);
		}
	}

	// Accumulate left and right edge pixels and all the pixels in between.
	for (int iy = 0; iy < max_iterations; ++iy)
	{
		if (iy < y_range)
		{
			float y = f_beg.y + 1.0 + float(iy);
			
			temp_color = QuickSampleByPixel(float2(f_beg.x, y) + offset, uvw.z, gamma);
			avg_color += area_w * temp_color;
			avg_luminance += area_w * pow(Luminance(temp_color.rgb, true), luminance_inv_gamma);
			temp_color = QuickSampleByPixel(float2(f_end.x, y) + offset, uvw.z, gamma);
			avg_color += area_e * temp_color;
			avg_luminance += area_e * pow(Luminance(temp_color.rgb, true), luminance_inv_gamma);

			for (int ix = 0; ix < max_iterations; ++ix)
			{
				if (ix < x_range)
				{
					float x = f_beg.x + 1.0 + float(ix);
					temp_color = QuickSampleByPixel(float2(x, y) + offset, uvw.z, gamma);
					avg_color += temp_color;
					avg_luminance += pow(Luminance(temp_color.rgb, true), luminance_inv_gamma);
				}
			}
		}
	}

	// Compute the area of the pixel box that was sampled.
	float area_corners = area_nw + area_ne + area_sw + area_se;
	float area_edges = float(x_range) * (area_n + area_s) + float(y_range) * (area_w + area_e);
	float area_center = float(x_range) * float(y_range);
	
	float4 nrm_color = avg_color / (area_corners + area_edges + area_center);
	float target_nrm_color_luminance = avg_luminance / (area_corners + area_edges + area_center);

	// Restore the averaged "gamma" space luminance, for better gamma correction.
	// This retains the best feature of gamma correct sampling (no hue shifts),
	// while also maintaining the perceptual "brightness" level of blending two colors with an alpha
	// (in linear space a 0.5 alpha won't produce a color that has a perceptual brightness in the middle point of the two source colors).
	float nrm_color_luminance = Luminance(nrm_color.rgb, true);
	if (nrm_color_luminance != 0.0)
	{
		nrm_color.rgb *= pow(target_nrm_color_luminance, luminance_gamma) / nrm_color_luminance;
	}

	// Return the normalized average color.
	return nrm_color;
}

/***** Main Functions *****/

// Returns an accurate (gamma corrected) sample of a gamma space space texture.
// Outputs in linear space for simplicity.
float4 LinearGammaCorrectedSample(float gamma)
{
	float3 uvw = v_tex0;
	float4 color = float4(0, 0, 0, 1);

	if (resampling_method <= 1) // Bilinear
	{
		color = BilinearSample(uvw, gamma);
	}
	else if (resampling_method == 2) // Bicubic: B-Spline
	{
		color = BicubicSample(uvw, gamma, CUBIC_COEFF_GEN(1.0, 0.0));
	}
	else if (resampling_method == 3) // Bicubic: Mitchell-Netravali
	{
		color = BicubicSample(uvw, gamma, CUBIC_COEFF_GEN(1.0 / 3.0, 1.0 / 3.0));
	}
	else if (resampling_method == 4) // Bicubic: Catmull-Rom
	{
		color = BicubicSample(uvw, gamma, CUBIC_COEFF_GEN(0.0, 0.5));
	}
	else if (resampling_method == 5) // Sharp Bilinear
	{
		color = SharpBilinearSample(uvw, gamma);
	}
	else if (resampling_method == 6) // Area Sampling
	{
		color = AreaSampling(uvw, gamma);
	}
	else if (resampling_method == 7) // Nearest Neighbor
	{
		color = QuickSample(uvw, gamma);
	}
	else if (resampling_method == 8) // Bicubic: Hermite
	{
		color = BicubicSample(uvw, gamma, CUBIC_COEFF_GEN(0.0, 0.0));
	}

	return color;
}

void main()
{
	// This tries to fall back on GPU HW sampling if it can (it won't be gamma corrected).
	bool raw_resampling = resampling_method <= 0;
	bool needs_rescaling = GetResolution() != GetWindowResolution();

	bool needs_resampling = needs_rescaling && (OptionEnabled(hdr_output) || OptionEnabled(correct_gamma) || !raw_resampling);

	float4 color;

	if (needs_resampling)
	{
		// Doing linear sampling in "gamma space" on linear texture formats isn't correct.
		// If the source and target resolutions don't match, the GPU will return a color
		// that is the average of 4 gamma space colors, but gamma space colors can't be blended together,
		// gamma needs to be de-applied first. This makes a big difference if colors change
		// drastically between two pixels.

		color = LinearGammaCorrectedSample(game_gamma);
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
  s32 resampling_method;
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
  int resampling_method;
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

std::string PixelShaderSource(const char* body)
{
  std::string source = s_uniform_block;
  if (g_backend_info.bSupportsGeometryShaders)
    source += "VARYING_LOCATION(0) in VertexData {\n  float3 v_tex0;\n};\n";
  else
    source += "VARYING_LOCATION(0) in float3 v_tex0;\n";
  source += s_pixel_header;
  source += body;
  return source;
}

std::string VertexShaderSource()
{
  std::string source = s_uniform_block;
  if (g_backend_info.bSupportsGeometryShaders)
    source += "VARYING_LOCATION(0) out VertexData {\n  float3 v_tex0;\n};\n";
  else
    source += "VARYING_LOCATION(0) out float3 v_tex0;\n";
  source += R"(void main() {
  v_tex0 = float3(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2), 0.0f);
  gl_Position = float4(v_tex0.xy * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
  v_tex0 = float3(src_rect.xy + (src_rect.zw * v_tex0.xy), float(src_layer));
)";
  // Vulkan's Y axis points down
  if (g_backend_info.api_type == APIType::Vulkan)
    source += "  gl_Position.y = -gl_Position.y;\n";
  source += "}\n";
  return source;
}
}  // namespace

PostProcessing::PostProcessing() = default;
PostProcessing::~PostProcessing() = default;

bool PostProcessing::Initialize(AbstractTextureFormat format)
{
  m_framebuffer_format = format;
  m_uniform_staging_buffer.resize(sizeof(Uniforms));
  return CompileShaders() && CompilePipelines();
}

void PostProcessing::RecompilePipeline()
{
  m_copy_pipeline.reset();
  m_correction_pipeline.reset();
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

  // The default bilinear sampler covers Default resampling; anything else, and colour
  // correction or HDR, takes the correction shader.
  const bool needs_correction =
      IsColorCorrectionActive() ||
      g_ActiveConfig.output_resampling_mode > OutputResamplingMode::Default;
  const AbstractPipeline* pipeline =
      needs_correction && m_correction_pipeline ? m_correction_pipeline.get() :
                                                  m_copy_pipeline.get();
  if (!pipeline)
    return;

  g_gfx->SetSamplerState(0, RenderState::GetLinearSamplerState());
  g_gfx->SetSamplerState(1, RenderState::GetPointSamplerState());
  g_gfx->SetTexture(0, src_tex);
  g_gfx->SetTexture(1, src_tex);

  FillUniformBuffer(src, src_tex, std::max(src_layer, 0), g_gfx->GetCurrentFramebuffer()->GetRect(),
                    g_presenter->GetTargetRectangle());
  g_vertex_manager->UploadUtilityUniforms(m_uniform_staging_buffer.data(),
                                          static_cast<u32>(m_uniform_staging_buffer.size()));

  g_gfx->SetViewportAndScissor(
      g_gfx->ConvertFramebufferRectangle(dst, g_gfx->GetCurrentFramebuffer()));
  g_gfx->SetPipeline(pipeline);
  g_gfx->Draw(0, 3);
}

bool PostProcessing::CompileShaders()
{
  m_vertex_shader = g_gfx->CreateShaderFromSource(ShaderStage::Vertex, VertexShaderSource(), "Present vertex shader");
  m_copy_pixel_shader = g_gfx->CreateShaderFromSource(
      ShaderStage::Pixel, PixelShaderSource(s_copy_pixel_shader),
      "Present copy pixel shader");
  // Optional: without it the output is copied uncorrected.
  m_correction_pixel_shader = g_gfx->CreateShaderFromSource(
      ShaderStage::Pixel, PixelShaderSource(s_correction_pixel_shader),
      "Present correction pixel shader");

  if (!m_vertex_shader || !m_copy_pixel_shader)
  {
    PanicAlertFmt("Failed to compile the presentation shaders");
    return false;
  }
  return true;
}

bool PostProcessing::CompilePipelines()
{
  // Some backends refuse pipelines with no targets; nothing is drawn then anyway.
  if (m_framebuffer_format == AbstractTextureFormat::Undefined)
    return true;

  AbstractPipelineConfig config = {};
  config.vertex_shader = m_vertex_shader.get();
  config.rasterization_state = RenderState::GetNoCullRasterizationState(PrimitiveType::Triangles);
  config.depth_state = RenderState::GetNoDepthTestingDepthState();
  config.blending_state = RenderState::GetNoBlendingBlendState();
  config.framebuffer_state = RenderState::GetColorFramebufferState(m_framebuffer_format);
  config.usage = AbstractPipelineUsage::Utility;

  if (m_correction_pixel_shader)
  {
    config.pixel_shader = m_correction_pixel_shader.get();
    m_correction_pipeline = g_gfx->CreatePipeline(config);
  }

  config.pixel_shader = m_copy_pixel_shader.get();
  m_copy_pipeline = g_gfx->CreatePipeline(config);
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
  u.resampling_method = static_cast<s32>(g_ActiveConfig.output_resampling_mode);
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
