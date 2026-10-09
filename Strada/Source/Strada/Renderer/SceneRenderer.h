#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Renderer/SceneRendererSettings.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <cstdint>
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
	};

	struct DirectionalLightSubmission
	{
		// Direction the light travels.
		glm::vec3 Direction = glm::vec3(0.0f, -1.0f, 0.0f);
		glm::vec3 Color = glm::vec3(1.0f);
		float Intensity = 1.0f;
	};

	struct PointLightSubmission
	{
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec3 Color = glm::vec3(1.0f);
		float Intensity = 1.0f;
		float Range = 10.0f;
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

	struct SceneRendererStatistics
	{
		uint32_t DrawCalls = 0;
		uint32_t Triangles = 0;
		uint32_t Lights = 0;
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

		void BeginScene(SceneRendererCamera const& camera, SceneRendererSettings const& settings);
		// materials: one per material slot of the mesh (null entries use the default material).
		void SubmitMesh(Ref<MeshSource> const& mesh, std::span<Ref<MaterialAsset> const> materials, glm::mat4 const& transform);
		void SubmitDirectionalLight(DirectionalLightSubmission const& light);
		void SubmitPointLight(PointLightSubmission const& light);
		void SubmitSpotLight(SpotLightSubmission const& light);
		// Uniform ambient light (linear color times intensity), used when there is no environment; also the background.
		void SetAmbientLight(glm::vec3 const& radiance);
		void SetEnvironment(EnvironmentSubmission const& environment);
		// Records and submits the frame.
		void EndScene();

		// RGBA8_UNORM, sRGB-encoded, in the ShaderResource state between frames. Valid after the first EndScene.
		nvrhi::ITexture* GetFinalImage() const { return m_FinalImage; }
		SceneRendererStatistics const& GetStatistics() const { return m_Statistics; }

	private:
		struct DrawItem
		{
			Ref<MeshSource> Mesh;
			Ref<MaterialAsset> Material;
			glm::mat4 Transform;
			uint32_t SubmeshIndex = 0;
			float ViewDepth = 0.0f;
			// Resolved before recording (resource creation uploads through its own command list).
			GpuMesh const* GpuData = nullptr;
			nvrhi::IBindingSet* MaterialBindings = nullptr;
			nvrhi::IGraphicsPipeline* Pipeline = nullptr;
		};

		void CreateTargets();
		void UpdateFrameBindings(nvrhi::ITexture* irradiance, nvrhi::ITexture* prefiltered, nvrhi::ITexture* radiance);
		nvrhi::IGraphicsPipeline* GetMeshPipeline(bool blend, bool doubleSided);
		void PrepareItems(std::vector<DrawItem>& items, bool blend);
		void DrawItems(nvrhi::ICommandList* commandList, std::vector<DrawItem> const& items);

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
		std::vector<uint8_t> m_LightData;
		uint32_t m_LightCount = 0;
		SceneRendererStatistics m_Statistics;

		nvrhi::CommandListHandle m_CommandList;
		nvrhi::TextureHandle m_ColorTarget;
		nvrhi::TextureHandle m_DepthTarget;
		nvrhi::TextureHandle m_FinalImage;
		nvrhi::FramebufferHandle m_SceneFramebuffer;
		nvrhi::FramebufferHandle m_FinalFramebuffer;

		nvrhi::BufferHandle m_FrameConstants;
		nvrhi::BufferHandle m_LightBuffer;
		nvrhi::BindingLayoutHandle m_FrameLayout;
		nvrhi::BindingSetHandle m_FrameBindings;
		// Environment textures the frame binding set was created with.
		nvrhi::ITexture* m_BoundTextures[3] = {};
		nvrhi::GraphicsPipelineHandle m_SkyPipeline;
		nvrhi::InputLayoutHandle m_InputLayout;
		nvrhi::GraphicsPipelineHandle m_MeshPipelines[4];

		nvrhi::BindingLayoutHandle m_TonemapLayout;
		nvrhi::BindingSetHandle m_TonemapBindings;
		nvrhi::GraphicsPipelineHandle m_TonemapPipeline;
	};
}
