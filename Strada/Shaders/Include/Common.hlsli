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

#endif
