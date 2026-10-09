// Physically based bloom (Jimenez 2014, "Next Generation Post Processing in Call of Duty"): a 13-tap downsample chain
// (Karis average on the first level) followed by a 3x3 tent upsample that accumulates every level into the first.

#include "Include/Common.hlsli"
#include "Include/RendererInterop.h"

[[vk::push_constant]] ConstantBuffer<BloomConstants> g_Bloom : register(b0);
Texture2D g_Source : register(t0);
SamplerState g_LinearClamp : register(s0);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> g_Output : register(u0);

float KarisWeight(float3 color)
{
	return 1.0 / (1.0 + Luminance(color));
}

float3 SampleSource(float2 uv)
{
	// Guards against NaN/negative values from the scene spreading through the whole chain.
	float3 color = g_Source.SampleLevel(g_LinearClamp, uv, 0).rgb;
	return max(select(isnan(color), 0.0, color), 0.0);
}

[numthreads(8, 8, 1)]
void CSDownsample(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= g_Bloom.OutputSize))
	{
		return;
	}
	float2 uv = (float2(id.xy) + 0.5) / float2(g_Bloom.OutputSize);
	float2 t = g_Bloom.SourceTexelSize;

	float3 a = SampleSource(uv + t * float2(-2.0, -2.0));
	float3 b = SampleSource(uv + t * float2(0.0, -2.0));
	float3 c = SampleSource(uv + t * float2(2.0, -2.0));
	float3 d = SampleSource(uv + t * float2(-1.0, -1.0));
	float3 e = SampleSource(uv + t * float2(1.0, -1.0));
	float3 f = SampleSource(uv + t * float2(-2.0, 0.0));
	float3 g = SampleSource(uv);
	float3 h = SampleSource(uv + t * float2(2.0, 0.0));
	float3 i = SampleSource(uv + t * float2(-1.0, 1.0));
	float3 j = SampleSource(uv + t * float2(1.0, 1.0));
	float3 k = SampleSource(uv + t * float2(-2.0, 2.0));
	float3 l = SampleSource(uv + t * float2(0.0, 2.0));
	float3 m = SampleSource(uv + t * float2(2.0, 2.0));

	// Five overlapping 2x2 boxes: the inner one weighs 0.5, the four corner ones 0.125 each.
	float3 boxes[5] = {(d + e + i + j) * 0.25, (a + b + f + g) * 0.25, (b + c + g + h) * 0.25, (f + g + k + l) * 0.25,
	                   (g + h + l + m) * 0.25};
	float weights[5] = {0.5, 0.125, 0.125, 0.125, 0.125};
	float3 result = 0.0;
	float weightSum = 0.0;
	for (uint box = 0; box < 5; box++)
	{
		float weight = weights[box] * (g_Bloom.FirstPass != 0 ? KarisWeight(boxes[box]) : 1.0);
		result += boxes[box] * weight;
		weightSum += weight;
	}
	g_Output[id.xy] = float4(result / weightSum, 1.0);
}

// Adds the tent-filtered next (smaller) level to this level.
[numthreads(8, 8, 1)]
void CSUpsample(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= g_Bloom.OutputSize))
	{
		return;
	}
	float2 uv = (float2(id.xy) + 0.5) / float2(g_Bloom.OutputSize);
	float2 t = g_Bloom.SourceTexelSize;
	float3 sum = SampleSource(uv) * 4.0;
	sum += (SampleSource(uv + float2(-t.x, 0.0)) + SampleSource(uv + float2(t.x, 0.0)) + SampleSource(uv + float2(0.0, -t.y)) +
	        SampleSource(uv + float2(0.0, t.y))) *
	       2.0;
	sum += SampleSource(uv - t) + SampleSource(uv + t) + SampleSource(uv + float2(-t.x, t.y)) + SampleSource(uv + float2(t.x, -t.y));
	float4 current = g_Output[id.xy];
	g_Output[id.xy] = float4(current.rgb + sum / 16.0, 1.0);
}
