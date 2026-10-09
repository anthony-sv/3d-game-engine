#include "stpch.h"
#include "Strada/Renderer/EnvironmentMap.h"

#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/RHI/ShaderLibrary.h"
#include "Strada/Renderer/TextureMips.h"

#include "RendererInterop.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <bit>
#include <vector>

namespace Strada
{
	static_assert(sizeof(ShaderInterop::EnvironmentConstants) == 32);

	namespace
	{
		constexpr uint32_t GroupSize = 8;
		constexpr uint32_t IrradianceSamples = 512;
		constexpr uint32_t PrefilterSamples = 512;
		constexpr uint32_t LutSamples = 512;

		uint32_t Groups(uint32_t size)
		{
			return (size + GroupSize - 1) / GroupSize;
		}

		nvrhi::TextureSubresourceSet MipSlices(uint32_t mip)
		{
			return nvrhi::TextureSubresourceSet(mip, 1, 0, 6);
		}

		nvrhi::BindingLayoutHandle CreateLayout(std::vector<nvrhi::BindingLayoutItem> const& items)
		{
			nvrhi::BindingLayoutDesc desc;
			desc.visibility = nvrhi::ShaderType::Compute;
			desc.bindings = items;
			return GraphicsDevice::GetDevice()->createBindingLayout(desc);
		}

		nvrhi::ComputePipelineHandle CreatePipeline(char const* shader, nvrhi::IBindingLayout* layout)
		{
			nvrhi::ComputePipelineDesc desc;
			desc.CS = ShaderLibrary::Get(shader);
			desc.bindingLayouts = {layout};
			return desc.CS ? GraphicsDevice::GetDevice()->createComputePipeline(desc) : nullptr;
		}
	}

	Result<void> EnvironmentProcessor::Init()
	{
		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		nvrhi::SamplerDesc samplerDesc;
		samplerDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		// Equirect images wrap horizontally.
		samplerDesc.setAddressU(nvrhi::SamplerAddressMode::Wrap);
		m_Sampler = device->createSampler(samplerDesc);

		uint32_t const constantsSize = sizeof(ShaderInterop::EnvironmentConstants);
		m_EquirectLayout =
			CreateLayout({nvrhi::BindingLayoutItem::PushConstants(0, constantsSize), nvrhi::BindingLayoutItem::Texture_SRV(0),
		                  nvrhi::BindingLayoutItem::Sampler(0), nvrhi::BindingLayoutItem::Texture_UAV(0)});
		m_DownsampleLayout = CreateLayout({nvrhi::BindingLayoutItem::PushConstants(0, constantsSize),
		                                   nvrhi::BindingLayoutItem::Texture_SRV(1), nvrhi::BindingLayoutItem::Texture_UAV(0)});
		m_ConvolutionLayout =
			CreateLayout({nvrhi::BindingLayoutItem::PushConstants(0, constantsSize), nvrhi::BindingLayoutItem::Texture_SRV(2),
		                  nvrhi::BindingLayoutItem::Sampler(0), nvrhi::BindingLayoutItem::Texture_UAV(0)});
		m_LutLayout = CreateLayout({nvrhi::BindingLayoutItem::PushConstants(0, constantsSize), nvrhi::BindingLayoutItem::Texture_UAV(1)});

		m_EquirectPipeline = CreatePipeline("EnvironmentEquirectToCube_CS", m_EquirectLayout);
		m_DownsamplePipeline = CreatePipeline("EnvironmentDownsample_CS", m_DownsampleLayout);
		m_IrradiancePipeline = CreatePipeline("EnvironmentIrradiance_CS", m_ConvolutionLayout);
		m_PrefilterPipeline = CreatePipeline("EnvironmentPrefilter_CS", m_ConvolutionLayout);
		m_LutPipeline = CreatePipeline("EnvironmentBrdfLut_CS", m_LutLayout);
		if (!m_Sampler || !m_EquirectPipeline || !m_DownsamplePipeline || !m_IrradiancePipeline || !m_PrefilterPipeline || !m_LutPipeline)
		{
			return Error{"Failed to create the image-based lighting pipelines"};
		}
		return {};
	}

