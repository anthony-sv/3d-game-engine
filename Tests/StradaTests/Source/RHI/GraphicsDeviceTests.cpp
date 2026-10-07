#include "GpuTestUtilities.h"

#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/RHI/ShaderLibrary.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

using namespace Strada;

TEST_CASE("RHI: headless device initializes with a supported adapter")
{
	ST_REQUIRE_GPU();

	CHECK(GraphicsDevice::IsInitialized());
	CHECK(GraphicsDevice::GetDevice() != nullptr);
	CHECK(GraphicsDevice::GetDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN);

	AdapterInfo const& adapter = GraphicsDevice::GetAdapterInfo();
	CHECK(adapter.IsSupported);
	CHECK_FALSE(adapter.Name.empty());
	CHECK(adapter.UnsupportedReason.empty());
	REQUIRE_FALSE(GraphicsDevice::GetAdapters().empty());
	CHECK(adapter.Index < GraphicsDevice::GetAdapters().size());
	CHECK(GraphicsDevice::GetSpecification().Headless);
}

TEST_CASE("RHI: shader library creates every embedded shader")
{
	ST_REQUIRE_GPU();

	std::vector<std::string> const names = ShaderLibrary::GetNames();
	REQUIRE_FALSE(names.empty());
	for (std::string const& name : names)
	{
		CAPTURE(name);
		CHECK(ShaderLibrary::Contains(name));
		nvrhi::ShaderHandle const shader = ShaderLibrary::Get(name);
		REQUIRE(shader != nullptr);
		// Subsequent lookups return the cached object.
		CHECK(ShaderLibrary::Get(name) == shader);
	}

	CHECK_FALSE(ShaderLibrary::Contains("DoesNotExist"));
	CHECK(ShaderLibrary::Get("DoesNotExist") == nullptr);
}

TEST_CASE("RHI: renders a solid color into an offscreen target and reads it back")
{
	ST_REQUIRE_GPU();
	nvrhi::IDevice* device = GraphicsDevice::GetDevice();

	constexpr uint32_t Width = 64;
	constexpr uint32_t Height = 32;

	nvrhi::TextureDesc textureDesc;
	textureDesc.width = Width;
	textureDesc.height = Height;
	textureDesc.format = nvrhi::Format::RGBA8_UNORM;
	textureDesc.isRenderTarget = true;
	textureDesc.initialState = nvrhi::ResourceStates::RenderTarget;
	textureDesc.keepInitialState = true;
	textureDesc.debugName = "TestTarget";
	nvrhi::TextureHandle target = device->createTexture(textureDesc);
	REQUIRE(target != nullptr);

	nvrhi::FramebufferHandle framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target));
	REQUIRE(framebuffer != nullptr);

	nvrhi::BindingLayoutDesc layoutDesc;
	layoutDesc.visibility = nvrhi::ShaderType::Pixel;
	layoutDesc.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, sizeof(float) * 4)};
	nvrhi::BindingLayoutHandle layout = device->createBindingLayout(layoutDesc);
	REQUIRE(layout != nullptr);

	nvrhi::BindingSetDesc setDesc;
	setDesc.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(float) * 4)};
	nvrhi::BindingSetHandle bindingSet = device->createBindingSet(setDesc, layout);
	REQUIRE(bindingSet != nullptr);

	nvrhi::GraphicsPipelineDesc pipelineDesc;
	pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
	pipelineDesc.VS = ShaderLibrary::Get("Fullscreen_VS");
	pipelineDesc.PS = ShaderLibrary::Get("SolidColor_PS");
	pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
	pipelineDesc.renderState.depthStencilState.depthWriteEnable = false;
	pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
	pipelineDesc.bindingLayouts = {layout};
	nvrhi::GraphicsPipelineHandle pipeline = device->createGraphicsPipeline(pipelineDesc, framebuffer->getFramebufferInfo());
	REQUIRE(pipeline != nullptr);

	nvrhi::StagingTextureHandle readback = device->createStagingTexture(textureDesc, nvrhi::CpuAccessMode::Read);
	REQUIRE(readback != nullptr);

	float const color[4] = {0.25f, 0.5f, 0.75f, 1.0f};

	nvrhi::CommandListHandle commandList = device->createCommandList();
	commandList->open();
	commandList->clearTextureFloat(target, nvrhi::AllSubresources, nvrhi::Color(0.0f));

	nvrhi::GraphicsState state;
	state.pipeline = pipeline;
	state.framebuffer = framebuffer;
	state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(Width), static_cast<float>(Height)));
	state.bindings = {bindingSet};
	commandList->setGraphicsState(state);
	commandList->setPushConstants(color, sizeof(color));
	commandList->draw(nvrhi::DrawArguments().setVertexCount(3));

	commandList->copyTexture(readback, nvrhi::TextureSlice(), target, nvrhi::TextureSlice());
	commandList->close();
	device->executeCommandList(commandList);
	device->waitForIdle();

	size_t rowPitch = 0;
	auto const* pixels =
		static_cast<uint8_t const*>(device->mapStagingTexture(readback, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
	REQUIRE(pixels != nullptr);
	REQUIRE(rowPitch >= Width * 4);

	auto const toByte = [](float value)
	{
		return static_cast<int>(std::lround(value * 255.0f));
	};

	int mismatches = 0;
	for (uint32_t y = 0; y < Height; y++)
	{
		for (uint32_t x = 0; x < Width; x++)
		{
			uint8_t const* pixel = pixels + y * rowPitch + x * 4;
			for (int channel = 0; channel < 4; channel++)
			{
				if (std::abs(static_cast<int>(pixel[channel]) - toByte(color[channel])) > 1)
				{
					mismatches++;
				}
			}
		}
	}
	device->unmapStagingTexture(readback);
	CHECK(mismatches == 0);
}

TEST_CASE("RHI: the device can be shut down and initialized again")
{
	for (int iteration = 0; iteration < 2; iteration++)
	{
		ST_REQUIRE_GPU();
		CHECK(GraphicsDevice::IsInitialized());
	}
	CHECK_FALSE(GraphicsDevice::IsInitialized());
}

TEST_CASE("RHI: requesting an adapter index that does not exist fails cleanly")
{
	GraphicsDeviceSpecification specification;
	specification.Headless = true;
	specification.AdapterIndex = 1000;
	Result<void> const result = GraphicsDevice::Init(specification);
	CHECK(result.IsError());
	CHECK_FALSE(GraphicsDevice::IsInitialized());
}
