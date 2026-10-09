// Fast approximate anti-aliasing (after Lottes' FXAA 3.11 quality algorithm) on the tonemapped, sRGB-encoded image:
// detects luma edges, searches along them for their ends and blends across the edge by the pixel's position on it.

#include "Include/Common.hlsli"
#include "Include/RendererInterop.h"

[[vk::push_constant]] ConstantBuffer<FxaaConstants> g_Fxaa : register(b0);
Texture2D g_Input : register(t0);
SamplerState g_LinearClamp : register(s0);

struct FullscreenVertexOutput
{
	float4 Position : SV_Position;
	float2 TexCoord : TEXCOORD0;
};

static float const EdgeThreshold = 0.125;
static float const EdgeThresholdMin = 0.0312;
static float const SubpixelQuality = 0.75;
static uint const SearchSteps = 12;
static float const SearchStepSizes[SearchSteps] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0};

float LumaAt(float2 uv)
{
	// Perceptual luma of the encoded color.
	return dot(g_Input.SampleLevel(g_LinearClamp, uv, 0).rgb, float3(0.299, 0.587, 0.114));
}

float4 PSMain(FullscreenVertexOutput input) : SV_Target0
{
	float2 uv = input.TexCoord;
	float2 texel = g_Fxaa.InverseSize;
	float4 center = g_Input.SampleLevel(g_LinearClamp, uv, 0);
	float lumaCenter = dot(center.rgb, float3(0.299, 0.587, 0.114));

	// UV y points down: "up" is -y.
	float lumaUp = LumaAt(uv + float2(0.0, -texel.y));
	float lumaDown = LumaAt(uv + float2(0.0, texel.y));
	float lumaLeft = LumaAt(uv + float2(-texel.x, 0.0));
	float lumaRight = LumaAt(uv + float2(texel.x, 0.0));
	float lumaMin = min(lumaCenter, min(min(lumaUp, lumaDown), min(lumaLeft, lumaRight)));
	float lumaMax = max(lumaCenter, max(max(lumaUp, lumaDown), max(lumaLeft, lumaRight)));
	float range = lumaMax - lumaMin;
	if (range < max(EdgeThresholdMin, lumaMax * EdgeThreshold))
	{
		return center;
	}

	float lumaUpLeft = LumaAt(uv + float2(-texel.x, -texel.y));
	float lumaUpRight = LumaAt(uv + float2(texel.x, -texel.y));
	float lumaDownLeft = LumaAt(uv + float2(-texel.x, texel.y));
	float lumaDownRight = LumaAt(uv + float2(texel.x, texel.y));

	// A horizontal edge separates rows (luma changes vertically).
	float edgeHorizontal = abs(lumaUpLeft - 2.0 * lumaLeft + lumaDownLeft) + 2.0 * abs(lumaUp - 2.0 * lumaCenter + lumaDown) +
	                       abs(lumaUpRight - 2.0 * lumaRight + lumaDownRight);
	float edgeVertical = abs(lumaUpLeft - 2.0 * lumaUp + lumaUpRight) + 2.0 * abs(lumaLeft - 2.0 * lumaCenter + lumaRight) +
	                     abs(lumaDownLeft - 2.0 * lumaDown + lumaDownRight);
	bool isHorizontal = edgeHorizontal >= edgeVertical;

	// The two neighbors across the edge; step towards the one with the steeper gradient.
	float luma1 = isHorizontal ? lumaUp : lumaLeft;
	float luma2 = isHorizontal ? lumaDown : lumaRight;
	float gradient1 = luma1 - lumaCenter;
	float gradient2 = luma2 - lumaCenter;
	bool is1Steepest = abs(gradient1) >= abs(gradient2);
	float gradientScaled = 0.25 * max(abs(gradient1), abs(gradient2));
	float2 acrossStep = isHorizontal ? float2(0.0, texel.y) : float2(texel.x, 0.0);
	float lumaLocalAverage;
	if (is1Steepest)
	{
		acrossStep = -acrossStep;
		lumaLocalAverage = 0.5 * (luma1 + lumaCenter);
	}
	else
	{
		lumaLocalAverage = 0.5 * (luma2 + lumaCenter);
	}

	// Search along the edge (on the boundary between the two rows/columns) for its ends.
	float2 edgeUV = uv + acrossStep * 0.5;
	float2 alongStep = isHorizontal ? float2(texel.x, 0.0) : float2(0.0, texel.y);
	float2 uv1 = edgeUV - alongStep;
	float2 uv2 = edgeUV + alongStep;
	float lumaEnd1 = LumaAt(uv1) - lumaLocalAverage;
	float lumaEnd2 = LumaAt(uv2) - lumaLocalAverage;
	bool reached1 = abs(lumaEnd1) >= gradientScaled;
	bool reached2 = abs(lumaEnd2) >= gradientScaled;
	for (uint i = 1; i < SearchSteps && !(reached1 && reached2); i++)
	{
		if (!reached1)
		{
			uv1 -= alongStep * SearchStepSizes[i];
			lumaEnd1 = LumaAt(uv1) - lumaLocalAverage;
			reached1 = abs(lumaEnd1) >= gradientScaled;
		}
		if (!reached2)
		{
			uv2 += alongStep * SearchStepSizes[i];
			lumaEnd2 = LumaAt(uv2) - lumaLocalAverage;
			reached2 = abs(lumaEnd2) >= gradientScaled;
		}
	}

	float distance1 = isHorizontal ? uv.x - uv1.x : uv.y - uv1.y;
	float distance2 = isHorizontal ? uv2.x - uv.x : uv2.y - uv.y;
	bool isDirection1 = distance1 < distance2;
	float edgeLength = distance1 + distance2;
	float pixelOffset = -min(distance1, distance2) / edgeLength + 0.5;
	// Only blend when the luma at the closer end varies in the direction consistent with the center.
	bool isCenterSmaller = lumaCenter < lumaLocalAverage;
	bool correctVariation = ((isDirection1 ? lumaEnd1 : lumaEnd2) < 0.0) != isCenterSmaller;
	float edgeOffset = correctVariation ? pixelOffset : 0.0;

	// Sub-pixel aliasing: blend by the contrast between the pixel and its 3x3 neighborhood average.
	float lumaAverage = (2.0 * (lumaUp + lumaDown + lumaLeft + lumaRight) + lumaUpLeft + lumaUpRight + lumaDownLeft + lumaDownRight) / 12.0;
	float subpixel = saturate(abs(lumaAverage - lumaCenter) / range);
	subpixel = smoothstep(0.0, 1.0, subpixel);
	float subpixelOffset = subpixel * subpixel * SubpixelQuality;

	float offset = max(edgeOffset, subpixelOffset);
	float2 isHorizontalStep = isHorizontal ? float2(0.0, 1.0) : float2(1.0, 0.0);
	float2 finalUV = uv + isHorizontalStep * (is1Steepest ? -1.0 : 1.0) * texel * offset;
	return float4(g_Input.SampleLevel(g_LinearClamp, finalUV, 0).rgb, 1.0);
}
