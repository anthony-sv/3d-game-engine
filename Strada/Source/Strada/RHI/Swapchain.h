#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"

#include <nvrhi/nvrhi.h>

struct GLFWwindow;

namespace Strada
{
	struct SwapchainSpecification
	{
		GLFWwindow* Window = nullptr;
		// Initial framebuffer size in pixels.
		uint32_t Width = 0;
		uint32_t Height = 0;
		bool VSync = true;
	};

	// Presentation surface and swapchain for one OS window (the main window or an ImGui platform window).
	// Images use an 8-bit UNORM format: rendered content must already be sRGB-encoded. Main thread only.
	class Swapchain
	{
	public:
		[[nodiscard]] static Result<Scope<Swapchain>> Create(SwapchainSpecification const& specification);
		~Swapchain();

		Swapchain(Swapchain const&) = delete;
		Swapchain& operator=(Swapchain const&) = delete;

		// Acquires the next image. Returns false when nothing can be presented this frame (zero-sized window or a
		// failure that was logged); the caller then skips rendering to this swapchain and must not call Present.
		bool BeginFrame();
		// Presents the acquired image once all rendering to it has been submitted.
		void Present();

		// Requests a resize (applied before the next acquire).
		void Resize(uint32_t width, uint32_t height);
		void SetVSync(bool enabled);
		bool IsVSync() const;

		nvrhi::ITexture* GetCurrentTexture() const;
		nvrhi::IFramebuffer* GetCurrentFramebuffer() const;
		nvrhi::Format GetFormat() const;
		uint32_t GetWidth() const;
		uint32_t GetHeight() const;

	private:
		Swapchain();

		struct Impl;
		Scope<Impl> m_Impl;
	};
}
