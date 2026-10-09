// Editor overlays drawn after tonemapping onto 8-bit sRGB-encoded targets: debug lines and the ground grid (the selection
// outline is in Outline.hlsl). Colors are sRGB-encoded with straight alpha (blended with SrcAlpha / InvSrcAlpha).
// Set 0: the overlay camera (b0), the scene depth (t0, grid) and push constants (b1, grid).

#include "Include/RendererInterop.h"

ConstantBuffer<OverlayConstants> g_Overlay : register(b0, space0);

// Output of Fullscreen.hlsl's VSMain.
struct FullscreenVertexOutput
{
	float4 Position : SV_Position;
	float2 TexCoord : TEXCOORD0;
};

// ------------------------------------------------------------------------------------------------------------- Lines

struct LineVertexInput
{
	[[vk::location(0)]] float3 Position : POSITION;
	[[vk::location(1)]] float4 Color : COLOR;
};

struct LineVertexOutput
{
	float4 Position : SV_Position;
	float4 Color : COLOR;
};

LineVertexOutput VSLine(LineVertexInput input)
{
	LineVertexOutput output;
	output.Position = mul(g_Overlay.ViewProjection, float4(input.Position, 1.0));
	output.Color = input.Color;
	return output;
}

float4 PSLine(LineVertexOutput input) : SV_Target0
{
	return input.Color;
}

// -------------------------------------------------------------------------------------------------------------- Grid

Texture2D<float> g_SceneDepth : register(t0, space0);
[[vk::push_constant]] ConstantBuffer<GridConstants> g_Grid : register(b1, space0);

// Coverage of 1-pixel-wide anti-aliased lines at integer values of the coordinate (per axis).
float2 GridLineCoverage(float2 coordinate, float2 derivative)
{
	float2 distanceInPixels = abs(frac(coordinate + 0.5) - 0.5) / max(derivative, 1e-6);
	return saturate(1.0 - distanceInPixels);
}

// Fades lines out before they get closer than about two pixels, where they would alias into moire patterns.
float GridDensityFade(float2 derivative)
{
	return 1.0 - saturate((max(derivative.x, derivative.y) - 0.15) / 0.35);
}

// Straight-alpha "over" compositing.
float4 Over(float4 destination, float3 color, float alpha)
{
	float resultAlpha = alpha + destination.a * (1.0 - alpha);
	float3 resultColor = (color * alpha + destination.rgb * destination.a * (1.0 - alpha)) / max(resultAlpha, 1e-5);
	return float4(resultColor, resultAlpha);
}

float4 PSGrid(FullscreenVertexOutput input) : SV_Target0
{
	// The view ray through this pixel, from the near plane (reversed Z: depth 1) towards depth 0.5. Two points instead of
	// the camera position make orthographic cameras work too.
	float2 ndc = input.Position.xy / g_Overlay.ViewportSize * float2(2.0, -2.0) + float2(-1.0, 1.0);
	float4 nearPoint = mul(g_Overlay.InverseViewProjection, float4(ndc, 1.0, 1.0));
	float4 midPoint = mul(g_Overlay.InverseViewProjection, float4(ndc, 0.5, 1.0));
	float3 origin = nearPoint.xyz / nearPoint.w;
	float3 direction = midPoint.xyz / midPoint.w - origin;
	float t = abs(direction.y) > 1e-8 ? -origin.y / direction.y : -1.0;
	float3 hit = origin + direction * max(t, 0.0);

	// Derivatives are taken before any discard (they are undefined in divergent control flow).
	float2 coordinate = hit.xz / g_Grid.CellSize;
	float2 derivative = fwidth(coordinate);
	float2 majorCoordinate = coordinate / g_Grid.MajorLineEvery;
	float2 majorDerivative = derivative / g_Grid.MajorLineEvery;

	float2 minorLines = GridLineCoverage(coordinate, derivative);
	float2 majorLines = GridLineCoverage(majorCoordinate, majorDerivative);
	float minor = max(minorLines.x, minorLines.y) * GridDensityFade(derivative);
	float major = max(majorLines.x, majorLines.y) * GridDensityFade(majorDerivative);
	// The X axis is the line z = 0, the Z axis the line x = 0 (1.5 pixels wide).
	float axisX = saturate(1.25 - abs(coordinate.y) / max(derivative.y, 1e-6));
	float axisZ = saturate(1.25 - abs(coordinate.x) / max(derivative.x, 1e-6));

	float4 color = float4(0.0, 0.0, 0.0, 0.0);
	color = Over(color, g_Grid.MinorColor.rgb, g_Grid.MinorColor.a * minor);
	color = Over(color, g_Grid.MajorColor.rgb, g_Grid.MajorColor.a * major);
	color = Over(color, g_Grid.AxisXColor.rgb, g_Grid.AxisXColor.a * axisX);
	color = Over(color, g_Grid.AxisZColor.rgb, g_Grid.AxisZColor.a * axisZ);

	float distanceToCamera = length(hit - g_Overlay.CameraPosition);
	color.a *= 1.0 - smoothstep(0.4 * g_Grid.FadeDistance, g_Grid.FadeDistance, distanceToCamera);

	// Reversed Z: the grid is hidden where the scene surface is closer (larger depth). The relative tolerance (1 cm at
	// 100 m) keeps it from z-fighting with ground surfaces lying exactly on the plane.
	float4 clip = mul(g_Overlay.ViewProjection, float4(hit, 1.0));
	float gridDepth = clip.z / clip.w;
	float sceneDepth = g_SceneDepth.Load(int3(int2(input.Position.xy), 0));
	if (t <= 0.0 || sceneDepth > gridDepth * (1.0 + 1e-4) || color.a <= 0.0)
	{
		discard;
	}
	return color;
}
