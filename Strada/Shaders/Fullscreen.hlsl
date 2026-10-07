// Fullscreen triangle and simple fullscreen pixel shaders.
// NVRHI flips the Vulkan viewport, so clip space follows D3D conventions: NDC +Y is up and UV (0, 0) is top-left.

struct FullscreenVertexOutput
{
	float4 Position : SV_Position;
	float2 TexCoord : TEXCOORD0;
};

FullscreenVertexOutput VSMain(uint vertexID : SV_VertexID)
{
	FullscreenVertexOutput output;
	output.TexCoord = float2((vertexID << 1) & 2, vertexID & 2);
	output.Position = float4(output.TexCoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	return output;
}

struct SolidColorConstants
{
	float4 Color;
};

[[vk::push_constant]] ConstantBuffer<SolidColorConstants> g_SolidColor : register(b0);

float4 PSSolidColor(FullscreenVertexOutput input) : SV_Target0
{
	return g_SolidColor.Color;
}
