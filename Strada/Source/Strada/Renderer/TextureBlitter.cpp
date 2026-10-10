#include "stpch.h"
#include "Strada/Renderer/TextureBlitter.h"

#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/RHI/ShaderLibrary.h"
#include "Strada/Renderer/Renderer.h"

namespace Strada
{
	TextureBlitter::TextureBlitter()
	{
		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		nvrhi::BindingLayoutDesc layout;
		layout.visibility = nvrhi::ShaderType::Pixel;
		layout.bindings = {
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Sampler(0),
		};
		m_Layout = device->createBindingLayout(layout);
		m_CommandList = device->createCommandList();
		ST_CORE_ASSERT(m_Layout && m_CommandList, "Failed to create the texture blitter's resources");
	}

	TextureBlitter::~TextureBlitter() = default;

	void TextureBlitter::Blit(nvrhi::ITexture* source, nvrhi::IFramebuffer* target)
	{
		ST_CORE_ASSERT(source != nullptr && target != nullptr, "TextureBlitter::Blit needs a source and a target");
		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		nvrhi::FramebufferInfoEx const& format = target->getFramebufferInfo();
		if (!m_PipelineFormat || *m_PipelineFormat != format)
		{
			nvrhi::GraphicsPipelineDesc desc;
			desc.VS = ShaderLibrary::Get("Fullscreen_VS");
			desc.PS = ShaderLibrary::Get("Copy_PS");
			desc.bindingLayouts = {m_Layout};
			desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
			desc.renderState.depthStencilState.depthTestEnable = false;
			desc.renderState.depthStencilState.depthWriteEnable = false;
			m_Pipeline = device->createGraphicsPipeline(desc, format);
			m_PipelineFormat = format;
			if (!m_Pipeline)
			{
				nvrhi::Format const color = format.colorFormats.empty() ? nvrhi::Format::UNKNOWN : format.colorFormats[0];
				ST_CORE_ERROR("Failed to create the blit pipeline for {} targets", nvrhi::getFormatInfo(color).name);
			}
		}
		if (!m_Pipeline)
		{
			return;
		}
		if (m_BoundSource.Get() != source)
		{
			nvrhi::BindingSetDesc bindings;
			bindings.bindings = {
				nvrhi::BindingSetItem::Texture_SRV(0, source),
				nvrhi::BindingSetItem::Sampler(0, Renderer::GetLinearClampSampler()),
			};
			m_BindingSet = device->createBindingSet(bindings, m_Layout);
			m_BoundSource = source;
		}

		m_CommandList->open();
		nvrhi::GraphicsState state;
		state.pipeline = m_Pipeline;
		state.framebuffer = target;
		state.bindings = {m_BindingSet};
		state.viewport.addViewportAndScissorRect(format.getViewport());
		m_CommandList->setGraphicsState(state);
		m_CommandList->draw(nvrhi::DrawArguments().setVertexCount(3));
		m_CommandList->close();
		device->executeCommandList(m_CommandList);
	}
}
