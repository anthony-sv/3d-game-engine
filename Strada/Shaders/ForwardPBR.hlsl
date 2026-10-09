// Forward metallic-roughness PBR (glTF 2.0 material model). Writes linear HDR radiance pre-multiplied by exposure.
// Set 0: frame constants, lights, environment, shadows, samplers. Set 1: material. Push constants: per-draw transforms.

#include "Include/IBL.hlsli"
#include "Include/RendererInterop.h"

ConstantBuffer<FrameConstants> g_Frame : register(b0, space0);
StructuredBuffer<LightData> g_Lights : register(t0, space0);
TextureCube g_IrradianceCube : register(t1, space0);
TextureCube g_PrefilteredCube : register(t2, space0);
Texture2D g_BrdfLut : register(t3, space0);
ConstantBuffer<ShadowConstants> g_Shadow : register(b2, space0);
Texture2DArray g_CascadeShadowMap : register(t5, space0);
Texture2DArray g_LocalShadowMap : register(t6, space0);
SamplerState g_MaterialSampler : register(s0, space0);
SamplerState g_ClampSampler : register(s1, space0);
// NVRHI's Vulkan comparison samplers use LESS: the result is 1 where the receiver lies behind the stored occluder
// (reversed Z), i.e. the occluded fraction.
SamplerComparisonState g_ShadowCompareSampler : register(s2, space0);
SamplerState g_ShadowPointSampler : register(s3, space0);

ConstantBuffer<MaterialConstants> g_Material : register(b0, space1);
Texture2D g_BaseColorTexture : register(t0, space1);
Texture2D g_NormalTexture : register(t1, space1);
Texture2D g_MetallicRoughnessTexture : register(t2, space1);
Texture2D g_OcclusionTexture : register(t3, space1);
Texture2D g_EmissiveTexture : register(t4, space1);

[[vk::push_constant]] ConstantBuffer<DrawConstants> g_Draw : register(b1, space0);

struct VertexInput
{
	[[vk::location(0)]] float3 Position : POSITION;
	[[vk::location(1)]] float3 Normal : NORMAL;
	[[vk::location(2)]] float4 Tangent : TANGENT;
	[[vk::location(3)]] float2 TexCoord : TEXCOORD0;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	float3 WorldPosition : POSITION1;
	float3 Normal : NORMAL;
	float4 Tangent : TANGENT;
	float2 TexCoord : TEXCOORD0;
};

VertexOutput VSMain(VertexInput input)
{
	VertexOutput output;
	float4 worldPosition = mul(g_Draw.Model, float4(input.Position, 1.0));
	output.Position = mul(g_Frame.ViewProjection, worldPosition);
	output.WorldPosition = worldPosition.xyz;
	output.Normal = mul((float3x3)g_Draw.NormalMatrix, input.Normal);
	output.Tangent = float4(mul((float3x3)g_Draw.Model, input.Tangent.xyz), input.Tangent.w);
	output.TexCoord = input.TexCoord * g_Material.UVTiling + g_Material.UVOffset;
	return output;
}

// GGX / Trowbridge-Reitz normal distribution.
float DistributionGGX(float NoH, float alpha)
{
	float alpha2 = alpha * alpha;
	float f = (NoH * alpha2 - NoH) * NoH + 1.0;
	return alpha2 / (PI * f * f);
}

// Height-correlated Smith visibility term (includes the 1 / (4 NoL NoV) denominator).
float VisibilitySmithGGXCorrelated(float NoV, float NoL, float alpha)
{
	float alpha2 = alpha * alpha;
	float lightTerm = NoV * sqrt((NoL - NoL * alpha2) * NoL + alpha2);
	float viewTerm = NoL * sqrt((NoV - NoV * alpha2) * NoV + alpha2);
	return 0.5 / max(lightTerm + viewTerm, 1e-5);
}

float3 FresnelSchlick(float3 f0, float VoH)
{
	float f = pow(1.0 - VoH, 5.0);
	return f + f0 * (1.0 - f);
}

// Smooth range window (Frostbite): reaches exactly zero at the light's range.
float RangeWindow(float distanceSquared, float range)
{
	float ratio = distanceSquared / max(range * range, 1e-4);
	float window = saturate(1.0 - ratio * ratio);
	return window * window;
}

