// Structures shared by the scene renderer (C++) and its shaders. Members are grouped in 16-byte rows (float3 + scalar) so
// constant-buffer, structured-buffer and C++ layouts agree.
#ifndef STRADA_RENDERER_INTEROP_H
#define STRADA_RENDERER_INTEROP_H

#include "ShaderInterop.h"

ST_SHADER_NAMESPACE_BEGIN

static uint const LightTypeDirectional = 0;
static uint const LightTypePoint = 1;
static uint const LightTypeSpot = 2;

static uint const MaxLights = 256;

static uint const MaterialFlagAlphaMask = 1;
static uint const MaterialFlagHasNormalMap = 2;

// Per frame (set 0, b0).
struct FrameConstants
{
	float4x4 ViewProjection;
	float4x4 InverseViewProjection;
	float3 CameraPosition;
	// Linear exposure multiplier derived from EV100.
	float Exposure;
	// Used when no environment map is bound (uniform ambient).
	float3 AmbientColor;
	uint LightCount;
	float2 ViewportSize;
	// Environment intensity (0 when there is no environment map) and rotation around +Y.
	float EnvironmentIntensity;
	float EnvironmentRotationSin;
	float EnvironmentRotationCos;
	// Highest mip of the prefiltered specular cube (roughness 1).
	float PrefilteredMaxMip;
	// Mip of the radiance cube shown by the sky (blur).
	float SkyboxLod;
	float Padding;
};

// IBL precomputation passes (push constants).
struct EnvironmentConstants
{
	uint OutputSize;
	float SourceSize;
	float SourceMaxMip;
	float Roughness;
	uint SampleCount;
	float3 Padding;
};

// One light (set 0, t0 structured buffer).
struct LightData
{
	float3 Position;
	// Point/spot: distance at which the light fades to zero.
	float Range;
	// Directional/spot: direction the light travels (normalized).
	float3 Direction;
	uint Type;
	// Linear color multiplied by the intensity.
	float3 Radiance;
	float SpotCosInner;
	float SpotCosOuter;
	float3 Padding;
};

// Per material (set 1, b0).
struct MaterialConstants
{
	float4 BaseColor;
	// Linear emissive color multiplied by the intensity.
	float3 Emissive;
	float Metallic;
	float Roughness;
	float NormalStrength;
	float OcclusionStrength;
	float AlphaCutoff;
	float2 UVTiling;
	float2 UVOffset;
	uint Flags;
	float3 Padding;
};

// Per draw (push constants).
struct DrawConstants
{
	float4x4 Model;
	// Inverse transpose of the model matrix (upper 3x3 used).
	float4x4 NormalMatrix;
};

// Tonemap pass (push constants).
struct TonemapConstants
{
	uint Operator;
	uint Dither;
	float2 Padding;
};

ST_SHADER_NAMESPACE_END

#endif
