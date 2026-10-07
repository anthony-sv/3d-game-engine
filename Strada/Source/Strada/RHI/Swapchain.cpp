#include "stpch.h"
#include "Strada/RHI/Swapchain.h"

#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/RHI/VulkanContext.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <limits>

namespace Strada
{
	struct Swapchain::Impl
	{
		GLFWwindow* Window = nullptr;
		VkSurfaceKHR Surface = VK_NULL_HANDLE;
		VkSwapchainKHR Handle = VK_NULL_HANDLE;
		VkSurfaceFormatKHR SurfaceFormat{};
		nvrhi::Format Format = nvrhi::Format::UNKNOWN;
		uint32_t Width = 0;
		uint32_t Height = 0;
		uint32_t RequestedWidth = 0;
		uint32_t RequestedHeight = 0;
		bool VSync = true;
		bool NeedsRecreate = false;

		std::vector<VkImage> Images;
		std::vector<nvrhi::TextureHandle> Textures;
		std::vector<nvrhi::FramebufferHandle> Framebuffers;
		// One present semaphore per image; acquire semaphores rotate.
		std::vector<VkSemaphore> PresentSemaphores;
		std::vector<VkSemaphore> AcquireSemaphores;
		uint32_t AcquireIndex = 0;
		uint32_t ImageIndex = 0;
		bool ImageAcquired = false;

		Result<void> CreateSwapchain();
		void DestroySwapchain();
		Result<void> Recreate();
	};

	namespace
	{
		nvrhi::Format ToNvrhiFormat(VkFormat format)
		{
			switch (format)
			{
				case VK_FORMAT_B8G8R8A8_UNORM:
					return nvrhi::Format::BGRA8_UNORM;
				case VK_FORMAT_R8G8B8A8_UNORM:
					return nvrhi::Format::RGBA8_UNORM;
				case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
					return nvrhi::Format::R10G10B10A2_UNORM;
				default:
					return nvrhi::Format::UNKNOWN;
			}
		}

		Result<VkSurfaceFormatKHR> ChooseSurfaceFormat(VkSurfaceKHR surface)
		{
			auto const& vk = Vulkan::Dispatch();
			VkPhysicalDevice const physicalDevice = Vulkan::GetContext().PhysicalDevice;

			uint32_t count = 0;
			vk.vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &count, nullptr);
			std::vector<VkSurfaceFormatKHR> formats(count);
			vk.vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &count, formats.data());
			formats.resize(count);

