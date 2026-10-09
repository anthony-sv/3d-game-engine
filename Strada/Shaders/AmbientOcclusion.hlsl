// Ground-truth ambient occlusion (Jimenez et al. 2016) from the opaque pass's depth and normals, a depth-aware denoise,
// and the composite that removes occluded indirect light from the scene color.

#include "Include/Common.hlsli"
#include "Include/RendererInterop.h"

ConstantBuffer<AmbientOcclusionConstants> g_AO : register(b0);
Texture2D<float> g_Depth : register(t0);
Texture2D<float2> g_Normals : register(t1);
Texture2D<float> g_RawOcclusion : register(t2);
Texture2D<float4> g_Indirect : register(t3);
Texture2D<float> g_Occlusion : register(t4);

[[vk::image_format("r32f")]] RWTexture2D<float> g_OcclusionOutput : register(u0);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> g_SceneColor : register(u1);

static float const HalfPI = PI * 0.5;

bool IsInside(int2 pixel)
{
	return all(pixel >= 0) && all(pixel < int2(g_AO.ViewportSize));
}

// View-space position of a pixel (reversed-Z depth; 0 is the far plane or infinity).
float3 ViewPosition(int2 pixel, float depth)
{
	float2 uv = (float2(pixel) + 0.5) * g_AO.InverseViewportSize;
	float4 position = mul(g_AO.InverseProjection, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
	return position.xyz / position.w;
}

float IntegrateArc(float horizon, float normalAngle)
{
	return 0.25 * (-cos(2.0 * horizon - normalAngle) + cos(normalAngle) + 2.0 * horizon * sin(normalAngle));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
	int2 pixel = int2(id.xy);
	if (!IsInside(pixel))
	{
		return;
	}
	float depth = g_Depth.Load(int3(pixel, 0));
	if (depth <= 0.0)
	{
		g_OcclusionOutput[pixel] = 1.0;
		return;
	}

	float3 position = ViewPosition(pixel, depth);
	float3 view = normalize(-position);
	float3 normal = normalize(mul((float3x3)g_AO.View, OctahedralDecode(g_Normals.Load(int3(pixel, 0)))));
	float radius = g_AO.Radius;
	float radiusPixels = g_AO.Orthographic != 0 ? radius * g_AO.ProjectionScale : radius * g_AO.ProjectionScale / max(-position.z, 1e-4);
	if (radiusPixels < 1.0)
	{
		g_OcclusionOutput[pixel] = 1.0;
		return;
	}

	float sliceNoise = InterleavedGradientNoise(float2(pixel));
	float stepNoise = InterleavedGradientNoise(float2(pixel) + float2(37.0, 17.0));
	uint const sliceCount = g_AO.SliceCount;
	uint const stepCount = g_AO.StepCount;
	float visibility = 0.0;
	for (uint slice = 0; slice < sliceCount; slice++)
	{
		float phi = (float(slice) + sliceNoise) / float(sliceCount) * PI;
		// Screen space has y down, view space y up.
		float2 direction = float2(cos(phi), sin(phi));
		float3 directionView = float3(direction.x, -direction.y, 0.0);
		float3 orthogonal = directionView - dot(directionView, view) * view;
		float3 axis = normalize(cross(directionView, view));
		float3 projectedNormal = normal - axis * dot(normal, axis);
		float projectedLength = length(projectedNormal);
		if (projectedLength < 1e-4)
		{
			visibility += 1.0;
			continue;
		}
		float cosNormal = saturate(dot(projectedNormal, view) / projectedLength);
		float normalAngle = (dot(projectedNormal, orthogonal) >= 0.0 ? 1.0 : -1.0) * acos(cosNormal);

		float sliceVisibility = 0.0;
		for (int side = -1; side <= 1; side += 2)
		{
			float cosHorizon = -1.0;
			for (uint i = 0; i < stepCount; i++)
			{
				float distancePixels = max((float(i) + stepNoise) / float(stepCount) * radiusPixels, 1.0);
				int2 samplePixel = pixel + int2(round(direction * distancePixels * float(side)));
				if (!IsInside(samplePixel))
				{
					break;
				}
				float sampleDepth = g_Depth.Load(int3(samplePixel, 0));
				if (sampleDepth <= 0.0)
				{
					continue;
				}
				float3 delta = ViewPosition(samplePixel, sampleDepth) - position;
				float distance = length(delta);
				if (distance < 1e-5)
				{
					continue;
				}
				// Occluders fade out towards the radius so distant geometry does not darken.
				float falloff = saturate((radius - distance) / (radius * 0.4));
				cosHorizon = max(cosHorizon, lerp(-1.0, dot(delta / distance, view), falloff));
			}
			// The side towards -direction has a negative horizon angle; both are limited to the normal's hemisphere.
			float horizon = float(side) * acos(clamp(cosHorizon, -1.0, 1.0));
			horizon = side > 0 ? normalAngle + min(horizon - normalAngle, HalfPI) : normalAngle + max(horizon - normalAngle, -HalfPI);
			sliceVisibility += IntegrateArc(horizon, normalAngle);
		}
		visibility += projectedLength * sliceVisibility;
	}
	visibility = saturate(visibility / float(sliceCount));
	g_OcclusionOutput[pixel] = pow(visibility, g_AO.Intensity);
}

// 5x5 Gaussian weighted by relative depth similarity: removes the noise without blurring across silhouettes.
[numthreads(8, 8, 1)]
void CSDenoise(uint3 id : SV_DispatchThreadID)
{
	int2 pixel = int2(id.xy);
	if (!IsInside(pixel))
	{
		return;
	}
	float depth = g_Depth.Load(int3(pixel, 0));
	if (depth <= 0.0)
	{
		g_OcclusionOutput[pixel] = 1.0;
		return;
	}
	float centerZ = -ViewPosition(pixel, depth).z;
	float sum = 0.0;
	float weightSum = 0.0;
	for (int y = -2; y <= 2; y++)
	{
		for (int x = -2; x <= 2; x++)
		{
			int2 samplePixel = clamp(pixel + int2(x, y), int2(0, 0), int2(g_AO.ViewportSize) - 1);
			float sampleDepth = g_Depth.Load(int3(samplePixel, 0));
			if (sampleDepth <= 0.0)
			{
				continue;
			}
			float sampleZ = -ViewPosition(samplePixel, sampleDepth).z;
			float spatial = exp(-float(x * x + y * y) / 4.5);
			float similarity = exp(-abs(sampleZ - centerZ) / max(0.05 * centerZ, 1e-3));
			float weight = spatial * similarity;
			sum += g_RawOcclusion.Load(int3(samplePixel, 0)) * weight;
			weightSum += weight;
		}
	}
	g_OcclusionOutput[pixel] = weightSum > 0.0 ? sum / weightSum : 1.0;
}

// Scene color minus the occluded part of the indirect (ambient and image-based) light.
[numthreads(8, 8, 1)]
void CSComposite(uint3 id : SV_DispatchThreadID)
{
	int2 pixel = int2(id.xy);
	if (!IsInside(pixel))
	{
		return;
	}
	float occlusion = g_Occlusion.Load(int3(pixel, 0));
	float3 indirect = g_Indirect.Load(int3(pixel, 0)).rgb;
	float4 color = g_SceneColor[pixel];
	color.rgb = max(color.rgb - indirect * (1.0 - occlusion), 0.0);
	g_SceneColor[pixel] = color;
}
