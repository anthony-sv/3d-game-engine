#ifndef STRADA_IBL_HLSLI
#define STRADA_IBL_HLSLI

#include "Common.hlsli"

// Direction through texel (x, y) of a cube face; faces are +X, -X, +Y, -Y, +Z, -Z (D3D/Vulkan order).
float3 CubeFaceDirection(uint face, float2 uv)
{
	// uv in [0, 1] with (0, 0) at the face's top-left.
	float2 p = uv * 2.0 - 1.0;
	float3 direction;
	switch (face)
	{
		case 0:
			direction = float3(1.0, -p.y, -p.x);
			break;
		case 1:
			direction = float3(-1.0, -p.y, p.x);
			break;
		case 2:
			direction = float3(p.x, 1.0, p.y);
			break;
		case 3:
			direction = float3(p.x, -1.0, -p.y);
			break;
		case 4:
			direction = float3(p.x, -p.y, 1.0);
			break;
		default:
			direction = float3(-p.x, -p.y, -1.0);
			break;
	}
	return normalize(direction);
}

// Equirectangular mapping: u = 0.5 looks down -Z, v = 0 is straight up.
float2 DirectionToEquirect(float3 direction)
{
	float u = 0.5 + atan2(direction.x, -direction.z) / (2.0 * PI);
	float v = acos(clamp(direction.y, -1.0, 1.0)) / PI;
	return float2(u, v);
}

// Rotates a direction around +Y by the angle with the given sine and cosine.
float3 RotateY(float3 direction, float sine, float cosine)
{
	return float3(cosine * direction.x + sine * direction.z, direction.y, -sine * direction.x + cosine * direction.z);
}

float2 Hammersley(uint index, uint count)
{
	uint bits = reversebits(index);
	return float2(float(index) / float(count), float(bits) * 2.3283064365386963e-10);
}

// GGX half vector around +Z for a uniform sample.
float3 ImportanceSampleGGX(float2 xi, float alpha)
{
	float phi = 2.0 * PI * xi.x;
	float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
	float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
	return float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}

float3 TangentToWorld(float3 value, float3 normal)
{
	float3 up = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
	float3 tangent = normalize(cross(up, normal));
	float3 bitangent = cross(normal, tangent);
	return tangent * value.x + bitangent * value.y + normal * value.z;
}

float DistributionGGXIBL(float NoH, float alpha)
{
	float alpha2 = alpha * alpha;
	float f = (NoH * alpha2 - NoH) * NoH + 1.0;
	return alpha2 / (PI * f * f);
}

float VisibilitySmithGGXCorrelatedIBL(float NoV, float NoL, float alpha)
{
	float alpha2 = alpha * alpha;
	float lightTerm = NoV * sqrt((NoL - NoL * alpha2) * NoL + alpha2);
	float viewTerm = NoL * sqrt((NoV - NoV * alpha2) * NoV + alpha2);
	return 0.5 / max(lightTerm + viewTerm, 1e-6);
}

#endif
