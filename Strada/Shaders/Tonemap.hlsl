// Maps exposed HDR radiance to display values: bloom mix, tone curve, dithering, sRGB encoding into an 8-bit UNORM target.

#include "Include/Common.hlsli"
#include "Include/RendererInterop.h"

static const uint TonemapACES = 0;
static const uint TonemapAgX = 1;
static const uint TonemapPBRNeutral = 2;
static const uint TonemapReinhard = 3;
static const uint TonemapNone = 4;

Texture2D g_SceneColor : register(t0);
Texture2D g_Bloom : register(t1);
SamplerState g_LinearClamp : register(s0);
[[vk::push_constant]] ConstantBuffer<TonemapConstants> g_Tonemap : register(b0);

struct FullscreenVertexOutput
{
	float4 Position : SV_Position;
	float2 TexCoord : TEXCOORD0;
};

// ACES filmic curve, Stephen Hill's fit of the RRT + ODT.
float3 TonemapACESFitted(float3 color)
{
	static const float3x3 InputMatrix = {
		{0.59719, 0.35458, 0.04823},
		{0.07600, 0.90834, 0.01566},
		{0.02840, 0.13383, 0.83777},
	};
	static const float3x3 OutputMatrix = {
		{1.60475, -0.53108, -0.07367},
		{-0.10208, 1.10813, -0.00605},
		{-0.00327, -0.07276, 1.07602},
	};
	color = mul(InputMatrix, color);
	float3 a = color * (color + 0.0245786) - 0.000090537;
	float3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
	return saturate(mul(OutputMatrix, a / b));
}

// AgX (Benjamin Wrensch's minimal fit of Troy Sobotka's AgX), returning linear sRGB.
float3 TonemapAgXFitted(float3 color)
{
	// Rows of the original column-major matrices: mul(vector, matrix) reproduces matrix * vector.
	static const float3x3 InsetMatrix = {
		{0.842479062253094, 0.0423282422610123, 0.0423756549057051},
		{0.0784335999999992, 0.878468636469772, 0.0784336},
		{0.0792237451477643, 0.0791661274605434, 0.879142973793104},
	};
	static const float3x3 OutsetMatrix = {
		{1.19687900512017, -0.0528968517574562, -0.0529716355144438},
		{-0.0980208811401368, 1.15190312990417, -0.0980434501171241},
		{-0.0990297440797205, -0.0989611768448433, 1.15107367264116},
	};
	static const float MinEv = -12.47393;
	static const float MaxEv = 4.026069;

	color = mul(max(color, 1e-10), InsetMatrix);
	color = clamp(log2(color), MinEv, MaxEv);
	color = (color - MinEv) / (MaxEv - MinEv);
	float3 x2 = color * color;
	float3 x4 = x2 * x2;
	color = 15.5 * x4 * x2 - 40.14 * x4 * color + 31.96 * x4 - 6.868 * x2 * color + 0.4298 * x2 + 0.1191 * color - 0.00232;
	color = mul(color, OutsetMatrix);
	// The curve produces display-encoded values (2.2 gamma); linearize so the common sRGB encoding applies.
	return pow(saturate(color), 2.2);
}

// Khronos PBR Neutral.
float3 TonemapPBRNeutralCurve(float3 color)
{
	static const float StartCompression = 0.8 - 0.04;
	static const float Desaturation = 0.15;
	float x = min(color.r, min(color.g, color.b));
	float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
	color -= offset;
	float peak = max(color.r, max(color.g, color.b));
	if (peak < StartCompression)
	{
		return color;
	}
	float d = 1.0 - StartCompression;
	float newPeak = 1.0 - d * d / (peak + d - StartCompression);
	color *= newPeak / peak;
	float g = 1.0 - 1.0 / (Desaturation * (peak - newPeak) + 1.0);
	return lerp(color, newPeak.xxx, g);
}

float4 PSMain(FullscreenVertexOutput input) : SV_Target0
{
	float3 color = max(g_SceneColor.Load(int3(input.Position.xy, 0)).rgb, 0.0);
	if (g_Tonemap.BloomIntensity > 0.0)
	{
		// Energy-conserving mix: bloom redistributes light instead of adding it.
		color = lerp(color, g_Bloom.SampleLevel(g_LinearClamp, input.TexCoord, 0).rgb * g_Tonemap.BloomNormalization,
		             g_Tonemap.BloomIntensity);
	}
	switch (g_Tonemap.Operator)
	{
		case TonemapACES:
			color = TonemapACESFitted(color);
			break;
		case TonemapAgX:
			color = TonemapAgXFitted(color);
			break;
		case TonemapPBRNeutral:
			color = TonemapPBRNeutralCurve(color);
			break;
		case TonemapReinhard:
			color = color / (1.0 + color);
			break;
		default:
			break;
	}

	float3 encoded = LinearToSrgb(saturate(color));
	if (g_Tonemap.Dither != 0)
	{
		// Triangular-distributed noise of one 8-bit step removes banding in gradients.
		float noise = InterleavedGradientNoise(input.Position.xy) + InterleavedGradientNoise(input.Position.xy + 17.0) - 1.0;
		encoded += noise / 255.0;
	}
	return float4(saturate(encoded), 1.0);
}
