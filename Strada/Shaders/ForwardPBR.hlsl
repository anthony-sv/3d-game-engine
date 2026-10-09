// Forward metallic-roughness PBR (glTF 2.0 material model). Writes linear HDR radiance pre-multiplied by exposure.
// Set 0: frame constants, lights, samplers. Set 1: material. Push constants: per-draw transforms.

#include "Include/Common.hlsli"
#include "Include/RendererInterop.h"

ConstantBuffer<FrameConstants> g_Frame : register(b0, space0);
StructuredBuffer<LightData> g_Lights : register(t0, space0);
SamplerState g_MaterialSampler : register(s0, space0);

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
		float3 halfVector = normalize(view + toLight);
		float NoH = saturate(dot(normal, halfVector));
		float VoH = saturate(dot(view, halfVector));

		float3 fresnel = FresnelSchlick(f0, VoH);
		float3 specular = DistributionGGX(NoH, alpha) * VisibilitySmithGGXCorrelated(NoV, NoL, alpha) * fresnel;
		float3 diffuse = diffuseColor / PI * (1.0 - fresnel);
		radiance += (diffuse + specular) * light.Radiance * (attenuation * NoL);
	}

	float3 ambient = g_Frame.AmbientColor * diffuseColor * occlusion;
	float3 emissive = g_Material.Emissive * g_EmissiveTexture.Sample(g_MaterialSampler, input.TexCoord).rgb;
	float3 color = (radiance + ambient + emissive) * g_Frame.Exposure;
	return float4(color, baseColor.a);
}
