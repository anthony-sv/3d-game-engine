#pragma once

#include <nvrhi/nvrhi.h>

#include <optional>

namespace Strada
{
	// Draws a texture over a framebuffer's first color target, scaled to cover it with linear filtering: how the game player
	// presents the scene renderer's final image in its window. Values are copied as sampled, without color conversion, so
	// both textures must share an encoding (the final image and swapchain images are sRGB-encoded UNORM). Requires
	// Renderer::Init. Main thread only.
	class TextureBlitter
	{
	public:
		TextureBlitter();
		~TextureBlitter();

		TextureBlitter(TextureBlitter const&) = delete;
		TextureBlitter& operator=(TextureBlitter const&) = delete;

		// Records and submits the draw. Draws nothing (logged once per framebuffer format) when the pipeline cannot be created.
		void Blit(nvrhi::ITexture* source, nvrhi::IFramebuffer* target);

	private:
		nvrhi::CommandListHandle m_CommandList;
		nvrhi::BindingLayoutHandle m_Layout;
		nvrhi::GraphicsPipelineHandle m_Pipeline;
		// The framebuffer format the pipeline was created for (also when creating it failed).
		std::optional<nvrhi::FramebufferInfo> m_PipelineFormat;
		// Holding the source keeps the binding set's texture alive and identifies it.
		nvrhi::TextureHandle m_BoundSource;
		nvrhi::BindingSetHandle m_BindingSet;
	};
}
