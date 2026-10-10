#include "Renderer/RenderTestUtilities.h"

#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/RHI/TextureReadback.h"
#include "Strada/Renderer/TextureBlitter.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <vector>

using namespace Strada;

namespace
{
	using TexelFunction = std::function<std::array<uint8_t, 4>(uint32_t x, uint32_t y)>;

	nvrhi::TextureHandle CreateSource(uint32_t width, uint32_t height, TexelFunction const& texel)
	{
		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		nvrhi::TextureDesc desc;
		desc.width = width;
		desc.height = height;
		desc.format = nvrhi::Format::RGBA8_UNORM;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "BlitSource";
		nvrhi::TextureHandle texture = device->createTexture(desc);
		REQUIRE(texture != nullptr);

		std::vector<uint8_t> pixels;
		for (uint32_t y = 0; y < height; y++)
		{
			for (uint32_t x = 0; x < width; x++)
			{
				std::array<uint8_t, 4> const value = texel(x, y);
				pixels.insert(pixels.end(), value.begin(), value.end());
			}
		}
		nvrhi::CommandListHandle commandList = device->createCommandList();
		commandList->open();
		commandList->writeTexture(texture, 0, 0, pixels.data(), static_cast<size_t>(width) * 4);
		commandList->close();
		device->executeCommandList(commandList);
		return texture;
	}

	struct Target
	{
		nvrhi::TextureHandle Texture;
		nvrhi::FramebufferHandle Framebuffer;
	};

	Target CreateTarget(nvrhi::Format format, uint32_t width, uint32_t height)
	{
		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		nvrhi::TextureDesc desc;
		desc.width = width;
		desc.height = height;
		desc.format = format;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		desc.debugName = "BlitTarget";
		Target target;
		target.Texture = device->createTexture(desc);
		REQUIRE(target.Texture != nullptr);
		target.Framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target.Texture));
		REQUIRE(target.Framebuffer != nullptr);
		return target;
	}

	// Every pixel of the image within a level of the expected texel.
	void CheckPixels(Image const& image, TexelFunction const& expected)
	{
		for (uint32_t y = 0; y < image.GetHeight(); y++)
		{
			for (uint32_t x = 0; x < image.GetWidth(); x++)
			{
				std::array<uint8_t, 4> const texel = expected(x, y);
				uint8_t const* pixel = image.GetPixel(x, y);
				for (uint32_t channel = 0; channel < 4; channel++)
				{
					CAPTURE(x);
					CAPTURE(y);
					CAPTURE(channel);
					CHECK(std::abs(static_cast<int>(pixel[channel]) - static_cast<int>(texel[channel])) <= 1);
				}
			}
		}
	}
}

TEST_CASE("Renderer: the texture blitter copies texels into RGBA and BGRA targets")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);

	// Channels differ per texel, so placement and channel order mistakes show.
	TexelFunction const texel = [](uint32_t x, uint32_t y)
	{
		return std::array<uint8_t, 4>{static_cast<uint8_t>(20 + x * 60), static_cast<uint8_t>(230 - y * 150),
		                              static_cast<uint8_t>(5 + x * 30 + y * 100), static_cast<uint8_t>(255 - x * 40)};
	};
	nvrhi::TextureHandle const source = CreateSource(4, 2, texel);
	TextureBlitter blitter;
	for (nvrhi::Format const format : {nvrhi::Format::RGBA8_UNORM, nvrhi::Format::BGRA8_UNORM})
	{
		CAPTURE(nvrhi::getFormatInfo(format).name);
		Target const target = CreateTarget(format, 4, 2);
		blitter.Blit(source, target.Framebuffer);
		Result<Image> image = ReadbackTexture(target.Texture);
		REQUIRE(image.IsOk());
		CheckPixels(image.GetValue(), texel);
	}
}

TEST_CASE("Renderer: the texture blitter scales its source over the whole target and follows new sources")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);

	TexelFunction const orange = [](uint32_t, uint32_t)
	{
		return std::array<uint8_t, 4>{255, 128, 0, 255};
	};
	TexelFunction const teal = [](uint32_t, uint32_t)
	{
		return std::array<uint8_t, 4>{0, 128, 128, 255};
	};
	nvrhi::TextureHandle const first = CreateSource(2, 2, orange);
	nvrhi::TextureHandle const second = CreateSource(3, 5, teal);
	Target const target = CreateTarget(nvrhi::Format::BGRA8_UNORM, 9, 6);

	TextureBlitter blitter;
	blitter.Blit(first, target.Framebuffer);
	Result<Image> scaled = ReadbackTexture(target.Texture);
	REQUIRE(scaled.IsOk());
	CheckPixels(scaled.GetValue(), orange);

	blitter.Blit(second, target.Framebuffer);
	Result<Image> replaced = ReadbackTexture(target.Texture);
	REQUIRE(replaced.IsOk());
	CheckPixels(replaced.GetValue(), teal);
}