static uint const ShadowFilterSamples = 16;
static uint const BlockerSearchSamples = 16;
// Upper bound of the soft-shadow filter radius, in texels (bounds the cost and the light leaking of huge penumbrae).
static float const MaxPenumbraTexels = 20.0;
// Normal offset applied to receivers, in texels.
static float const NormalOffsetTexels = 1.5;
static float const LocalFilterTexels = 1.75;

// Points of a Vogel (golden angle) disk in the unit circle, rotated by phi.
float2 VogelDisk(uint index, uint count, float phi)
{
	float radius = sqrt((float(index) + 0.5) / float(count));
	float theta = float(index) * 2.39996323 + phi;
	return radius * float2(cos(theta), sin(theta));
}

float2 ClipToShadowUV(float2 clip)
{
	return clip * float2(0.5, -0.5) + 0.5;
}

// Fraction of light reaching a receiver at depth through a disk filter of radiusUV.
float FilterShadow(Texture2DArray shadowMap, float2 uv, float slice, float depth, float radiusUV, float phi)
{
	float occluded = 0.0;
	for (uint i = 0; i < ShadowFilterSamples; i++)
	{
		float2 offset = VogelDisk(i, ShadowFilterSamples, phi) * radiusUV;
		occluded += shadowMap.SampleCmpLevelZero(g_ShadowCompareSampler, float3(uv + offset, slice), depth);
	}
	return 1.0 - occluded / float(ShadowFilterSamples);
}

float SampleCascade(uint cascade, float3 worldPosition, float3 geometricNormal, float phi)
{
	float texelSize = g_Shadow.CascadeTexelSizes[cascade];
	float texelUV = 1.0 / g_Shadow.CascadeMapSize;
	float width = g_Shadow.CascadeWidths[cascade];
	float depthRange = g_Shadow.CascadeDepthRanges[cascade];
	float4x4 viewProjection = g_Shadow.CascadeViewProjection[cascade];

	float3 position = worldPosition + geometricNormal * (texelSize * NormalOffsetTexels);
	float3 clip = mul(viewProjection, float4(position, 1.0)).xyz;
	float2 uv = ClipToShadowUV(clip.xy);
	if (any(uv < 0.0) || any(uv > 1.0) || clip.z <= 0.0)
	{
		return 1.0;
	}

	float radiusUV = NormalOffsetTexels * texelUV;
	if (g_Shadow.SoftShadows != 0 && g_Shadow.LightTanHalfAngle > 0.0)
	{
		// Blocker search over the region from which an occluder anywhere between the receiver and the light could
		// cast a penumbra onto it.
		float searchUV = clamp(g_Shadow.LightTanHalfAngle * (1.0 - clip.z) * depthRange / width, texelUV, MaxPenumbraTexels * texelUV);
		float blockerDepth = 0.0;
		float blockerCount = 0.0;
		for (uint i = 0; i < BlockerSearchSamples; i++)
		{
			float2 offset = VogelDisk(i, BlockerSearchSamples, phi) * searchUV;
			float storedDepth = g_CascadeShadowMap.SampleLevel(g_ShadowPointSampler, float3(uv + offset, cascade), 0).r;
			if (storedDepth > clip.z)
			{
				blockerDepth += storedDepth;
				blockerCount += 1.0;
			}
		}
		if (blockerCount == 0.0)
		{
			return 1.0;
		}
		float blockerDistance = (blockerDepth / blockerCount - clip.z) * depthRange;
		radiusUV = clamp(blockerDistance * g_Shadow.LightTanHalfAngle / width, texelUV, MaxPenumbraTexels * texelUV);

		// Push the receiver off the surface by the filter radius so wide kernels do not self-shadow sloped surfaces.
		float radiusWorld = radiusUV * width;
		position = worldPosition + geometricNormal * max(texelSize * NormalOffsetTexels, radiusWorld);
		clip = mul(viewProjection, float4(position, 1.0)).xyz;
		uv = ClipToShadowUV(clip.xy);
	}
	return FilterShadow(g_CascadeShadowMap, uv, float(cascade), clip.z, radiusUV, phi);
}

