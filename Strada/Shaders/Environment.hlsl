// Image-based lighting precomputation (compute): equirect to cubemap, cubemap mip downsampling, diffuse irradiance,
// GGX-prefiltered specular radiance and the split-sum BRDF lookup table.

#include "Include/IBL.hlsli"
#include "Include/RendererInterop.h"

[[vk::push_constant]] ConstantBuffer<EnvironmentConstants> g_Environment : register(b0);

Texture2D g_Equirect : register(t0);
Texture2DArray g_SourceMip : register(t1);
TextureCube g_SourceCube : register(t2);
SamplerState g_LinearSampler : register(s0);

// Explicit storage formats keep the SPIR-V portable (no shaderStorageImageWriteWithoutFormat).
[[vk::image_format("rgba16f")]] RWTexture2DArray<float4> g_OutputCube : register(u0);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> g_OutputLut : register(u1);

float2 FaceUV(uint2 texel, uint size)
{
	return (float2(texel) + 0.5) / float(size);
}

[numthreads(8, 8, 1)]
void CSEquirectToCube(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= g_Environment.OutputSize || id.y >= g_Environment.OutputSize)
	{
		return;
	}
	float3 direction = CubeFaceDirection(id.z, FaceUV(id.xy, g_Environment.OutputSize));
	float3 radiance = g_Equirect.SampleLevel(g_LinearSampler, DirectionToEquirect(direction), 0).rgb;
	// Clamp extreme values (a direct sun can exceed half-float range after filtering).
	g_OutputCube[id] = float4(min(radiance, 60000.0), 1.0);
}

[numthreads(8, 8, 1)]
void CSDownsample(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= g_Environment.OutputSize || id.y >= g_Environment.OutputSize)
	{
		return;
	}
	uint2 source = id.xy * 2;
	float4 sum = g_SourceMip.Load(int4(source, id.z, 0)) + g_SourceMip.Load(int4(source + uint2(1, 0), id.z, 0)) +
	             g_SourceMip.Load(int4(source + uint2(0, 1), id.z, 0)) + g_SourceMip.Load(int4(source + uint2(1, 1), id.z, 0));
	g_OutputCube[id] = sum * 0.25;
}

// Cosine-weighted hemisphere integral with filtered importance sampling (Krivanek & Colbert).
[numthreads(8, 8, 1)]
void CSIrradiance(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= g_Environment.OutputSize || id.y >= g_Environment.OutputSize)
	{
		return;
	}
	float3 normal = CubeFaceDirection(id.z, FaceUV(id.xy, g_Environment.OutputSize));
	float sourceTexelSolidAngle = 4.0 * PI / (6.0 * g_Environment.SourceSize * g_Environment.SourceSize);
	float3 sum = float3(0.0, 0.0, 0.0);
	uint const sampleCount = g_Environment.SampleCount;
	for (uint i = 0; i < sampleCount; i++)
	{
		float2 xi = Hammersley(i, sampleCount);
		float phi = 2.0 * PI * xi.x;
		float cosTheta = sqrt(1.0 - xi.y);
		float sinTheta = sqrt(xi.y);
		float3 local = float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
		float pdf = cosTheta / PI;
		float sampleSolidAngle = 1.0 / (float(sampleCount) * max(pdf, 1e-6));
		float mip = clamp(0.5 * log2(sampleSolidAngle / sourceTexelSolidAngle) + 1.0, 0.0, g_Environment.SourceMaxMip);
		sum += g_SourceCube.SampleLevel(g_LinearSampler, TangentToWorld(local, normal), mip).rgb;
	}
	// The Monte Carlo estimate of the cosine-weighted average; Lambert's 1/pi is applied by the consumer through albedo.
	g_OutputCube[id] = float4(sum / float(sampleCount), 1.0);
}

// GGX-prefiltered radiance for one roughness level (N = V = R approximation).
[numthreads(8, 8, 1)]
void CSPrefilter(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= g_Environment.OutputSize || id.y >= g_Environment.OutputSize)
	{
		return;
	}
	float3 normal = CubeFaceDirection(id.z, FaceUV(id.xy, g_Environment.OutputSize));
	float roughness = g_Environment.Roughness;
	if (roughness <= 0.0)
	{
		g_OutputCube[id] = float4(g_SourceCube.SampleLevel(g_LinearSampler, normal, 0).rgb, 1.0);
		return;
	}

	float alpha = roughness * roughness;
	float sourceTexelSolidAngle = 4.0 * PI / (6.0 * g_Environment.SourceSize * g_Environment.SourceSize);
	float3 sum = float3(0.0, 0.0, 0.0);
	float weight = 0.0;
	uint const sampleCount = g_Environment.SampleCount;
	for (uint i = 0; i < sampleCount; i++)
	{
		float3 halfVector = TangentToWorld(ImportanceSampleGGX(Hammersley(i, sampleCount), alpha), normal);
		float3 light = 2.0 * dot(normal, halfVector) * halfVector - normal;
		float NoL = dot(normal, light);
		if (NoL <= 0.0)
		{
			continue;
		}
		float NoH = saturate(dot(normal, halfVector));
		// With N = V, pdf = D(h) * NoH / (4 VoH) = D / 4.
		float pdf = DistributionGGXIBL(NoH, alpha) * 0.25;
		float sampleSolidAngle = 1.0 / (float(sampleCount) * max(pdf, 1e-6));
		float mip = clamp(0.5 * log2(sampleSolidAngle / sourceTexelSolidAngle) + 1.0, 0.0, g_Environment.SourceMaxMip);
		sum += g_SourceCube.SampleLevel(g_LinearSampler, light, mip).rgb * NoL;
		weight += NoL;
	}
	g_OutputCube[id] = float4(sum / max(weight, 1e-6), 1.0);
}

// Split-sum DFG terms: specular = F0 * A + B for (NoV = x, perceptual roughness = y).
[numthreads(8, 8, 1)]
void CSBrdfLut(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= g_Environment.OutputSize || id.y >= g_Environment.OutputSize)
	{
		return;
	}
	float NoV = max((float(id.x) + 0.5) / float(g_Environment.OutputSize), 1e-4);
	float roughness = (float(id.y) + 0.5) / float(g_Environment.OutputSize);
	float alpha = roughness * roughness;
	float3 view = float3(sqrt(1.0 - NoV * NoV), 0.0, NoV);

	float a = 0.0;
	float b = 0.0;
	uint const sampleCount = g_Environment.SampleCount;
	for (uint i = 0; i < sampleCount; i++)
	{
		float3 halfVector = ImportanceSampleGGX(Hammersley(i, sampleCount), alpha);
		float3 light = 2.0 * dot(view, halfVector) * halfVector - view;
		float NoL = saturate(light.z);
		float NoH = saturate(halfVector.z);
		float VoH = saturate(dot(view, halfVector));
		if (NoL > 0.0)
		{
			float visibility = VisibilitySmithGGXCorrelatedIBL(NoV, NoL, alpha) * 4.0 * NoL * VoH / max(NoH, 1e-6);
			float fresnel = pow(1.0 - VoH, 5.0);
			a += (1.0 - fresnel) * visibility;
			b += fresnel * visibility;
		}
	}
	g_OutputLut[id.xy] = float4(float2(a, b) / float(sampleCount), 0.0, 1.0);
}
