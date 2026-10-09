// Editor selection outline, drawn over the final 8-bit image: outlines the visible silhouette of selected entities using
// the entity ID target (selected entities have EntityIdSelectedBit set). The color is sRGB-encoded with straight alpha.
// Set 0: the overlay camera (b0), the entity IDs (t0) and push constants (b1).

#include "Include/RendererInterop.h"

ConstantBuffer<OverlayConstants> g_Overlay : register(b0, space0);

// Output of Fullscreen.hlsl's VSMain.
struct FullscreenVertexOutput
{
	float4 Position : SV_Position;
	float2 TexCoord : TEXCOORD0;
};


Texture2D<uint> g_EntityIds : register(t0, space0);
[[vk::push_constant]] ConstantBuffer<OutlineConstants> g_Outline : register(b1, space0);

// Pixels outside the selection within Radius pixels of it.
float4 PSMain(FullscreenVertexOutput input) : SV_Target0
{
	int2 pixel = int2(input.Position.xy);
	int2 lastPixel = int2(g_Overlay.ViewportSize) - 1;
	if ((g_EntityIds.Load(int3(pixel, 0)) & EntityIdSelectedBit) != 0)
	{
		discard;
	}

	int radius = g_Outline.Radius;
	float closest = float(radius) + 1.0;
	for (int y = -radius; y <= radius; y++)
	{
		for (int x = -radius; x <= radius; x++)
		{
			int2 neighbor = clamp(pixel + int2(x, y), int2(0, 0), lastPixel);
			if ((g_EntityIds.Load(int3(neighbor, 0)) & EntityIdSelectedBit) != 0)
			{
				closest = min(closest, length(float2(x, y)));
			}
		}
	}

	// Solid up to Radius pixels away, anti-aliased beyond (diagonal neighbors at the edge of the window).
	float coverage = saturate(float(radius) + 1.0 - closest);
	if (coverage <= 0.0)
	{
		discard;
	}
	return float4(g_Outline.Color.rgb, g_Outline.Color.a * coverage);
}
