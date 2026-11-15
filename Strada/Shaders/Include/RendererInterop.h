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
static uint const MaxShadowCascades = 4;
// Shadow-map slices for spot lights (one each) and point lights (six each).
static uint const MaxLocalShadowSlices = 24;

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
	// First shadow-map slice (-1 without shadows): the cascades for the directional light, the local map otherwise.
	int ShadowIndex;
	// Local lights: world size of a shadow-map texel per unit of distance from the light.
	float ShadowTexelScale;
	float Padding;
};

// Shadow mapping (set 0, b2). Shadow maps use reversed Z like the scene: closer to the light is larger.
struct ShadowConstants
{
	float4x4 CascadeViewProjection[MaxShadowCascades];
	float4x4 LocalViewProjection[MaxLocalShadowSlices];
	// View distance at which each cascade ends.
	float4 CascadeSplits;
	// World size of a texel, width and depth range of each cascade.
	float4 CascadeTexelSizes;
	float4 CascadeWidths;
	float4 CascadeDepthRanges;
	float3 CameraForward;
	uint CascadeCount;
	// tan of half the directional light's angular diameter: penumbra size per unit of blocker distance.
	float LightTanHalfAngle;
	float CascadeMapSize;
	float LocalMapSize;
	// Percentage-closer soft shadows for the directional light (otherwise a fixed filter).
	uint SoftShadows;
	float ShadowDistance;
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

// Per mesh draw of the opaque and transparent passes (push constants): the draw's run of the frame's instance index buffer.
// A draw's instances are g_Instances[g_InstanceIndices[InstanceOffset + SV_InstanceID]].
struct DrawConstants
{
	uint InstanceOffset;
	float3 Padding;
};

// Per mesh draw of the shadow pass (push constants): the shadow view's view-projection and the draw's run of the frame's
// instance index buffer, read as DrawConstants' run is.
struct ShadowDrawConstants
{
	float4x4 ViewProjection;
	uint InstanceOffset;
	float3 Padding;
};

// One mesh instance of a frame: an entity's mesh as SubmitMesh received it, drawn by every pass that draws the mesh
// (Docs/Architecture.md §6.2, "Instancing"). A structured buffer per scene renderer holds every instance of the frame;
// mesh draws find theirs through the frame's instance index buffer, at their push constants' offset plus SV_InstanceID.
struct InstanceData
{
	float4x4 Model;
	// The columns of the inverse transpose of Model's upper 3x3 times the absolute value of its determinant (the cofactor
	// matrix times the determinant's sign): shaders normalize the normals they transform, so no division is needed, and a
	// singular scale gives a finite basis. Computed once per instance, for every pass. W is 0.
	float4 NormalBasis[3];
	// Picking ID with EntityIdSelectedBit for selected meshes; 0 when not pickable.
	uint EntityId;
	float3 Padding;
	// The entity's world matrix when the scene's current frame began, which is the previous frame's when that frame was
	// drawn (Scene's CachedWorldTransform::Previous); equal to Model for an instance that did not move. Read for
	// velocities, which a renderer reads as zero in a draw that does not follow its last one (Docs/Architecture.md §6.2,
	// "Velocities and cuts").
	float4x4 PreviousModel;
};

// Ground-truth ambient occlusion (compute, b0).
struct AmbientOcclusionConstants
{
	float4x4 InverseProjection;
	float4x4 View;
	float2 ViewportSize;
	float2 InverseViewportSize;
	// World-space radius of the occlusion search.
	float Radius;
	// Exponent applied to the visibility (contrast).
	float Intensity;
	// Pixels per world unit at view distance 1 (perspective) or everywhere (orthographic).
	float ProjectionScale;
	uint Orthographic;
	uint SliceCount;
	uint StepCount;
	float2 Padding;
};

// Bloom downsample and upsample passes (push constants).
struct BloomConstants
{
	float2 SourceTexelSize;
	uint2 OutputSize;
	// Karis average on the first downsample (suppresses fireflies).
	uint FirstPass;
	float3 Padding;
};

// FXAA (push constants).
struct FxaaConstants
{
	float2 InverseSize;
	float2 Padding;
};

// Tonemap pass (push constants).
struct TonemapConstants
{
	uint Operator;
	uint Dither;
	// Fraction of the bloom texture mixed into the scene color (0 disables bloom).
	float BloomIntensity;
	// 1 / number of bloom levels: the upsample chain sums every level into the first.
	float BloomNormalization;
	// Color grading (SceneRendererSettings).
	float Contrast;
	float Saturation;
	float Temperature;
	float Tint;
	float3 ColorFilter;
	float Vignette;
	float VignetteSmoothness;
	float ChromaticAberration;
	float FilmGrain;
	// Changes every frame: animates the film grain.
	uint Frame;
};

// Entity IDs (editor picking): the top bit marks selected entities, the remaining bits hold the picking ID (0 = none).
static uint const EntityIdSelectedBit = 0x80000000u;
static uint const EntityIdMask = 0x7FFFFFFFu;

// Camera of the editor overlay passes: entity IDs, grid, lines, selection outline (b0).
struct OverlayConstants
{
	float4x4 ViewProjection;
	float4x4 InverseViewProjection;
	float3 CameraPosition;
	float Padding0;
	float2 ViewportSize;
	float2 Padding1;
};

// Per mesh draw of the entity ID pass (push constants): as DrawConstants; each instance carries its entity ID.
struct EntityIdDrawConstants
{
	uint InstanceOffset;
	float3 Padding;
};

// Ground grid on the y = 0 plane (push constants). Colors are sRGB-encoded with straight alpha.
struct GridConstants
{
	float4 MinorColor;
	float4 MajorColor;
	float4 AxisXColor;
	float4 AxisZColor;
	float CellSize;
	// Every n-th line is a major line.
	float MajorLineEvery;
	// Distance from the camera at which the grid has faded out.
	float FadeDistance;
	float Padding;
};

// Selection outline (push constants). The color is sRGB-encoded with straight alpha.
struct OutlineConstants
{
	float4 Color;
	// Outline width in pixels.
	int Radius;
	float3 Padding;
};

// Debug lines (push constants), drawn as screen-space quads. The outline color is sRGB-encoded with straight alpha.
struct LineConstants
{
	float4 OutlineColor;
	// Pixels.
	float Width;
	// Pixels on each side of the line.
	float OutlineWidth;
	// Alpha multiplier of the parts of depth-tested lines behind surfaces (0 hides them).
	float OccludedOpacity;
	float Padding;
};

static uint const QuadModeSprite = 0;
static uint const QuadModeText = 1;
// A sprite whose light adds to what is behind it (particles).
static uint const QuadModeAdditive = 2;
// A rectangle of the screen-space UI: a sprite with anti-aliased, optionally rounded edges.
static uint const QuadModeUIRect = 3;
// A UI rectangle placed in the world: its edges are anti-aliased over a pixel of the screen at any distance.
static uint const QuadModeWorldUIRect = 4;

// Sprites, text and UI rectangles (push constants).
struct QuadConstants
{
	// World quads: the view-projection; screen quads: identity (their positions are in clip space already).
	float4x4 Transform;
	// QuadModeSprite, QuadModeText, QuadModeAdditive, QuadModeUIRect or QuadModeWorldUIRect.
	uint Mode;
	// Non-zero when drawing onto the 8-bit sRGB-encoded final image.
	uint EncodeSrgb;
	// Text: atlas pixels per unit of a distance-field sample, and the sample on outlines.
	float DistanceScale;
	float EdgeSample;
	// Text: the atlas size in pixels.
	float2 AtlasSize;
	float2 Padding;
};

ST_SHADER_NAMESPACE_END

#if defined(__cplusplus)
// InstanceData is uploaded as is into a structured buffer: its rows must stay where the shaders read them.
static_assert(sizeof(Strada::ShaderInterop::InstanceData) == 192, "InstanceData must keep its 192-byte layout");
static_assert(offsetof(Strada::ShaderInterop::InstanceData, Model) == 0);
static_assert(offsetof(Strada::ShaderInterop::InstanceData, NormalBasis) == 64);
static_assert(offsetof(Strada::ShaderInterop::InstanceData, EntityId) == 112);
static_assert(offsetof(Strada::ShaderInterop::InstanceData, PreviousModel) == 128);
static_assert(sizeof(Strada::ShaderInterop::InstanceData) % 16 == 0, "Structured buffer rows are 16 bytes");
#endif

#endif
