// Editor picking: renders the entity ID of the nearest surface per pixel into an R32_UINT target with its own depth
// buffer. Masked materials apply their alpha test so cut-out pixels are not pickable.
// Set 0: overlay camera, push constants (the draw's run of instance indices), the material sampler and the frame's
// instances. Set 1: the material (same layout as the forward pass).

#include "Include/RendererInterop.h"

ConstantBuffer<OverlayConstants> g_Overlay : register(b0, space0);
[[vk::push_constant]] ConstantBuffer<EntityIdDrawConstants> g_Draw : register(b1, space0);
SamplerState g_MaterialSampler : register(s0, space0);
StructuredBuffer<InstanceData> g_Instances : register(t0, space0);
StructuredBuffer<uint> g_InstanceIndices : register(t1, space0);

ConstantBuffer<MaterialConstants> g_Material : register(b0, space1);
Texture2D g_BaseColorTexture : register(t0, space1);

struct VertexInput
{
	[[vk::location(0)]] float3 Position : POSITION;
	// The depth-only input layout provides positions and texture coordinates only.
	[[vk::location(1)]] float2 TexCoord : TEXCOORD0;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	float2 TexCoord : TEXCOORD0;
	nointerpolation uint EntityId : ENTITYID;
};

VertexOutput VSMain(VertexInput input, uint instanceId : SV_InstanceID)
{
	InstanceData instance = g_Instances[g_InstanceIndices[g_Draw.InstanceOffset + instanceId]];
	VertexOutput output;
	output.Position = mul(g_Overlay.ViewProjection, mul(instance.Model, float4(input.Position, 1.0)));
	output.TexCoord = input.TexCoord * g_Material.UVTiling + g_Material.UVOffset;
	output.EntityId = instance.EntityId;
	return output;
}

uint PSMain(VertexOutput input) : SV_Target0
{
	if ((g_Material.Flags & MaterialFlagAlphaMask) != 0)
	{
		float alpha = g_Material.BaseColor.a * g_BaseColorTexture.Sample(g_MaterialSampler, input.TexCoord).a;
		if (alpha < g_Material.AlphaCutoff)
		{
			discard;
		}
	}
	return input.EntityId;
}
