// Draws the environment behind the scene: a fullscreen triangle at the far plane (reversed Z: depth 0), so only pixels
// that no geometry covered pass the depth test.

#include "Include/IBL.hlsli"
#include "Include/RendererInterop.h"

ConstantBuffer<FrameConstants> g_Frame : register(b0, space0);
TextureCube g_RadianceCube : register(t4, space0);
SamplerState g_ClampSampler : register(s1, space0);

struct SkyVertexOutput
{
	float4 Position : SV_Position;
	float2 NDC : TEXCOORD0;
};

SkyVertexOutput VSMain(uint vertexID : SV_VertexID)
{
	SkyVertexOutput output;
	float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
	output.NDC = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
	output.Position = float4(output.NDC, 0.0, 1.0);
	return output;
}

float4 PSMain(SkyVertexOutput input) : SV_Target0
{
	// Unproject a point on the near plane (depth 1 in reversed Z) to get the view ray.
	float4 nearPoint = mul(g_Frame.InverseViewProjection, float4(input.NDC, 1.0, 1.0));
	float3 direction = normalize(nearPoint.xyz / nearPoint.w - g_Frame.CameraPosition);
	float3 radiance;
	if (g_Frame.EnvironmentIntensity > 0.0)
	{
		float3 lookup = RotateY(direction, g_Frame.EnvironmentRotationSin, g_Frame.EnvironmentRotationCos);
		radiance = g_RadianceCube.SampleLevel(g_ClampSampler, lookup, g_Frame.SkyboxLod).rgb * g_Frame.EnvironmentIntensity;
	}
	else
	{
		radiance = g_Frame.AmbientColor;
	}
	return float4(radiance * g_Frame.Exposure, 1.0);
}
