#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Math/AABB.h"
#include "Strada/Math/Frustum.h"
#include "Strada/Renderer/QuadRenderer.h"
#include "Strada/Renderer/SceneRendererSettings.h"

#include "RendererInterop.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
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

	// One mesh instance (SceneRenderer::SubmitMesh).
	struct MeshSubmission
	{
		// World matrix.
		glm::mat4 Transform = glm::mat4(1.0f);
		// The mesh's world matrix when the scene's current frame began, which is the previous frame's when that frame was
		// drawn (Scene's CachedWorldTransform::Previous): equal to Transform for a mesh that did not move. Velocities read
		// it.
		glm::mat4 PreviousTransform = glm::mat4(1.0f);
		bool CastShadows = true;
		// Non-zero ID reported by picking (below SceneRenderer::MaxPickingId; 0 = not pickable); selected meshes get the
		// selection outline.
		uint32_t PickingId = 0;
		bool Selected = false;
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
		// Debug lines (SubmitLine), anti-aliased: their width in pixels (clamped to MinLineWidth-MaxLineWidth) and an
		// outline on each side (pixels, clamped to 0-MaxLineOutlineWidth) that keeps light lines visible on bright
		// surfaces. The outline color is sRGB-encoded with straight alpha.
		float LineWidth = 1.5f;
		float LineOutlineWidth = 0.0f;
		glm::vec4 LineOutlineColor = glm::vec4(0.0f, 0.0f, 0.0f, 0.6f);
		// Opacity of the parts of depth-tested lines that surfaces hide (clamped to 0-1): 0 hides them, values between
		// show them dimmed.
		float OccludedLineOpacity = 0.0f;

		static constexpr float MinLineWidth = 0.5f;
		static constexpr float MaxLineWidth = 16.0f;
		static constexpr float MaxLineOutlineWidth = 8.0f;
	};

	// What one frame of a scene renderer drew (Docs/Architecture.md §6.2, "Instancing", holds the definitions).
	struct SceneRendererStatistics
	{
		// Every draw command the frame recorded, in every pass that ran: the mesh batches (Batches); every quad batch where
		// it is recorded (world sprites and text in the transparent pass, the UI placed in the world among the overlays, the
		// screen-space sprites, text and UI, and the IDs of all of them in the entity-ID pass); and one each for the sky,
		// the tonemap, FXAA, the grid, the debug lines' instanced draw and their composite, and the selection outline, when
		// they are drawn. Compute dispatches (ambient occlusion, bloom), copies and clears are not draws, and neither are the
		// draws its host records outside every scene renderer (the player's blit to its window, the editor's ImGui).
		uint32_t DrawCalls = 0;
		// Mesh draws of the shadow, opaque, transparent and entity-ID passes: each draws one or more instances of one
		// submesh with one material and pipeline (blended items are one instance each). Part of DrawCalls.
		uint32_t Batches = 0;
		// The mesh instances those batches drew, summed: an instance counts once in every batch that draws it, so once per
		// pass and shadow view. The mesh draws a renderer without instancing would issue.
		uint32_t Instances = 0;
		// Triangles of the batches (a submesh's triangles times its batch's instances, in the four mesh passes), plus two
		// per quad.
		uint32_t Triangles = 0;
		uint32_t Lights = 0;
		// The shadow pass's batches (part of Batches).
		uint32_t ShadowDrawCalls = 0;
		// Cascades plus local light slices rendered this frame.
		uint32_t ShadowMapViews = 0;
		// Shadow-casting local lights that did not fit the shadow map budget (MaxLocalShadowSlices).
		uint32_t ShadowsDropped = 0;
		// Sprites, text glyphs and UI rectangles.
		uint32_t Quads = 0;
		// Submeshes outside the camera's frustum or beyond its MaxDistance, which the camera's passes skip (shadow casters
		// among them still reach the shadow maps).
		uint32_t Culled = 0;
		// Point and spot lights whose range does not reach the camera's view: not in Lights, and without shadow maps.
		uint32_t CulledLights = 0;
		// GPU time in milliseconds of this renderer's latest frame that finished: frames finish after EndScene returns, so
		// the time is a frame or two old. Empty until one has finished, and when the GPU cannot measure time
		// (AdapterInfo::SupportsTimerQueries).
		std::optional<float> GpuMilliseconds;
	};

	// Renders one view of a submitted frame into its own HDR target and tonemaps it into an 8-bit, sRGB-encoded image
	// (GetFinalImage) that the editor viewport displays and the runtime presents. Independent of the ECS: scenes gather
	// their data and submit it between BeginScene and EndScene. Requires Renderer::Init. Main thread only.
	//
	// Every GPU pass is wrapped in an NVRHI marker named after it, so a RenderDoc capture reads by pass. While the profiler
	// records (Docs/Architecture.md §4.4) and the GPU can measure time, each pass is also timed, and the durations reach
	// Profiler::RecordGpuPasses under the renderer's view name once the GPU has finished the frame.
	class SceneRenderer
	{
	public:
		// The most passes of one frame the profiler times; later passes keep their markers only.
		static constexpr uint32_t MaxTimedPasses = 64;

		// view: the name the profiler reports this renderer's GPU passes under ("Viewport", "Game"), at most
		// Profiler::MaxNameBytes long; copied.
		explicit SceneRenderer(std::string_view view = "Scene");
		~SceneRenderer();

		SceneRenderer(SceneRenderer const&) = delete;
		SceneRenderer& operator=(SceneRenderer const&) = delete;

		// Zero sizes are clamped to 1.
		void SetViewportSize(uint32_t width, uint32_t height);
		std::string const& GetViewName() const { return m_ViewName; }
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
		// The same with the mesh's previous world matrix (MeshSubmission); the overload above submits transform as both.
		// Each call is one instance (ShaderInterop::InstanceData), whose normal basis is computed here, once for every pass.
		// The mesh and the materials are kept alive until EndScene returns, one reference per distinct asset of the frame.
		void SubmitMesh(Ref<MeshSource> const& mesh, std::span<Ref<MaterialAsset> const> materials, MeshSubmission const& submission);
		// Sprites and text, in world space (unlit, alpha blended, sorted back to front) or over the final image (see
		// SpriteSubmission and TextSubmission). Picking IDs and selection work as for meshes.
		void SubmitSprite(SpriteSubmission const& sprite);
		void SubmitText(TextSubmission text);
		// The screen-space UI, drawn over everything else in submission order (see UIRectSubmission and UITextSubmission);
		// UI placed in the world is drawn after tonemapping over the grid and under the lines, hidden behind surfaces. Picking
		// IDs and selection work as for meshes.
		void SubmitUIRect(UIRectSubmission const& rect);
		void SubmitUIText(UITextSubmission text);
		// Up to ShaderInterop::MaxLights lights per frame; point and spot lights that cannot reach the camera's view are
		// skipped (SceneRendererStatistics::CulledLights).
		void SubmitDirectionalLight(DirectionalLightSubmission const& light);
		void SubmitPointLight(PointLightSubmission const& light);
		void SubmitSpotLight(SpotLightSubmission const& light);
		// Uniform ambient light (linear color times intensity), used when there is no environment; also the background.
		void SetAmbientLight(glm::vec3 const& radiance);
		void SetEnvironment(EnvironmentSubmission const& environment);
		// Debug line in world space, drawn after tonemapping (color sRGB-encoded, straight alpha) in the width and outline of
		// the frame's overlays. Depth-tested lines are hidden behind surfaces, or dimmed there
		// (SceneRendererOverlays::OccludedLineOpacity); the others are drawn over everything.
		void SubmitLine(glm::vec3 const& from, glm::vec3 const& to, glm::vec4 const& color, bool depthTest = true);
		void SetOverlays(SceneRendererOverlays const& overlays);
		// Records and submits the frame.
		void EndScene();

		// RGBA8_UNORM, sRGB-encoded, in the ShaderResource state between frames. Valid after the first EndScene.
		nvrhi::ITexture* GetFinalImage() const { return m_FinalImage; }
		SceneRendererStatistics const& GetStatistics() const { return m_Statistics; }

		// The instances each mesh pass drew in the last frame, and those each shadow view drew, in the order of the views:
		// the parts of SceneRendererStatistics::Instances.
		struct PassInstanceCounts
		{
			uint32_t Opaque = 0;
			uint32_t Transparent = 0;
			uint32_t EntityIds = 0;
			std::vector<uint32_t> ShadowViews;
		};
		// For tests, valid until the next BeginScene: the last frame's instances as EndScene uploaded them (one per mesh
		// submission, in submission order), and the instances of each pass.
		std::span<ShaderInterop::InstanceData const> GetFrameInstances() const;
		PassInstanceCounts const& GetPassInstanceCounts() const;

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
		// --- Instancing (Docs/Architecture.md §6.2, "Instancing"): the frame's tables, its slim draw items and its batches ---

		// A mesh of the frame: one reference per distinct MeshSource submitted, which keeps it alive until EndScene returns,
		// and its GPU buffers (Renderer::GetMesh), resolved once per frame before recording for the meshes a pass may draw.
		struct FrameMesh
		{
			Ref<MeshSource> Source;
			GpuMesh const* Gpu = nullptr;
			bool Resolved = false;
		};

		// A material of the frame: one reference per distinct MaterialAsset submitted (the default material for empty slots),
		// and what the passes need of it, resolved once per frame before recording for the materials a pass may draw.
		struct FrameMaterial
		{
			Ref<MaterialAsset> Source;
			nvrhi::IBindingSet* Bindings = nullptr;
			// Null in a pass that does not draw the material: blended materials cast no shadows, and the entity-ID pipeline
			// is null while entity IDs are off.
			nvrhi::IGraphicsPipeline* Pipeline = nullptr;
			nvrhi::IGraphicsPipeline* ShadowPipeline = nullptr;
			nvrhi::IGraphicsPipeline* EntityIdPipeline = nullptr;
			bool Blend = false;
			bool Resolved = false;
		};

		// One submitted submesh: what culling, sorting and batching read, and its instance, mesh, submesh and material by
		// index. It holds no reference and no matrix: its instance's data (ShaderInterop::InstanceData) holds the matrices,
		// and the frame's tables the assets.
		struct SubmeshItem
		{
			// World bounds of the submesh: culled against the camera's view and each shadow view.
			AABB WorldBounds;
			// Distance along the view direction (camera looking down -Z) of the submesh's bounds' center: sorting.
			float ViewDepth = 0.0f;
			// Index of its instance among the frame's instances.
			uint32_t Instance = 0;
			// Index into the frame's mesh table, and of the submesh in that mesh.
			uint32_t Mesh = 0;
			uint32_t Submesh = 0;
			// Index into the frame's material table.
			uint32_t Material = 0;
			// The frame's dense index of its mesh, submesh and material: items of one group are drawn by one batch per pass.
			uint32_t Group = 0;
			bool CastShadows = true;
			// Inside the camera's view: drawn by its passes (items outside it are kept only as shadow casters).
			bool InView = true;
		};

		// The key of a group: the frame's mesh and material indices and the submesh.
		struct GroupKey
		{
			uint32_t Mesh = 0;
			uint32_t Submesh = 0;
			uint32_t Material = 0;

			bool operator==(GroupKey const& other) const = default;
		};

		struct GroupKeyHash
		{
			size_t operator()(GroupKey const& key) const;
		};

		// What batching keeps of a group while it builds one pass's batches.
		struct GroupBatch
		{
			// The pass's items of the group, then the next free place of its run while the run is filled.
			uint32_t Count = 0;
			uint32_t Cursor = 0;
			// The view depth of its nearest item (the first one met, items being met front to back) and its first item.
			float Nearest = 0.0f;
			uint32_t FirstItem = 0;
		};

		// One mesh draw of a pass (a drawIndexed of InstanceCount instances): the items of equal pipeline, GPU mesh, submesh
		// and material binding set, whose instances are a run of the frame's instance index buffer.
		struct MeshBatch
		{
			nvrhi::IGraphicsPipeline* Pipeline = nullptr;
			GpuMesh const* Mesh = nullptr;
			nvrhi::IBindingSet* MaterialBindings = nullptr;
			// The submesh's range of the mesh's buffers.
			uint32_t BaseIndex = 0;
			uint32_t IndexCount = 0;
			uint32_t BaseVertex = 0;
			// The run of the instance index buffer it draws: the push constants' InstanceOffset, then SV_InstanceID from 0.
			uint32_t FirstInstance = 0;
			uint32_t InstanceCount = 0;
		};

		// One instance of the line quad (OverlayLines.hlsl).
		struct LineInstance
		{
			glm::vec3 From;
			glm::vec3 To;
			glm::vec4 Color;
			uint32_t DepthTested = 1;
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

		// What the frame binding set names besides the renderer's constant buffers and samplers.
		struct FrameResources
		{
			nvrhi::ITexture* Irradiance = nullptr;
			nvrhi::ITexture* Prefiltered = nullptr;
			nvrhi::ITexture* Radiance = nullptr;
			nvrhi::ITexture* CascadeShadowMap = nullptr;
			nvrhi::ITexture* LocalShadowMap = nullptr;
			nvrhi::IBuffer* Instances = nullptr;
			nvrhi::IBuffer* InstanceIndices = nullptr;

			bool operator==(FrameResources const& other) const = default;
		};

		struct PassTimer
		{
			nvrhi::TimerQueryHandle Query;
			// The pass's name (a string literal).
			char const* Name = nullptr;
		};

		struct FrameTimer
		{
			nvrhi::TimerQueryHandle Query;
			// Recorded in a frame whose time has not been read yet.
			bool Pending = false;
			// Timers of the frame's passes (MaxTimedPasses), made the first time the renderer records a frame while the
			// profiler records, and the passes the pending frame timed with them (none when it was not recorded then).
			std::vector<PassTimer> Passes;
			uint32_t PassCount = 0;
			// Profiler::GetFrameIndex() when the frame was recorded.
			uint64_t ProfilerFrame = 0;
		};

		// One GPU pass of a frame: an NVRHI marker, and the pass's timer when the frame's passes are timed. Ends both when it
		// goes out of scope, so they always pair.
		class PassScope
		{
		public:
			PassScope(SceneRenderer& renderer, nvrhi::ICommandList* commandList, char const* name);
			~PassScope();

			PassScope(PassScope const&) = delete;
			PassScope& operator=(PassScope const&) = delete;

		private:
			nvrhi::ICommandList* m_CommandList;
			nvrhi::ITimerQuery* m_Timer = nullptr;
		};

		void CreateTargets();
		// Marks the items inside the camera's view (SceneRendererStatistics::Culled counts the others).
		void CullToView();
		// The frame's index of a mesh or material, adding it to the frame's table on its first submission.
		uint32_t GetFrameMesh(Ref<MeshSource> const& mesh);
		uint32_t GetFrameMaterial(Ref<MaterialAsset> const& material);
		// Resolves the GPU resources and pipelines of the meshes and materials of the items a pass may draw: those in the
		// camera's view and, with shadows, the casters.
		void ResolveFrameResources(bool shadows);
		// Whether the item's mesh and material were resolved and can be drawn by the camera's passes, and by the shadow pass.
		bool IsDrawable(SubmeshItem const& item) const;
		bool IsShadowCaster(SubmeshItem const& item) const;
		// The batches of the opaque, transparent and entity-ID passes and the instance index buffer's content
		// (Renderer.Batch).
		void BuildBatches();
		MeshBatch MakeBatch(SubmeshItem const& item, nvrhi::IGraphicsPipeline* pipeline, uint32_t firstInstance,
		                    uint32_t instanceCount) const;
		// Grows the instance and instance index buffers to hold the frame (and remakes the binding sets that name them);
		// false when a buffer cannot be made, and the frame then draws no mesh.
		bool EnsureInstanceBuffers(size_t instanceCount, size_t indexCount);
		void CreateEntityIdBindings();
		// Records the batches of one pass, counting them in the statistics; returns the instances they drew.
		uint32_t DrawBatches(nvrhi::ICommandList* commandList, std::span<MeshBatch const> batches, nvrhi::IFramebuffer* framebuffer,
		                     nvrhi::IBindingSet* passBindings);
		// Reads the GPU times of finished frames (handing the times of their passes to the profiler) and returns the timer
		// for this frame: null when the GPU cannot measure time, or when every timer still waits for its frame (that frame
		// goes unmeasured rather than waiting). While the profiler records, the frame's passes are timed with it.
		FrameTimer* AcquireFrameTimer();
		// Hands the times of a finished frame's passes to the profiler and makes their timers reusable.
		void ReportPassTimes(FrameTimer& timer);
		void UpdateFrameBindings(FrameResources const& resources);
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
		nvrhi::IGraphicsPipeline* GetEntityIdPipeline(bool blend, bool doubleSided);
		// Entity IDs of the opaque and then the blended items, and of the quads.
		void RenderEntityIds(nvrhi::ICommandList* commandList);
		// Grid, UI placed in the world and lines onto the tonemapped image (before FXAA when it is enabled).
		void RenderOverlays(nvrhi::ICommandList* commandList, bool ldrTarget);
		// The line layer at the viewport's size and the pipeline that draws into it (before recording; lines need them).
		void CreateLineLayer();
		void RenderSelectionOutline(nvrhi::ICommandList* commandList);
		void CreateOverlayPipelines();

		uint32_t m_Width = 1;
		uint32_t m_Height = 1;
		bool m_TargetsDirty = true;
		bool m_InScene = false;

		SceneRendererCamera m_Camera;
		// The camera's view volume, limited to its MaxDistance.
		Frustum m_ViewFrustum = Frustum(glm::mat4(1.0f));
		SceneRendererSettings m_Settings;
		glm::vec3 m_AmbientRadiance = glm::vec3(0.0f);
		EnvironmentSubmission m_Environment;

		// The frame's meshes and materials (references dropped at the end of EndScene), its instances (kept until the next
		// BeginScene, GetFrameInstances) and its submeshes in submission order.
		std::vector<FrameMesh> m_FrameMeshes;
		std::vector<FrameMaterial> m_FrameMaterials;
		std::unordered_map<MeshSource const*, uint32_t> m_FrameMeshIndices;
		std::unordered_map<MaterialAsset const*, uint32_t> m_FrameMaterialIndices;
		// The default material's index in the frame's table (for empty slots), once a submission needed it.
		std::optional<uint32_t> m_DefaultMaterial;
		std::vector<ShaderInterop::InstanceData> m_Instances;
		std::vector<SubmeshItem> m_Items;
		std::unordered_map<GroupKey, uint32_t, GroupKeyHash> m_GroupIndices;
		// Batching's working state, kept between frames for its memory: per group, and the items of one pass.
		std::vector<GroupBatch> m_GroupBatches;
		std::vector<uint32_t> m_PassItems;
		std::vector<uint32_t> m_PassGroups;
		// The frame's batches: the opaque pass's (front to back), the entity-ID pass's (the opaque batches by first
		// submission, then the blended items) and the transparent pass's (one per blended item, back to front); and the runs
		// of instance indices they draw.
		std::vector<MeshBatch> m_OpaqueBatches;
		std::vector<MeshBatch> m_EntityIdBatches;
		std::vector<MeshBatch> m_BlendBatches;
		std::vector<uint32_t> m_InstanceIndices;
		PassInstanceCounts m_PassInstanceCounts;

		std::vector<ShaderInterop::LightData> m_Lights;
		int32_t m_ShadowedDirectionalLight = -1;
		float m_DirectionalLightSize = 0.0f;
		std::vector<LocalShadowLight> m_LocalShadowLights;
		std::vector<ShadowView> m_ShadowViews;
		SceneRendererStatistics m_Statistics;

		// More than the frames the CPU may run ahead of the GPU, so measuring never waits.
		static constexpr size_t FrameTimerCount = 4;
		std::array<FrameTimer, FrameTimerCount> m_FrameTimers;
		// The oldest timer, reused next.
		size_t m_NextFrameTimer = 0;
		std::optional<float> m_GpuMilliseconds;
		// The timer of the frame being recorded when its passes are timed (PassScope), otherwise null.
		FrameTimer* m_PassTimers = nullptr;
		// The GPU had no timers left for the passes: they are not timed again (the device reported it once).
		bool m_PassTimersUnavailable = false;
		std::string m_ViewName;

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
		// Counts tonemapped frames (film grain changes with it).
		uint32_t m_PostFrame = 0;
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
		// The frame's instances and the runs of instance indices its mesh draws read (grown to the next power of two of what a
		// frame needs, never shrunk).
		nvrhi::BufferHandle m_InstanceBuffer;
		nvrhi::BufferHandle m_InstanceIndexBuffer;
		nvrhi::BindingLayoutHandle m_FrameLayout;
		nvrhi::BindingSetHandle m_FrameBindings;
		// Resources the frame binding set was created with.
		FrameResources m_BoundResources;
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
		std::vector<LineInstance> m_Lines;
		bool m_HasSelection = false;
		nvrhi::BufferHandle m_OverlayConstants;
		nvrhi::BufferHandle m_LineBuffer;
		nvrhi::InputLayoutHandle m_LineInputLayout;
		nvrhi::BindingLayoutHandle m_LineLayout;
		nvrhi::BindingSetHandle m_LineBindings;
		nvrhi::GraphicsPipelineHandle m_LinePipeline;
		// Lines are drawn into a layer (keeping the line nearest to each pixel, by the distance in its depth) that is then
		// blended over the image. Created when lines are first drawn at a size.
		nvrhi::TextureHandle m_LineLayer;
		nvrhi::TextureHandle m_LineLayerDepth;
		nvrhi::FramebufferHandle m_LineLayerFramebuffer;
		nvrhi::BindingLayoutHandle m_LineCompositeLayout;
		nvrhi::BindingSetHandle m_LineCompositeBindings;
		nvrhi::GraphicsPipelineHandle m_LineCompositePipeline;
		nvrhi::BindingLayoutHandle m_GridLayout;
		nvrhi::BindingSetHandle m_GridBindings;
		nvrhi::GraphicsPipelineHandle m_GridPipeline;
		// Tonemapped / final image with the scene depth attached read-only (the UI placed in the world).
		nvrhi::FramebufferHandle m_LdrWorldUIFramebuffer;
		nvrhi::FramebufferHandle m_FinalWorldUIFramebuffer;

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
