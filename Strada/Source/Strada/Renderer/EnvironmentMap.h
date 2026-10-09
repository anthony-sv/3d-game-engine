#pragma once

#include "Strada/Core/Image.h"
#include "Strada/Core/Result.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>

namespace Strada
{
	// GPU image-based lighting data of one environment (RGBA16F cubemaps).
	struct GpuEnvironment
	{
		// The environment itself, with a box-filtered mip chain (sky and blur).
		nvrhi::TextureHandle Radiance;
		uint32_t RadianceMipCount = 0;
		// Cosine-weighted average radiance per normal (diffuse).
		nvrhi::TextureHandle Irradiance;
		// GGX-prefiltered radiance, perceptual roughness 0..1 across the mips (specular).
		nvrhi::TextureHandle Prefiltered;
		uint32_t PrefilteredMipCount = 0;
	};

	// Precomputes image-based lighting with compute shaders. Main thread only; requires the GraphicsDevice.
	class EnvironmentProcessor
	{
	public:
		static constexpr uint32_t IrradianceSize = 32;
		static constexpr uint32_t PrefilteredSize = 256;
		static constexpr uint32_t PrefilteredMipCount = 6;
		static constexpr uint32_t BrdfLutSize = 128;

		[[nodiscard]] Result<void> Init();

		// Converts an equirectangular HDR image (RGBA float) into a GpuEnvironment.
		[[nodiscard]] Result<GpuEnvironment> Process(HdrImage const& equirect, nvrhi::ICommandList* commandList) const;
		// Split-sum DFG table: RG = (A, B) with specular = F0 * A + B, x = NoV, y = perceptual roughness.
		[[nodiscard]] Result<nvrhi::TextureHandle> CreateBrdfLut(nvrhi::ICommandList* commandList) const;

	private:
		nvrhi::TextureHandle CreateCube(uint32_t size, uint32_t mipLevels, char const* name) const;

		nvrhi::SamplerHandle m_Sampler;
		nvrhi::BindingLayoutHandle m_EquirectLayout;
		nvrhi::BindingLayoutHandle m_DownsampleLayout;
		nvrhi::BindingLayoutHandle m_ConvolutionLayout;
		nvrhi::BindingLayoutHandle m_LutLayout;
		nvrhi::ComputePipelineHandle m_EquirectPipeline;
		nvrhi::ComputePipelineHandle m_DownsamplePipeline;
		nvrhi::ComputePipelineHandle m_IrradiancePipeline;
		nvrhi::ComputePipelineHandle m_PrefilterPipeline;
		nvrhi::ComputePipelineHandle m_LutPipeline;
	};
}
