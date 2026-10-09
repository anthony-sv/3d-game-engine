#ifndef STRADA_COMMON_HLSLI
#define STRADA_COMMON_HLSLI

static const float PI = 3.14159265358979323846;

float3 LinearToSrgb(float3 color)
{
	float3 low = color * 12.92;
	float3 high = 1.055 * pow(max(color, 0.0), 1.0 / 2.4) - 0.055;
	return select(color <= 0.0031308, low, high);
}

float3 SrgbToLinear(float3 color)
{
	float3 low = color / 12.92;
	float3 high = pow((max(color, 0.0) + 0.055) / 1.055, 2.4);
	return select(color <= 0.04045, low, high);
}

// Interleaved gradient noise (Jimenez 2014) in [0, 1).
float InterleavedGradientNoise(float2 pixel)
{
	return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// Octahedral encoding of unit vectors into [-1, 1]^2 (Cigolle et al. 2014).
float2 OctahedralEncode(float3 n)
{
	n /= abs(n.x) + abs(n.y) + abs(n.z);
	float2 folded = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, 1.0, -1.0);
	return n.z >= 0.0 ? n.xy : folded;
}

float3 OctahedralDecode(float2 e)
{
	float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
	float t = saturate(-n.z);
	n.xy += select(n.xy >= 0.0, -t, t);
	return normalize(n);
}

float Luminance(float3 color)
{
	return dot(color, float3(0.2126, 0.7152, 0.0722));
}

#endif
