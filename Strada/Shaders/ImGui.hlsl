// Dear ImGui rendering. Vertex colors and textures are in sRGB space and are written unchanged to the UNORM target.

struct ImGuiConstants
{
	float2 Scale;
	float2 Translate;
};

[[vk::push_constant]] ConstantBuffer<ImGuiConstants> g_Constants : register(b0);

Texture2D g_Texture : register(t0);
SamplerState g_Sampler : register(s0);

struct VertexInput
{
	[[vk::location(0)]] float2 Position : POSITION;
	[[vk::location(1)]] float2 TexCoord : TEXCOORD0;
	[[vk::location(2)]] float4 Color : COLOR0;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	float2 TexCoord : TEXCOORD0;
	float4 Color : COLOR0;
};

VertexOutput VSMain(VertexInput input)
{
	VertexOutput output;
	output.Position = float4(input.Position * g_Constants.Scale + g_Constants.Translate, 0.0, 1.0);
	output.TexCoord = input.TexCoord;
	output.Color = input.Color;
	return output;
}

float4 PSMain(VertexOutput input) : SV_Target0
{
	return input.Color * g_Texture.Sample(g_Sampler, input.TexCoord);
}