float SampleDirectionalShadow(float3 worldPosition, float3 geometricNormal, float phi)
{
	float viewDepth = dot(worldPosition - g_Frame.CameraPosition, g_Shadow.CameraForward);
	uint cascadeCount = g_Shadow.CascadeCount;
	if (cascadeCount == 0 || viewDepth >= g_Shadow.ShadowDistance)
	{
		return 1.0;
	}
	uint cascade = 0;
	while (cascade + 1 < cascadeCount && viewDepth >= g_Shadow.CascadeSplits[cascade])
	{
		cascade++;
	}
	float shadow = SampleCascade(cascade, worldPosition, geometricNormal, phi);

	// Blend into the next cascade (or out of the shadowed range) over the last tenth of this one.
	float start = cascade == 0 ? 0.0 : g_Shadow.CascadeSplits[cascade - 1];
	float end = g_Shadow.CascadeSplits[cascade];
	float blendStart = end - (end - start) * 0.1;
	if (viewDepth > blendStart)
	{
		float next = cascade + 1 < cascadeCount ? SampleCascade(cascade + 1, worldPosition, geometricNormal, phi) : 1.0;
		shadow = lerp(shadow, next, saturate((viewDepth - blendStart) / max(end - blendStart, 1e-4)));
	}
	return shadow;
}

uint GetCubeFace(float3 direction)
{
	float3 magnitude = abs(direction);
	if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z)
	{
		return direction.x >= 0.0 ? 0 : 1;
	}
	if (magnitude.y >= magnitude.z)
	{
		return direction.y >= 0.0 ? 2 : 3;
	}
	return direction.z >= 0.0 ? 4 : 5;
}

float SampleLocalShadow(LightData light, float3 worldPosition, float3 geometricNormal, float phi)
{
	float3 fromLight = worldPosition - light.Position;
	uint slice = uint(light.ShadowIndex);
	if (light.Type == LightTypePoint)
	{
		slice += GetCubeFace(fromLight);
	}
	float texelSize = light.ShadowTexelScale * length(fromLight);
	float3 position = worldPosition + geometricNormal * (texelSize * NormalOffsetTexels);
	float4 clip = mul(g_Shadow.LocalViewProjection[slice], float4(position, 1.0));
	if (clip.w <= 0.0)
	{
		return 1.0;
	}
	float3 ndc = clip.xyz / clip.w;
	float2 uv = ClipToShadowUV(ndc.xy);
	if (any(uv < 0.0) || any(uv > 1.0))
	{
		return 1.0;
	}
	return FilterShadow(g_LocalShadowMap, uv, float(slice), ndc.z, LocalFilterTexels / g_Shadow.LocalMapSize, phi);
}

