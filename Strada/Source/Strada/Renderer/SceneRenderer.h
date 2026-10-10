#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Math/AABB.h"
#include "Strada/Renderer/QuadRenderer.h"
#include "Strada/Renderer/SceneRendererSettings.h"

#include "RendererInterop.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace Strada
{
	class EnvironmentAsset;
	class MaterialAsset;
	class MeshSource;
	struct GpuMesh;

	struct SceneRendererCamera
	{
		glm::mat4 View = glm::mat4(1.0f);
		// Reversed-Z projection (see Math::PerspectiveReversedZ / SceneCamera).
		glm::mat4 Projection = glm::mat4(1.0f);
		glm::vec3 Position = glm::vec3(0.0f);
		// View distance limit (the camera's far plane): meshes beyond it are culled and shadows do not extend beyond it.
		// Infinite by default: everything in the projection's view is drawn.
		float MaxDistance = std::numeric_limits<float>::infinity();
	};

	struct DirectionalLightSubmission
	{
		// Direction the light travels.
		glm::vec3 Direction = glm::vec3(0.0f, -1.0f, 0.0f);
		glm::vec3 Color = glm::vec3(1.0f);
		float Intensity = 1.0f;
		// Only the first shadow-casting directional light gets shadows.
		bool CastShadows = false;
		// Apparent angular diameter in degrees; sizes the soft shadow penumbrae.
		float LightSize = 0.5f;
	};

	struct PointLightSubmission
	{
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec3 Color = glm::vec3(1.0f);
		float Intensity = 1.0f;
		float Range = 10.0f;
		// Uses six shadow-map slices.
		bool CastShadows = false;
	};

	struct SpotLightSubmission
	{
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec3 Direction = glm::vec3(0.0f, 0.0f, -1.0f);
		glm::vec3 Color = glm::vec3(1.0f);
		float Intensity = 1.0f;
		float Range = 10.0f;
		// Half-angles in degrees.
		float InnerConeAngle = 20.0f;
		float OuterConeAngle = 30.0f;
		bool CastShadows = false;
	};

	struct EnvironmentSubmission
	{
		// Equirectangular HDR environment for image-based lighting and the sky.
		Ref<EnvironmentAsset> Environment;
		float Intensity = 1.0f;
		// Degrees around +Y.
		float Rotation = 0.0f;
		// 0 = sharp sky, 1 = fully blurred.
		float SkyboxBlur = 0.0f;
		bool DrawSkybox = true;
	};

	// Editor ground grid on the y = 0 plane, drawn after tonemapping and hidden behind surfaces. Colors are sRGB-encoded with
	// straight alpha.
	struct GridOverlay
	{
		bool Enabled = false;
		float CellSize = 1.0f;
		// Every n-th line is a major line.
		uint32_t MajorLineEvery = 10;
		// Distance from the camera at which the grid has faded out.
		float FadeDistance = 150.0f;
		glm::vec4 MinorColor = glm::vec4(0.55f, 0.55f, 0.55f, 0.35f);
		glm::vec4 MajorColor = glm::vec4(0.65f, 0.65f, 0.65f, 0.6f);
		// The X axis is the line z = 0, the Z axis the line x = 0.
		glm::vec4 AxisXColor = glm::vec4(0.9f, 0.3f, 0.3f, 0.9f);
		glm::vec4 AxisZColor = glm::vec4(0.3f, 0.5f, 0.95f, 0.9f);
	};

	// Editor overlays of one frame (set between BeginScene and EndScene; everything is off by default).
	struct SceneRendererOverlays
	{
		GridOverlay Grid;
		// Outline around selected meshes (requires entity IDs). sRGB-encoded with straight alpha.
		glm::vec4 SelectionColor = glm::vec4(1.0f, 0.55f, 0.1f, 1.0f);
		// Pixels, clamped to 1-8.
		uint32_t SelectionOutlineWidth = 2;
	};

	struct SceneRendererStatistics
	{
		// Including shadow-map draws.
		uint32_t DrawCalls = 0;
		uint32_t Triangles = 0;
		uint32_t Lights = 0;
		uint32_t ShadowDrawCalls = 0;
		// Cascades plus local light slices rendered this frame.
		uint32_t ShadowMapViews = 0;
		// Shadow-casting local lights that did not fit the shadow map budget (MaxLocalShadowSlices).
		uint32_t ShadowsDropped = 0;
		// Sprites and text glyphs.
		uint32_t Quads = 0;
		// Submeshes outside the camera's frustum or beyond its MaxDistance, which the camera's passes skip (shadow casters
		// among them still reach the shadow maps).
		uint32_t Culled = 0;
	};

	// Renders one view of a submitted frame into its own HDR target and tonemaps it into an 8-bit, sRGB-encoded image
	// (GetFinalImage) that the editor viewport displays and the runtime presents. Independent of the ECS: scenes gather
	// their data and submit it between BeginScene and EndScene. Requires Renderer::Init. Main thread only.
	class SceneRenderer
	{
	public:
		SceneRenderer();
		~SceneRenderer();

		SceneRenderer(SceneRenderer const&) = delete;
		SceneRenderer& operator=(SceneRenderer const&) = delete;

		// Zero sizes are clamped to 1.
		void SetViewportSize(uint32_t width, uint32_t height);
		uint32_t GetViewportWidth() const { return m_Width; }
		uint32_t GetViewportHeight() const { return m_Height; }

		// Editor picking: renders the picking ID of the nearest surface per pixel (an extra depth pass over pickable meshes)
		// and enables the selection outline. Off by default.
		void SetEntityIdsEnabled(bool enabled);
		bool AreEntityIdsEnabled() const { return m_EntityIdsEnabled; }

		void BeginScene(SceneRendererCamera const& camera, SceneRendererSettings const& settings);
		// materials: one per material slot of the mesh (null entries use the default material). pickingId: non-zero ID
		// reported by picking (below MaxPickingId; 0 = not pickable); selected meshes get the selection outline.
		void SubmitMesh(Ref<MeshSource> const& mesh, std::span<Ref<MaterialAsset> const> materials, glm::mat4 const& transform,
		                bool castShadows = true, uint32_t pickingId = 0, bool selected = false);
		// Sprites and text, in world space (unlit, alpha blended, sorted back to front) or over the final image (see
		// SpriteSubmission and TextSubmission). Picking IDs and selection work as for meshes.
		void SubmitSprite(SpriteSubmission const& sprite);
		void SubmitText(TextSubmission text);
		void SubmitDirectionalLight(DirectionalLightSubmission const& light);
		void SubmitPointLight(PointLightSubmission const& light);
		void SubmitSpotLight(SpotLightSubmission const& light);
		// Uniform ambient light (linear color times intensity), used when there is no environment; also the background.
		void SetAmbientLight(glm::vec3 const& radiance);
		void SetEnvironment(EnvironmentSubmission const& environment);
		// Debug line in world space, drawn after tonemapping (color sRGB-encoded, straight alpha). Depth-tested lines are
		// hidden behind surfaces.
		void SubmitLine(glm::vec3 const& from, glm::vec3 const& to, glm::vec4 const& color, bool depthTest = true);
		void SetOverlays(SceneRendererOverlays const& overlays);
		// Records and submits the frame.
		void EndScene();

		// RGBA8_UNORM, sRGB-encoded, in the ShaderResource state between frames. Valid after the first EndScene.
		nvrhi::ITexture* GetFinalImage() const { return m_FinalImage; }
		SceneRendererStatistics const& GetStatistics() const { return m_Statistics; }

		// --- Picking ---

		static constexpr uint32_t MaxPickingId = ShaderInterop::EntityIdMask;

		// Reads the picking ID under a pixel of the next rendered frame (the latest request wins). The result arrives through
		// TakePickResult once the GPU has finished that frame: the picking ID, or 0 where no pickable mesh was drawn (also
		// outside the viewport, or when entity IDs are disabled).
		void RequestPick(uint32_t x, uint32_t y);
		// The result of a finished pick request, consumed by this call; empty while none is ready.
		std::optional<uint32_t> TakePickResult();
		bool IsPickPending() const { return m_PickRequest.has_value() || m_PickInFlight; }

	private:
		struct DrawItem
		{
			Ref<MeshSource> Mesh;
			Ref<MaterialAsset> Material;
			glm::mat4 Transform;
			uint32_t SubmeshIndex = 0;
			float ViewDepth = 0.0f;
			AABB WorldBounds;
			bool CastShadows = true;
			// Inside the camera's view: drawn by its passes (items outside it are kept only as shadow casters).
			bool InView = true;
			// Picking ID with ShaderInterop::EntityIdSelectedBit for selected meshes; 0 when not pickable.
			uint32_t EntityId = 0;
			// Resolved before recording (resource creation uploads through its own command list).
			GpuMesh const* GpuData = nullptr;
			nvrhi::IBindingSet* MaterialBindings = nullptr;
			nvrhi::IGraphicsPipeline* Pipeline = nullptr;
			nvrhi::IGraphicsPipeline* ShadowPipeline = nullptr;
			nvrhi::IGraphicsPipeline* EntityIdPipeline = nullptr;
		};

		struct LineVertex
		{
			glm::vec3 Position;
			glm::vec4 Color;
		};

		struct LocalShadowLight
		{
			uint32_t LightIndex = 0;
			bool Point = false;
			glm::vec3 Position = glm::vec3(0.0f);
			glm::vec3 Direction = glm::vec3(0.0f);
			float Range = 0.0f;
			float OuterConeAngle = 0.0f;
		};

		struct ShadowView
		{
			glm::mat4 ViewProjection = glm::mat4(1.0f);
			nvrhi::IFramebuffer* Framebuffer = nullptr;
			// Directional views cull casters against their projection, local views against the light's range sphere.
			bool Directional = false;
			glm::vec3 Center = glm::vec3(0.0f);
			float Radius = 0.0f;
		};

		struct FrameTextures
		{
			nvrhi::ITexture* Irradiance = nullptr;
			nvrhi::ITexture* Prefiltered = nullptr;
			nvrhi::ITexture* Radiance = nullptr;
			nvrhi::ITexture* CascadeShadowMap = nullptr;
			nvrhi::ITexture* LocalShadowMap = nullptr;

			bool operator==(FrameTextures const& other) const = default;
		};

		void CreateTargets();
		// Marks the items inside the camera's view and drops the others, except shadow casters while shadows are drawn.
		void CullToView();
		void UpdateFrameBindings(FrameTextures const& textures);
		nvrhi::IGraphicsPipeline* GetMeshPipeline(bool blend, bool doubleSided);
		nvrhi::IGraphicsPipeline* GetShadowPipeline(bool masked, bool doubleSided);
		void EnsureShadowMap(nvrhi::TextureHandle& texture, std::vector<nvrhi::FramebufferHandle>& framebuffers, uint32_t size,
		                     uint32_t slices, char const* name);
		void ReleaseShadowMap(nvrhi::TextureHandle& texture, std::vector<nvrhi::FramebufferHandle>& framebuffers);
		// Assigns shadow maps to the lights and fills the shadow constants and views.
		void PrepareShadows(ShaderInterop::ShadowConstants& constants);
		void RenderShadows(nvrhi::ICommandList* commandList);
		void RenderAmbientOcclusion(nvrhi::ICommandList* commandList);
		void RenderBloom(nvrhi::ICommandList* commandList);
		void PrepareItems(std::vector<DrawItem>& items, bool blend);
		void DrawItems(nvrhi::ICommandList* commandList, std::vector<DrawItem> const& items, nvrhi::IFramebuffer* framebuffer);
		nvrhi::IGraphicsPipeline* GetEntityIdPipeline(bool blend, bool doubleSided);
		// Entity IDs of the opaque and then the blended items, plus the copy for a pending pick request.
		void RenderEntityIds(nvrhi::ICommandList* commandList);
		// Grid and lines onto the tonemapped image (before FXAA when it is enabled).
		void RenderOverlays(nvrhi::ICommandList* commandList, bool ldrTarget);
		void RenderSelectionOutline(nvrhi::ICommandList* commandList);
		void CreateOverlayPipelines();

		uint32_t m_Width = 1;
		uint32_t m_Height = 1;
		bool m_TargetsDirty = true;
		bool m_InScene = false;

		SceneRendererCamera m_Camera;
		SceneRendererSettings m_Settings;
		glm::vec3 m_AmbientRadiance = glm::vec3(0.0f);
		EnvironmentSubmission m_Environment;
		std::vector<DrawItem> m_OpaqueItems;
		std::vector<DrawItem> m_BlendItems;
		std::vector<ShaderInterop::LightData> m_Lights;
		int32_t m_ShadowedDirectionalLight = -1;
		float m_DirectionalLightSize = 0.0f;
		std::vector<LocalShadowLight> m_LocalShadowLights;
		std::vector<ShadowView> m_ShadowViews;
		SceneRendererStatistics m_Statistics;

		nvrhi::CommandListHandle m_CommandList;
		nvrhi::TextureHandle m_ColorTarget;
		nvrhi::TextureHandle m_DepthTarget;
		// Written by the opaque pass for ambient occlusion: world normals (octahedral) and exposed indirect light.
		nvrhi::TextureHandle m_NormalTarget;
		nvrhi::TextureHandle m_IndirectTarget;
		nvrhi::TextureHandle m_RawOcclusion;
		nvrhi::TextureHandle m_Occlusion;
		// Half resolution with a mip chain; mip 0 holds the accumulated bloom.
		nvrhi::TextureHandle m_BloomTexture;
		uint32_t m_BloomMipCount = 0;
		// Tonemapped image before FXAA.
		nvrhi::TextureHandle m_LdrTarget;
		nvrhi::TextureHandle m_FinalImage;
		// Color, normals, indirect light and depth (opaque pass); color and depth (sky and blended surfaces).
		nvrhi::FramebufferHandle m_OpaqueFramebuffer;
		nvrhi::FramebufferHandle m_SceneFramebuffer;
		nvrhi::FramebufferHandle m_LdrFramebuffer;
		nvrhi::FramebufferHandle m_FinalFramebuffer;

		nvrhi::BufferHandle m_FrameConstants;
		nvrhi::BufferHandle m_LightBuffer;
		nvrhi::BindingLayoutHandle m_FrameLayout;
		nvrhi::BindingSetHandle m_FrameBindings;
		// Textures the frame binding set was created with.
		FrameTextures m_BoundTextures;
		nvrhi::GraphicsPipelineHandle m_SkyPipeline;
		nvrhi::InputLayoutHandle m_InputLayout;
		// Positions and texture coordinates only (shadow and entity ID passes).
		nvrhi::InputLayoutHandle m_DepthInputLayout;
		nvrhi::GraphicsPipelineHandle m_MeshPipelines[4];

		nvrhi::BufferHandle m_ShadowConstants;
		nvrhi::SamplerHandle m_ShadowCompareSampler;
		nvrhi::SamplerHandle m_ShadowPointSampler;
		// One slice per cascade / per local light view; 1x1 fallback bound when there is no shadow map.
		nvrhi::TextureHandle m_CascadeShadowMap;
		std::vector<nvrhi::FramebufferHandle> m_CascadeFramebuffers;
		nvrhi::TextureHandle m_LocalShadowMap;
		std::vector<nvrhi::FramebufferHandle> m_LocalFramebuffers;
		nvrhi::TextureHandle m_FallbackShadowMap;
		nvrhi::BindingLayoutHandle m_ShadowLayout;
		nvrhi::BindingSetHandle m_ShadowBindings;
		nvrhi::GraphicsPipelineHandle m_ShadowPipelines[4];

		nvrhi::BufferHandle m_OcclusionConstants;
		nvrhi::BindingLayoutHandle m_OcclusionLayout;
		nvrhi::BindingLayoutHandle m_DenoiseLayout;
		nvrhi::BindingLayoutHandle m_CompositeLayout;
		nvrhi::BindingSetHandle m_OcclusionBindings;
		nvrhi::BindingSetHandle m_DenoiseBindings;
		nvrhi::BindingSetHandle m_CompositeBindings;
		nvrhi::ComputePipelineHandle m_OcclusionPipeline;
		nvrhi::ComputePipelineHandle m_DenoisePipeline;
		nvrhi::ComputePipelineHandle m_CompositePipeline;

		nvrhi::BindingLayoutHandle m_BloomLayout;
		// Per mip: downsample into it, and (except the last) upsample the next mip into it.
		std::vector<nvrhi::BindingSetHandle> m_BloomDownsampleBindings;
		std::vector<nvrhi::BindingSetHandle> m_BloomUpsampleBindings;
		nvrhi::ComputePipelineHandle m_BloomDownsamplePipeline;
		nvrhi::ComputePipelineHandle m_BloomUpsamplePipeline;

		nvrhi::BindingLayoutHandle m_FxaaLayout;
		nvrhi::BindingSetHandle m_FxaaBindings;
		nvrhi::GraphicsPipelineHandle m_FxaaPipeline;

		nvrhi::BindingLayoutHandle m_TonemapLayout;
		nvrhi::BindingSetHandle m_TonemapBindings;
		nvrhi::GraphicsPipelineHandle m_TonemapPipeline;

		Scope<QuadRenderer> m_Quads;

		// Editor overlays.
		SceneRendererOverlays m_Overlays;
		std::vector<LineVertex> m_DepthTestedLines;
		std::vector<LineVertex> m_OnTopLines;
		bool m_HasSelection = false;
		nvrhi::BufferHandle m_OverlayConstants;
		nvrhi::BufferHandle m_LineVertexBuffer;
		nvrhi::InputLayoutHandle m_LineInputLayout;
		nvrhi::BindingLayoutHandle m_LineLayout;
		nvrhi::BindingSetHandle m_LineBindings;
		// [depth tested]
		nvrhi::GraphicsPipelineHandle m_LinePipelines[2];
		nvrhi::BindingLayoutHandle m_GridLayout;
		nvrhi::BindingSetHandle m_GridBindings;
		nvrhi::GraphicsPipelineHandle m_GridPipeline;
		// Tonemapped / final image with the scene depth attached read-only (depth-tested lines).
		nvrhi::FramebufferHandle m_LdrOverlayFramebuffer;
		nvrhi::FramebufferHandle m_FinalOverlayFramebuffer;

		// Entity IDs and picking.
		bool m_EntityIdsEnabled = false;
		nvrhi::TextureHandle m_EntityIdTarget;
		nvrhi::TextureHandle m_EntityIdDepth;
		nvrhi::FramebufferHandle m_EntityIdFramebuffer;
		nvrhi::BindingLayoutHandle m_EntityIdLayout;
		nvrhi::BindingSetHandle m_EntityIdBindings;
		nvrhi::GraphicsPipelineHandle m_EntityIdPipelines[4];
		nvrhi::BindingLayoutHandle m_OutlineLayout;
		nvrhi::BindingSetHandle m_OutlineBindings;
		nvrhi::GraphicsPipelineHandle m_OutlinePipeline;
		std::optional<glm::uvec2> m_PickRequest;
		bool m_PickInFlight = false;
		std::optional<uint32_t> m_PickResult;
		nvrhi::StagingTextureHandle m_PickStaging;
		nvrhi::EventQueryHandle m_PickQuery;
	};
}
