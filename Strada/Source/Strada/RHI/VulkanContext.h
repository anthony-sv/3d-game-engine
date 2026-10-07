#pragma once

// Internal to the RHI module: include only from RHI implementation files. It pulls in vulkan.hpp (with the dynamic
// dispatcher that NVRHI also uses) and, on Windows, <Windows.h> through vulkan.h.

#include "Strada/Core/Base.h"

#include <nvrhi/vulkan.h>
#include <vulkan/vulkan.hpp>

#include <cstdint>

namespace Strada::Vulkan
{
	// Raw Vulkan objects owned by GraphicsDevice. Valid between GraphicsDevice::Init and Shutdown.
	struct Context
	{
		VkInstance Instance = VK_NULL_HANDLE;
		VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
		VkDevice Device = VK_NULL_HANDLE;
		VkQueue GraphicsQueue = VK_NULL_HANDLE;
		uint32_t GraphicsQueueFamily = 0;
	};

	Context const& GetContext();

	// The NVRHI Vulkan device without the validation wrapper, for Vulkan-specific calls (queue semaphores).
	nvrhi::vulkan::IDevice* GetNvrhiVulkanDevice();

	// Function table for every Vulkan call (the dynamic dispatcher shared with NVRHI).
	inline vk::detail::DispatchLoaderDynamic const& Dispatch()
	{
		return VULKAN_HPP_DEFAULT_DISPATCHER;
	}

	char const* ResultToString(VkResult result);
}
