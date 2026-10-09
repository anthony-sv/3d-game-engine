#include "stpch.h"
#include "Strada/Renderer/SceneRenderer.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Asset/EnvironmentAsset.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/MeshSource.h"
#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/RHI/ShaderLibrary.h"
#include "Strada/Renderer/EnvironmentMap.h"
#include "Strada/Renderer/Renderer.h"

#include "RendererInterop.h"

#include <algorithm>
#include <cstring>

namespace Strada
{
	static_assert(sizeof(ShaderInterop::FrameConstants) == 192);
	static_assert(sizeof(ShaderInterop::LightData) == 64);
	static_assert(sizeof(ShaderInterop::DrawConstants) == 128, "Push constants must fit the 128 bytes Vulkan guarantees");
	static_assert(sizeof(ShaderInterop::TonemapConstants) == 16);

	namespace
	{
		constexpr nvrhi::Format ColorFormat = nvrhi::Format::RGBA16_FLOAT;
		constexpr nvrhi::Format DepthFormat = nvrhi::Format::D32;
		constexpr nvrhi::Format FinalFormat = nvrhi::Format::RGBA8_UNORM;

		uint32_t PipelineIndex(bool blend, bool doubleSided)
		{
			return (blend ? 2u : 0u) + (doubleSided ? 1u : 0u);
		}

		Ref<MaterialAsset> GetDefaultMaterial()
		{
			return AssetManager::GetAsset<MaterialAsset>(GetBuiltInHandle(BuiltInAsset::DefaultMaterial));
		}
	}

	SceneRenderer::SceneRenderer()
	{
		ST_CORE_ASSERT(Renderer::IsInitialized(), "SceneRenderer requires Renderer::Init");
		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		m_CommandList = device->createCommandList();

		nvrhi::BufferDesc frameDesc;
		frameDesc.byteSize = sizeof(ShaderInterop::FrameConstants);
		frameDesc.isConstantBuffer = true;
		frameDesc.isVolatile = true;
		frameDesc.maxVersions = 16;
		frameDesc.debugName = "Frame constants";
		m_FrameConstants = device->createBuffer(frameDesc);

		nvrhi::BufferDesc lightDesc;
		lightDesc.byteSize = sizeof(ShaderInterop::LightData) * ShaderInterop::MaxLights;
		lightDesc.structStride = sizeof(ShaderInterop::LightData);
		lightDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		lightDesc.keepInitialState = true;
		lightDesc.debugName = "Lights";
		m_LightBuffer = device->createBuffer(lightDesc);

		nvrhi::BindingLayoutDesc frameLayout;
		frameLayout.visibility = nvrhi::ShaderType::All;
		frameLayout.registerSpace = 0;
		frameLayout.registerSpaceIsDescriptorSet = true;
		frameLayout.bindings = {
			nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),
			nvrhi::BindingLayoutItem::Texture_SRV(1),
			nvrhi::BindingLayoutItem::Texture_SRV(2),
			nvrhi::BindingLayoutItem::Texture_SRV(3),
			nvrhi::BindingLayoutItem::Texture_SRV(4),
			nvrhi::BindingLayoutItem::Sampler(0),
			nvrhi::BindingLayoutItem::Sampler(1),
			nvrhi::BindingLayoutItem::PushConstants(1, sizeof(ShaderInterop::DrawConstants)),
		};
		m_FrameLayout = device->createBindingLayout(frameLayout);
		nvrhi::ITexture* fallback = Renderer::GetFallbackCube();
		UpdateFrameBindings(fallback, fallback, fallback);