	nvrhi::TextureHandle EnvironmentProcessor::CreateCube(uint32_t size, uint32_t mipLevels, char const* name) const
	{
		nvrhi::TextureDesc desc;
		desc.dimension = nvrhi::TextureDimension::TextureCube;
		desc.width = size;
		desc.height = size;
		desc.arraySize = 6;
		desc.mipLevels = mipLevels;
		desc.format = nvrhi::Format::RGBA16_FLOAT;
		desc.isUAV = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = name;
		return GraphicsDevice::GetDevice()->createTexture(desc);
	}

	Result<GpuEnvironment> EnvironmentProcessor::Process(HdrImage const& equirect, nvrhi::ICommandList* commandList) const
	{
		if (equirect.IsEmpty() || equirect.GetChannels() != 4)
		{
			return Error{"environment images must be RGBA"};
		}
		nvrhi::IDevice* device = GraphicsDevice::GetDevice();

		// Upload as half floats (half the memory and bandwidth of 32-bit floats; ample range for HDRIs).
		nvrhi::TextureDesc sourceDesc;
		sourceDesc.width = equirect.GetWidth();
		sourceDesc.height = equirect.GetHeight();
		sourceDesc.format = nvrhi::Format::RGBA16_FLOAT;
		sourceDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		sourceDesc.keepInitialState = true;
		sourceDesc.debugName = "Environment equirect";
		nvrhi::TextureHandle source = device->createTexture(sourceDesc);

		// Radiance cube: a quarter of the equirect width (the same angular resolution), power of two, 16..1024.
		uint32_t const cubeSize = std::clamp(std::bit_floor(std::max(equirect.GetWidth() / 4, 16u)), 16u, 1024u);
		uint32_t const radianceMips = GetMipLevelCount(cubeSize, cubeSize);
		GpuEnvironment environment;
		environment.Radiance = CreateCube(cubeSize, radianceMips, "Environment radiance");
		environment.RadianceMipCount = radianceMips;
		environment.Irradiance = CreateCube(IrradianceSize, 1, "Environment irradiance");
		environment.Prefiltered = CreateCube(PrefilteredSize, PrefilteredMipCount, "Environment prefiltered");
		environment.PrefilteredMipCount = PrefilteredMipCount;
		if (!source || !environment.Radiance || !environment.Irradiance || !environment.Prefiltered)
		{
			return Error{"failed to create the environment textures"};
		}

		std::vector<uint16_t> halves(equirect.GetPixels().size());
		for (size_t i = 0; i < halves.size(); i++)
		{
			halves[i] = static_cast<uint16_t>(glm::packHalf1x16(std::min(equirect.GetPixels()[i], 60000.0f)));
		}

		commandList->open();
		commandList->writeTexture(source, 0, 0, halves.data(), static_cast<size_t>(equirect.GetWidth()) * 4 * sizeof(uint16_t));

		ShaderInterop::EnvironmentConstants constants{};
		auto const dispatch = [&](nvrhi::IComputePipeline* pipeline, nvrhi::BindingSetDesc const& bindings, nvrhi::IBindingLayout* layout,
		                          uint32_t size, uint32_t slices)
		{
			nvrhi::BindingSetHandle set = device->createBindingSet(bindings, layout);
			nvrhi::ComputeState state;
			state.pipeline = pipeline;
			state.bindings = {set};
			commandList->setComputeState(state);
			commandList->setPushConstants(&constants, sizeof(constants));
			commandList->dispatch(Groups(size), Groups(size), slices);
		};
		uint32_t const pushSize = sizeof(constants);

		constants.OutputSize = cubeSize;
		dispatch(m_EquirectPipeline,
		         nvrhi::BindingSetDesc()
		             .addItem(nvrhi::BindingSetItem::PushConstants(0, pushSize))
		             .addItem(nvrhi::BindingSetItem::Texture_SRV(0, source))
		             .addItem(nvrhi::BindingSetItem::Sampler(0, m_Sampler))
		             .addItem(nvrhi::BindingSetItem::Texture_UAV(0, environment.Radiance, nvrhi::Format::UNKNOWN, MipSlices(0),
		                                                         nvrhi::TextureDimension::Texture2DArray)),
		         m_EquirectLayout, cubeSize, 6);

		for (uint32_t mip = 1; mip < radianceMips; mip++)
		{
			constants.OutputSize = std::max(cubeSize >> mip, 1u);
			dispatch(m_DownsamplePipeline,
			         nvrhi::BindingSetDesc()
			             .addItem(nvrhi::BindingSetItem::PushConstants(0, pushSize))
			             .addItem(nvrhi::BindingSetItem::Texture_SRV(1, environment.Radiance, nvrhi::Format::UNKNOWN, MipSlices(mip - 1),
			                                                         nvrhi::TextureDimension::Texture2DArray))
			             .addItem(nvrhi::BindingSetItem::Texture_UAV(0, environment.Radiance, nvrhi::Format::UNKNOWN, MipSlices(mip),
			                                                         nvrhi::TextureDimension::Texture2DArray)),
			         m_DownsampleLayout, constants.OutputSize, 6);
		}

		nvrhi::BindingSetItem const radianceCube = nvrhi::BindingSetItem::Texture_SRV(
			2, environment.Radiance, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, nvrhi::TextureDimension::TextureCube);
		constants.SourceSize = static_cast<float>(cubeSize);
		constants.SourceMaxMip = static_cast<float>(radianceMips - 1);

		constants.OutputSize = IrradianceSize;
		constants.SampleCount = IrradianceSamples;
		dispatch(m_IrradiancePipeline,
		         nvrhi::BindingSetDesc()
		             .addItem(nvrhi::BindingSetItem::PushConstants(0, pushSize))
		             .addItem(radianceCube)
		             .addItem(nvrhi::BindingSetItem::Sampler(0, m_Sampler))
		             .addItem(nvrhi::BindingSetItem::Texture_UAV(0, environment.Irradiance, nvrhi::Format::UNKNOWN, MipSlices(0),
		                                                         nvrhi::TextureDimension::Texture2DArray)),
		         m_ConvolutionLayout, IrradianceSize, 6);

		constants.SampleCount = PrefilterSamples;
		for (uint32_t mip = 0; mip < PrefilteredMipCount; mip++)
		{
			constants.OutputSize = std::max(PrefilteredSize >> mip, 1u);
			constants.Roughness = static_cast<float>(mip) / static_cast<float>(PrefilteredMipCount - 1);
			dispatch(m_PrefilterPipeline,
			         nvrhi::BindingSetDesc()
			             .addItem(nvrhi::BindingSetItem::PushConstants(0, pushSize))
			             .addItem(radianceCube)
			             .addItem(nvrhi::BindingSetItem::Sampler(0, m_Sampler))
			             .addItem(nvrhi::BindingSetItem::Texture_UAV(0, environment.Prefiltered, nvrhi::Format::UNKNOWN, MipSlices(mip),
			                                                         nvrhi::TextureDimension::Texture2DArray)),
			         m_ConvolutionLayout, constants.OutputSize, 6);
		}

		commandList->close();
		device->executeCommandList(commandList);
		return environment;
	}

