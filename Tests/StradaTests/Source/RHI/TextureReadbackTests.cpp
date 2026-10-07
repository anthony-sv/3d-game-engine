#include "GpuTestUtilities.h"

#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/RHI/TextureReadback.h"

#include <doctest/doctest.h>

using namespace Strada;

namespace
{
	nvrhi::TextureHandle CreateClearedTarget(nvrhi::Format format, nvrhi::Color const& color)
	{
		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		nvrhi::TextureDesc desc;
		desc.width = 8;
		desc.height = 4;
		desc.format = format;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		desc.debugName = "ReadbackTarget";
		nvrhi::TextureHandle texture = device->createTexture(desc);
		REQUIRE(texture != nullptr);

		nvrhi::CommandListHandle commandList = device->createCommandList();
		commandList->open();
		commandList->clearTextureFloat(texture, nvrhi::AllSubresources, color);
		commandList->close();
		device->executeCommandList(commandList);
		// NVRHI's Vulkan clear does not record the texture as referenced by the command buffer, so a texture that is only
		// cleared must not be released before the GPU finished; wait so callers may drop it at any time.
		device->waitForIdle();
		return texture;
	}
}

TEST_CASE("RHI: texture readback returns RGBA8 pixels for RGBA and BGRA targets")
{
	ST_REQUIRE_GPU();

	// Red = 1, green = 0.5, blue = 0, alpha = 1 makes channel order mistakes visible.
	nvrhi::Color const color(1.0f, 0.5f, 0.0f, 1.0f);
	for (nvrhi::Format const format : {nvrhi::Format::RGBA8_UNORM, nvrhi::Format::BGRA8_UNORM})
	{
		CAPTURE(nvrhi::getFormatInfo(format).name);
		nvrhi::TextureHandle texture = CreateClearedTarget(format, color);

		Result<Image> image = ReadbackTexture(texture);
		REQUIRE(image.IsOk());
		REQUIRE(image.GetValue().GetWidth() == 8);
		REQUIRE(image.GetValue().GetHeight() == 4);
		REQUIRE(image.GetValue().GetChannels() == 4);

		uint8_t const* pixel = image.GetValue().GetPixel(5, 3);
		CHECK(pixel[0] == 255);
		CHECK(std::abs(static_cast<int>(pixel[1]) - 128) <= 1);
		CHECK(pixel[2] == 0);
		CHECK(pixel[3] == 255);
	}
}

TEST_CASE("RHI: texture readback rejects unsupported formats and null textures")
{
	ST_REQUIRE_GPU();

	CHECK(ReadbackTexture(nullptr).IsError());

	nvrhi::TextureHandle floatTexture = CreateClearedTarget(nvrhi::Format::RGBA16_FLOAT, nvrhi::Color(0.0f));
	Result<Image> const result = ReadbackTexture(floatTexture);
	CHECK(result.IsError());
	CHECK(result.GetError().find("cannot be read back") != std::string::npos);
}
