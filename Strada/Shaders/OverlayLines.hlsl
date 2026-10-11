// Debug lines, drawn after tonemapping onto 8-bit sRGB-encoded targets in two steps:
// 1. Every line is an instance expanded to a screen-space quad (VSMain) whose pixels get the line's anti-aliased color
//    over its outline (PSMain): a capsule of LineConstants.Width pixels with round caps, so polylines join smoothly.
//    They are written to the line layer, where each pixel keeps only the line whose center is nearest (the distance is
//    the depth, tested less): lines that meet or cross never blend twice, so joints show no seams and dimmed lines no
//    beads. Depth-tested lines compare their depth with the scene depth: where a surface is in front of them their
//    alpha is scaled by OccludedOpacity (0 hides them).
// 2. The layer is blended over the image (PSComposite).
// Colors are sRGB-encoded with straight alpha. Set 0: the overlay camera (b0), the scene depth (t0), the line constants
// (b1, push constants) and, for the composite, the line layer (t1).

#include "Include/RendererInterop.h"

ConstantBuffer<OverlayConstants> g_Overlay : register(b0, space0);
Texture2D<float> g_SceneDepth : register(t0, space0);
[[vk::push_constant]] ConstantBuffer<LineConstants> g_Line : register(b1, space0);
Texture2D<float4> g_LineLayer : register(t1, space0);

struct LineInstanceInput
{
	[[vk::location(0)]] float3 From : FROM;
	[[vk::location(1)]] float3 To : TO;
	[[vk::location(2)]] float4 Color : COLOR;
	[[vk::location(3)]] uint DepthTested : DEPTHTESTED;
};

struct LineVertexOutput
{
	float4 Position : SV_Position;
	nointerpolation float4 Color : COLOR;
	// Pixels along the line from its start (x) and across it from its center (y).
	noperspective float2 Offset : TEXCOORD0;
	// Length of the line on screen in pixels.
	nointerpolation float Length : TEXCOORD1;
	nointerpolation uint DepthTested : TEXCOORD2;
};

// Distance from the near plane, positive in front of it (reversed Z: depth z / w is 1 on the near plane).
float NearPlaneDistance(float4 clip)
{
	return clip.w - clip.z;
}

// Half the width of a line's quad: the line, its outline and a pixel for anti-aliasing.
float LineExtent()
{
	return g_Line.Width * 0.5 + g_Line.OutlineWidth + 1.0;
}

LineVertexOutput VSMain(LineInstanceInput input, uint vertexId : SV_VertexID)
{
	// Two triangles: x selects the end (0 = From, 1 = To), y the side.
	static const float2 Corners[6] = {float2(0.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0),
	                                  float2(0.0, -1.0), float2(1.0, 1.0), float2(0.0, 1.0)};
	float2 corner = Corners[vertexId];

	LineVertexOutput output;
	output.Color = input.Color;
	output.DepthTested = input.DepthTested;
	output.Offset = float2(0.0, 0.0);
	output.Length = 0.0;

	float4 clipFrom = mul(g_Overlay.ViewProjection, float4(input.From, 1.0));
	float4 clipTo = mul(g_Overlay.ViewProjection, float4(input.To, 1.0));
	float distanceFrom = NearPlaneDistance(clipFrom);
	float distanceTo = NearPlaneDistance(clipTo);
	if (distanceFrom < 0.0 && distanceTo < 0.0)
	{
		// Entirely behind the near plane: outside the clip volume, so the rasterizer drops both triangles.
		output.Position = float4(2.0, 2.0, 2.0, 1.0);
		return output;
	}
	// Ends behind the camera do not project: cut the line where it crosses the near plane.
	if (distanceFrom < 0.0)
	{
		clipFrom = lerp(clipFrom, clipTo, distanceFrom / (distanceFrom - distanceTo));
	}
	else if (distanceTo < 0.0)
	{
		clipTo = lerp(clipTo, clipFrom, distanceTo / (distanceTo - distanceFrom));
	}

	// Pixels from the viewport's center (+Y up, like NDC).
	float2 halfViewport = g_Overlay.ViewportSize * 0.5;
	float2 screenFrom = clipFrom.xy / clipFrom.w * halfViewport;
	float2 screenTo = clipTo.xy / clipTo.w * halfViewport;
	float2 delta = screenTo - screenFrom;
	float lineLength = length(delta);
	float2 direction = lineLength > 1e-4 ? delta / lineLength : float2(1.0, 0.0);
	float2 normal = float2(-direction.y, direction.x);
	// The quad reaches as far past each end as to each side.
	float extent = LineExtent();

	float4 clip = corner.x == 0.0 ? clipFrom : clipTo;
	float2 screen = corner.x == 0.0 ? screenFrom : screenTo;
	float along = corner.x == 0.0 ? -extent : extent;
	screen += direction * along + normal * (corner.y * extent);
	output.Position = float4(screen / halfViewport * clip.w, clip.z, clip.w);
	output.Offset = float2(corner.x == 0.0 ? -extent : lineLength + extent, corner.y * extent);
	output.Length = lineLength;
	return output;
}

struct LinePixelOutput
{
	float4 Color : SV_Target0;
	// The distance to the line's center relative to the quad's extent: the nearest line keeps the pixel.
	float Depth : SV_Depth;
};

LinePixelOutput PSMain(LineVertexOutput input)
{
	// Distance in pixels to the line (a capsule: round caps).
	float beyondEnds = input.Offset.x - clamp(input.Offset.x, 0.0, input.Length);
	float distanceToLine = length(float2(beyondEnds, input.Offset.y));
	float halfWidth = g_Line.Width * 0.5;

	// Coverage of the pixel by the line and its outline, with a pixel of anti-aliasing at their edges; the line is over
	// its outline.
	float lineAlpha = input.Color.a * saturate(halfWidth + 0.5 - distanceToLine);
	float outlineAlpha = g_Line.OutlineWidth > 0.0 ? g_Line.OutlineColor.a * saturate(halfWidth + g_Line.OutlineWidth + 0.5 - distanceToLine) : 0.0;
	float alpha = lineAlpha + outlineAlpha * (1.0 - lineAlpha);
	if (alpha <= 0.0)
	{
		discard;
	}
	float3 color = (input.Color.rgb * lineAlpha + g_Line.OutlineColor.rgb * outlineAlpha * (1.0 - lineAlpha)) / alpha;

	if (input.DepthTested != 0)
	{
		// Reversed Z: a surface with a larger depth is in front of the line. The relative tolerance (2 mm per meter)
		// keeps lines lying on a surface from flickering between dimmed and full.
		float sceneDepth = g_SceneDepth.Load(int3(int2(input.Position.xy), 0));
		if (sceneDepth > input.Position.z * (1.0 + 2e-3))
		{
			alpha *= g_Line.OccludedOpacity;
			if (alpha <= 0.0)
			{
				discard;
			}
		}
	}

	LinePixelOutput output;
	output.Color = float4(color, alpha);
	output.Depth = saturate(distanceToLine / LineExtent());
	return output;
}

// Output of Fullscreen.hlsl's VSMain.
struct FullscreenVertexOutput
{
	float4 Position : SV_Position;
	float2 TexCoord : TEXCOORD0;
};

float4 PSComposite(FullscreenVertexOutput input) : SV_Target0
{
	return g_LineLayer.Load(int3(int2(input.Position.xy), 0));
}
