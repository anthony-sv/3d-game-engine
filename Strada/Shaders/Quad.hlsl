// Batched sprites, signed-distance text and UI rectangles (QuadRenderer). Vertex colors are linear with straight alpha;
// quads drawn onto the 8-bit final image are sRGB-encoded here. The output is premultiplied (blended with One,
// InvSrcAlpha): additive sprites premultiply and write no coverage, so their light adds to what is behind. The entity ID
// entry point writes the picking ID where the quad is opaque (additive sprites are never picked).
// Set 0: the sprite texture or font atlas (t0), its sampler (s0) and push constants (b0).

#include "Include/Common.hlsli"
#include "Include/RendererInterop.h"

Texture2D g_Texture : register(t0, space0);
SamplerState g_Sampler : register(s0, space0);
[[vk::push_constant]] ConstantBuffer<QuadConstants> g_Quad : register(b0, space0);

struct VertexInput
{
	[[vk::location(0)]] float4 Position : POSITION;
	[[vk::location(1)]] float4 Color : COLOR;
	[[vk::location(2)]] float2 TexCoord : TEXCOORD0;
	[[vk::location(3)]] uint EntityId : ENTITYID;
	// UI rectangles: the corner radius, and the offset from the rectangle's center (xy) and its half size (zw), in pixels.
	[[vk::location(4)]] float CornerRadius : RADIUS;
	[[vk::location(5)]] float4 Shape : SHAPE;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	float4 Color : COLOR;
	float2 TexCoord : TEXCOORD0;
	nointerpolation uint EntityId : ENTITYID;
	nointerpolation float CornerRadius : RADIUS;
	float4 Shape : SHAPE;
};

VertexOutput VSMain(VertexInput input)
{
	VertexOutput output;
	output.Position = mul(g_Quad.Transform, input.Position);
	output.Color = input.Color;
	output.TexCoord = input.TexCoord;
	output.EntityId = input.EntityId;
	output.CornerRadius = input.CornerRadius;
	output.Shape = input.Shape;
	return output;
}

// Signed distance in pixels from a rounded rectangle's outline (negative inside).
float RoundedRectangleDistance(float2 offset, float2 halfSize, float radius)
{
	float2 q = abs(offset) - halfSize + radius;
	return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

// The quad's color with straight alpha: the color times the sprite texture, the color covered by the distance-field
// outline of text (anti-aliased over one screen pixel at any scale), or a textured UI rectangle covered by its rounded
// outline (anti-aliased over one pixel).
float4 ShadeQuad(VertexOutput input)
{
	if (g_Quad.Mode == QuadModeText)
	{
		float sample = g_Texture.Sample(g_Sampler, input.TexCoord).r;
		float distance = (sample - g_Quad.EdgeSample) * g_Quad.DistanceScale;
		float2 atlasPixelsPerScreenPixel = fwidth(input.TexCoord * g_Quad.AtlasSize);
		float scale = max(0.5 * (atlasPixelsPerScreenPixel.x + atlasPixelsPerScreenPixel.y), 1e-4);
		float coverage = saturate(distance / scale + 0.5);
		return float4(input.Color.rgb, input.Color.a * coverage);
	}
	float4 color = input.Color * g_Texture.Sample(g_Sampler, input.TexCoord);
	if (g_Quad.Mode == QuadModeUIRect)
	{
		float coverage = saturate(0.5 - RoundedRectangleDistance(input.Shape.xy, input.Shape.zw, input.CornerRadius));
		color.a *= coverage;
	}
	return color;
}

float4 PSMain(VertexOutput input) : SV_Target0
{
	float4 color = ShadeQuad(input);
	if (g_Quad.EncodeSrgb != 0)
	{
		color.rgb = LinearToSrgb(color.rgb);
	}
	color.rgb *= color.a;
	if (g_Quad.Mode == QuadModeAdditive)
	{
		color.a = 0.0;
	}
	return color;
}

uint PSEntityId(VertexOutput input) : SV_Target0
{
	if (g_Quad.Mode == QuadModeAdditive || ShadeQuad(input).a < 0.5)
	{
		discard;
	}
	return input.EntityId;
}