	Result<nvrhi::TextureHandle> EnvironmentProcessor::CreateBrdfLut(nvrhi::ICommandList* commandList) const
	{
		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		nvrhi::TextureDesc desc;
		desc.width = BrdfLutSize;
		desc.height = BrdfLutSize;
		desc.format = nvrhi::Format::RGBA16_FLOAT;
		desc.isUAV = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "BRDF LUT";
		nvrhi::TextureHandle lut = device->createTexture(desc);
		if (!lut)
		{
			return Error{"failed to create the BRDF LUT"};
		}

		ShaderInterop::EnvironmentConstants constants{};
		constants.OutputSize = BrdfLutSize;
		constants.SampleCount = LutSamples;
		nvrhi::BindingSetHandle set = device->createBindingSet(nvrhi::BindingSetDesc()
		                                                           .addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(constants)))
		                                                           .addItem(nvrhi::BindingSetItem::Texture_UAV(1, lut)),
		                                                       m_LutLayout);
		commandList->open();
		nvrhi::ComputeState state;
		state.pipeline = m_LutPipeline;
		state.bindings = {set};
		commandList->setComputeState(state);
		commandList->setPushConstants(&constants, sizeof(constants));
		commandList->dispatch(Groups(BrdfLutSize), Groups(BrdfLutSize), 1);
		commandList->close();
		device->executeCommandList(commandList);
		return lut;
	}
}
