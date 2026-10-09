// Shadow-map depth rendering. VSMain alone renders opaque casters; PSMain adds the alpha test for masked materials.
// Set 0: push constants and the material sampler. Set 1: the material (same layout as the forward pass).

#include "Include/RendererInterop.h"

[[vk::push_constant]] ConstantBuffer<ShadowDrawConstants> g_Draw : register(b0, space0);
SamplerState g_MaterialSampler : register(s0, space0);

ConstantBuffer<MaterialConstants> g_Material : register(b0, space1);
Texture2D g_BaseColorTexture : register(t0, space1);

struct VertexInput
{
	[[vk::location(0)]] float3 Position : POSITION;
	[[vk::location(3)]] float2 TexCoord : TEXCOORD0;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	float2 TexCoord : TEXCOORD0;
};

VertexOutput VSMain(VertexInput input)
{
	VertexOutput output;
	output.Position = mul(g_Draw.ViewProjection, mul(g_Draw.Model, float4(input.Position, 1.0)));
	output.TexCoord = input.TexCoord;
	return output;
}

void PSMain(VertexOutput input)
{
	float2 uv = input.TexCoord * g_Material.UVTiling + g_Material.UVOffset;
	float alpha = g_Material.BaseColor.a * g_BaseColorTexture.Sample(g_MaterialSampler, uv).a;
	if (alpha < g_Material.AlphaCutoff)
	{
		discard;
	}
}
