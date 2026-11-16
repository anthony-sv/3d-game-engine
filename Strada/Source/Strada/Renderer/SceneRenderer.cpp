#include "stpch.h"
#include "Strada/Renderer/SceneRenderer.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Asset/EnvironmentAsset.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/MeshSource.h"
#include "Strada/Core/Profiler.h"
#include "Strada/Math/Frustum.h"
#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/RHI/ShaderLibrary.h"
#include "Strada/Renderer/EnvironmentMap.h"
#include "Strada/Renderer/InstanceMath.h"
#include "Strada/Renderer/Renderer.h"
#include "Strada/Renderer/ShadowMath.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace Strada
{
	static_assert(sizeof(ShaderInterop::FrameConstants) == 192);
	static_assert(sizeof(ShaderInterop::LightData) == 64);
	static_assert(sizeof(ShaderInterop::ShadowConstants) == 1904);
	static_assert(sizeof(ShaderInterop::DrawConstants) == 16);
	static_assert(sizeof(ShaderInterop::ShadowDrawConstants) == 80, "Push constants must fit the 128 bytes Vulkan guarantees");
	static_assert(sizeof(ShaderInterop::TonemapConstants) == 64);
	static_assert(sizeof(ShaderInterop::AmbientOcclusionConstants) == 176);
	static_assert(sizeof(ShaderInterop::BloomConstants) == 32);
	static_assert(sizeof(ShaderInterop::FxaaConstants) == 16);
	static_assert(sizeof(ShaderInterop::OverlayConstants) == 160);
	static_assert(sizeof(ShaderInterop::EntityIdDrawConstants) == 16);
	static_assert(sizeof(ShaderInterop::GridConstants) == 80);
	static_assert(sizeof(ShaderInterop::OutlineConstants) == 32);
	static_assert(sizeof(ShaderInterop::LineConstants) == 32);
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
		constexpr nvrhi::Format EntityIdFormat = nvrhi::Format::R32_UINT;
		// The line layer's depth holds distances to line centers (0-1).
		constexpr nvrhi::Format LineLayerDepthFormat = nvrhi::Format::D16;
		// Debug line instances: float3 from + float3 to + float4 color + uint depth tested.
		constexpr uint32_t LineInstanceSize = 44;
		constexpr uint32_t MaxOutlineWidth = 8;
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
		// The fewest entries the instance and instance index buffers hold.
		constexpr size_t MinInstanceCapacity = 1024;

		nvrhi::BufferHandle CreateStructuredBuffer(nvrhi::IDevice* device, size_t count, uint32_t stride, char const* name)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = static_cast<uint64_t>(count) * stride;
			desc.structStride = stride;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = name;
			return device->createBuffer(desc);
		}

		size_t GetCapacity(nvrhi::BufferHandle const& buffer)
		{
			return buffer ? static_cast<size_t>(buffer->getDesc().byteSize / buffer->getDesc().structStride) : 0u;
		}

		// A key whose unsigned order is the order of (depth, item): the depth's bits made to sort as the float does (negative
		// numbers reversed below the positive ones, -0 as +0; depths are never NaN), then the item's index, so that equal
		// depths keep the order of submission.
		uint64_t MakeSortKey(float depth, uint32_t item)
		{
			uint32_t const bits = std::bit_cast<uint32_t>(depth == 0.0f ? 0.0f : depth);
			uint32_t const ordered = (bits & 0x80000000u) != 0 ? ~bits : bits | 0x80000000u;
			return (static_cast<uint64_t>(ordered) << 32) | item;
		}

		// Gives the items of (depth, item) keys in the keys' order. The keys must be listed in increasing item order: a stable
		// least-significant-digit radix sort of their depths' 32 bits then leaves equal depths in item order, as the keys'
		// order wants, in time linear in the keys (the opaque pass orders thousands of items each frame). A byte equal in
		// every key needs no pass.
		void TakeSortedItems(std::vector<uint64_t>& keys, std::vector<uint64_t>& scratch, std::vector<uint32_t>& items)
		{
			scratch.resize(keys.size());
			std::span<uint64_t> from(keys);
			std::span<uint64_t> to(scratch);
			for (uint32_t shift = 32; shift < 64 && !from.empty(); shift += 8)
			{
				std::array<uint32_t, 256> starts{};
				for (uint64_t const key : from)
				{
					starts[(key >> shift) & 0xFFu]++;
				}
				if (starts[(from.front() >> shift) & 0xFFu] == from.size())
				{
					continue;
				}
				uint32_t total = 0;
				for (uint32_t& start : starts)
				{
					uint32_t const count = start;
					start = total;
					total += count;
				}
				for (uint64_t const key : from)
				{
					to[starts[(key >> shift) & 0xFFu]++] = key;
				}
				std::swap(from, to);
			}
			items.resize(keys.size());
			for (size_t index = 0; index < from.size(); index++)
			{
				items[index] = static_cast<uint32_t>(from[index]);
			}
		}

		// The pipeline arrays' index of a variant: blending (or masking, for shadows), double-sidedness and mirroring.
		uint32_t PipelineIndex(bool blend, bool doubleSided, bool mirrored)
		{
			return (mirrored ? 4u : 0u) + (blend ? 2u : 0u) + (doubleSided ? 1u : 0u);
		}

		// Mirrored instances flip the winding of what they draw: their front faces are the clockwise ones.
		size_t MirrorVariant(bool mirrored)
		{
			return mirrored ? 1u : 0u;
		}

		Ref<MaterialAsset> GetDefaultMaterial()
		{
			return AssetManager::GetAsset<MaterialAsset>(GetBuiltInHandle(BuiltInAsset::DefaultMaterial));
		}

		uint32_t ShadowMapSize(uint32_t requested)
		{
			return std::bit_floor(std::clamp(requested, 256u, 8192u));
		}

		bool IntersectsSphere(AABB const& bounds, glm::vec3 const& center, float radius)
		{
			glm::vec3 const closest = glm::clamp(center, bounds.Min, bounds.Max);
			glm::vec3 const offset = closest - center;
			return glm::dot(offset, offset) <= radius * radius;
		}

		struct BoundingSphere
		{
			glm::vec3 Center = glm::vec3(0.0f);
			float Radius = 0.0f;
		};

		// Around the region a spot light reaches: its range sphere cut by the cone. Below 60 degrees, the sphere through the
		// apex and the rim of the cone's cap (centered on the axis) is the smaller one.
		BoundingSphere ComputeSpotLightBounds(glm::vec3 const& position, glm::vec3 const& direction, float range, float outerAngle)
		{
			float const cosine = std::cos(outerAngle);
			if (cosine > 0.5f)
			{
				float const radius = range / (2.0f * cosine);
				return {position + direction * radius, radius};
			}
			return {position, range};
		}
	}

	SceneRenderer::SceneRenderer(std::string_view view)
		: m_ViewName(view)
	{
		ST_CORE_ASSERT(Renderer::IsInitialized(), "SceneRenderer requires Renderer::Init");
		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		m_CommandList = device->createCommandList();
		m_Quads = CreateScope<QuadRenderer>();
		if (GraphicsDevice::GetAdapterInfo().SupportsTimerQueries)
		{
			for (FrameTimer& timer : m_FrameTimers)
			{
				timer.Query = device->createTimerQuery();
			}
		}

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
		// Frames without meshes bind them too.
		m_InstanceBuffer = CreateStructuredBuffer(device, MinInstanceCapacity, sizeof(ShaderInterop::InstanceData), "Instances");
		m_InstanceIndexBuffer = CreateStructuredBuffer(device, MinInstanceCapacity, sizeof(uint32_t), "Instance indices");

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
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(7),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(8),
			nvrhi::BindingLayoutItem::PushConstants(1, sizeof(ShaderInterop::DrawConstants)),
		};
		m_FrameLayout = device->createBindingLayout(frameLayout);
		nvrhi::ITexture* fallbackCube = Renderer::GetFallbackCube();
		UpdateFrameBindings(
			{fallbackCube, fallbackCube, fallbackCube, m_FallbackShadowMap, m_FallbackShadowMap, m_InstanceBuffer, m_InstanceIndexBuffer});

		nvrhi::BindingLayoutDesc shadowLayout;
		shadowLayout.visibility = nvrhi::ShaderType::All;
		shadowLayout.registerSpace = 0;
		shadowLayout.registerSpaceIsDescriptorSet = true;
		shadowLayout.bindings = {
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(ShaderInterop::ShadowDrawConstants)),
			nvrhi::BindingLayoutItem::Sampler(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),
		};
		m_ShadowLayout = device->createBindingLayout(shadowLayout);
		CreateShadowBindings();

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
		// Depth-only passes (shadows, entity IDs) read positions and texture coordinates (alpha masks) only.
		nvrhi::VertexAttributeDesc const depthAttributes[] = {attributes[0], attributes[3]};
		m_DepthInputLayout =
			device->createInputLayout(depthAttributes, static_cast<uint32_t>(std::size(depthAttributes)), ShaderLibrary::Get("Shadow_VS"));

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
		nvrhi::BufferDesc overlayDesc = frameDesc;
		overlayDesc.byteSize = sizeof(ShaderInterop::OverlayConstants);
		overlayDesc.debugName = "Overlay constants";
		m_OverlayConstants = device->createBuffer(overlayDesc);

		// Grid and outline: the overlay camera, one input texture and their push constants.
		auto const createOverlayLayout = [device](uint32_t pushConstantSize)
		{
			nvrhi::BindingLayoutDesc desc;
			desc.visibility = nvrhi::ShaderType::All;
			desc.bindings = {
				nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
				nvrhi::BindingLayoutItem::Texture_SRV(0),
				nvrhi::BindingLayoutItem::PushConstants(1, pushConstantSize),
			};
			return device->createBindingLayout(desc);
		};
		m_GridLayout = createOverlayLayout(sizeof(ShaderInterop::GridConstants));
		m_OutlineLayout = createOverlayLayout(sizeof(ShaderInterop::OutlineConstants));
		// Lines test their depth against the scene depth themselves (to dim instead of hide).
		m_LineLayout = createOverlayLayout(sizeof(ShaderInterop::LineConstants));
		nvrhi::BindingLayoutDesc lineCompositeLayout;
		lineCompositeLayout.visibility = nvrhi::ShaderType::All;
		lineCompositeLayout.bindings = {nvrhi::BindingLayoutItem::Texture_SRV(1)};
		m_LineCompositeLayout = device->createBindingLayout(lineCompositeLayout);

		static_assert(sizeof(LineInstance) == LineInstanceSize);
		auto const lineAttribute = [](char const* name, nvrhi::Format format, uint32_t offset)
		{
			return nvrhi::VertexAttributeDesc()
			    .setName(name)
			    .setFormat(format)
			    .setOffset(offset)
			    .setElementStride(sizeof(LineInstance))
			    .setIsInstanced(true);
		};
		nvrhi::VertexAttributeDesc const lineAttributes[] = {
			lineAttribute("FROM", nvrhi::Format::RGB32_FLOAT, offsetof(LineInstance, From)),
			lineAttribute("TO", nvrhi::Format::RGB32_FLOAT, offsetof(LineInstance, To)),
			lineAttribute("COLOR", nvrhi::Format::RGBA32_FLOAT, offsetof(LineInstance, Color)),
			lineAttribute("DEPTHTESTED", nvrhi::Format::R32_UINT, offsetof(LineInstance, DepthTested)),
		};
		m_LineInputLayout = device->createInputLayout(lineAttributes, static_cast<uint32_t>(std::size(lineAttributes)),
		                                              ShaderLibrary::Get("OverlayLine_VS"));

		nvrhi::BindingLayoutDesc entityIdLayout;
		entityIdLayout.visibility = nvrhi::ShaderType::All;
		entityIdLayout.registerSpace = 0;
		entityIdLayout.registerSpaceIsDescriptorSet = true;
		entityIdLayout.bindings = {
			nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
			nvrhi::BindingLayoutItem::PushConstants(1, sizeof(ShaderInterop::EntityIdDrawConstants)),
			nvrhi::BindingLayoutItem::Sampler(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),
		};
		m_EntityIdLayout = device->createBindingLayout(entityIdLayout);
		CreateEntityIdBindings();

		nvrhi::TextureDesc pickDesc;
		pickDesc.width = 1;
		pickDesc.height = 1;
		pickDesc.format = EntityIdFormat;
		pickDesc.debugName = "Pick readback";
		m_PickStaging = device->createStagingTexture(pickDesc, nvrhi::CpuAccessMode::Read);
		m_PickQuery = device->createEventQuery();

		m_OcclusionPipeline = CreateComputePipeline(device, "AmbientOcclusion_CS", m_OcclusionLayout);
		m_DenoisePipeline = CreateComputePipeline(device, "AmbientOcclusionDenoise_CS", m_DenoiseLayout);
		m_CompositePipeline = CreateComputePipeline(device, "AmbientOcclusionComposite_CS", m_CompositeLayout);
		m_BloomDownsamplePipeline = CreateComputePipeline(device, "BloomDownsample_CS", m_BloomLayout);
		m_BloomUpsamplePipeline = CreateComputePipeline(device, "BloomUpsample_CS", m_BloomLayout);

		ST_CORE_ASSERT(m_CommandList && m_FrameConstants && m_ShadowConstants && m_OcclusionConstants && m_LightBuffer &&
		                   m_InstanceBuffer && m_InstanceIndexBuffer && m_ShadowCompareSampler && m_ShadowPointSampler &&
		                   m_FallbackShadowMap && m_FrameLayout && m_FrameBindings && m_ShadowLayout && m_ShadowBindings && m_InputLayout &&
		                   m_DepthInputLayout && m_TonemapLayout && m_FxaaLayout && m_OcclusionPipeline && m_DenoisePipeline &&
		                   m_CompositePipeline && m_BloomDownsamplePipeline && m_BloomUpsamplePipeline,
		               "Failed to create scene renderer resources");
		ST_CORE_ASSERT(m_OverlayConstants && m_GridLayout && m_OutlineLayout && m_LineLayout && m_LineCompositeLayout &&
		                   m_LineInputLayout && m_EntityIdLayout && m_EntityIdBindings && m_PickStaging && m_PickQuery,
		               "Failed to create scene renderer overlay resources");
	}

	SceneRenderer::~SceneRenderer()
	{
		// The last frames may still use the targets, and clears (which NVRHI does not track) can be their only use.
		if (m_ColorTarget)
		{
			GraphicsDevice::GetDevice()->waitForIdle();
		}
	}

	void SceneRenderer::SetEntityIdsEnabled(bool enabled)
	{
		if (enabled != m_EntityIdsEnabled)
		{
			m_EntityIdsEnabled = enabled;
			m_TargetsDirty = true;
		}
	}

	void SceneRenderer::CreateEntityIdBindings()
	{
		m_EntityIdBindings = GraphicsDevice::GetDevice()->createBindingSet(
			nvrhi::BindingSetDesc()
				.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, m_OverlayConstants))
				.addItem(nvrhi::BindingSetItem::PushConstants(1, sizeof(ShaderInterop::EntityIdDrawConstants)))
				.addItem(nvrhi::BindingSetItem::Sampler(0, Renderer::GetMaterialSampler()))
				.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_InstanceBuffer))
				.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_InstanceIndexBuffer)),
			m_EntityIdLayout);
	}

	void SceneRenderer::CreateShadowBindings()
	{
		m_ShadowBindings = GraphicsDevice::GetDevice()->createBindingSet(
			nvrhi::BindingSetDesc()
				.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(ShaderInterop::ShadowDrawConstants)))
				.addItem(nvrhi::BindingSetItem::Sampler(0, Renderer::GetMaterialSampler()))
				.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_InstanceBuffer))
				.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_InstanceIndexBuffer)),
			m_ShadowLayout);
	}

	bool SceneRenderer::EnsureInstanceBuffers(size_t instanceCount, size_t indexCount)
	{
		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		bool grown = false;
		if (GetCapacity(m_InstanceBuffer) < instanceCount)
		{
			// Command lists in flight keep the old buffer alive through the binding sets they used.
			m_InstanceBuffer =
				CreateStructuredBuffer(device, std::bit_ceil(instanceCount), sizeof(ShaderInterop::InstanceData), "Instances");
			grown = true;
		}
		if (GetCapacity(m_InstanceIndexBuffer) < indexCount)
		{
			m_InstanceIndexBuffer = CreateStructuredBuffer(device, std::bit_ceil(indexCount), sizeof(uint32_t), "Instance indices");
			grown = true;
		}
		if (!m_InstanceBuffer || !m_InstanceIndexBuffer)
		{
			ST_CORE_ERROR("Failed to create the instance buffers for {} instances", instanceCount);
			m_InstanceBuffer = CreateStructuredBuffer(device, MinInstanceCapacity, sizeof(ShaderInterop::InstanceData), "Instances");
			m_InstanceIndexBuffer = CreateStructuredBuffer(device, MinInstanceCapacity, sizeof(uint32_t), "Instance indices");
			grown = true;
		}
		if (grown)
		{
			// The frame binding set follows in EndScene (UpdateFrameBindings compares the buffers it names).
			CreateEntityIdBindings();
			CreateShadowBindings();
		}
		return GetCapacity(m_InstanceBuffer) >= instanceCount && GetCapacity(m_InstanceIndexBuffer) >= indexCount;
	}

	void SceneRenderer::UpdateFrameBindings(FrameResources const& resources)
	{
		if (m_FrameBindings && m_BoundResources == resources)
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
			nvrhi::BindingSetItem::Texture_SRV(1, resources.Irradiance, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, cube),
			nvrhi::BindingSetItem::Texture_SRV(2, resources.Prefiltered, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, cube),
			nvrhi::BindingSetItem::Texture_SRV(3, Renderer::GetBrdfLut()),
			nvrhi::BindingSetItem::Texture_SRV(4, resources.Radiance, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, cube),
			nvrhi::BindingSetItem::Texture_SRV(5, resources.CascadeShadowMap, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, array),
			nvrhi::BindingSetItem::Texture_SRV(6, resources.LocalShadowMap, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, array),
			nvrhi::BindingSetItem::Sampler(0, Renderer::GetMaterialSampler()),
			nvrhi::BindingSetItem::Sampler(1, Renderer::GetLinearClampSampler()),
			nvrhi::BindingSetItem::Sampler(2, m_ShadowCompareSampler),
			nvrhi::BindingSetItem::Sampler(3, m_ShadowPointSampler),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(7, resources.Instances),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(8, resources.InstanceIndices),
			nvrhi::BindingSetItem::PushConstants(1, sizeof(ShaderInterop::DrawConstants)),
		};
		m_FrameBindings = GraphicsDevice::GetDevice()->createBindingSet(frameBindings, m_FrameLayout);
		m_BoundResources = resources;
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
		// Previous frames may still use the old targets, and clears (which NVRHI does not track) can be their only use, so
		// the GPU is drained before they are released. This only happens on resizes and reconfiguration.
		if (m_ColorTarget)
		{
			device->waitForIdle();
		}

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

		// Overlays: the UI placed in the world is tested against the scene depth, attached read-only; the grid and the lines
		// sample it.
		nvrhi::FramebufferAttachment const readOnlyDepth = nvrhi::FramebufferAttachment().setTexture(m_DepthTarget).setReadOnly(true);
		nvrhi::FramebufferDesc ldrWorldUIDesc = nvrhi::FramebufferDesc().addColorAttachment(m_LdrTarget);
		ldrWorldUIDesc.depthAttachment = readOnlyDepth;
		m_LdrWorldUIFramebuffer = device->createFramebuffer(ldrWorldUIDesc);
		nvrhi::FramebufferDesc finalWorldUIDesc = nvrhi::FramebufferDesc().addColorAttachment(m_FinalImage);
		finalWorldUIDesc.depthAttachment = readOnlyDepth;
		m_FinalWorldUIFramebuffer = device->createFramebuffer(finalWorldUIDesc);
		m_GridBindings =
			device->createBindingSet(nvrhi::BindingSetDesc()
		                                 .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, m_OverlayConstants))
		                                 .addItem(nvrhi::BindingSetItem::Texture_SRV(0, m_DepthTarget))
		                                 .addItem(nvrhi::BindingSetItem::PushConstants(1, sizeof(ShaderInterop::GridConstants))),
		                             m_GridLayout);
		m_LineBindings =
			device->createBindingSet(nvrhi::BindingSetDesc()
		                                 .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, m_OverlayConstants))
		                                 .addItem(nvrhi::BindingSetItem::Texture_SRV(0, m_DepthTarget))
		                                 .addItem(nvrhi::BindingSetItem::PushConstants(1, sizeof(ShaderInterop::LineConstants))),
		                             m_LineLayout);
		// Recreated at the new size by the next frame with lines.
		m_LineLayer = nullptr;
		m_LineLayerDepth = nullptr;
		m_LineLayerFramebuffer = nullptr;
		m_LineCompositeBindings = nullptr;

		m_EntityIdTarget = nullptr;
		m_EntityIdDepth = nullptr;
		m_EntityIdFramebuffer = nullptr;
		m_OutlineBindings = nullptr;
		if (m_EntityIdsEnabled)
		{
			nvrhi::TextureDesc idDesc;
			idDesc.width = m_Width;
			idDesc.height = m_Height;
			idDesc.format = EntityIdFormat;
			idDesc.isRenderTarget = true;
			idDesc.initialState = nvrhi::ResourceStates::ShaderResource;
			idDesc.keepInitialState = true;
			idDesc.setClearValue(nvrhi::Color(0.0f));
			idDesc.debugName = "Entity IDs";
			m_EntityIdTarget = device->createTexture(idDesc);

			// Its own depth buffer: the ID pass need not reproduce the forward pass's depth exactly.
			nvrhi::TextureDesc idDepthDesc = idDesc;
			idDepthDesc.format = DepthFormat;
			idDepthDesc.initialState = nvrhi::ResourceStates::DepthWrite;
			idDepthDesc.debugName = "Entity ID depth";
			m_EntityIdDepth = device->createTexture(idDepthDesc);

			m_EntityIdFramebuffer = device->createFramebuffer(
				nvrhi::FramebufferDesc().addColorAttachment(m_EntityIdTarget).setDepthAttachment(m_EntityIdDepth));
			m_OutlineBindings =
				device->createBindingSet(nvrhi::BindingSetDesc()
			                                 .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, m_OverlayConstants))
			                                 .addItem(nvrhi::BindingSetItem::Texture_SRV(0, m_EntityIdTarget))
			                                 .addItem(nvrhi::BindingSetItem::PushConstants(1, sizeof(ShaderInterop::OutlineConstants))),
			                             m_OutlineLayout);
		}

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
		CreateOverlayPipelines();
		m_TargetsDirty = false;
	}

	void SceneRenderer::CreateOverlayPipelines()
	{
		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		auto const enableAlphaBlending = [](nvrhi::GraphicsPipelineDesc& desc)
		{
			nvrhi::BlendState::RenderTarget& target = desc.renderState.blendState.targets[0];
			target.blendEnable = true;
			target.srcBlend = nvrhi::BlendFactor::SrcAlpha;
			target.destBlend = nvrhi::BlendFactor::InvSrcAlpha;
			target.srcBlendAlpha = nvrhi::BlendFactor::Zero;
			target.destBlendAlpha = nvrhi::BlendFactor::One;
		};

		if (!m_GridPipeline)
		{
			nvrhi::GraphicsPipelineDesc desc;
			desc.VS = ShaderLibrary::Get("Fullscreen_VS");
			desc.PS = ShaderLibrary::Get("Grid_PS");
			desc.bindingLayouts = {m_GridLayout};
			desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
			desc.renderState.depthStencilState.depthTestEnable = false;
			desc.renderState.depthStencilState.depthWriteEnable = false;
			enableAlphaBlending(desc);
			// The tonemapped and final images share their format.
			m_GridPipeline = device->createGraphicsPipeline(desc, m_FinalFramebuffer->getFramebufferInfo());
		}
		if (!m_LineCompositePipeline)
		{
			nvrhi::GraphicsPipelineDesc desc;
			desc.VS = ShaderLibrary::Get("Fullscreen_VS");
			desc.PS = ShaderLibrary::Get("OverlayLineComposite_PS");
			desc.bindingLayouts = {m_LineCompositeLayout};
			desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
			desc.renderState.depthStencilState.depthTestEnable = false;
			desc.renderState.depthStencilState.depthWriteEnable = false;
			enableAlphaBlending(desc);
			m_LineCompositePipeline = device->createGraphicsPipeline(desc, m_FinalFramebuffer->getFramebufferInfo());
		}
		if (!m_OutlinePipeline)
		{
			nvrhi::GraphicsPipelineDesc desc;
			desc.VS = ShaderLibrary::Get("Fullscreen_VS");
			desc.PS = ShaderLibrary::Get("Outline_PS");
			desc.bindingLayouts = {m_OutlineLayout};
			desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
			desc.renderState.depthStencilState.depthTestEnable = false;
			desc.renderState.depthStencilState.depthWriteEnable = false;
			enableAlphaBlending(desc);
			m_OutlinePipeline = device->createGraphicsPipeline(desc, m_FinalFramebuffer->getFramebufferInfo());
		}
		if (!m_GridPipeline || !m_LineCompositePipeline || !m_OutlinePipeline)
		{
			ST_CORE_ERROR("Failed to create the overlay pipelines");
		}
	}

	void SceneRenderer::CreateLineLayer()
	{
		if (m_LineLayerFramebuffer)
		{
			return;
		}
		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		nvrhi::TextureDesc layerDesc;
		layerDesc.width = m_Width;
		layerDesc.height = m_Height;
		layerDesc.format = FinalFormat;
		layerDesc.isRenderTarget = true;
		layerDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		layerDesc.keepInitialState = true;
		layerDesc.setClearValue(nvrhi::Color(0.0f));
		layerDesc.debugName = "Line layer";
		m_LineLayer = device->createTexture(layerDesc);
		nvrhi::TextureDesc depthDesc = layerDesc;
		depthDesc.format = LineLayerDepthFormat;
		depthDesc.initialState = nvrhi::ResourceStates::DepthWrite;
		depthDesc.setClearValue(nvrhi::Color(1.0f));
		depthDesc.debugName = "Line layer depth";
		m_LineLayerDepth = device->createTexture(depthDesc);
		if (!m_LineLayer || !m_LineLayerDepth)
		{
			ST_CORE_ERROR("Failed to create the debug line layer");
			m_LineLayer = nullptr;
			m_LineLayerDepth = nullptr;
			return;
		}
		m_LineLayerFramebuffer =
			device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_LineLayer).setDepthAttachment(m_LineLayerDepth));
		m_LineCompositeBindings = device->createBindingSet(
			nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::Texture_SRV(1, m_LineLayer)), m_LineCompositeLayout);
		if (!m_LinePipeline && m_LineLayerFramebuffer)
		{
			nvrhi::GraphicsPipelineDesc desc;
			desc.primType = nvrhi::PrimitiveType::TriangleList;
			desc.inputLayout = m_LineInputLayout;
			desc.VS = ShaderLibrary::Get("OverlayLine_VS");
			desc.PS = ShaderLibrary::Get("OverlayLine_PS");
			desc.bindingLayouts = {m_LineLayout};
			desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
			// The depth is the distance to the line's center: the nearest line keeps the pixel, without blending. The
			// pixel shader compares the scene depth itself, so hidden parts can be dimmed instead of discarded.
			desc.renderState.depthStencilState.depthTestEnable = true;
			desc.renderState.depthStencilState.depthWriteEnable = true;
			desc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::Less;
			m_LinePipeline = device->createGraphicsPipeline(desc, m_LineLayerFramebuffer->getFramebufferInfo());
		}
		if (!m_LineLayerFramebuffer || !m_LineCompositeBindings || !m_LinePipeline)
		{
			ST_CORE_ERROR("Failed to create the debug line layer");
			m_LineLayerFramebuffer = nullptr;
		}
	}

	nvrhi::IGraphicsPipeline* SceneRenderer::GetEntityIdPipeline(bool blend, bool doubleSided, bool mirrored)
	{
		nvrhi::GraphicsPipelineHandle& pipeline = m_EntityIdPipelines[PipelineIndex(blend, doubleSided, mirrored)];
		if (pipeline || !m_EntityIdFramebuffer)
		{
			return pipeline;
		}

		nvrhi::GraphicsPipelineDesc desc;
		desc.inputLayout = m_DepthInputLayout;
		desc.VS = ShaderLibrary::Get("EntityId_VS");
		desc.PS = ShaderLibrary::Get("EntityId_PS");
		desc.bindingLayouts = {m_EntityIdLayout, Renderer::GetMaterialBindingLayout()};
		desc.renderState.rasterState.cullMode = doubleSided ? nvrhi::RasterCullMode::None : nvrhi::RasterCullMode::Back;
		desc.renderState.rasterState.frontCounterClockwise = !mirrored;
		// Opaque items are drawn first and write depth; blended items follow back to front and only test it, so the nearest
		// blended surface in front of the opaque scene wins.
		desc.renderState.depthStencilState.depthTestEnable = true;
		desc.renderState.depthStencilState.depthWriteEnable = !blend;
		desc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::GreaterOrEqual;
		pipeline = GraphicsDevice::GetDevice()->createGraphicsPipeline(desc, m_EntityIdFramebuffer->getFramebufferInfo());
		if (!pipeline)
		{
			ST_CORE_ERROR("Failed to create an entity ID pipeline");
		}
		return pipeline;
	}

	nvrhi::IGraphicsPipeline* SceneRenderer::GetMeshPipeline(bool blend, bool doubleSided, bool mirrored)
	{
		nvrhi::GraphicsPipelineHandle& pipeline = m_MeshPipelines[PipelineIndex(blend, doubleSided, mirrored)];
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
		// Double-sided surfaces need it too: the pixel shader turns the normals of back faces (SV_IsFrontFace).
		desc.renderState.rasterState.frontCounterClockwise = !mirrored;
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

	nvrhi::IGraphicsPipeline* SceneRenderer::GetShadowPipeline(bool masked, bool doubleSided, bool mirrored)
	{
		nvrhi::GraphicsPipelineHandle& pipeline = m_ShadowPipelines[PipelineIndex(masked, doubleSided, mirrored)];
		if (pipeline)
		{
			return pipeline;
		}

		nvrhi::GraphicsPipelineDesc desc;
		desc.inputLayout = m_DepthInputLayout;
		desc.VS = ShaderLibrary::Get("Shadow_VS");
		// Opaque casters only write depth.
		desc.PS = masked ? ShaderLibrary::Get("Shadow_PS") : nullptr;
		desc.bindingLayouts = {m_ShadowLayout, Renderer::GetMaterialBindingLayout()};
		desc.renderState.rasterState.cullMode = doubleSided ? nvrhi::RasterCullMode::None : nvrhi::RasterCullMode::Back;
		desc.renderState.rasterState.frontCounterClockwise = !mirrored;
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
		m_ViewFrustum = Frustum(camera.View, camera.Projection, camera.MaxDistance);
		m_Settings = settings;
		m_AmbientRadiance = glm::vec3(0.0f);
		m_Environment = {};
		m_Instances.clear();
		m_PassInstanceCounts = {};
		m_Lights.clear();
		m_ShadowedDirectionalLight = -1;
		m_DirectionalLightSize = 0.0f;
		m_LocalShadowLights.clear();
		m_ShadowViews.clear();
		m_Statistics = {};
		m_Overlays = {};
		m_Lines.clear();
		m_Quads->Clear();
		m_HasSelection = false;
	}

	void SceneRenderer::SubmitMesh(Ref<MeshSource> const& mesh, std::span<Ref<MaterialAsset> const> materials, glm::mat4 const& transform,
	                               bool castShadows, uint32_t pickingId, bool selected)
	{
		MeshSubmission submission;
		submission.Transform = transform;
		submission.PreviousTransform = transform;
		submission.CastShadows = castShadows;
		submission.PickingId = pickingId;
		submission.Selected = selected;
		SubmitMesh(mesh, materials, submission);
	}

	void SceneRenderer::SubmitMesh(Ref<MeshSource> const& mesh, std::span<Ref<MaterialAsset> const> materials,
	                               MeshSubmission const& submission)
	{
		ST_CORE_ASSERT(m_InScene, "SubmitMesh outside BeginScene/EndScene");
		SubmitMesh(PrepareMeshDraw(mesh, materials), submission);
	}

	MeshDraw SceneRenderer::PrepareMeshDraw(Ref<MeshSource> const& mesh, std::span<Ref<MaterialAsset> const> materials)
	{
		ST_CORE_ASSERT(m_InScene, "PrepareMeshDraw outside BeginScene/EndScene");
		if (!mesh)
		{
			return {};
		}
		uint32_t const meshIndex = GetFrameMesh(mesh);
		// No call below adds a mesh to the frame's table, so the reference stays valid.
		FrameMesh& frameMesh = m_FrameMeshes[meshIndex];
		FrameMeshDraw draw;
		draw.Mesh = meshIndex;
		draw.FirstSubmesh = static_cast<uint32_t>(m_DrawSubmeshes.size());
		std::vector<Submesh> const& submeshes = mesh->GetSubmeshes();
		for (uint32_t i = 0; i < submeshes.size(); i++)
		{
			Submesh const& submesh = submeshes[i];
			Ref<MaterialAsset> const* requested =
				submesh.MaterialIndex < materials.size() && materials[submesh.MaterialIndex] ? &materials[submesh.MaterialIndex] : nullptr;
			MaterialAsset const* requestedSource = requested != nullptr ? requested->get() : nullptr;
			auto const known = std::find_if(frameMesh.Groups.begin(), frameMesh.Groups.end(),
			                                [i, requestedSource](FrameMesh::SubmeshGroup const& entry)
			                                {
												return entry.Submesh == i && entry.Material == requestedSource;
											});
			uint32_t material = 0;
			uint32_t group = 0;
			if (known != frameMesh.Groups.end())
			{
				material = known->FrameMaterial;
				group = known->Group;
			}
			else
			{
				if (requested != nullptr)
				{
					material = GetFrameMaterial(*requested);
				}
				else if (m_DefaultMaterial)
				{
					material = *m_DefaultMaterial;
				}
				else if (Ref<MaterialAsset> const fallback = GetDefaultMaterial())
				{
					material = GetFrameMaterial(fallback);
					m_DefaultMaterial = material;
				}
				else
				{
					continue;
				}
				group = m_GroupIndices.try_emplace(GroupKey{meshIndex, i, material}, static_cast<uint32_t>(m_GroupIndices.size()))
				            .first->second;
				if (frameMesh.Groups.size() < FrameMesh::MaxSubmeshGroups)
				{
					frameMesh.Groups.push_back({i, requestedSource, material, group});
				}
			}
			m_DrawSubmeshes.push_back({submesh.Bounds, i, material, group});
		}
		draw.SubmeshCount = static_cast<uint32_t>(m_DrawSubmeshes.size()) - draw.FirstSubmesh;
		m_MeshDraws.push_back(draw);
		return MeshDraw{static_cast<uint32_t>(m_MeshDraws.size() - 1)};
	}

	void SceneRenderer::SubmitMesh(MeshDraw draw, MeshSubmission const& submission)
	{
		ST_CORE_ASSERT(m_InScene, "SubmitMesh outside BeginScene/EndScene");
		ST_CORE_ASSERT(submission.PickingId <= MaxPickingId, "Picking IDs must stay below the selection bit");
		ST_CORE_ASSERT(!draw.IsValid() || draw.Index < m_MeshDraws.size(), "A mesh draw of another frame");
		if (!draw.IsValid() || draw.Index >= m_MeshDraws.size())
		{
			return;
		}
		uint32_t const entityId =
			submission.PickingId == 0
				? 0u
				: ((submission.PickingId & MaxPickingId) | (submission.Selected ? ShaderInterop::EntityIdSelectedBit : 0u));
		m_HasSelection = m_HasSelection || (entityId & ShaderInterop::EntityIdSelectedBit) != 0;
		uint32_t const instance = static_cast<uint32_t>(m_Instances.size());
		m_Instances.push_back(Instancing::MakeInstanceData(submission.Transform, submission.PreviousTransform, entityId));

		FrameMeshDraw const& meshDraw = m_MeshDraws[draw.Index];
		// The camera's depth axis: a point's distance along the view direction (the camera looks down -Z) is minus its
		// view-space z, the third row of the view matrix applied to it.
		glm::mat4 const& view = m_Camera.View;
		glm::vec4 const depthRow(-view[0][2], -view[1][2], -view[2][2], -view[3][2]);
		// World bounds by Arvo's method, as AABB::Transform computes them, with the matrix's absolute linear part made once
		// for the instance's submeshes.
		glm::mat4 const& transform = submission.Transform;
		glm::mat3 absolute;
		for (int column = 0; column < 3; column++)
		{
			absolute[column] = glm::abs(glm::vec3(transform[column]));
		}
		// A negative determinant mirrors the instance (a zero or undefined one does not, as for its normal basis).
		bool const mirrored = glm::dot(glm::vec3(transform[0]), glm::cross(glm::vec3(transform[1]), glm::vec3(transform[2]))) < 0.0f;
		for (uint32_t index = meshDraw.FirstSubmesh; index < meshDraw.FirstSubmesh + meshDraw.SubmeshCount; index++)
		{
			FrameDrawSubmesh& part = m_DrawSubmeshes[index];
			if (mirrored && part.MirroredGroup == FrameDrawSubmesh::NoGroup)
			{
				part.MirroredGroup =
					m_GroupIndices
						.try_emplace(GroupKey{meshDraw.Mesh, part.Submesh, part.Material, 1u}, static_cast<uint32_t>(m_GroupIndices.size()))
						.first->second;
			}
			SubmeshItem item;
			glm::vec4 const center = transform * glm::vec4(part.Bounds.GetCenter(), 1.0f);
			if (part.Bounds.IsValid())
			{
				glm::vec3 const extents = absolute * part.Bounds.GetExtents();
				item.WorldBounds = {glm::vec3(center) - extents, glm::vec3(center) + extents};
			}
			// Sorted by the depth of its bounds' center; a matrix that is not a number sorts last, so that the orders stay
			// strict.
			item.ViewDepth = glm::dot(depthRow, center);
			if (std::isnan(item.ViewDepth))
			{
				item.ViewDepth = std::numeric_limits<float>::infinity();
			}
			item.Instance = instance;
			item.Mesh = meshDraw.Mesh;
			item.Submesh = part.Submesh;
			item.Material = part.Material;
			item.Group = mirrored ? part.MirroredGroup : part.Group;
			item.CastShadows = submission.CastShadows;
			item.Mirrored = mirrored;
			m_Items.push_back(item);
		}
	}

	uint32_t SceneRenderer::GetFrameMesh(Ref<MeshSource> const& mesh)
	{
		auto const [entry, added] = m_FrameMeshIndices.try_emplace(mesh.get(), static_cast<uint32_t>(m_FrameMeshes.size()));
		if (added)
		{
			FrameMesh& frameMesh = m_FrameMeshes.emplace_back();
			frameMesh.Source = mesh;
		}
		return entry->second;
	}

	uint32_t SceneRenderer::GetFrameMaterial(Ref<MaterialAsset> const& material)
	{
		auto const [entry, added] = m_FrameMaterialIndices.try_emplace(material.get(), static_cast<uint32_t>(m_FrameMaterials.size()));
		if (added)
		{
			m_FrameMaterials.push_back({material});
		}
		return entry->second;
	}

	size_t SceneRenderer::GroupKeyHash::operator()(GroupKey const& key) const
	{
		// Mixes the key's words (64-bit FNV-1a).
		uint64_t hash = 14695981039346656037ull;
		for (uint32_t const word : {key.Mesh, key.Submesh, key.Material, key.Mirrored})
		{
			hash = (hash ^ word) * 1099511628211ull;
		}
		return static_cast<size_t>(hash);
	}

	void SceneRenderer::SubmitSprite(SpriteSubmission const& sprite)
	{
		ST_CORE_ASSERT(m_InScene, "SubmitSprite outside BeginScene/EndScene");
		ST_CORE_ASSERT(sprite.PickingId <= MaxPickingId, "Picking IDs must stay below the selection bit");
		uint32_t const entityId =
			sprite.PickingId == 0 ? 0u : ((sprite.PickingId & MaxPickingId) | (sprite.Selected ? ShaderInterop::EntityIdSelectedBit : 0u));
		m_HasSelection = m_HasSelection || (entityId & ShaderInterop::EntityIdSelectedBit) != 0;
		m_Quads->SubmitSprite(sprite, entityId);
	}

	void SceneRenderer::SubmitText(TextSubmission text)
	{
		ST_CORE_ASSERT(m_InScene, "SubmitText outside BeginScene/EndScene");
		ST_CORE_ASSERT(text.PickingId <= MaxPickingId, "Picking IDs must stay below the selection bit");
		uint32_t const entityId =
			text.PickingId == 0 ? 0u : ((text.PickingId & MaxPickingId) | (text.Selected ? ShaderInterop::EntityIdSelectedBit : 0u));
		m_HasSelection = m_HasSelection || (entityId & ShaderInterop::EntityIdSelectedBit) != 0;
		m_Quads->SubmitText(std::move(text), entityId);
	}

	void SceneRenderer::SubmitUIRect(UIRectSubmission const& rect)
	{
		ST_CORE_ASSERT(m_InScene, "SubmitUIRect outside BeginScene/EndScene");
		ST_CORE_ASSERT(rect.PickingId <= MaxPickingId, "Picking IDs must stay below the selection bit");
		uint32_t const entityId =
			rect.PickingId == 0 ? 0u : ((rect.PickingId & MaxPickingId) | (rect.Selected ? ShaderInterop::EntityIdSelectedBit : 0u));
		m_HasSelection = m_HasSelection || (entityId & ShaderInterop::EntityIdSelectedBit) != 0;
		m_Quads->SubmitUIRect(rect, entityId);
	}

	void SceneRenderer::SubmitUIText(UITextSubmission text)
	{
		ST_CORE_ASSERT(m_InScene, "SubmitUIText outside BeginScene/EndScene");
		ST_CORE_ASSERT(text.PickingId <= MaxPickingId, "Picking IDs must stay below the selection bit");
		uint32_t const entityId =
			text.PickingId == 0 ? 0u : ((text.PickingId & MaxPickingId) | (text.Selected ? ShaderInterop::EntityIdSelectedBit : 0u));
		m_HasSelection = m_HasSelection || (entityId & ShaderInterop::EntityIdSelectedBit) != 0;
		m_Quads->SubmitUIText(std::move(text), entityId);
	}

	void SceneRenderer::SubmitLine(glm::vec3 const& from, glm::vec3 const& to, glm::vec4 const& color, bool depthTest)
	{
		ST_CORE_ASSERT(m_InScene, "SubmitLine outside BeginScene/EndScene");
		m_Lines.push_back({from, to, color, depthTest ? 1u : 0u});
	}

	void SceneRenderer::SetOverlays(SceneRendererOverlays const& overlays)
	{
		ST_CORE_ASSERT(m_InScene, "SetOverlays outside BeginScene/EndScene");
		m_Overlays = overlays;
	}

	std::span<ShaderInterop::InstanceData const> SceneRenderer::GetFrameInstances() const
	{
		return m_Instances;
	}

	SceneRenderer::PassInstanceCounts const& SceneRenderer::GetPassInstanceCounts() const
	{
		return m_PassInstanceCounts;
	}

	void SceneRenderer::RequestPick(uint32_t x, uint32_t y)
	{
		m_PickRequest = glm::uvec2(x, y);
	}

	std::optional<uint32_t> SceneRenderer::TakePickResult()
	{
		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		if (m_PickInFlight && device->pollEventQuery(m_PickQuery))
		{
			size_t rowPitch = 0;
			void const* data = device->mapStagingTexture(m_PickStaging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch);
			uint32_t id = 0;
			if (data != nullptr)
			{
				std::memcpy(&id, data, sizeof(id));
				device->unmapStagingTexture(m_PickStaging);
			}
			m_PickResult = id & ShaderInterop::EntityIdMask;
			m_PickInFlight = false;
		}
		return std::exchange(m_PickResult, std::nullopt);
	}

	void SceneRenderer::SubmitDirectionalLight(DirectionalLightSubmission const& light)
	{
		ST_CORE_ASSERT(m_InScene, "SubmitDirectionalLight outside BeginScene/EndScene");
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
		ST_CORE_ASSERT(m_InScene, "SubmitPointLight outside BeginScene/EndScene");
		float const range = std::max(light.Range, 1e-3f);
		if (!m_ViewFrustum.IntersectsSphere(light.Position, range))
		{
			m_Statistics.CulledLights++;
			return;
		}
		if (m_Lights.size() >= ShaderInterop::MaxLights)
		{
			return;
		}
		ShaderInterop::LightData data{};
		data.Type = ShaderInterop::LightTypePoint;
		data.Position = light.Position;
		data.Range = range;
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
		ST_CORE_ASSERT(m_InScene, "SubmitSpotLight outside BeginScene/EndScene");
		glm::vec3 const direction = glm::normalize(light.Direction);
		float const range = std::max(light.Range, 1e-3f);
		float const outer = glm::radians(std::clamp(light.OuterConeAngle, 0.1f, 89.9f));
		BoundingSphere const bounds = ComputeSpotLightBounds(light.Position, direction, range, outer);
		if (!m_ViewFrustum.IntersectsSphere(bounds.Center, bounds.Radius))
		{
			m_Statistics.CulledLights++;
			return;
		}
		if (m_Lights.size() >= ShaderInterop::MaxLights)
		{
			return;
		}
		ShaderInterop::LightData data{};
		data.Type = ShaderInterop::LightTypeSpot;
		data.Position = light.Position;
		data.Direction = direction;
		data.Range = range;
		data.Radiance = light.Color * light.Intensity;
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

	void SceneRenderer::ResolveFrameResources(bool shadows)
	{
		for (SubmeshItem const& item : m_Items)
		{
			// Nothing else outside the view needs its resources: casters outside it can still shadow what it shows, and
			// blended materials cast no shadows.
			FrameMaterial& material = m_FrameMaterials[item.Material];
			if (!item.InView && !(shadows && item.CastShadows && material.Source->GetData().AlphaMode != MaterialAlphaMode::Blend))
			{
				continue;
			}
			FrameMesh& mesh = m_FrameMeshes[item.Mesh];
			if (!mesh.Resolved)
			{
				mesh.Gpu = Renderer::GetMesh(mesh.Source);
				mesh.Resolved = true;
			}
			MaterialData const& data = material.Source->GetData();
			if (!material.Resolved)
			{
				material.Blend = data.AlphaMode == MaterialAlphaMode::Blend;
				material.Bindings = Renderer::GetMaterialBindingSet(material.Source);
				material.Resolved = true;
			}
			size_t const variant = MirrorVariant(item.Mirrored);
			if (!material.PipelinesResolved[variant])
			{
				material.Pipelines[variant] = GetMeshPipeline(material.Blend, data.DoubleSided, item.Mirrored);
				// Every mesh is drawn into the ID target (non-pickable ones write 0) so it hides what lies behind it.
				material.EntityIdPipelines[variant] =
					m_EntityIdsEnabled ? GetEntityIdPipeline(material.Blend, data.DoubleSided, item.Mirrored) : nullptr;
				// Blended surfaces do not cast shadows.
				material.ShadowPipelines[variant] =
					material.Blend ? nullptr
								   : GetShadowPipeline(data.AlphaMode == MaterialAlphaMode::Mask, data.DoubleSided, item.Mirrored);
				material.PipelinesResolved[variant] = true;
			}
		}
	}

	bool SceneRenderer::IsDrawable(SubmeshItem const& item) const
	{
		FrameMesh const& mesh = m_FrameMeshes[item.Mesh];
		FrameMaterial const& material = m_FrameMaterials[item.Material];
		return mesh.Gpu != nullptr && material.Bindings != nullptr && material.Pipelines[MirrorVariant(item.Mirrored)] != nullptr;
	}

	bool SceneRenderer::IsShadowCaster(SubmeshItem const& item) const
	{
		return item.CastShadows && IsDrawable(item) &&
		       m_FrameMaterials[item.Material].ShadowPipelines[MirrorVariant(item.Mirrored)] != nullptr;
	}

	SceneRenderer::MeshBatch SceneRenderer::MakeBatch(SubmeshItem const& item, nvrhi::IGraphicsPipeline* pipeline, uint32_t firstInstance,
	                                                  uint32_t instanceCount) const
	{
		FrameMesh const& mesh = m_FrameMeshes[item.Mesh];
		Submesh const& submesh = mesh.Source->GetSubmeshes()[item.Submesh];
		MeshBatch batch;
		batch.Pipeline = pipeline;
		batch.Mesh = mesh.Gpu;
		batch.MaterialBindings = m_FrameMaterials[item.Material].Bindings;
		batch.BaseIndex = submesh.BaseIndex;
		batch.IndexCount = submesh.IndexCount;
		batch.BaseVertex = submesh.BaseVertex;
		batch.FirstInstance = firstInstance;
		batch.InstanceCount = instanceCount;
		return batch;
	}

	void SceneRenderer::BuildBatches()
	{
		ST_PROFILE_SCOPE("Renderer.Batch");
		m_OpaqueBatches.clear();
		m_EntityIdBatches.clear();
		m_BlendBatches.clear();
		m_ShadowBatches.clear();
		m_InstanceIndices.clear();

		// The opaque and masked items in the camera's view, front to back (ties by submission).
		m_SortKeys.clear();
		for (uint32_t index = 0; index < m_Items.size(); index++)
		{
			SubmeshItem const& item = m_Items[index];
			if (item.InView && IsDrawable(item) && !m_FrameMaterials[item.Material].Blend)
			{
				m_SortKeys.push_back(MakeSortKey(item.ViewDepth, index));
			}
		}
		TakeSortedItems(m_SortKeys, m_SortScratch, m_PassItems);

		// Each group of the pass is one batch. Met front to back, a group's first item is its nearest, and its first
		// submission is its lowest item index.
		m_GroupBatches.assign(m_GroupIndices.size(), GroupBatch{});
		m_PassGroups.clear();
		for (uint32_t const index : m_PassItems)
		{
			GroupBatch& group = m_GroupBatches[m_Items[index].Group];
			if (group.Count++ == 0)
			{
				m_PassGroups.push_back(m_Items[index].Group);
				group.Nearest = m_Items[index].ViewDepth;
				group.FirstItem = index;
			}
			group.FirstItem = std::min(group.FirstItem, index);
		}
		// Batches front to back by their nearest instance, ties by their first submission.
		std::sort(m_PassGroups.begin(), m_PassGroups.end(),
		          [this](uint32_t a, uint32_t b)
		          {
					  GroupBatch const& left = m_GroupBatches[a];
					  GroupBatch const& right = m_GroupBatches[b];
					  return left.Nearest != right.Nearest ? left.Nearest < right.Nearest : left.FirstItem < right.FirstItem;
				  });
		uint32_t offset = 0;
		for (uint32_t const groupIndex : m_PassGroups)
		{
			GroupBatch& group = m_GroupBatches[groupIndex];
			group.Cursor = offset;
			SubmeshItem const& first = m_Items[group.FirstItem];
			m_OpaqueBatches.push_back(
				MakeBatch(first, m_FrameMaterials[first.Material].Pipelines[MirrorVariant(first.Mirrored)], offset, group.Count));
			offset += group.Count;
		}
		// Each run holds its instances front to back, as the pass's items are ordered.
		m_InstanceIndices.resize(offset);
		for (uint32_t const index : m_PassItems)
		{
			m_InstanceIndices[m_GroupBatches[m_Items[index].Group].Cursor++] = m_Items[index].Instance;
		}

		// The entity-ID pass draws the opaque batches' runs again, in the order of their first submission: the order changes
		// no ID, the depth test keeps the nearest surface.
		if (m_EntityIdsEnabled)
		{
			std::sort(m_PassGroups.begin(), m_PassGroups.end(),
			          [this](uint32_t a, uint32_t b)
			          {
						  return m_GroupBatches[a].FirstItem < m_GroupBatches[b].FirstItem;
					  });
			for (uint32_t const groupIndex : m_PassGroups)
			{
				GroupBatch const& group = m_GroupBatches[groupIndex];
				SubmeshItem const& first = m_Items[group.FirstItem];
				nvrhi::IGraphicsPipeline* pipeline = m_FrameMaterials[first.Material].EntityIdPipelines[MirrorVariant(first.Mirrored)];
				if (pipeline != nullptr)
				{
					// The run's cursor ended at its end.
					m_EntityIdBatches.push_back(MakeBatch(first, pipeline, group.Cursor - group.Count, group.Count));
				}
			}
		}

		BuildShadowBatches();

		// Blended items are never batched: one instance each, back to front (ties by submission), in the transparent pass
		// and then in the entity-ID pass.
		m_SortKeys.clear();
		for (uint32_t index = 0; index < m_Items.size(); index++)
		{
			SubmeshItem const& item = m_Items[index];
			if (item.InView && IsDrawable(item) && m_FrameMaterials[item.Material].Blend)
			{
				m_SortKeys.push_back(MakeSortKey(-item.ViewDepth, index));
			}
		}
		TakeSortedItems(m_SortKeys, m_SortScratch, m_PassItems);
		for (uint32_t const index : m_PassItems)
		{
			SubmeshItem const& item = m_Items[index];
			FrameMaterial const& material = m_FrameMaterials[item.Material];
			uint32_t const run = static_cast<uint32_t>(m_InstanceIndices.size());
			m_InstanceIndices.push_back(item.Instance);
			size_t const variant = MirrorVariant(item.Mirrored);
			m_BlendBatches.push_back(MakeBatch(item, material.Pipelines[variant], run, 1));
			if (material.EntityIdPipelines[variant] != nullptr)
			{
				m_EntityIdBatches.push_back(MakeBatch(item, material.EntityIdPipelines[variant], run, 1));
			}
		}
	}

	void SceneRenderer::BuildShadowBatches()
	{
		if (m_ShadowViews.empty())
		{
			return;
		}

		// The opaque pass left its state in the groups.
		m_GroupBatches.assign(m_GroupIndices.size(), GroupBatch{});
		ShadowView const* previous = nullptr;
		for (ShadowView& view : m_ShadowViews)
		{
			// A point light's faces share its range sphere, so the same casters reach each of them: they draw the same runs.
			if (previous != nullptr && !view.Directional && !previous->Directional && view.Center == previous->Center &&
			    view.Radius == previous->Radius)
			{
				view.FirstBatch = previous->FirstBatch;
				view.BatchCount = previous->BatchCount;
				previous = &view;
				continue;
			}
			previous = &view;

			// The casters that reach the view, in submission order; met in that order, a group's first item is its first
			// submission.
			m_PassItems.clear();
			m_PassGroups.clear();
			Shadows::LightSpaceCascadeTest const cascade(view.Projection);
			for (uint32_t index = 0; index < m_ShadowCasters.size(); index++)
			{
				ShadowCaster const& caster = m_ShadowCasters[index];
				bool const visible =
					view.Directional ? cascade.Intersects(caster.LightBox) : IntersectsSphere(caster.Bounds, view.Center, view.Radius);
				if (!visible)
				{
					continue;
				}
				m_PassItems.push_back(index);
				GroupBatch& group = m_GroupBatches[caster.Group];
				if (group.Count++ == 0)
				{
					m_PassGroups.push_back(caster.Group);
					group.FirstItem = caster.Item;
				}
			}

			view.FirstBatch = static_cast<uint32_t>(m_ShadowBatches.size());
			uint32_t offset = static_cast<uint32_t>(m_InstanceIndices.size());
			for (uint32_t const groupIndex : m_PassGroups)
			{
				GroupBatch& group = m_GroupBatches[groupIndex];
				group.Cursor = offset;
				SubmeshItem const& first = m_Items[group.FirstItem];
				m_ShadowBatches.push_back(
					MakeBatch(first, m_FrameMaterials[first.Material].ShadowPipelines[MirrorVariant(first.Mirrored)], offset, group.Count));
				offset += group.Count;
			}
			view.BatchCount = static_cast<uint32_t>(m_ShadowBatches.size()) - view.FirstBatch;
			m_InstanceIndices.resize(offset);
			for (uint32_t const index : m_PassItems)
			{
				ShadowCaster const& caster = m_ShadowCasters[index];
				m_InstanceIndices[m_GroupBatches[caster.Group].Cursor++] = caster.Instance;
			}
			// The next view counts its groups from zero.
			for (uint32_t const groupIndex : m_PassGroups)
			{
				m_GroupBatches[groupIndex] = GroupBatch{};
			}
		}
	}

	void SceneRenderer::CountBatch(MeshBatch const& batch)
	{
		m_Statistics.DrawCalls++;
		m_Statistics.Batches++;
		m_Statistics.Instances += batch.InstanceCount;
		m_Statistics.Triangles += batch.IndexCount / 3 * batch.InstanceCount;
	}

	uint32_t SceneRenderer::DrawBatches(nvrhi::ICommandList* commandList, std::span<MeshBatch const> batches,
	                                    nvrhi::IFramebuffer* framebuffer, nvrhi::IBindingSet* passBindings)
	{
		nvrhi::Viewport const viewport(static_cast<float>(m_Width), static_cast<float>(m_Height));
		uint32_t instances = 0;
		for (MeshBatch const& batch : batches)
		{
			nvrhi::GraphicsState state;
			state.pipeline = batch.Pipeline;
			state.framebuffer = framebuffer;
			state.viewport.addViewportAndScissorRect(viewport);
			state.bindings = {passBindings, batch.MaterialBindings};
			state.vertexBuffers = {nvrhi::VertexBufferBinding().setBuffer(batch.Mesh->VertexBuffer).setSlot(0).setOffset(0)};
			state.indexBuffer =
				nvrhi::IndexBufferBinding().setBuffer(batch.Mesh->IndexBuffer).setFormat(nvrhi::Format::R32_UINT).setOffset(0);
			commandList->setGraphicsState(state);

			// The opaque, transparent and entity-ID constants share their layout.
			static_assert(sizeof(ShaderInterop::DrawConstants) == sizeof(ShaderInterop::EntityIdDrawConstants));
			ShaderInterop::DrawConstants constants{};
			constants.InstanceOffset = batch.FirstInstance;
			commandList->setPushConstants(&constants, sizeof(constants));

			// SV_InstanceID counts from 0: the run's offset is in the push constants.
			nvrhi::DrawArguments arguments;
			arguments.vertexCount = batch.IndexCount;
			arguments.instanceCount = batch.InstanceCount;
			arguments.startIndexLocation = batch.BaseIndex;
			arguments.startVertexLocation = batch.BaseVertex;
			arguments.startInstanceLocation = 0;
			commandList->drawIndexed(arguments);

			CountBatch(batch);
			instances += batch.InstanceCount;
		}
		return instances;
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
		// The frame's casters, in submission order: the cascades reach to their bounds, and each shadow view tests them.
		AABB casterBounds;
		m_ShadowCasters.clear();
		for (uint32_t index = 0; index < m_Items.size(); index++)
		{
			SubmeshItem const& item = m_Items[index];
			// A caster without bounds (an empty submesh) reaches no view.
			if (IsShadowCaster(item) && item.WorldBounds.IsValid())
			{
				casterBounds.Expand(item.WorldBounds);
				ShadowCaster& caster = m_ShadowCasters.emplace_back();
				caster.Bounds = item.WorldBounds;
				caster.Item = index;
				caster.Group = item.Group;
				caster.Instance = item.Instance;
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
				// The cascades share the light's view: each caster is moved into it once, for all of them.
				if (cascade == 0)
				{
					Shadows::LightSpaceTransform const lightSpace(projection.LightView);
					for (ShadowCaster& caster : m_ShadowCasters)
					{
						caster.LightBox = lightSpace.Apply(caster.Bounds);
					}
				}

				constants.CascadeViewProjection[cascade] = projection.ViewProjection;
				constants.CascadeSplits[static_cast<int>(cascade)] = splits[cascade];
				constants.CascadeTexelSizes[static_cast<int>(cascade)] = projection.TexelSize;
				constants.CascadeWidths[static_cast<int>(cascade)] = projection.Width;
				constants.CascadeDepthRanges[static_cast<int>(cascade)] = projection.DepthRange;

				ShadowView view;
				view.ViewProjection = projection.ViewProjection;
				view.Projection = projection.Projection;
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
		m_PassInstanceCounts.ShadowViews.assign(m_ShadowViews.size(), 0u);
		for (size_t viewIndex = 0; viewIndex < m_ShadowViews.size(); viewIndex++)
		{
			ShadowView const& view = m_ShadowViews[viewIndex];
			nvrhi::IFramebuffer* framebuffer = view.Framebuffer;
			nvrhi::FramebufferAttachment const& attachment = framebuffer->getDesc().depthAttachment;
			commandList->clearDepthStencilTexture(attachment.texture, attachment.subresources, true, 0.0f, false, 0);
			nvrhi::FramebufferInfoEx const& info = framebuffer->getFramebufferInfo();
			m_Statistics.ShadowMapViews++;

			nvrhi::Viewport const viewport = info.getViewport();
			std::span<MeshBatch const> const batches =
				std::span<MeshBatch const>(m_ShadowBatches).subspan(view.FirstBatch, view.BatchCount);
			uint32_t instances = 0;
			for (MeshBatch const& batch : batches)
			{
				nvrhi::GraphicsState state;
				state.pipeline = batch.Pipeline;
				state.framebuffer = framebuffer;
				state.viewport.addViewportAndScissorRect(viewport);
				state.bindings = {m_ShadowBindings, batch.MaterialBindings};
				state.vertexBuffers = {nvrhi::VertexBufferBinding().setBuffer(batch.Mesh->VertexBuffer).setSlot(0).setOffset(0)};
				state.indexBuffer =
					nvrhi::IndexBufferBinding().setBuffer(batch.Mesh->IndexBuffer).setFormat(nvrhi::Format::R32_UINT).setOffset(0);
				commandList->setGraphicsState(state);

				ShaderInterop::ShadowDrawConstants constants{};
				constants.ViewProjection = view.ViewProjection;
				constants.InstanceOffset = batch.FirstInstance;
				commandList->setPushConstants(&constants, sizeof(constants));

				nvrhi::DrawArguments arguments;
				arguments.vertexCount = batch.IndexCount;
				arguments.instanceCount = batch.InstanceCount;
				arguments.startIndexLocation = batch.BaseIndex;
				arguments.startVertexLocation = batch.BaseVertex;
				arguments.startInstanceLocation = 0;
				commandList->drawIndexed(arguments);

				CountBatch(batch);
				m_Statistics.ShadowDrawCalls++;
				instances += batch.InstanceCount;
			}
			m_PassInstanceCounts.ShadowViews[viewIndex] = instances;
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

	void SceneRenderer::CullToView()
	{
		// Casters outside the view can still shadow what it shows: the shadow pass culls them against each light's volume.
		for (SubmeshItem& item : m_Items)
		{
			item.InView = m_ViewFrustum.Intersects(item.WorldBounds);
			m_Statistics.Culled += item.InView ? 0u : 1u;
		}
	}

	SceneRenderer::FrameTimer* SceneRenderer::AcquireFrameTimer()
	{
		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		// Oldest first, so the newest finished frame's time is the one kept. Finished frames are read whether or not the
		// profiler still records, so that their pass timers are free again.
		for (size_t i = 0; i < FrameTimerCount; i++)
		{
			FrameTimer& timer = m_FrameTimers[(m_NextFrameTimer + i) % FrameTimerCount];
			if (timer.Pending && device->pollTimerQuery(timer.Query))
			{
				m_GpuMilliseconds = device->getTimerQueryTime(timer.Query) * 1000.0f;
				device->resetTimerQuery(timer.Query);
				timer.Pending = false;
				ReportPassTimes(timer);
			}
		}
		FrameTimer& timer = m_FrameTimers[m_NextFrameTimer];
		if (!timer.Query || timer.Pending)
		{
			return nullptr;
		}
		m_NextFrameTimer = (m_NextFrameTimer + 1) % FrameTimerCount;
		timer.Pending = true;
		timer.PassCount = 0;
		if (Profiler::IsRecording() && !m_PassTimersUnavailable)
		{
			if (timer.Passes.empty())
			{
				// All or none: a frame's passes are timed only when every pass has a timer.
				std::vector<PassTimer> passes(MaxTimedPasses);
				bool created = true;
				for (PassTimer& pass : passes)
				{
					pass.Query = device->createTimerQuery();
					created = created && pass.Query != nullptr;
				}
				if (created)
				{
					timer.Passes = std::move(passes);
				}
				else
				{
					m_PassTimersUnavailable = true;
				}
			}
			if (!timer.Passes.empty())
			{
				timer.ProfilerFrame = Profiler::GetFrameIndex();
				m_PassTimers = &timer;
			}
		}
		return &timer;
	}

	void SceneRenderer::ReportPassTimes(FrameTimer& timer)
	{
		if (timer.PassCount == 0)
		{
			return;
		}
		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		std::array<ProfilerGpuPassTiming, MaxTimedPasses> timings{};
		uint32_t count = 0;
		for (uint32_t index = 0; index < timer.PassCount; index++)
		{
			PassTimer const& pass = timer.Passes[index];
			// The frame's timer ended after its passes' timers, so theirs are ready; one that is not is left out.
			if (device->pollTimerQuery(pass.Query))
			{
				double const seconds = std::max(static_cast<double>(device->getTimerQueryTime(pass.Query)), 0.0);
				timings[count++] = {pass.Name, static_cast<uint64_t>(std::llround(seconds * 1.0e9))};
			}
			device->resetTimerQuery(pass.Query);
		}
		timer.PassCount = 0;
		// The profiler drops passes of a frame it does not hold (the recording stopped, or the frame left its history).
		Profiler::RecordGpuPasses(timer.ProfilerFrame, m_ViewName, std::span<ProfilerGpuPassTiming const>(timings.data(), count));
	}

	SceneRenderer::PassScope::PassScope(SceneRenderer& renderer, nvrhi::ICommandList* commandList, char const* name)
		: m_CommandList(commandList)
	{
		m_CommandList->beginMarker(name);
		FrameTimer* const timers = renderer.m_PassTimers;
		if (timers != nullptr && timers->PassCount < timers->Passes.size())
		{
			PassTimer& pass = timers->Passes[timers->PassCount++];
			pass.Name = name;
			m_Timer = pass.Query;
			m_CommandList->beginTimerQuery(m_Timer);
		}
	}

	SceneRenderer::PassScope::~PassScope()
	{
		if (m_Timer != nullptr)
		{
			m_CommandList->endTimerQuery(m_Timer);
		}
		m_CommandList->endMarker();
	}

	void SceneRenderer::EndScene()
	{
		ST_CORE_ASSERT(m_InScene, "EndScene without BeginScene");
		ST_PROFILE_SCOPE("Renderer.EndScene");
		m_InScene = false;
		if (m_TargetsDirty)
		{
			CreateTargets();
		}
		m_Statistics.Lights = static_cast<uint32_t>(m_Lights.size());
		{
			ST_PROFILE_SCOPE("Renderer.Cull");
			CullToView();
		}

		ShaderInterop::ShadowConstants shadows{};
		GpuEnvironment const* environment = nullptr;
		{
			ST_PROFILE_SCOPE("Renderer.Prepare");
			// Uploads of meshes, textures, materials and font atlases submit their own command lists, so they happen before
			// recording.
			ResolveFrameResources(m_Settings.Shadows && (m_ShadowedDirectionalLight >= 0 || !m_LocalShadowLights.empty()));
			m_Quads->Prepare(m_Camera.Position, glm::uvec2(m_Width, m_Height));
			m_Statistics.Quads = m_Quads->GetQuadCount();
			m_Statistics.Triangles += 2 * m_Quads->GetQuadCount();

			PrepareShadows(shadows);
			BuildBatches();
			// The frame draws its meshes only when the instance buffers hold them (the error is logged); the shadow maps are
			// still cleared.
			if (!EnsureInstanceBuffers(m_Instances.size(), m_InstanceIndices.size()))
			{
				m_OpaqueBatches.clear();
				m_EntityIdBatches.clear();
				m_BlendBatches.clear();
				m_ShadowBatches.clear();
				for (ShadowView& view : m_ShadowViews)
				{
					view.FirstBatch = 0;
					view.BatchCount = 0;
				}
			}
			if (!m_Lines.empty())
			{
				CreateLineLayer();
			}

			environment = m_Environment.Environment ? Renderer::GetEnvironment(m_Environment.Environment) : nullptr;
			FrameResources resources;
			nvrhi::ITexture* fallbackCube = Renderer::GetFallbackCube();
			resources.Irradiance = environment != nullptr ? environment->Irradiance.Get() : fallbackCube;
			resources.Prefiltered = environment != nullptr ? environment->Prefiltered.Get() : fallbackCube;
			resources.Radiance = environment != nullptr ? environment->Radiance.Get() : fallbackCube;
			resources.CascadeShadowMap = m_CascadeShadowMap ? m_CascadeShadowMap.Get() : m_FallbackShadowMap.Get();
			resources.LocalShadowMap = m_LocalShadowMap ? m_LocalShadowMap.Get() : m_FallbackShadowMap.Get();
			resources.Instances = m_InstanceBuffer;
			resources.InstanceIndices = m_InstanceIndexBuffer;
			UpdateFrameBindings(resources);
		}

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

		ShaderInterop::OverlayConstants overlay{};
		overlay.ViewProjection = frame.ViewProjection;
		overlay.InverseViewProjection = frame.InverseViewProjection;
		overlay.CameraPosition = m_Camera.Position;
		overlay.ViewportSize = frame.ViewportSize;

		// Recording, submitting and the end of the frame.
		ST_PROFILE_SCOPE("Renderer.Record");
		m_PassTimers = nullptr;
		FrameTimer* const frameTimer = AcquireFrameTimer();
		m_Statistics.GpuMilliseconds = m_GpuMilliseconds;

		nvrhi::ICommandList* commandList = m_CommandList;
		commandList->open();
		if (frameTimer != nullptr)
		{
			commandList->beginTimerQuery(frameTimer->Query);
		}
		{
			PassScope const pass(*this, commandList, "Upload");
			commandList->writeBuffer(m_FrameConstants, &frame, sizeof(frame));
			commandList->writeBuffer(m_OverlayConstants, &overlay, sizeof(overlay));
			commandList->writeBuffer(m_ShadowConstants, &shadows, sizeof(shadows));
			if (!m_Lights.empty())
			{
				commandList->writeBuffer(m_LightBuffer, m_Lights.data(), m_Lights.size() * sizeof(ShaderInterop::LightData));
			}
			if (!m_Instances.empty() && m_Instances.size() <= GetCapacity(m_InstanceBuffer))
			{
				commandList->writeBuffer(m_InstanceBuffer, m_Instances.data(), m_Instances.size() * sizeof(ShaderInterop::InstanceData));
			}
			if (!m_InstanceIndices.empty() && m_InstanceIndices.size() <= GetCapacity(m_InstanceIndexBuffer))
			{
				commandList->writeBuffer(m_InstanceIndexBuffer, m_InstanceIndices.data(), m_InstanceIndices.size() * sizeof(uint32_t));
			}
			m_Quads->Upload(commandList);
		}

		if (!m_ShadowViews.empty())
		{
			PassScope const pass(*this, commandList, "Shadows");
			RenderShadows(commandList);
		}

		{
			PassScope const pass(*this, commandList, "Opaque");
			// Without a sky, the background is the ambient light.
			glm::vec3 const background = m_AmbientRadiance * exposure;
			commandList->clearTextureFloat(m_ColorTarget, nvrhi::AllSubresources,
			                               nvrhi::Color(background.r, background.g, background.b, 1.0f));
			commandList->clearDepthStencilTexture(m_DepthTarget, nvrhi::AllSubresources, true, 0.0f, false, 0);
			commandList->clearTextureFloat(m_NormalTarget, nvrhi::AllSubresources, nvrhi::Color(0.0f));
			commandList->clearTextureFloat(m_IndirectTarget, nvrhi::AllSubresources, nvrhi::Color(0.0f));
			m_PassInstanceCounts.Opaque = DrawBatches(commandList, m_OpaqueBatches, m_OpaqueFramebuffer, m_FrameBindings);
		}
		if (m_Settings.AmbientOcclusion)
		{
			PassScope const pass(*this, commandList, "AmbientOcclusion");
			RenderAmbientOcclusion(commandList);
		}
		if (environment != nullptr && m_Environment.DrawSkybox)
		{
			PassScope const pass(*this, commandList, "Sky");
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
			m_Statistics.DrawCalls++;
		}
		{
			PassScope const pass(*this, commandList, "Transparent");
			m_PassInstanceCounts.Transparent = DrawBatches(commandList, m_BlendBatches, m_SceneFramebuffer, m_FrameBindings);
			m_Statistics.DrawCalls += m_Quads->RenderWorld(commandList, m_SceneFramebuffer, frame.ViewProjection);
		}
		bool pickCopied = false;
		if (m_EntityIdsEnabled)
		{
			PassScope const pass(*this, commandList, "EntityIds");
			RenderEntityIds(commandList);
			if (m_PickRequest && !m_PickInFlight)
			{
				glm::uvec2 const pixel = *m_PickRequest;
				m_PickRequest.reset();
				if (pixel.x < m_Width && pixel.y < m_Height)
				{
					commandList->copyTexture(m_PickStaging, nvrhi::TextureSlice().setWidth(1).setHeight(1), m_EntityIdTarget,
					                         nvrhi::TextureSlice().setOrigin(pixel.x, pixel.y).setWidth(1).setHeight(1));
					pickCopied = true;
				}
				else
				{
					m_PickResult = 0u;
				}
			}
		}
		else if (m_PickRequest)
		{
			m_PickRequest.reset();
			m_PickResult = 0u;
		}

		float const bloomIntensity = m_Settings.Bloom ? std::clamp(m_Settings.BloomIntensity, 0.0f, 1.0f) : 0.0f;
		if (bloomIntensity > 0.0f)
		{
			PassScope const pass(*this, commandList, "Bloom");
			RenderBloom(commandList);
		}

		{
			PassScope const pass(*this, commandList, "Tonemap");
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
			tonemap.Contrast = std::clamp(m_Settings.Contrast, 0.0f, 2.0f);
			tonemap.Saturation = std::clamp(m_Settings.Saturation, 0.0f, 2.0f);
			tonemap.Temperature = std::clamp(m_Settings.Temperature, -1.0f, 1.0f);
			tonemap.Tint = std::clamp(m_Settings.Tint, -1.0f, 1.0f);
			tonemap.ColorFilter = glm::clamp(m_Settings.ColorFilter, glm::vec3(0.0f), glm::vec3(2.0f));
			tonemap.Vignette = std::clamp(m_Settings.Vignette, 0.0f, 1.0f);
			tonemap.VignetteSmoothness = std::clamp(m_Settings.VignetteSmoothness, 0.01f, 1.0f);
			tonemap.ChromaticAberration = std::clamp(m_Settings.ChromaticAberration, 0.0f, 1.0f);
			tonemap.FilmGrain = std::clamp(m_Settings.FilmGrain, 0.0f, 1.0f);
			tonemap.Frame = m_PostFrame++;
			commandList->setPushConstants(&tonemap, sizeof(tonemap));
			commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
			m_Statistics.DrawCalls++;
		}

		{
			PassScope const pass(*this, commandList, "Overlays");
			RenderOverlays(commandList, m_Settings.FXAA);
		}

		if (m_Settings.FXAA)
		{
			PassScope const pass(*this, commandList, "FXAA");
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
			m_Statistics.DrawCalls++;
		}
		if (m_EntityIdsEnabled && m_HasSelection)
		{
			PassScope const pass(*this, commandList, "SelectionOutline");
			RenderSelectionOutline(commandList);
		}
		{
			// Screen-space sprites and text cover everything else.
			PassScope const pass(*this, commandList, "ScreenSpace");
			m_Statistics.DrawCalls += m_Quads->RenderScreen(commandList, m_FinalFramebuffer);
		}

		if (frameTimer != nullptr)
		{
			commandList->endTimerQuery(frameTimer->Query);
		}
		m_PassTimers = nullptr;
		commandList->close();
		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		device->executeCommandList(commandList);
		if (pickCopied)
		{
			device->resetEventQuery(m_PickQuery);
			device->setEventQuery(m_PickQuery, nvrhi::CommandQueue::Graphics);
			m_PickInFlight = true;
		}

		// The frame's tables keep its meshes and materials alive only for this frame; its instances stay readable
		// (GetFrameInstances) until the next BeginScene.
		m_Items.clear();
		m_MeshDraws.clear();
		m_DrawSubmeshes.clear();
		m_FrameMeshes.clear();
		m_FrameMaterials.clear();
		m_FrameMeshIndices.clear();
		m_FrameMaterialIndices.clear();
		m_DefaultMaterial.reset();
		m_GroupIndices.clear();
		m_OpaqueBatches.clear();
		m_EntityIdBatches.clear();
		m_BlendBatches.clear();
		m_ShadowBatches.clear();
		m_ShadowViews.clear();
		m_Lines.clear();
		m_Quads->Clear();
		Renderer::CollectGarbage();
		// An idle profiler costs one relaxed load here.
		if (Profiler::IsRecording())
		{
			RecordProfilerCounters();
		}
	}

	void SceneRenderer::RecordProfilerCounters() const
	{
		std::array<ProfilerCounter, 11> const counters = {{
			{"DrawCalls", m_Statistics.DrawCalls},
			{"Batches", m_Statistics.Batches},
			{"Instances", m_Statistics.Instances},
			{"Triangles", m_Statistics.Triangles},
			{"ShadowDrawCalls", m_Statistics.ShadowDrawCalls},
			{"ShadowMapViews", m_Statistics.ShadowMapViews},
			{"ShadowsDropped", m_Statistics.ShadowsDropped},
			{"Lights", m_Statistics.Lights},
			{"CulledLights", m_Statistics.CulledLights},
			{"Culled", m_Statistics.Culled},
			{"Quads", m_Statistics.Quads},
		}};
		Profiler::RecordCounters(m_ViewName, counters);
	}

	void SceneRenderer::RenderEntityIds(nvrhi::ICommandList* commandList)
	{
		commandList->clearTextureUInt(m_EntityIdTarget, nvrhi::AllSubresources, 0u);
		commandList->clearDepthStencilTexture(m_EntityIdDepth, nvrhi::AllSubresources, true, 0.0f, false, 0);

		m_PassInstanceCounts.EntityIds = DrawBatches(commandList, m_EntityIdBatches, m_EntityIdFramebuffer, m_EntityIdBindings);
		m_Statistics.DrawCalls += m_Quads->RenderEntityIds(commandList, m_EntityIdFramebuffer, m_Camera.Projection * m_Camera.View);
	}

	void SceneRenderer::RenderOverlays(nvrhi::ICommandList* commandList, bool ldrTarget)
	{
		nvrhi::Viewport const viewport(static_cast<float>(m_Width), static_cast<float>(m_Height));

		GridOverlay const& grid = m_Overlays.Grid;
		if (grid.Enabled && m_GridPipeline && grid.CellSize > 0.0f && grid.FadeDistance > 0.0f)
		{
			nvrhi::GraphicsState state;
			state.pipeline = m_GridPipeline;
			state.framebuffer = ldrTarget ? m_LdrFramebuffer : m_FinalFramebuffer;
			state.viewport.addViewportAndScissorRect(viewport);
			state.bindings = {m_GridBindings};
			commandList->setGraphicsState(state);
			ShaderInterop::GridConstants constants{};
			constants.MinorColor = grid.MinorColor;
			constants.MajorColor = grid.MajorColor;
			constants.AxisXColor = grid.AxisXColor;
			constants.AxisZColor = grid.AxisZColor;
			constants.CellSize = grid.CellSize;
			constants.MajorLineEvery = static_cast<float>(std::max(grid.MajorLineEvery, 1u));
			constants.FadeDistance = grid.FadeDistance;
			commandList->setPushConstants(&constants, sizeof(constants));
			commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
			m_Statistics.DrawCalls++;
		}

		// UI placed in the world (the editor's scene view shows canvases this way): over the grid, under the lines, and hidden
		// behind surfaces.
		m_Statistics.DrawCalls += m_Quads->RenderWorldUI(commandList, ldrTarget ? m_LdrWorldUIFramebuffer : m_FinalWorldUIFramebuffer,
		                                                 m_Camera.Projection * m_Camera.View);

		if (m_Lines.empty() || !m_LineLayerFramebuffer || !m_LineCompositePipeline)
		{
			return;
		}
		uint64_t const requiredSize = m_Lines.size() * sizeof(LineInstance);
		if (!m_LineBuffer || m_LineBuffer->getDesc().byteSize < requiredSize)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = std::bit_ceil(std::max<uint64_t>(requiredSize, 4096));
			desc.isVertexBuffer = true;
			desc.initialState = nvrhi::ResourceStates::VertexBuffer;
			desc.keepInitialState = true;
			desc.debugName = "Debug lines";
			m_LineBuffer = GraphicsDevice::GetDevice()->createBuffer(desc);
			if (!m_LineBuffer)
			{
				ST_CORE_ERROR("Failed to create the debug line buffer");
				return;
			}
		}
		commandList->writeBuffer(m_LineBuffer, m_Lines.data(), requiredSize);

		ShaderInterop::LineConstants constants{};
		constants.OutlineColor = m_Overlays.LineOutlineColor;
		constants.Width = std::clamp(m_Overlays.LineWidth, SceneRendererOverlays::MinLineWidth, SceneRendererOverlays::MaxLineWidth);
		constants.OutlineWidth = std::clamp(m_Overlays.LineOutlineWidth, 0.0f, SceneRendererOverlays::MaxLineOutlineWidth);
		constants.OccludedOpacity = std::clamp(m_Overlays.OccludedLineOpacity, 0.0f, 1.0f);

		commandList->clearTextureFloat(m_LineLayer, nvrhi::AllSubresources, nvrhi::Color(0.0f));
		commandList->clearDepthStencilTexture(m_LineLayerDepth, nvrhi::AllSubresources, true, 1.0f, false, 0);
		nvrhi::GraphicsState lineState;
		lineState.pipeline = m_LinePipeline;
		lineState.framebuffer = m_LineLayerFramebuffer;
		lineState.viewport.addViewportAndScissorRect(viewport);
		lineState.bindings = {m_LineBindings};
		lineState.vertexBuffers = {nvrhi::VertexBufferBinding().setBuffer(m_LineBuffer).setSlot(0).setOffset(0)};
		commandList->setGraphicsState(lineState);
		commandList->setPushConstants(&constants, sizeof(constants));
		commandList->draw(nvrhi::DrawArguments().setVertexCount(6).setInstanceCount(static_cast<uint32_t>(m_Lines.size())));
		m_Statistics.DrawCalls++;

		nvrhi::GraphicsState compositeState;
		compositeState.pipeline = m_LineCompositePipeline;
		compositeState.framebuffer = ldrTarget ? m_LdrFramebuffer : m_FinalFramebuffer;
		compositeState.viewport.addViewportAndScissorRect(viewport);
		compositeState.bindings = {m_LineCompositeBindings};
		commandList->setGraphicsState(compositeState);
		commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
		m_Statistics.DrawCalls++;
	}

	void SceneRenderer::RenderSelectionOutline(nvrhi::ICommandList* commandList)
	{
		if (!m_OutlinePipeline || !m_OutlineBindings)
		{
			return;
		}
		nvrhi::GraphicsState state;
		state.pipeline = m_OutlinePipeline;
		state.framebuffer = m_FinalFramebuffer;
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(m_Width), static_cast<float>(m_Height)));
		state.bindings = {m_OutlineBindings};
		commandList->setGraphicsState(state);
		ShaderInterop::OutlineConstants constants{};
		constants.Color = m_Overlays.SelectionColor;
		constants.Radius = static_cast<int32_t>(std::clamp(m_Overlays.SelectionOutlineWidth, 1u, MaxOutlineWidth));
		commandList->setPushConstants(&constants, sizeof(constants));
		commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
		m_Statistics.DrawCalls++;
	}
}