float4 PSMain(VertexOutput input, bool isFrontFace : SV_IsFrontFace) : SV_Target0
{
	float4 baseColor = g_Material.BaseColor * g_BaseColorTexture.Sample(g_MaterialSampler, input.TexCoord);
	if ((g_Material.Flags & MaterialFlagAlphaMask) != 0 && baseColor.a < g_Material.AlphaCutoff)
	{
		discard;
	}

	float3 normal = normalize(input.Normal);
	float3 tangent = input.Tangent.xyz - normal * dot(normal, input.Tangent.xyz);
	// Back faces of double-sided materials are lit from their own side.
	if (!isFrontFace)
	{
		normal = -normal;
	}
	float3 geometricNormal = normal;
	// Rotates the shadow filter kernels per pixel; the noise turns banding into fine grain.
	float shadowPhi = InterleavedGradientNoise(input.Position.xy) * 2.0 * PI;
	if ((g_Material.Flags & MaterialFlagHasNormalMap) != 0 && dot(tangent, tangent) > 1e-12)
	{
		tangent = normalize(tangent);
		float3 bitangent = cross(normal, tangent) * input.Tangent.w;
		float3 tangentNormal = g_NormalTexture.Sample(g_MaterialSampler, input.TexCoord).xyz * 2.0 - 1.0;
		tangentNormal.xy *= g_Material.NormalStrength;
		normal = normalize(tangentNormal.x * tangent + tangentNormal.y * bitangent + tangentNormal.z * normal);
	}

	float4 metallicRoughness = g_MetallicRoughnessTexture.Sample(g_MaterialSampler, input.TexCoord);
	float metallic = saturate(g_Material.Metallic * metallicRoughness.b);
	float perceptualRoughness = clamp(g_Material.Roughness * metallicRoughness.g, 0.045, 1.0);
	float alpha = perceptualRoughness * perceptualRoughness;
	float occlusion = lerp(1.0, g_OcclusionTexture.Sample(g_MaterialSampler, input.TexCoord).r, g_Material.OcclusionStrength);

	float3 diffuseColor = baseColor.rgb * (1.0 - metallic);
	float3 f0 = lerp(float3(0.04, 0.04, 0.04), baseColor.rgb, metallic);

	float3 view = normalize(g_Frame.CameraPosition - input.WorldPosition);
	float NoV = max(dot(normal, view), 1e-4);

	float3 radiance = float3(0.0, 0.0, 0.0);
	for (uint i = 0; i < g_Frame.LightCount; i++)
	{
		LightData light = g_Lights[i];
		float3 toLight;
		float attenuation = 1.0;
		if (light.Type == LightTypeDirectional)
		{
			toLight = -light.Direction;
		}
		else
		{
			float3 offset = light.Position - input.WorldPosition;
			float distanceSquared = max(dot(offset, offset), 1e-4);
			toLight = offset * rsqrt(distanceSquared);
			attenuation = RangeWindow(distanceSquared, light.Range) / distanceSquared;
			if (light.Type == LightTypeSpot)
			{
				float cosAngle = dot(-toLight, light.Direction);
				float spot = saturate((cosAngle - light.SpotCosOuter) / max(light.SpotCosInner - light.SpotCosOuter, 1e-4));
				attenuation *= spot * spot;
			}
		}

		float NoL = dot(normal, toLight);
		if (NoL <= 0.0 || attenuation <= 0.0)
		{
			continue;
		}
		if (light.ShadowIndex >= 0)
		{
			attenuation *= light.Type == LightTypeDirectional ? SampleDirectionalShadow(input.WorldPosition, geometricNormal, shadowPhi)
			                                                  : SampleLocalShadow(light, input.WorldPosition, geometricNormal, shadowPhi);
			if (attenuation <= 0.0)
			{
				continue;
			}
		}
		float3 halfVector = normalize(view + toLight);
		float NoH = saturate(dot(normal, halfVector));
		float VoH = saturate(dot(view, halfVector));

		float3 fresnel = FresnelSchlick(f0, VoH);
		float3 specular = DistributionGGX(NoH, alpha) * VisibilitySmithGGXCorrelated(NoV, NoL, alpha) * fresnel;
		float3 diffuse = diffuseColor / PI * (1.0 - fresnel);
		radiance += (diffuse + specular) * light.Radiance * (attenuation * NoL);
	}

	// Image-based (or uniform ambient) lighting: split-sum specular with multiple-scattering energy compensation
	// (Fdez-Aguera 2019) and the matching diffuse term.
	float3 reflected = reflect(-view, normal);
	float2 dfg = g_BrdfLut.SampleLevel(g_ClampSampler, float2(NoV, perceptualRoughness), 0).rg;
	float3 singleScatter = f0 * dfg.x + dfg.y;
	float singleScatterEnergy = dfg.x + dfg.y;
	float multiScatterEnergy = 1.0 - singleScatterEnergy;
	float3 averageFresnel = f0 + (1.0 - f0) / 21.0;
	float3 multiScatter = singleScatter * averageFresnel / (1.0 - multiScatterEnergy * averageFresnel);
	float3 irradiance;
	float3 prefiltered;
	if (g_Frame.EnvironmentIntensity > 0.0)
	{
		float3 normalLookup = RotateY(normal, g_Frame.EnvironmentRotationSin, g_Frame.EnvironmentRotationCos);
		float3 reflectedLookup = RotateY(reflected, g_Frame.EnvironmentRotationSin, g_Frame.EnvironmentRotationCos);
		irradiance = g_IrradianceCube.SampleLevel(g_ClampSampler, normalLookup, 0).rgb * g_Frame.EnvironmentIntensity;
		prefiltered = g_PrefilteredCube.SampleLevel(g_ClampSampler, reflectedLookup, perceptualRoughness * g_Frame.PrefilteredMaxMip).rgb *
		              g_Frame.EnvironmentIntensity;
	}
	else
	{
		irradiance = g_Frame.AmbientColor;
		prefiltered = g_Frame.AmbientColor;
	}
	float3 diffuseWeight = diffuseColor * (1.0 - singleScatter - multiScatter * multiScatterEnergy);
	float3 ambient = (singleScatter * prefiltered + multiScatter * multiScatterEnergy * irradiance + diffuseWeight * irradiance) * occlusion;
	float3 emissive = g_Material.Emissive * g_EmissiveTexture.Sample(g_MaterialSampler, input.TexCoord).rgb;
	float3 color = (radiance + ambient + emissive) * g_Frame.Exposure;
	return float4(color, baseColor.a);
}
