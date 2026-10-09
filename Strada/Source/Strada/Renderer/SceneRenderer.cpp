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
#include "Strada/Renderer/ShadowMath.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace Strada
{
	static_assert(sizeof(ShaderInterop::FrameConstants) == 192);
	static_assert(sizeof(ShaderInterop::LightData) == 64);
	static_assert(sizeof(ShaderInterop::ShadowConstants) == 1904);
	static_assert(sizeof(ShaderInterop::DrawConstants) == 128, "Push constants must fit the 128 bytes Vulkan guarantees");
	static_assert(sizeof(ShaderInterop::ShadowDrawConstants) == 128, "Push constants must fit the 128 bytes Vulkan guarantees");
	static_assert(sizeof(ShaderInterop::TonemapConstants) == 16);
	static_assert(sizeof(ShaderInterop::AmbientOcclusionConstants) == 176);
	static_assert(sizeof(ShaderInterop::BloomConstants) == 32);
	static_assert(sizeof(ShaderInterop::FxaaConstants) == 16);
	static_assert(ShaderInterop::MaxShadowCascades == Shadows::MaxCascades);

	namespace
	{
		constexpr nvrhi::Format ColorFormat = nvrhi::Format::RGBA16_FLOAT;
		constexpr nvrhi::Format DepthFormat = nvrhi::Format::D32;
		constexpr nvrhi::Format FinalFormat = nvrhi::Format::RGBA8_UNORM;
		constexpr nvrhi::Format ShadowFormat = nvrhi::Format::D32;
		constexpr nvrhi::Format NormalFormat = nvrhi::Format::RG16_FLOAT;
		// R32 is the single-channel format every Vulkan device must support for storage images.
		constexpr nvrhi::Format OcclusionFormat = nvrhi::Format::R32_FLOAT;
		constexpr nvrhi::Format BloomFormat = nvrhi::Format::RGBA16_FLOAT;
		constexpr uint32_t ComputeGroupSize = 8;
		constexpr uint32_t MaxBloomMips = 6;
		constexpr uint32_t OcclusionSlices = 3;
		constexpr uint32_t OcclusionSteps = 6;

		uint32_t ComputeGroups(uint32_t size)
		{
			return (size + ComputeGroupSize - 1) / ComputeGroupSize;
		}

		nvrhi::BindingLayoutHandle CreateComputeLayout(nvrhi::IDevice* device, std::vector<nvrhi::BindingLayoutItem> const& items)
		{
			nvrhi::BindingLayoutDesc desc;
			desc.visibility = nvrhi::ShaderType::Compute;
			desc.bindings = items;
			return device->createBindingLayout(desc);
		}

		nvrhi::ComputePipelineHandle CreateComputePipeline(nvrhi::IDevice* device, char const* shader, nvrhi::IBindingLayout* layout)
		{
			nvrhi::ComputePipelineDesc desc;
			desc.CS = ShaderLibrary::Get(shader);
			desc.bindingLayouts = {layout};
			return desc.CS ? device->createComputePipeline(desc) : nullptr;
		}
		// Texels kept free around each point light face for the shadow filter (see ComputePointLightViewProjections).
		constexpr uint32_t PointShadowGuardTexels = 3;

		uint32_t PipelineIndex(bool blend, bool doubleSided)
		{
			return (blend ? 2u : 0u) + (doubleSided ? 1u : 0u);
		}

		Ref<MaterialAsset> GetDefaultMaterial()
		{
			return AssetManager::GetAsset<MaterialAsset>(GetBuiltInHandle(BuiltInAsset::DefaultMaterial));
		}

		uint32_t ShadowMapSize(uint32_t requested)
		{
			return std::bit_floor(std::clamp(requested, 256u, 8192u));
		}

		// Whether a world-space box can cast into an orthographic shadow view: its projected footprint overlaps the map and
		// it is not entirely behind the farthest receiver. Casters towards the light are always inside (FitCascade).
		bool IntersectsOrthographicView(AABB const& bounds, glm::mat4 const& viewProjection)
		{
			glm::vec3 minimum(std::numeric_limits<float>::max());
			glm::vec3 maximum(std::numeric_limits<float>::lowest());
			for (uint32_t i = 0; i < 8; i++)
			{
				glm::vec3 const corner((i & 1) ? bounds.Max.x : bounds.Min.x, (i & 2) ? bounds.Max.y : bounds.Min.y,
				                       (i & 4) ? bounds.Max.z : bounds.Min.z);
				glm::vec3 const clip = glm::vec3(viewProjection * glm::vec4(corner, 1.0f));
				minimum = glm::min(minimum, clip);
				maximum = glm::max(maximum, clip);
			}
			return maximum.x >= -1.0f && minimum.x <= 1.0f && maximum.y >= -1.0f && minimum.y <= 1.0f && maximum.z >= 0.0f;
		}

		bool IntersectsSphere(AABB const& bounds, glm::vec3 const& center, float radius)
		{
			glm::vec3 const closest = glm::clamp(center, bounds.Min, bounds.Max);
			glm::vec3 const offset = closest - center;
			return glm::dot(offset, offset) <= radius * radius;
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

		nvrhi::BufferDesc shadowDesc = frameDesc;
		shadowDesc.byteSize = sizeof(ShaderInterop::ShadowConstants);
		shadowDesc.debugName = "Shadow constants";
		m_ShadowConstants = device->createBuffer(shadowDesc);

		nvrhi::BufferDesc occlusionDesc = frameDesc;
		occlusionDesc.byteSize = sizeof(ShaderInterop::AmbientOcclusionConstants);
		occlusionDesc.debugName = "Ambient occlusion constants";
		m_OcclusionConstants = device->createBuffer(occlusionDesc);

		nvrhi::BufferDesc lightDesc;
		lightDesc.byteSize = sizeof(ShaderInterop::LightData) * ShaderInterop::MaxLights;
		lightDesc.structStride = sizeof(ShaderInterop::LightData);
		lightDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		lightDesc.keepInitialState = true;
		lightDesc.debugName = "Lights";
		m_LightBuffer = device->createBuffer(lightDesc);

		nvrhi::SamplerDesc compareDesc;
		compareDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		compareDesc.setReductionType(nvrhi::SamplerReductionType::Comparison);
		m_ShadowCompareSampler = device->createSampler(compareDesc);
		nvrhi::SamplerDesc pointDesc;
		pointDesc.setAllFilters(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		m_ShadowPointSampler = device->createSampler(pointDesc);

		// Bound when a view has no shadow map; depth 0 is the far plane, so nothing is occluded.
		nvrhi::TextureDesc fallbackDesc;
		fallbackDesc.dimension = nvrhi::TextureDimension::Texture2DArray;
		fallbackDesc.width = 1;
		fallbackDesc.height = 1;
		fallbackDesc.arraySize = 1;
		fallbackDesc.format = ShadowFormat;
		fallbackDesc.isRenderTarget = true;
		fallbackDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		fallbackDesc.keepInitialState = true;
		fallbackDesc.setClearValue(nvrhi::Color(0.0f));
		fallbackDesc.debugName = "Fallback shadow map";
		m_FallbackShadowMap = device->createTexture(fallbackDesc);
		if (m_FallbackShadowMap)
		{
			m_CommandList->open();
			m_CommandList->clearDepthStencilTexture(m_FallbackShadowMap, nvrhi::AllSubresources, true, 0.0f, false, 0);
			m_CommandList->close();
			device->executeCommandList(m_CommandList);
		}

		nvrhi::BindingLayoutDesc frameLayout;
		frameLayout.visibility = nvrhi::ShaderType::All;
		frameLayout.registerSpace = 0;
		frameLayout.registerSpaceIsDescriptorSet = true;
		frameLayout.bindings = {
			nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
			nvrhi::BindingLayoutItem::VolatileConstantBuffer(2),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),
			nvrhi::BindingLayoutItem::Texture_SRV(1),
			nvrhi::BindingLayoutItem::Texture_SRV(2),
			nvrhi::BindingLayoutItem::Texture_SRV(3),
			nvrhi::BindingLayoutItem::Texture_SRV(4),
			nvrhi::BindingLayoutItem::Texture_SRV(5),
			nvrhi::BindingLayoutItem::Texture_SRV(6),
			nvrhi::BindingLayoutItem::Sampler(0),
			nvrhi::BindingLayoutItem::Sampler(1),
			nvrhi::BindingLayoutItem::Sampler(2),
			nvrhi::BindingLayoutItem::Sampler(3),
			nvrhi::BindingLayoutItem::PushConstants(1, sizeof(ShaderInterop::DrawConstants)),
		};
		m_FrameLayout = device->createBindingLayout(frameLayout);
		nvrhi::ITexture* fallbackCube = Renderer::GetFallbackCube();
		UpdateFrameBindings({fallbackCube, fallbackCube, fallbackCube, m_FallbackShadowMap, m_FallbackShadowMap});

		nvrhi::BindingLayoutDesc shadowLayout;
		shadowLayout.visibility = nvrhi::ShaderType::All;
		shadowLayout.registerSpace = 0;
		shadowLayout.registerSpaceIsDescriptorSet = true;
		shadowLayout.bindings = {
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(ShaderInterop::ShadowDrawConstants)),
			nvrhi::BindingLayoutItem::Sampler(0),
		};
		m_ShadowLayout = device->createBindingLayout(shadowLayout);
		nvrhi::BindingSetDesc shadowBindings;
		shadowBindings.bindings = {
			nvrhi::BindingSetItem::PushConstants(0, sizeof(ShaderInterop::ShadowDrawConstants)),
			nvrhi::BindingSetItem::Sampler(0, Renderer::GetMaterialSampler()),
		};
		m_ShadowBindings = device->createBindingSet(shadowBindings, m_ShadowLayout);

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
			nvrhi::BindingLayoutItem::Texture_SRV(1),
			nvrhi::BindingLayoutItem::Sampler(0),
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(ShaderInterop::TonemapConstants)),
		};
		m_TonemapLayout = device->createBindingLayout(tonemapLayout);

		nvrhi::BindingLayoutDesc fxaaLayout;
		fxaaLayout.visibility = nvrhi::ShaderType::All;
		fxaaLayout.bindings = {
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Sampler(0),
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(ShaderInterop::FxaaConstants)),
		};
		m_FxaaLayout = device->createBindingLayout(fxaaLayout);

		m_OcclusionLayout = CreateComputeLayout(device, {
															nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
															nvrhi::BindingLayoutItem::Texture_SRV(0),
															nvrhi::BindingLayoutItem::Texture_SRV(1),
															nvrhi::BindingLayoutItem::Texture_UAV(0),
														});
		m_DenoiseLayout = CreateComputeLayout(device, {
														  nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
														  nvrhi::BindingLayoutItem::Texture_SRV(0),
														  nvrhi::BindingLayoutItem::Texture_SRV(2),
														  nvrhi::BindingLayoutItem::Texture_UAV(0),
													  });
		m_CompositeLayout = CreateComputeLayout(device, {
															nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
															nvrhi::BindingLayoutItem::Texture_SRV(3),
															nvrhi::BindingLayoutItem::Texture_SRV(4),
															nvrhi::BindingLayoutItem::Texture_UAV(1),
														});
		m_BloomLayout = CreateComputeLayout(device, {
														nvrhi::BindingLayoutItem::PushConstants(0, sizeof(ShaderInterop::BloomConstants)),
														nvrhi::BindingLayoutItem::Texture_SRV(0),
														nvrhi::BindingLayoutItem::Sampler(0),
														nvrhi::BindingLayoutItem::Texture_UAV(0),
													});
		m_OcclusionPipeline = CreateComputePipeline(device, "AmbientOcclusion_CS", m_OcclusionLayout);
		m_DenoisePipeline = CreateComputePipeline(device, "AmbientOcclusionDenoise_CS", m_DenoiseLayout);
		m_CompositePipeline = CreateComputePipeline(device, "AmbientOcclusionComposite_CS", m_CompositeLayout);
		m_BloomDownsamplePipeline = CreateComputePipeline(device, "BloomDownsample_CS", m_BloomLayout);
		m_BloomUpsamplePipeline = CreateComputePipeline(device, "BloomUpsample_CS", m_BloomLayout);

		ST_CORE_ASSERT(m_CommandList && m_FrameConstants && m_ShadowConstants && m_OcclusionConstants && m_LightBuffer &&
		                   m_ShadowCompareSampler && m_ShadowPointSampler && m_FallbackShadowMap && m_FrameLayout && m_FrameBindings &&
		                   m_ShadowLayout && m_ShadowBindings && m_InputLayout && m_TonemapLayout && m_FxaaLayout && m_OcclusionPipeline &&
		                   m_DenoisePipeline && m_CompositePipeline && m_BloomDownsamplePipeline && m_BloomUpsamplePipeline,
		               "Failed to create scene renderer resources");
	}

	SceneRenderer::~SceneRenderer() = default;

	void SceneRenderer::UpdateFrameBindings(FrameTextures const& textures)
	{
		if (m_FrameBindings && m_BoundTextures == textures)
		{
			return;
		}
		nvrhi::TextureDimension const cube = nvrhi::TextureDimension::TextureCube;
		nvrhi::TextureDimension const array = nvrhi::TextureDimension::Texture2DArray;
		nvrhi::BindingSetDesc frameBindings;
		frameBindings.bindings = {
			nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstants),
			nvrhi::BindingSetItem::ConstantBuffer(2, m_ShadowConstants),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_LightBuffer),
			nvrhi::BindingSetItem::Texture_SRV(1, textures.Irradiance, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, cube),
			nvrhi::BindingSetItem::Texture_SRV(2, textures.Prefiltered, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, cube),
			nvrhi::BindingSetItem::Texture_SRV(3, Renderer::GetBrdfLut()),
			nvrhi::BindingSetItem::Texture_SRV(4, textures.Radiance, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, cube),
			nvrhi::BindingSetItem::Texture_SRV(5, textures.CascadeShadowMap, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, array),
			nvrhi::BindingSetItem::Texture_SRV(6, textures.LocalShadowMap, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, array),
			nvrhi::BindingSetItem::Sampler(0, Renderer::GetMaterialSampler()),
			nvrhi::BindingSetItem::Sampler(1, Renderer::GetLinearClampSampler()),
			nvrhi::BindingSetItem::Sampler(2, m_ShadowCompareSampler),
			nvrhi::BindingSetItem::Sampler(3, m_ShadowPointSampler),
			nvrhi::BindingSetItem::PushConstants(1, sizeof(ShaderInterop::DrawConstants)),
		};
		m_FrameBindings = GraphicsDevice::GetDevice()->createBindingSet(frameBindings, m_FrameLayout);
		m_BoundTextures = textures;
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
		// The ambient occlusion composite updates it in place.
		colorDesc.isUAV = true;
		colorDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		colorDesc.keepInitialState = true;
		colorDesc.setClearValue(nvrhi::Color(0.0f));
		colorDesc.debugName = "Scene color";
		m_ColorTarget = device->createTexture(colorDesc);

		nvrhi::TextureDesc depthDesc = colorDesc;
		depthDesc.format = DepthFormat;
		// Depth formats need not support storage images (Mesa lavapipe does not): depth is only rendered and sampled.
		depthDesc.isUAV = false;
		depthDesc.initialState = nvrhi::ResourceStates::DepthWrite;
		// Reversed Z: the far plane is 0.
		depthDesc.setClearValue(nvrhi::Color(0.0f));
		depthDesc.debugName = "Scene depth";
		m_DepthTarget = device->createTexture(depthDesc);

		nvrhi::TextureDesc normalDesc = colorDesc;
		normalDesc.format = NormalFormat;
		normalDesc.isUAV = false;
		normalDesc.debugName = "Scene normals";
		m_NormalTarget = device->createTexture(normalDesc);

		nvrhi::TextureDesc indirectDesc = normalDesc;
		indirectDesc.format = ColorFormat;
		indirectDesc.debugName = "Scene indirect light";
		m_IndirectTarget = device->createTexture(indirectDesc);

		nvrhi::TextureDesc occlusionDesc;
		occlusionDesc.width = m_Width;
		occlusionDesc.height = m_Height;
		occlusionDesc.format = OcclusionFormat;
		occlusionDesc.isUAV = true;
		occlusionDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		occlusionDesc.keepInitialState = true;
		occlusionDesc.debugName = "Raw ambient occlusion";
		m_RawOcclusion = device->createTexture(occlusionDesc);
		occlusionDesc.debugName = "Ambient occlusion";
		m_Occlusion = device->createTexture(occlusionDesc);

		// Half resolution, halving down to at least 2 texels per side.
		uint32_t const bloomWidth = std::max(m_Width / 2, 1u);
		uint32_t const bloomHeight = std::max(m_Height / 2, 1u);
		m_BloomMipCount = 1;
		while (m_BloomMipCount < MaxBloomMips && std::min(bloomWidth, bloomHeight) >> m_BloomMipCount >= 2)
		{
			m_BloomMipCount++;
		}
		nvrhi::TextureDesc bloomDesc;
		bloomDesc.width = bloomWidth;
		bloomDesc.height = bloomHeight;
		bloomDesc.mipLevels = m_BloomMipCount;
		bloomDesc.format = BloomFormat;
		bloomDesc.isUAV = true;
		bloomDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		bloomDesc.keepInitialState = true;
		bloomDesc.debugName = "Bloom";
		m_BloomTexture = device->createTexture(bloomDesc);

		nvrhi::TextureDesc finalDesc = colorDesc;
		finalDesc.format = FinalFormat;
		finalDesc.isUAV = false;
		finalDesc.debugName = "Final image";
		m_FinalImage = device->createTexture(finalDesc);
		finalDesc.debugName = "Tonemapped image";
		m_LdrTarget = device->createTexture(finalDesc);

		m_OpaqueFramebuffer = device->createFramebuffer(nvrhi::FramebufferDesc()
		                                                    .addColorAttachment(m_ColorTarget)
		                                                    .addColorAttachment(m_NormalTarget)
		                                                    .addColorAttachment(m_IndirectTarget)
		                                                    .setDepthAttachment(m_DepthTarget));
		m_SceneFramebuffer =
			device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_ColorTarget).setDepthAttachment(m_DepthTarget));
		m_LdrFramebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_LdrTarget));
		m_FinalFramebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_FinalImage));

		nvrhi::ISampler* linearClamp = Renderer::GetLinearClampSampler();
		nvrhi::BindingSetDesc tonemapBindings;
		tonemapBindings.bindings = {
			nvrhi::BindingSetItem::Texture_SRV(0, m_ColorTarget),
			nvrhi::BindingSetItem::Texture_SRV(1, m_BloomTexture, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 1)),
			nvrhi::BindingSetItem::Sampler(0, linearClamp),
			nvrhi::BindingSetItem::PushConstants(0, sizeof(ShaderInterop::TonemapConstants)),
		};
		m_TonemapBindings = device->createBindingSet(tonemapBindings, m_TonemapLayout);

		nvrhi::BindingSetDesc fxaaBindings;
		fxaaBindings.bindings = {
			nvrhi::BindingSetItem::Texture_SRV(0, m_LdrTarget),
			nvrhi::BindingSetItem::Sampler(0, linearClamp),
			nvrhi::BindingSetItem::PushConstants(0, sizeof(ShaderInterop::FxaaConstants)),
		};
		m_FxaaBindings = device->createBindingSet(fxaaBindings, m_FxaaLayout);

		m_OcclusionBindings = device->createBindingSet(nvrhi::BindingSetDesc()
		                                                   .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, m_OcclusionConstants))
		                                                   .addItem(nvrhi::BindingSetItem::Texture_SRV(0, m_DepthTarget))
		                                                   .addItem(nvrhi::BindingSetItem::Texture_SRV(1, m_NormalTarget))
		                                                   .addItem(nvrhi::BindingSetItem::Texture_UAV(0, m_RawOcclusion)),
		                                               m_OcclusionLayout);
		m_DenoiseBindings = device->createBindingSet(nvrhi::BindingSetDesc()
		                                                 .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, m_OcclusionConstants))
		                                                 .addItem(nvrhi::BindingSetItem::Texture_SRV(0, m_DepthTarget))
		                                                 .addItem(nvrhi::BindingSetItem::Texture_SRV(2, m_RawOcclusion))
		                                                 .addItem(nvrhi::BindingSetItem::Texture_UAV(0, m_Occlusion)),
		                                             m_DenoiseLayout);
		m_CompositeBindings = device->createBindingSet(nvrhi::BindingSetDesc()
		                                                   .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, m_OcclusionConstants))
		                                                   .addItem(nvrhi::BindingSetItem::Texture_SRV(3, m_IndirectTarget))
		                                                   .addItem(nvrhi::BindingSetItem::Texture_SRV(4, m_Occlusion))
		                                                   .addItem(nvrhi::BindingSetItem::Texture_UAV(1, m_ColorTarget)),
		                                               m_CompositeLayout);

		m_BloomDownsampleBindings.clear();
		m_BloomUpsampleBindings.clear();
		for (uint32_t mip = 0; mip < m_BloomMipCount; mip++)
		{
			nvrhi::TextureSubresourceSet const target(mip, 1, 0, 1);
			nvrhi::BindingSetItem const source = mip == 0
			                                         ? nvrhi::BindingSetItem::Texture_SRV(0, m_ColorTarget)
			                                         : nvrhi::BindingSetItem::Texture_SRV(0, m_BloomTexture, nvrhi::Format::UNKNOWN,
			                                                                              nvrhi::TextureSubresourceSet(mip - 1, 1, 0, 1));
			m_BloomDownsampleBindings.push_back(device->createBindingSet(
				nvrhi::BindingSetDesc()
					.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(ShaderInterop::BloomConstants)))
					.addItem(source)
					.addItem(nvrhi::BindingSetItem::Sampler(0, linearClamp))
					.addItem(nvrhi::BindingSetItem::Texture_UAV(0, m_BloomTexture, nvrhi::Format::UNKNOWN, target)),
				m_BloomLayout));
			if (mip + 1 < m_BloomMipCount)
			{
				m_BloomUpsampleBindings.push_back(device->createBindingSet(
					nvrhi::BindingSetDesc()
						.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(ShaderInterop::BloomConstants)))
						.addItem(nvrhi::BindingSetItem::Texture_SRV(0, m_BloomTexture, nvrhi::Format::UNKNOWN,
				                                                    nvrhi::TextureSubresourceSet(mip + 1, 1, 0, 1)))
						.addItem(nvrhi::BindingSetItem::Sampler(0, linearClamp))
						.addItem(nvrhi::BindingSetItem::Texture_UAV(0, m_BloomTexture, nvrhi::Format::UNKNOWN, target)),
					m_BloomLayout));
			}
		}

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
		if (!m_FxaaPipeline)
		{
			nvrhi::GraphicsPipelineDesc desc;
			desc.VS = ShaderLibrary::Get("Fullscreen_VS");
			desc.PS = ShaderLibrary::Get("Fxaa_PS");
			desc.bindingLayouts = {m_FxaaLayout};
			desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
			desc.renderState.depthStencilState.depthTestEnable = false;
			desc.renderState.depthStencilState.depthWriteEnable = false;
			m_FxaaPipeline = device->createGraphicsPipeline(desc, m_FinalFramebuffer->getFramebufferInfo());
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
		// Blended surfaces are drawn after ambient occlusion into the color target only.
		desc.PS = ShaderLibrary::Get(blend ? "ForwardPBR_BlendPS" : "ForwardPBR_PS");
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
		nvrhi::IFramebuffer* framebuffer = blend ? m_SceneFramebuffer : m_OpaqueFramebuffer;
		pipeline = GraphicsDevice::GetDevice()->createGraphicsPipeline(desc, framebuffer->getFramebufferInfo());
		if (!pipeline)
		{
			ST_CORE_ERROR("Failed to create a mesh pipeline");
		}
		return pipeline;
	}

	nvrhi::IGraphicsPipeline* SceneRenderer::GetShadowPipeline(bool masked, bool doubleSided)
	{
		nvrhi::GraphicsPipelineHandle& pipeline = m_ShadowPipelines[(masked ? 2u : 0u) + (doubleSided ? 1u : 0u)];
		if (pipeline)
		{
			return pipeline;
		}

		nvrhi::GraphicsPipelineDesc desc;
		desc.inputLayout = m_InputLayout;
		desc.VS = ShaderLibrary::Get("Shadow_VS");
		// Opaque casters only write depth.
		desc.PS = masked ? ShaderLibrary::Get("Shadow_PS") : nullptr;
		desc.bindingLayouts = {m_ShadowLayout, Renderer::GetMaterialBindingLayout()};
		desc.renderState.rasterState.cullMode = doubleSided ? nvrhi::RasterCullMode::None : nvrhi::RasterCullMode::Back;
		desc.renderState.rasterState.frontCounterClockwise = true;
		// Reversed Z: a negative slope-scaled bias moves stored depths away from the light, against self-shadowing on
		// surfaces at grazing angles (receivers also apply a normal offset).
		desc.renderState.rasterState.slopeScaledDepthBias = -1.5f;
		desc.renderState.depthStencilState.depthTestEnable = true;
		desc.renderState.depthStencilState.depthWriteEnable = true;
		desc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::GreaterOrEqual;

		nvrhi::FramebufferInfo info;
		info.depthFormat = ShadowFormat;
		pipeline = GraphicsDevice::GetDevice()->createGraphicsPipeline(desc, info);
		if (!pipeline)
		{
			ST_CORE_ERROR("Failed to create a shadow pipeline");
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
		m_Lights.clear();
		m_ShadowedDirectionalLight = -1;
		m_DirectionalLightSize = 0.0f;
		m_LocalShadowLights.clear();
		m_ShadowViews.clear();
		m_Statistics = {};
	}

	void SceneRenderer::SubmitMesh(Ref<MeshSource> const& mesh, std::span<Ref<MaterialAsset> const> materials, glm::mat4 const& transform,
	                               bool castShadows)
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
			item.WorldBounds = submesh.Bounds.Transform(transform);
			item.CastShadows = castShadows;
			(material->GetData().AlphaMode == MaterialAlphaMode::Blend ? m_BlendItems : m_OpaqueItems).push_back(std::move(item));
		}
	}

	void SceneRenderer::SubmitDirectionalLight(DirectionalLightSubmission const& light)
	{
		if (m_Lights.size() >= ShaderInterop::MaxLights)
		{
			return;
		}
		ShaderInterop::LightData data{};
		data.Type = ShaderInterop::LightTypeDirectional;
		data.Direction = glm::normalize(light.Direction);
		data.Radiance = light.Color * light.Intensity;
		data.ShadowIndex = -1;
		if (light.CastShadows && m_ShadowedDirectionalLight < 0)
		{
			m_ShadowedDirectionalLight = static_cast<int32_t>(m_Lights.size());
			m_DirectionalLightSize = std::clamp(light.LightSize, 0.0f, 20.0f);
		}
		m_Lights.push_back(data);
	}

	void SceneRenderer::SubmitPointLight(PointLightSubmission const& light)
	{
		if (m_Lights.size() >= ShaderInterop::MaxLights)
		{
			return;
		}
		ShaderInterop::LightData data{};
		data.Type = ShaderInterop::LightTypePoint;
		data.Position = light.Position;
		data.Range = std::max(light.Range, 1e-3f);
		data.Radiance = light.Color * light.Intensity;
		data.ShadowIndex = -1;
		if (light.CastShadows)
		{
			LocalShadowLight shadow;
			shadow.LightIndex = static_cast<uint32_t>(m_Lights.size());
			shadow.Point = true;
			shadow.Position = data.Position;
			shadow.Range = data.Range;
			m_LocalShadowLights.push_back(shadow);
		}
		m_Lights.push_back(data);
	}

	void SceneRenderer::SubmitSpotLight(SpotLightSubmission const& light)
	{
		if (m_Lights.size() >= ShaderInterop::MaxLights)
		{
			return;
		}
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
		data.ShadowIndex = -1;
		if (light.CastShadows)
		{
			LocalShadowLight shadow;
			shadow.LightIndex = static_cast<uint32_t>(m_Lights.size());
			shadow.Position = data.Position;
			shadow.Direction = data.Direction;
			shadow.Range = data.Range;
			shadow.OuterConeAngle = outer;
			m_LocalShadowLights.push_back(shadow);
		}
		m_Lights.push_back(data);
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
			MaterialData const& material = item.Material->GetData();
			item.GpuData = Renderer::GetMesh(item.Mesh);
			item.MaterialBindings = Renderer::GetMaterialBindingSet(item.Material);
			item.Pipeline = GetMeshPipeline(blend, material.DoubleSided);
			// Blended surfaces do not cast shadows.
			item.CastShadows = item.CastShadows && !blend;
			if (item.CastShadows)
			{
				item.ShadowPipeline = GetShadowPipeline(material.AlphaMode == MaterialAlphaMode::Mask, material.DoubleSided);
				item.CastShadows = item.ShadowPipeline != nullptr;
			}
		}
		std::erase_if(items,
		              [](DrawItem const& item)
		              {
						  return item.GpuData == nullptr || item.MaterialBindings == nullptr || item.Pipeline == nullptr;
					  });
	}

	void SceneRenderer::EnsureShadowMap(nvrhi::TextureHandle& texture, std::vector<nvrhi::FramebufferHandle>& framebuffers, uint32_t size,
	                                    uint32_t slices, char const* name)
	{
		if (texture && texture->getDesc().width == size && texture->getDesc().arraySize == slices)
		{
			return;
		}
		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		nvrhi::TextureDesc desc;
		desc.dimension = nvrhi::TextureDimension::Texture2DArray;
		desc.width = size;
		desc.height = size;
		desc.arraySize = slices;
		desc.format = ShadowFormat;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.setClearValue(nvrhi::Color(0.0f));
		desc.debugName = name;
		texture = device->createTexture(desc);
		framebuffers.clear();
		if (!texture)
		{
			ST_CORE_ERROR("Failed to create the {} ({}x{}, {} slices)", name, size, size, slices);
			return;
		}
		for (uint32_t slice = 0; slice < slices; slice++)
		{
			framebuffers.push_back(device->createFramebuffer(
				nvrhi::FramebufferDesc().setDepthAttachment(nvrhi::FramebufferAttachment().setTexture(texture).setArraySlice(slice))));
		}
	}

	void SceneRenderer::ReleaseShadowMap(nvrhi::TextureHandle& texture, std::vector<nvrhi::FramebufferHandle>& framebuffers)
	{
		// In-flight command lists keep the texture alive through the framebuffers and binding sets they used.
		framebuffers.clear();
		texture = nullptr;
	}

	void SceneRenderer::PrepareShadows(ShaderInterop::ShadowConstants& constants)
	{
		constants.CameraForward = -glm::normalize(glm::vec3(glm::inverse(m_Camera.View)[2]));
		if (!m_Settings.Shadows)
		{
			m_ShadowedDirectionalLight = -1;
			m_LocalShadowLights.clear();
		}

		// Directional light: cascades over the view up to the shadow distance.
		uint32_t const cascadeCount = std::clamp(m_Settings.CascadeCount, 1u, Shadows::MaxCascades);
		float const shadowDistance = std::min(std::max(m_Settings.ShadowDistance, 0.1f), std::max(m_Camera.MaxDistance, 0.1f));
		AABB casterBounds;
		for (DrawItem const& item : m_OpaqueItems)
		{
			if (item.CastShadows)
			{
				casterBounds.Expand(item.WorldBounds);
			}
		}
		if (m_ShadowedDirectionalLight >= 0)
		{
			uint32_t const mapSize = ShadowMapSize(m_Settings.ShadowMapSize);
			EnsureShadowMap(m_CascadeShadowMap, m_CascadeFramebuffers, mapSize, cascadeCount, "Cascade shadow map");
		}
		if (m_ShadowedDirectionalLight >= 0 && m_CascadeShadowMap)
		{
			ShaderInterop::LightData& light = m_Lights[static_cast<size_t>(m_ShadowedDirectionalLight)];
			light.ShadowIndex = 0;

			// The near plane of the view, from the projection: the depth-1 point on the view axis.
			glm::vec4 const nearPoint = glm::inverse(m_Camera.Projection) * glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
			float const nearDistance = std::max(-nearPoint.z / nearPoint.w, 0.0f);
			std::array<float, Shadows::MaxCascades> const splits =
				Shadows::ComputeCascadeSplits(nearDistance, shadowDistance, cascadeCount, m_Settings.CascadeSplitLambda);

			float const mapSize = static_cast<float>(m_CascadeShadowMap->getDesc().width);
			float sliceStart = nearDistance;
			for (uint32_t cascade = 0; cascade < cascadeCount; cascade++)
			{
				std::array<glm::vec3, 8> const corners =
					Shadows::ComputeFrustumSliceCorners(m_Camera.View, m_Camera.Projection, sliceStart, splits[cascade]);
				Shadows::CascadeProjection const projection =
					Shadows::FitCascade(corners, light.Direction, casterBounds, static_cast<uint32_t>(mapSize));
				sliceStart = splits[cascade];

				constants.CascadeViewProjection[cascade] = projection.ViewProjection;
				constants.CascadeSplits[static_cast<int>(cascade)] = splits[cascade];
				constants.CascadeTexelSizes[static_cast<int>(cascade)] = projection.TexelSize;
				constants.CascadeWidths[static_cast<int>(cascade)] = projection.Width;
				constants.CascadeDepthRanges[static_cast<int>(cascade)] = projection.DepthRange;

				ShadowView view;
				view.ViewProjection = projection.ViewProjection;
				view.Framebuffer = m_CascadeFramebuffers[cascade];
				view.Directional = true;
				m_ShadowViews.push_back(view);
			}
			constants.CascadeCount = cascadeCount;
			constants.CascadeMapSize = mapSize;
			constants.LightTanHalfAngle = std::tan(glm::radians(m_DirectionalLightSize) * 0.5f);
			constants.SoftShadows = m_Settings.SoftShadows ? 1u : 0u;
			constants.ShadowDistance = shadowDistance;
		}
		else
		{
			ReleaseShadowMap(m_CascadeShadowMap, m_CascadeFramebuffers);
		}

		// Local lights: one slice per spot light, six per point light, in submission order while slices remain.
		uint32_t slicesNeeded = 0;
		for (LocalShadowLight const& shadow : m_LocalShadowLights)
		{
			uint32_t const slices = shadow.Point ? 6u : 1u;
			if (slicesNeeded + slices <= ShaderInterop::MaxLocalShadowSlices)
			{
				slicesNeeded += slices;
			}
		}
		if (slicesNeeded == 0)
		{
			ReleaseShadowMap(m_LocalShadowMap, m_LocalFramebuffers);
			return;
		}
		// Grow in steps of six slices so scenes that toggle a light do not reallocate every frame.
		uint32_t const capacity = std::min((slicesNeeded + 5u) / 6u * 6u, ShaderInterop::MaxLocalShadowSlices);
		uint32_t const localSize = ShadowMapSize(m_Settings.LocalShadowMapSize);
		uint32_t const currentSlices = m_LocalShadowMap ? m_LocalShadowMap->getDesc().arraySize : 0u;
		bool const sameSize = m_LocalShadowMap && m_LocalShadowMap->getDesc().width == localSize;
		EnsureShadowMap(m_LocalShadowMap, m_LocalFramebuffers, localSize, sameSize ? std::max(capacity, currentSlices) : capacity,
		                "Local shadow map");
		if (!m_LocalShadowMap)
		{
			return;
		}

		uint32_t slice = 0;
		float const mapSize = static_cast<float>(localSize);
		for (LocalShadowLight const& shadow : m_LocalShadowLights)
		{
			uint32_t const slices = shadow.Point ? 6u : 1u;
			if (slice + slices > ShaderInterop::MaxLocalShadowSlices)
			{
				m_Statistics.ShadowsDropped++;
				continue;
			}
			ShaderInterop::LightData& light = m_Lights[shadow.LightIndex];
			light.ShadowIndex = static_cast<int32_t>(slice);
			if (shadow.Point)
			{
				std::array<glm::mat4, 6> const faces =
					Shadows::ComputePointLightViewProjections(shadow.Position, shadow.Range, localSize, PointShadowGuardTexels);
				// A face spans 2 * tan(fov / 2) = 2 * size / (size - 2 * guard) at unit distance.
				light.ShadowTexelScale = 2.0f / (mapSize - 2.0f * static_cast<float>(PointShadowGuardTexels));
				for (uint32_t face = 0; face < 6; face++)
				{
					constants.LocalViewProjection[slice + face] = faces[face];
				}
			}
			else
			{
				constants.LocalViewProjection[slice] =
					Shadows::ComputeSpotLightViewProjection(shadow.Position, shadow.Direction, shadow.OuterConeAngle, shadow.Range);
				float const halfFieldOfView = std::min(shadow.OuterConeAngle + glm::radians(2.0f), glm::radians(89.0f));
				light.ShadowTexelScale = 2.0f * std::tan(halfFieldOfView) / mapSize;
			}
			for (uint32_t i = 0; i < slices; i++)
			{
				ShadowView view;
				view.ViewProjection = constants.LocalViewProjection[slice + i];
				view.Framebuffer = m_LocalFramebuffers[slice + i];
				view.Center = shadow.Position;
				view.Radius = shadow.Range;
				m_ShadowViews.push_back(view);
			}
			slice += slices;
		}
		constants.LocalMapSize = mapSize;
	}

	void SceneRenderer::RenderShadows(nvrhi::ICommandList* commandList)
	{
		for (ShadowView const& view : m_ShadowViews)
		{
			nvrhi::IFramebuffer* framebuffer = view.Framebuffer;
			nvrhi::FramebufferAttachment const& attachment = framebuffer->getDesc().depthAttachment;
			commandList->clearDepthStencilTexture(attachment.texture, attachment.subresources, true, 0.0f, false, 0);
			nvrhi::FramebufferInfoEx const& info = framebuffer->getFramebufferInfo();
			m_Statistics.ShadowMapViews++;

			for (DrawItem const& item : m_OpaqueItems)
			{
				if (!item.CastShadows)
				{
					continue;
				}
				bool const visible = view.Directional ? IntersectsOrthographicView(item.WorldBounds, view.ViewProjection)
				                                      : IntersectsSphere(item.WorldBounds, view.Center, view.Radius);
				if (!visible)
				{
					continue;
				}

				GpuMesh const* mesh = item.GpuData;
				nvrhi::GraphicsState state;
				state.pipeline = item.ShadowPipeline;
				state.framebuffer = framebuffer;
				state.viewport.addViewportAndScissorRect(info.getViewport());
				state.bindings = {m_ShadowBindings, item.MaterialBindings};
				state.vertexBuffers = {nvrhi::VertexBufferBinding().setBuffer(mesh->VertexBuffer).setSlot(0).setOffset(0)};
				state.indexBuffer =
					nvrhi::IndexBufferBinding().setBuffer(mesh->IndexBuffer).setFormat(nvrhi::Format::R32_UINT).setOffset(0);
				commandList->setGraphicsState(state);

				ShaderInterop::ShadowDrawConstants constants;
				constants.Model = item.Transform;
				constants.ViewProjection = view.ViewProjection;
				commandList->setPushConstants(&constants, sizeof(constants));

				Submesh const& submesh = item.Mesh->GetSubmeshes()[item.SubmeshIndex];
				nvrhi::DrawArguments arguments;
				arguments.vertexCount = submesh.IndexCount;
				arguments.startIndexLocation = submesh.BaseIndex;
				arguments.startVertexLocation = submesh.BaseVertex;
				commandList->drawIndexed(arguments);

				m_Statistics.DrawCalls++;
				m_Statistics.ShadowDrawCalls++;
				m_Statistics.Triangles += submesh.IndexCount / 3;
			}
		}
	}

	void SceneRenderer::RenderAmbientOcclusion(nvrhi::ICommandList* commandList)
	{
		float const height = static_cast<float>(m_Height);
		ShaderInterop::AmbientOcclusionConstants constants{};
		constants.InverseProjection = glm::inverse(m_Camera.Projection);
		constants.View = m_Camera.View;
		constants.ViewportSize = glm::vec2(static_cast<float>(m_Width), height);
		constants.InverseViewportSize = 1.0f / constants.ViewportSize;
		constants.Radius = std::clamp(m_Settings.AmbientOcclusionRadius, 0.01f, 10.0f);
		constants.Intensity = std::clamp(m_Settings.AmbientOcclusionIntensity, 0.1f, 8.0f);
		// Projection[1][1] maps view-space height to clip space (1 / tan(fov / 2), or 2 / size for orthographic views).
		constants.ProjectionScale = 0.5f * height * m_Camera.Projection[1][1];
		constants.Orthographic = m_Camera.Projection[3][3] > 0.5f ? 1u : 0u;
		constants.SliceCount = OcclusionSlices;
		constants.StepCount = OcclusionSteps;
		commandList->writeBuffer(m_OcclusionConstants, &constants, sizeof(constants));

		std::pair<nvrhi::IComputePipeline*, nvrhi::IBindingSet*> const passes[] = {
			{m_OcclusionPipeline, m_OcclusionBindings},
			{m_DenoisePipeline, m_DenoiseBindings},
			{m_CompositePipeline, m_CompositeBindings},
		};
		for (auto const& [pipeline, bindings] : passes)
		{
			nvrhi::ComputeState state;
			state.pipeline = pipeline;
			state.bindings = {bindings};
			commandList->setComputeState(state);
			commandList->dispatch(ComputeGroups(m_Width), ComputeGroups(m_Height), 1);
		}
	}

	void SceneRenderer::RenderBloom(nvrhi::ICommandList* commandList)
	{
		uint32_t const width = m_BloomTexture->getDesc().width;
		uint32_t const height = m_BloomTexture->getDesc().height;
		auto const mipSize = [&](uint32_t mip)
		{
			return glm::uvec2(std::max(width >> mip, 1u), std::max(height >> mip, 1u));
		};

		for (uint32_t mip = 0; mip < m_BloomMipCount; mip++)
		{
			glm::uvec2 const source = mip == 0 ? glm::uvec2(m_Width, m_Height) : mipSize(mip - 1);
			ShaderInterop::BloomConstants constants{};
			constants.SourceTexelSize = 1.0f / glm::vec2(source);
			constants.OutputSize = mipSize(mip);
			constants.FirstPass = mip == 0 ? 1u : 0u;
			nvrhi::ComputeState state;
			state.pipeline = m_BloomDownsamplePipeline;
			state.bindings = {m_BloomDownsampleBindings[mip]};
			commandList->setComputeState(state);
			commandList->setPushConstants(&constants, sizeof(constants));
			commandList->dispatch(ComputeGroups(constants.OutputSize.x), ComputeGroups(constants.OutputSize.y), 1);
		}
		for (uint32_t mip = m_BloomMipCount - 1; mip-- > 0;)
		{
			ShaderInterop::BloomConstants constants{};
			constants.SourceTexelSize = 1.0f / glm::vec2(mipSize(mip + 1));
			constants.OutputSize = mipSize(mip);
			nvrhi::ComputeState state;
			state.pipeline = m_BloomUpsamplePipeline;
			state.bindings = {m_BloomUpsampleBindings[mip]};
			commandList->setComputeState(state);
			commandList->setPushConstants(&constants, sizeof(constants));
			commandList->dispatch(ComputeGroups(constants.OutputSize.x), ComputeGroups(constants.OutputSize.y), 1);
		}
	}

	void SceneRenderer::DrawItems(nvrhi::ICommandList* commandList, std::vector<DrawItem> const& items, nvrhi::IFramebuffer* framebuffer)
	{
		for (DrawItem const& item : items)
		{
			GpuMesh const* mesh = item.GpuData;
			nvrhi::GraphicsState state;
			state.pipeline = item.Pipeline;
			state.framebuffer = framebuffer;
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
		m_Statistics.Lights = static_cast<uint32_t>(m_Lights.size());

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

		ShaderInterop::ShadowConstants shadows{};
		PrepareShadows(shadows);

		GpuEnvironment const* environment = m_Environment.Environment ? Renderer::GetEnvironment(m_Environment.Environment) : nullptr;
		FrameTextures textures;
		nvrhi::ITexture* fallbackCube = Renderer::GetFallbackCube();
		textures.Irradiance = environment != nullptr ? environment->Irradiance.Get() : fallbackCube;
		textures.Prefiltered = environment != nullptr ? environment->Prefiltered.Get() : fallbackCube;
		textures.Radiance = environment != nullptr ? environment->Radiance.Get() : fallbackCube;
		textures.CascadeShadowMap = m_CascadeShadowMap ? m_CascadeShadowMap.Get() : m_FallbackShadowMap.Get();
		textures.LocalShadowMap = m_LocalShadowMap ? m_LocalShadowMap.Get() : m_FallbackShadowMap.Get();
		UpdateFrameBindings(textures);

		float const exposure = m_Settings.GetExposure();
		ShaderInterop::FrameConstants frame{};
		frame.ViewProjection = m_Camera.Projection * m_Camera.View;
		frame.InverseViewProjection = glm::inverse(frame.ViewProjection);
		frame.CameraPosition = m_Camera.Position;
		frame.Exposure = exposure;
		frame.AmbientColor = m_AmbientRadiance;
		frame.LightCount = static_cast<uint32_t>(m_Lights.size());
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
		commandList->writeBuffer(m_ShadowConstants, &shadows, sizeof(shadows));
		if (!m_Lights.empty())
		{
			commandList->writeBuffer(m_LightBuffer, m_Lights.data(), m_Lights.size() * sizeof(ShaderInterop::LightData));
		}

		RenderShadows(commandList);

		// Without a sky, the background is the ambient light.
		glm::vec3 const background = m_AmbientRadiance * exposure;
		commandList->clearTextureFloat(m_ColorTarget, nvrhi::AllSubresources, nvrhi::Color(background.r, background.g, background.b, 1.0f));
		commandList->clearDepthStencilTexture(m_DepthTarget, nvrhi::AllSubresources, true, 0.0f, false, 0);
		commandList->clearTextureFloat(m_NormalTarget, nvrhi::AllSubresources, nvrhi::Color(0.0f));
		commandList->clearTextureFloat(m_IndirectTarget, nvrhi::AllSubresources, nvrhi::Color(0.0f));

		DrawItems(commandList, m_OpaqueItems, m_OpaqueFramebuffer);
		if (m_Settings.AmbientOcclusion)
		{
			RenderAmbientOcclusion(commandList);
		}
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
		DrawItems(commandList, m_BlendItems, m_SceneFramebuffer);

		float const bloomIntensity = m_Settings.Bloom ? std::clamp(m_Settings.BloomIntensity, 0.0f, 1.0f) : 0.0f;
		if (bloomIntensity > 0.0f)
		{
			RenderBloom(commandList);
		}

		nvrhi::GraphicsState tonemapState;
		tonemapState.pipeline = m_TonemapPipeline;
		tonemapState.framebuffer = m_Settings.FXAA ? m_LdrFramebuffer : m_FinalFramebuffer;
		tonemapState.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(m_Width), static_cast<float>(m_Height)));
		tonemapState.bindings = {m_TonemapBindings};
		commandList->setGraphicsState(tonemapState);
		ShaderInterop::TonemapConstants tonemap{};
		tonemap.Operator = static_cast<uint32_t>(m_Settings.Tonemapper);
		tonemap.Dither = m_Settings.Dithering ? 1u : 0u;
		tonemap.BloomIntensity = bloomIntensity;
		tonemap.BloomNormalization = 1.0f / static_cast<float>(std::max(m_BloomMipCount, 1u));
		commandList->setPushConstants(&tonemap, sizeof(tonemap));
		commandList->draw(nvrhi::DrawArguments().setVertexCount(3));

		if (m_Settings.FXAA)
		{
			nvrhi::GraphicsState fxaaState;
			fxaaState.pipeline = m_FxaaPipeline;
			fxaaState.framebuffer = m_FinalFramebuffer;
			fxaaState.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(m_Width), static_cast<float>(m_Height)));
			fxaaState.bindings = {m_FxaaBindings};
			commandList->setGraphicsState(fxaaState);
			ShaderInterop::FxaaConstants fxaa{};
			fxaa.InverseSize = 1.0f / glm::vec2(static_cast<float>(m_Width), static_cast<float>(m_Height));
			commandList->setPushConstants(&fxaa, sizeof(fxaa));
			commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
		}

		commandList->close();
		GraphicsDevice::GetDevice()->executeCommandList(commandList);

		// Draw items keep assets alive only for this frame.
		m_OpaqueItems.clear();
		m_BlendItems.clear();
		m_ShadowViews.clear();
		Renderer::CollectGarbage();
	}
}