			// UNORM keeps UI colors (authored in sRGB) correct; the renderer encodes sRGB itself.
			for (VkFormat const preferred : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32})
			{
				for (VkSurfaceFormatKHR const& format : formats)
				{
					if (format.format == preferred && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
					{
						return format;
					}
				}
			}
			return Error{"The window surface supports no 8-bit UNORM sRGB-nonlinear format"};
		}

		VkPresentModeKHR ChoosePresentMode(VkSurfaceKHR surface, bool vsync)
		{
			if (vsync)
			{
				return VK_PRESENT_MODE_FIFO_KHR;
			}

			auto const& vk = Vulkan::Dispatch();
			VkPhysicalDevice const physicalDevice = Vulkan::GetContext().PhysicalDevice;
			uint32_t count = 0;
			vk.vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &count, nullptr);
			std::vector<VkPresentModeKHR> modes(count);
			vk.vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &count, modes.data());
			modes.resize(count);

			for (VkPresentModeKHR const preferred : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
			{
				if (std::find(modes.begin(), modes.end(), preferred) != modes.end())
				{
					return preferred;
				}
			}
			return VK_PRESENT_MODE_FIFO_KHR;
		}

		void DestroySemaphores(std::vector<VkSemaphore>& semaphores)
		{
			auto const& vk = Vulkan::Dispatch();
			VkDevice const device = Vulkan::GetContext().Device;
			for (VkSemaphore semaphore : semaphores)
			{
				vk.vkDestroySemaphore(device, semaphore, nullptr);
			}
			semaphores.clear();
		}

		Result<void> CreateSemaphores(std::vector<VkSemaphore>& semaphores, size_t count)
		{
			auto const& vk = Vulkan::Dispatch();
			VkDevice const device = Vulkan::GetContext().Device;
			VkSemaphoreCreateInfo info{};
			info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
			for (size_t i = 0; i < count; i++)
			{
				VkSemaphore semaphore = VK_NULL_HANDLE;
				VkResult const result = vk.vkCreateSemaphore(device, &info, nullptr, &semaphore);
				if (result != VK_SUCCESS)
				{
					return MakeError("vkCreateSemaphore failed: {}", Vulkan::ResultToString(result));
				}
				semaphores.push_back(semaphore);
			}
			return {};
		}
	}

	Result<void> Swapchain::Impl::CreateSwapchain()
	{
		auto const& vk = Vulkan::Dispatch();
		Vulkan::Context const& context = Vulkan::GetContext();

		VkSurfaceCapabilitiesKHR capabilities{};
		VkResult result = vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(context.PhysicalDevice, Surface, &capabilities);
		if (result != VK_SUCCESS)
		{
			return MakeError("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: {}", Vulkan::ResultToString(result));
		}

		VkExtent2D extent = capabilities.currentExtent;
		if (extent.width == std::numeric_limits<uint32_t>::max())
		{
			// The surface size is determined by the swapchain: use the window's framebuffer size.
			extent.width = std::clamp(RequestedWidth, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
			extent.height = std::clamp(RequestedHeight, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
		}
		if (extent.width == 0 || extent.height == 0)
		{
			// Minimized: there is nothing to create until the window has an area again.
			Width = 0;
			Height = 0;
			return {};
		}

		uint32_t imageCount = capabilities.minImageCount + 1;
		if (capabilities.maxImageCount > 0)
		{
			imageCount = std::min(imageCount, capabilities.maxImageCount);
		}

		VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		// Allows reading back window contents (editor screenshots) where supported.
		if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0)
		{
			usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		}

		VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		if ((capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) == 0)
		{
			for (VkCompositeAlphaFlagBitsKHR const candidate :
			     {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
			      VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
			{
				if ((capabilities.supportedCompositeAlpha & candidate) != 0)
				{
					compositeAlpha = candidate;
					break;
				}
			}
		}

		VkSwapchainCreateInfoKHR createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
		createInfo.surface = Surface;
		createInfo.minImageCount = imageCount;
		createInfo.imageFormat = SurfaceFormat.format;
		createInfo.imageColorSpace = SurfaceFormat.colorSpace;
		createInfo.imageExtent = extent;
		createInfo.imageArrayLayers = 1;
		createInfo.imageUsage = usage;
		createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		createInfo.preTransform = capabilities.currentTransform;
		createInfo.compositeAlpha = compositeAlpha;
		createInfo.presentMode = ChoosePresentMode(Surface, VSync);
		createInfo.clipped = VK_TRUE;

		result = vk.vkCreateSwapchainKHR(context.Device, &createInfo, nullptr, &Handle);
		if (result != VK_SUCCESS)
		{
			return MakeError("vkCreateSwapchainKHR failed: {}", Vulkan::ResultToString(result));
		}

		uint32_t actualCount = 0;
		vk.vkGetSwapchainImagesKHR(context.Device, Handle, &actualCount, nullptr);
		Images.resize(actualCount);
		vk.vkGetSwapchainImagesKHR(context.Device, Handle, &actualCount, Images.data());

		Width = extent.width;
		Height = extent.height;

		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		for (VkImage image : Images)
		{
			nvrhi::TextureDesc textureDesc;
			textureDesc.width = Width;
			textureDesc.height = Height;
			textureDesc.format = Format;
			textureDesc.debugName = "Swapchain image";
			textureDesc.initialState = nvrhi::ResourceStates::Present;
			textureDesc.keepInitialState = true;
			textureDesc.isRenderTarget = true;

			nvrhi::TextureHandle texture =
				device->createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image, nvrhi::Object(image), textureDesc);
			Textures.push_back(texture);
			Framebuffers.push_back(device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture)));
		}

		if (Result<void> semaphores = CreateSemaphores(PresentSemaphores, Images.size()); !semaphores)
		{
			return semaphores;
		}
		// Enough acquire semaphores that one is never re-signaled before the GPU waited on it.
		size_t const acquireCount = Images.size() + GraphicsDevice::GetSpecification().MaxFramesInFlight;
		if (Result<void> semaphores = CreateSemaphores(AcquireSemaphores, acquireCount); !semaphores)
		{
			return semaphores;
		}
		AcquireIndex = 0;
		ImageIndex = 0;
		return {};
	}

	void Swapchain::Impl::DestroySwapchain()
	{
		Framebuffers.clear();
		Textures.clear();
		Images.clear();
		DestroySemaphores(PresentSemaphores);
		DestroySemaphores(AcquireSemaphores);

		if (Handle != VK_NULL_HANDLE)
		{
			Vulkan::Dispatch().vkDestroySwapchainKHR(Vulkan::GetContext().Device, Handle, nullptr);
			Handle = VK_NULL_HANDLE;
		}
		Width = 0;
		Height = 0;
	}

	Result<void> Swapchain::Impl::Recreate()
	{
		// The images and semaphores may still be in use by the GPU.
		GraphicsDevice::WaitForIdle();
		GraphicsDevice::GetDevice()->runGarbageCollection();
		DestroySwapchain();
		NeedsRecreate = false;
		return CreateSwapchain();
	}

	Swapchain::Swapchain()
		: m_Impl(CreateScope<Impl>())
	{
	}

	Result<Scope<Swapchain>> Swapchain::Create(SwapchainSpecification const& specification)
	{
		ST_CORE_ASSERT(GraphicsDevice::IsInitialized(), "Swapchain requires an initialized GraphicsDevice");
		ST_CORE_ASSERT(specification.Window != nullptr, "Swapchain requires a window");

		Scope<Swapchain> swapchain(new Swapchain());
		Impl& impl = *swapchain->m_Impl;
		impl.Window = specification.Window;
		impl.RequestedWidth = specification.Width;
		impl.RequestedHeight = specification.Height;
		impl.VSync = specification.VSync;

		Vulkan::Context const& context = Vulkan::GetContext();
		VkResult result = glfwCreateWindowSurface(context.Instance, specification.Window, nullptr, &impl.Surface);
		if (result != VK_SUCCESS)
		{
			return MakeError("glfwCreateWindowSurface failed: {}", Vulkan::ResultToString(result));
		}

		VkBool32 presentSupported = VK_FALSE;
		Vulkan::Dispatch().vkGetPhysicalDeviceSurfaceSupportKHR(context.PhysicalDevice, context.GraphicsQueueFamily, impl.Surface,
		                                                        &presentSupported);
		if (presentSupported != VK_TRUE)
		{
			return Error{"The graphics queue cannot present to this window surface"};
		}

		Result<VkSurfaceFormatKHR> surfaceFormat = ChooseSurfaceFormat(impl.Surface);
		if (!surfaceFormat)
		{
			return Error{surfaceFormat.GetError()};
		}
		impl.SurfaceFormat = surfaceFormat.GetValue();
		impl.Format = ToNvrhiFormat(impl.SurfaceFormat.format);

		if (Result<void> created = impl.CreateSwapchain(); !created)
		{
			return Error{created.GetError()};
		}
		return swapchain;
	}

	Swapchain::~Swapchain()
	{
		if (!m_Impl || !GraphicsDevice::IsInitialized())
		{
			return;
		}

		GraphicsDevice::WaitForIdle();
		GraphicsDevice::GetDevice()->runGarbageCollection();
		m_Impl->DestroySwapchain();
		if (m_Impl->Surface != VK_NULL_HANDLE)
		{
			Vulkan::Dispatch().vkDestroySurfaceKHR(Vulkan::GetContext().Instance, m_Impl->Surface, nullptr);
			m_Impl->Surface = VK_NULL_HANDLE;
		}
	}

	bool Swapchain::BeginFrame()
	{
		Impl& impl = *m_Impl;
		ST_CORE_ASSERT(!impl.ImageAcquired, "Swapchain::BeginFrame called twice without Present");

		if (impl.NeedsRecreate || impl.Handle == VK_NULL_HANDLE)
		{
			if (impl.RequestedWidth == 0 || impl.RequestedHeight == 0)
			{
				return false;
			}
			if (Result<void> result = impl.Recreate(); !result)
			{
				ST_CORE_ERROR("Failed to recreate the swapchain: {}", result.GetError());
				return false;
			}
			if (impl.Handle == VK_NULL_HANDLE)
			{
				return false;
			}
		}

		auto const& vk = Vulkan::Dispatch();
		VkDevice const device = Vulkan::GetContext().Device;

		for (int attempt = 0; attempt < 2; attempt++)
		{
			VkSemaphore const semaphore = impl.AcquireSemaphores[impl.AcquireIndex];
			VkResult const result = vk.vkAcquireNextImageKHR(device, impl.Handle, std::numeric_limits<uint64_t>::max(), semaphore,
			                                                 VK_NULL_HANDLE, &impl.ImageIndex);

			if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
			{
				impl.AcquireIndex = (impl.AcquireIndex + 1) % static_cast<uint32_t>(impl.AcquireSemaphores.size());
				// The wait is attached to the next submission on the graphics queue.
				Vulkan::GetNvrhiVulkanDevice()->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, semaphore, 0);
				impl.ImageAcquired = true;
				if (result == VK_SUBOPTIMAL_KHR)
				{
					impl.NeedsRecreate = true;
				}
				return true;
			}

			if (result != VK_ERROR_OUT_OF_DATE_KHR)
			{
				ST_CORE_ERROR("vkAcquireNextImageKHR failed: {}", Vulkan::ResultToString(result));
				return false;
			}

			if (Result<void> recreated = impl.Recreate(); !recreated || impl.Handle == VK_NULL_HANDLE)
			{
				if (!recreated)
				{
					ST_CORE_ERROR("Failed to recreate the swapchain: {}", recreated.GetError());
				}
				return false;
			}
		}
		return false;
	}

	void Swapchain::Present()
	{
		Impl& impl = *m_Impl;
		ST_CORE_ASSERT(impl.ImageAcquired, "Swapchain::Present called without a successful BeginFrame");
		impl.ImageAcquired = false;

		VkSemaphore const semaphore = impl.PresentSemaphores[impl.ImageIndex];
		nvrhi::vulkan::IDevice* vulkanDevice = Vulkan::GetNvrhiVulkanDevice();
		vulkanDevice->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, semaphore, 0);
		// NVRHI attaches pending waits/signals to the next submission; an empty submission flushes them now. It must go to
		// the Vulkan device directly: the validation wrapper does not forward submissions without command lists.
		vulkanDevice->executeCommandLists(nullptr, 0);

		VkPresentInfoKHR presentInfo{};
		presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = &semaphore;
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = &impl.Handle;
		presentInfo.pImageIndices = &impl.ImageIndex;

		VkResult const result = Vulkan::Dispatch().vkQueuePresentKHR(Vulkan::GetContext().GraphicsQueue, &presentInfo);
		if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
		{
			impl.NeedsRecreate = true;
		}
		else if (result != VK_SUCCESS)
		{
			ST_CORE_ERROR("vkQueuePresentKHR failed: {}", Vulkan::ResultToString(result));
		}
	}

	void Swapchain::Resize(uint32_t width, uint32_t height)
	{
		Impl& impl = *m_Impl;
		if (width == impl.RequestedWidth && height == impl.RequestedHeight && width == impl.Width && height == impl.Height)
		{
			return;
		}
		impl.RequestedWidth = width;
		impl.RequestedHeight = height;
		impl.NeedsRecreate = true;
	}

	void Swapchain::SetVSync(bool enabled)
	{
		if (m_Impl->VSync != enabled)
		{
			m_Impl->VSync = enabled;
			m_Impl->NeedsRecreate = true;
		}
	}

	bool Swapchain::IsVSync() const
	{
		return m_Impl->VSync;
	}

	nvrhi::ITexture* Swapchain::GetCurrentTexture() const
	{
		ST_CORE_ASSERT(m_Impl->ImageAcquired, "No swapchain image is acquired");
		return m_Impl->Textures[m_Impl->ImageIndex].Get();
	}

	nvrhi::IFramebuffer* Swapchain::GetCurrentFramebuffer() const
	{
		ST_CORE_ASSERT(m_Impl->ImageAcquired, "No swapchain image is acquired");
		return m_Impl->Framebuffers[m_Impl->ImageIndex].Get();
	}

	nvrhi::Format Swapchain::GetFormat() const
	{
		return m_Impl->Format;
	}

	uint32_t Swapchain::GetWidth() const
	{
		return m_Impl->Width;
	}

	uint32_t Swapchain::GetHeight() const
	{
		return m_Impl->Height;
	}
}