		nvrhi::VertexAttributeDesc const attributes[] = {
			nvrhi::VertexAttributeDesc()
				.setName("POSITION")
				.setFormat(nvrhi::Format::RGB32_FLOAT)
				.setOffset(offsetof(Vertex, Position))
				.setElementStride(sizeof(Vertex)),
			nvrhi::VertexAttributeDesc()
				.setName("NORMAL")
				.setFormat(nvrhi::Format::RGB32_FLOAT)
				.setOffset(offsetof(Vertex, Normal))
				.setElementStride(sizeof(Vertex)),
			nvrhi::VertexAttributeDesc()
				.setName("TANGENT")
				.setFormat(nvrhi::Format::RGBA32_FLOAT)
				.setOffset(offsetof(Vertex, Tangent))
				.setElementStride(sizeof(Vertex)),
			nvrhi::VertexAttributeDesc()
				.setName("TEXCOORD")
				.setFormat(nvrhi::Format::RG32_FLOAT)
				.setOffset(offsetof(Vertex, TexCoord))
				.setElementStride(sizeof(Vertex)),
		};
		m_InputLayout =
			device->createInputLayout(attributes, static_cast<uint32_t>(std::size(attributes)), ShaderLibrary::Get("ForwardPBR_VS"));

		nvrhi::BindingLayoutDesc tonemapLayout;
		tonemapLayout.visibility = nvrhi::ShaderType::All;
		tonemapLayout.bindings = {
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(ShaderInterop::TonemapConstants)),
		};
		m_TonemapLayout = device->createBindingLayout(tonemapLayout);

		ST_CORE_ASSERT(m_CommandList && m_FrameConstants && m_LightBuffer && m_FrameLayout && m_FrameBindings && m_InputLayout &&
		                   m_TonemapLayout,
		               "Failed to create scene renderer resources");
	}

	SceneRenderer::~SceneRenderer() = default;

	void SceneRenderer::UpdateFrameBindings(nvrhi::ITexture* irradiance, nvrhi::ITexture* prefiltered, nvrhi::ITexture* radiance)
	{
		if (m_FrameBindings && m_BoundTextures[0] == irradiance && m_BoundTextures[1] == prefiltered && m_BoundTextures[2] == radiance)
		{
			return;
		}
		nvrhi::TextureDimension const cube = nvrhi::TextureDimension::TextureCube;
		nvrhi::BindingSetDesc frameBindings;
		frameBindings.bindings = {
			nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstants),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_LightBuffer),
			nvrhi::BindingSetItem::Texture_SRV(1, irradiance, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, cube),
			nvrhi::BindingSetItem::Texture_SRV(2, prefiltered, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, cube),
			nvrhi::BindingSetItem::Texture_SRV(3, Renderer::GetBrdfLut()),
			nvrhi::BindingSetItem::Texture_SRV(4, radiance, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, cube),
			nvrhi::BindingSetItem::Sampler(0, Renderer::GetMaterialSampler()),
			nvrhi::BindingSetItem::Sampler(1, Renderer::GetLinearClampSampler()),
			nvrhi::BindingSetItem::PushConstants(1, sizeof(ShaderInterop::DrawConstants)),
		};
		m_FrameBindings = GraphicsDevice::GetDevice()->createBindingSet(frameBindings, m_FrameLayout);
		m_BoundTextures[0] = irradiance;
		m_BoundTextures[1] = prefiltered;
		m_BoundTextures[2] = radiance;
	}

	void SceneRenderer::SetViewportSize(uint32_t width, uint32_t height)
	{
		width = std::max(width, 1u);
		height = std::max(height, 1u);
		if (width != m_Width || height != m_Height)
		{
			m_Width = width;
			m_Height = height;
			m_TargetsDirty = true;
		}
	}

	void SceneRenderer::CreateTargets()
	{
		nvrhi::IDevice* device = GraphicsDevice::GetDevice();

		nvrhi::TextureDesc colorDesc;
		colorDesc.width = m_Width;
		colorDesc.height = m_Height;
		colorDesc.format = ColorFormat;
		colorDesc.isRenderTarget = true;
		colorDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		colorDesc.keepInitialState = true;
		colorDesc.setClearValue(nvrhi::Color(0.0f));
		colorDesc.debugName = "Scene color";
		m_ColorTarget = device->createTexture(colorDesc);

		nvrhi::TextureDesc depthDesc = colorDesc;
		depthDesc.format = DepthFormat;
		depthDesc.initialState = nvrhi::ResourceStates::DepthWrite;
		// Reversed Z: the far plane is 0.
		depthDesc.setClearValue(nvrhi::Color(0.0f));
		depthDesc.debugName = "Scene depth";
		m_DepthTarget = device->createTexture(depthDesc);

		nvrhi::TextureDesc finalDesc = colorDesc;
		finalDesc.format = FinalFormat;
		finalDesc.debugName = "Final image";
		m_FinalImage = device->createTexture(finalDesc);

		m_SceneFramebuffer =
			device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_ColorTarget).setDepthAttachment(m_DepthTarget));
		m_FinalFramebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_FinalImage));

		nvrhi::BindingSetDesc tonemapBindings;
		tonemapBindings.bindings = {
			nvrhi::BindingSetItem::Texture_SRV(0, m_ColorTarget),
			nvrhi::BindingSetItem::PushConstants(0, sizeof(ShaderInterop::TonemapConstants)),
		};
		m_TonemapBindings = device->createBindingSet(tonemapBindings, m_TonemapLayout);

		if (!m_SkyPipeline)
		{
			nvrhi::GraphicsPipelineDesc desc;
			desc.VS = ShaderLibrary::Get("Sky_VS");
			desc.PS = ShaderLibrary::Get("Sky_PS");
			desc.bindingLayouts = {m_FrameLayout};
			desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
			// The triangle sits at the far plane (depth 0), so only pixels without geometry pass.
			desc.renderState.depthStencilState.depthTestEnable = true;
			desc.renderState.depthStencilState.depthWriteEnable = false;
			desc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::GreaterOrEqual;
			m_SkyPipeline = device->createGraphicsPipeline(desc, m_SceneFramebuffer->getFramebufferInfo());
		}
		if (!m_TonemapPipeline)
		{
			nvrhi::GraphicsPipelineDesc desc;
			desc.VS = ShaderLibrary::Get("Fullscreen_VS");
			desc.PS = ShaderLibrary::Get("Tonemap_PS");
			desc.bindingLayouts = {m_TonemapLayout};
			desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
			desc.renderState.depthStencilState.depthTestEnable = false;
			desc.renderState.depthStencilState.depthWriteEnable = false;
			m_TonemapPipeline = device->createGraphicsPipeline(desc, m_FinalFramebuffer->getFramebufferInfo());
		}
		m_TargetsDirty = false;
	}

	nvrhi::IGraphicsPipeline* SceneRenderer::GetMeshPipeline(bool blend, bool doubleSided)
	{
		nvrhi::GraphicsPipelineHandle& pipeline = m_MeshPipelines[PipelineIndex(blend, doubleSided)];
		if (pipeline)
		{
			return pipeline;
		}

		nvrhi::GraphicsPipelineDesc desc;
		desc.inputLayout = m_InputLayout;
		desc.VS = ShaderLibrary::Get("ForwardPBR_VS");
		desc.PS = ShaderLibrary::Get("ForwardPBR_PS");
		desc.bindingLayouts = {m_FrameLayout, Renderer::GetMaterialBindingLayout()};
		desc.renderState.rasterState.cullMode = doubleSided ? nvrhi::RasterCullMode::None : nvrhi::RasterCullMode::Back;
		desc.renderState.rasterState.frontCounterClockwise = true;
		desc.renderState.depthStencilState.depthTestEnable = true;
		desc.renderState.depthStencilState.depthWriteEnable = !blend;
		desc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::GreaterOrEqual;
		if (blend)
		{
			nvrhi::BlendState::RenderTarget& target = desc.renderState.blendState.targets[0];
			target.blendEnable = true;
			target.srcBlend = nvrhi::BlendFactor::SrcAlpha;
			target.destBlend = nvrhi::BlendFactor::InvSrcAlpha;
			target.srcBlendAlpha = nvrhi::BlendFactor::One;
			target.destBlendAlpha = nvrhi::BlendFactor::InvSrcAlpha;
		}
		pipeline = GraphicsDevice::GetDevice()->createGraphicsPipeline(desc, m_SceneFramebuffer->getFramebufferInfo());
		if (!pipeline)
		{
			ST_CORE_ERROR("Failed to create a mesh pipeline");
		}
		return pipeline;
	}

	void SceneRenderer::BeginScene(SceneRendererCamera const& camera, SceneRendererSettings const& settings)
	{
		ST_CORE_ASSERT(!m_InScene, "BeginScene called twice without EndScene");
		m_InScene = true;
		m_Camera = camera;
		m_Settings = settings;
		m_AmbientRadiance = glm::vec3(0.0f);
		m_Environment = {};
		m_OpaqueItems.clear();
		m_BlendItems.clear();
		m_LightData.clear();
		m_LightCount = 0;
		m_Statistics = {};
	}

	void SceneRenderer::SubmitMesh(Ref<MeshSource> const& mesh, std::span<Ref<MaterialAsset> const> materials, glm::mat4 const& transform)
	{
		ST_CORE_ASSERT(m_InScene, "SubmitMesh outside BeginScene/EndScene");
		if (!mesh)
		{
			return;
		}
		glm::mat4 const viewModel = m_Camera.View * transform;
		std::vector<Submesh> const& submeshes = mesh->GetSubmeshes();
		for (uint32_t i = 0; i < submeshes.size(); i++)
		{
			Submesh const& submesh = submeshes[i];
			Ref<MaterialAsset> material = submesh.MaterialIndex < materials.size() ? materials[submesh.MaterialIndex] : nullptr;
			if (!material)
			{
				material = GetDefaultMaterial();
			}
			if (!material)
			{
				continue;
			}

			DrawItem item;
			item.Mesh = mesh;
			item.Material = material;
			item.Transform = transform;
			item.SubmeshIndex = i;
			// Distance along the view direction (camera looks down -Z) for sorting.
			item.ViewDepth = -(viewModel * glm::vec4(submesh.Bounds.GetCenter(), 1.0f)).z;
			(material->GetData().AlphaMode == MaterialAlphaMode::Blend ? m_BlendItems : m_OpaqueItems).push_back(std::move(item));
		}
	}

	void SceneRenderer::SubmitDirectionalLight(DirectionalLightSubmission const& light)
	{
		ShaderInterop::LightData data{};
		data.Type = ShaderInterop::LightTypeDirectional;
		data.Direction = glm::normalize(light.Direction);
		data.Radiance = light.Color * light.Intensity;
		if (m_LightCount < ShaderInterop::MaxLights)
		{
			m_LightData.insert(m_LightData.end(), reinterpret_cast<uint8_t const*>(&data),
			                   reinterpret_cast<uint8_t const*>(&data) + sizeof(data));
			m_LightCount++;
		}
	}

	void SceneRenderer::SubmitPointLight(PointLightSubmission const& light)
	{
		ShaderInterop::LightData data{};
		data.Type = ShaderInterop::LightTypePoint;
		data.Position = light.Position;
		data.Range = std::max(light.Range, 1e-3f);
		data.Radiance = light.Color * light.Intensity;
		if (m_LightCount < ShaderInterop::MaxLights)
		{
			m_LightData.insert(m_LightData.end(), reinterpret_cast<uint8_t const*>(&data),
			                   reinterpret_cast<uint8_t const*>(&data) + sizeof(data));
			m_LightCount++;
		}
	}

	void SceneRenderer::SubmitSpotLight(SpotLightSubmission const& light)
	{
		ShaderInterop::LightData data{};
		data.Type = ShaderInterop::LightTypeSpot;
		data.Position = light.Position;
		data.Direction = glm::normalize(light.Direction);
		data.Range = std::max(light.Range, 1e-3f);
		data.Radiance = light.Color * light.Intensity;
		float const outer = glm::radians(std::clamp(light.OuterConeAngle, 0.1f, 89.9f));
		float const inner = glm::radians(std::clamp(light.InnerConeAngle, 0.0f, glm::degrees(outer)));
		data.SpotCosInner = std::cos(inner);
		data.SpotCosOuter = std::cos(outer);
		if (m_LightCount < ShaderInterop::MaxLights)
		{
			m_LightData.insert(m_LightData.end(), reinterpret_cast<uint8_t const*>(&data),
			                   reinterpret_cast<uint8_t const*>(&data) + sizeof(data));
			m_LightCount++;
		}
	}

	void SceneRenderer::SetAmbientLight(glm::vec3 const& radiance)
	{
		m_AmbientRadiance = radiance;
	}

	void SceneRenderer::SetEnvironment(EnvironmentSubmission const& environment)
	{
		m_Environment = environment;
	}

	void SceneRenderer::PrepareItems(std::vector<DrawItem>& items, bool blend)
	{
		for (DrawItem& item : items)
		{
			item.GpuData = Renderer::GetMesh(item.Mesh);
			item.MaterialBindings = Renderer::GetMaterialBindingSet(item.Material);
			item.Pipeline = GetMeshPipeline(blend, item.Material->GetData().DoubleSided);
		}
		std::erase_if(items,
		              [](DrawItem const& item)
		              {
						  return item.GpuData == nullptr || item.MaterialBindings == nullptr || item.Pipeline == nullptr;
					  });
	}

	void SceneRenderer::DrawItems(nvrhi::ICommandList* commandList, std::vector<DrawItem> const& items)
	{
		for (DrawItem const& item : items)
		{
			GpuMesh const* mesh = item.GpuData;
			nvrhi::GraphicsState state;
			state.pipeline = item.Pipeline;
			state.framebuffer = m_SceneFramebuffer;
			state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(m_Width), static_cast<float>(m_Height)));
			state.bindings = {m_FrameBindings, item.MaterialBindings};
			state.vertexBuffers = {nvrhi::VertexBufferBinding().setBuffer(mesh->VertexBuffer).setSlot(0).setOffset(0)};
			state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(mesh->IndexBuffer).setFormat(nvrhi::Format::R32_UINT).setOffset(0);
			commandList->setGraphicsState(state);

			ShaderInterop::DrawConstants constants;
			constants.Model = item.Transform;
			constants.NormalMatrix = glm::transpose(glm::inverse(item.Transform));
			commandList->setPushConstants(&constants, sizeof(constants));

			Submesh const& submesh = item.Mesh->GetSubmeshes()[item.SubmeshIndex];
			nvrhi::DrawArguments arguments;
			arguments.vertexCount = submesh.IndexCount;
			arguments.startIndexLocation = submesh.BaseIndex;
			arguments.startVertexLocation = submesh.BaseVertex;
			commandList->drawIndexed(arguments);

			m_Statistics.DrawCalls++;
			m_Statistics.Triangles += submesh.IndexCount / 3;
		}
	}

	void SceneRenderer::EndScene()
	{
		ST_CORE_ASSERT(m_InScene, "EndScene without BeginScene");
		m_InScene = false;
		if (m_TargetsDirty)
		{
			CreateTargets();
		}
		m_Statistics.Lights = m_LightCount;

		// Front to back for opaque geometry (early depth rejection), back to front for blending.
		std::sort(m_OpaqueItems.begin(), m_OpaqueItems.end(),
		          [](DrawItem const& a, DrawItem const& b)
		          {
					  return a.ViewDepth < b.ViewDepth;
				  });
		std::stable_sort(m_BlendItems.begin(), m_BlendItems.end(),
		                 [](DrawItem const& a, DrawItem const& b)
		                 {
							 return a.ViewDepth > b.ViewDepth;
						 });

		// Uploads of meshes, textures and materials submit their own command lists, so they happen before recording.
		PrepareItems(m_OpaqueItems, false);
		PrepareItems(m_BlendItems, true);

		GpuEnvironment const* environment = m_Environment.Environment ? Renderer::GetEnvironment(m_Environment.Environment) : nullptr;
		if (environment != nullptr)
		{
			UpdateFrameBindings(environment->Irradiance, environment->Prefiltered, environment->Radiance);
		}
		else
		{
			nvrhi::ITexture* fallback = Renderer::GetFallbackCube();
			UpdateFrameBindings(fallback, fallback, fallback);
		}

		float const exposure = m_Settings.GetExposure();
		ShaderInterop::FrameConstants frame{};
		frame.ViewProjection = m_Camera.Projection * m_Camera.View;
		frame.InverseViewProjection = glm::inverse(frame.ViewProjection);
		frame.CameraPosition = m_Camera.Position;
		frame.Exposure = exposure;
		frame.AmbientColor = m_AmbientRadiance;
		frame.LightCount = m_LightCount;
		frame.ViewportSize = glm::vec2(static_cast<float>(m_Width), static_cast<float>(m_Height));
		frame.EnvironmentRotationCos = 1.0f;
		if (environment != nullptr)
		{
			float const rotation = glm::radians(m_Environment.Rotation);
			frame.EnvironmentIntensity = std::max(m_Environment.Intensity, 1e-6f);
			frame.EnvironmentRotationSin = std::sin(rotation);
			frame.EnvironmentRotationCos = std::cos(rotation);
			frame.PrefilteredMaxMip = static_cast<float>(environment->PrefilteredMipCount - 1);
			frame.SkyboxLod = std::clamp(m_Environment.SkyboxBlur, 0.0f, 1.0f) * static_cast<float>(environment->RadianceMipCount - 1);
		}

		nvrhi::ICommandList* commandList = m_CommandList;
		commandList->open();
		commandList->writeBuffer(m_FrameConstants, &frame, sizeof(frame));
		if (!m_LightData.empty())
		{
			commandList->writeBuffer(m_LightBuffer, m_LightData.data(), m_LightData.size());
		}

		// The background is the ambient light until the sky pass exists.
		glm::vec3 const background = m_AmbientRadiance * exposure;
		commandList->clearTextureFloat(m_ColorTarget, nvrhi::AllSubresources, nvrhi::Color(background.r, background.g, background.b, 1.0f));
		commandList->clearDepthStencilTexture(m_DepthTarget, nvrhi::AllSubresources, true, 0.0f, false, 0);

		DrawItems(commandList, m_OpaqueItems);
		if (environment != nullptr && m_Environment.DrawSkybox)
		{
			nvrhi::GraphicsState skyState;
			skyState.pipeline = m_SkyPipeline;
			skyState.framebuffer = m_SceneFramebuffer;
			skyState.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(m_Width), static_cast<float>(m_Height)));
			skyState.bindings = {m_FrameBindings};
			commandList->setGraphicsState(skyState);
			// The frame layout declares the per-draw push constants; the sky does not read them.
			ShaderInterop::DrawConstants const unused{};
			commandList->setPushConstants(&unused, sizeof(unused));
			commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
		}
		DrawItems(commandList, m_BlendItems);

		nvrhi::GraphicsState tonemapState;
		tonemapState.pipeline = m_TonemapPipeline;
		tonemapState.framebuffer = m_FinalFramebuffer;
		tonemapState.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(m_Width), static_cast<float>(m_Height)));
		tonemapState.bindings = {m_TonemapBindings};
		commandList->setGraphicsState(tonemapState);
		ShaderInterop::TonemapConstants tonemap{};
		tonemap.Operator = static_cast<uint32_t>(m_Settings.Tonemapper);
		tonemap.Dither = m_Settings.Dithering ? 1u : 0u;
		commandList->setPushConstants(&tonemap, sizeof(tonemap));
		commandList->draw(nvrhi::DrawArguments().setVertexCount(3));

		commandList->close();
		GraphicsDevice::GetDevice()->executeCommandList(commandList);

		// Draw items keep assets alive only for this frame.
		m_OpaqueItems.clear();
		m_BlendItems.clear();
		Renderer::CollectGarbage();
	}
}
