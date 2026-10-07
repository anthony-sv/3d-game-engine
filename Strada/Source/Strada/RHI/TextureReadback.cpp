#include "stpch.h"
#include "Strada/RHI/TextureReadback.h"

#include "Strada/RHI/GraphicsDevice.h"

namespace Strada
{
	Result<Image> ReadbackTexture(nvrhi::ITexture* texture)
	{
		ST_CORE_ASSERT(GraphicsDevice::IsInitialized(), "ReadbackTexture requires an initialized GraphicsDevice");
		if (texture == nullptr)
		{
			return Error{"Cannot read back a null texture"};
		}

		nvrhi::TextureDesc const& sourceDesc = texture->getDesc();
		bool swapRedBlue = false;
		switch (sourceDesc.format)
		{
			case nvrhi::Format::RGBA8_UNORM:
			case nvrhi::Format::SRGBA8_UNORM:
				break;
			case nvrhi::Format::BGRA8_UNORM:
			case nvrhi::Format::SBGRA8_UNORM:
				swapRedBlue = true;
				break;
			default:
				return MakeError("Texture format {} cannot be read back as RGBA8", nvrhi::getFormatInfo(sourceDesc.format).name);
		}

		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		nvrhi::TextureDesc stagingDesc;
		stagingDesc.width = sourceDesc.width;
		stagingDesc.height = sourceDesc.height;
		stagingDesc.format = sourceDesc.format;
		stagingDesc.debugName = "Readback";
		nvrhi::StagingTextureHandle staging = device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
		if (!staging)
		{
			return Error{"Failed to create a readback staging texture"};
		}

		nvrhi::CommandListHandle commandList = device->createCommandList();
		commandList->open();
		commandList->copyTexture(staging, nvrhi::TextureSlice(), texture, nvrhi::TextureSlice());
		commandList->close();
		device->executeCommandList(commandList);
		device->waitForIdle();

		size_t rowPitch = 0;
		auto const* source =
			static_cast<uint8_t const*>(device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
		if (source == nullptr)
		{
			return Error{"Failed to map the readback staging texture"};
		}

		Image image(sourceDesc.width, sourceDesc.height, 4);
		for (uint32_t y = 0; y < sourceDesc.height; y++)
		{
			uint8_t const* sourceRow = source + y * rowPitch;
			uint8_t* destinationRow = image.GetPixel(0, y);
			for (uint32_t x = 0; x < sourceDesc.width; x++)
			{
				uint8_t const* pixel = sourceRow + x * 4;
				destinationRow[x * 4 + 0] = swapRedBlue ? pixel[2] : pixel[0];
				destinationRow[x * 4 + 1] = pixel[1];
				destinationRow[x * 4 + 2] = swapRedBlue ? pixel[0] : pixel[2];
				destinationRow[x * 4 + 3] = pixel[3];
			}
		}
		device->unmapStagingTexture(staging);
		return image;
	}
}
