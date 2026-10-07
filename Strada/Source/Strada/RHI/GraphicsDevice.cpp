#include "stpch.h"
#include "Strada/RHI/GraphicsDevice.h"

#include "Strada/Core/Version.h"
#include "Strada/RHI/VulkanContext.h"

#include <nvrhi/validation.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <queue>
#include <stdexcept>

// Storage for the vulkan.hpp default dispatcher used by Strada and NVRHI (exactly one definition per program).
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace Strada
{
	namespace
	{
		constexpr char const* ValidationLayerName = "VK_LAYER_KHRONOS_validation";

		std::atomic<uint64_t> s_ValidationErrorCount = 0;

		class NvrhiMessageCallback final : public nvrhi::IMessageCallback
		{
		public:
			void message(nvrhi::MessageSeverity severity, char const* messageText) override
			{
				switch (severity)
				{
					case nvrhi::MessageSeverity::Info:
						ST_CORE_TRACE("[NVRHI] {}", messageText);
						break;
					case nvrhi::MessageSeverity::Warning:
						ST_CORE_WARN("[NVRHI] {}", messageText);
						break;
					case nvrhi::MessageSeverity::Error:
						s_ValidationErrorCount++;
						ST_CORE_ERROR("[NVRHI] {}", messageText);
						break;
					case nvrhi::MessageSeverity::Fatal:
						s_ValidationErrorCount++;
						ST_CORE_CRITICAL("[NVRHI] {}", messageText);
						break;
				}
			}
		};

		VKAPI_ATTR VkBool32 VKAPI_CALL DebugUtilsCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
		                                                  VkDebugUtilsMessageTypeFlagsEXT types,
		                                                  VkDebugUtilsMessengerCallbackDataEXT const* callbackData, void* userData)
		{
			(void)userData;
			char const* message = callbackData != nullptr && callbackData->pMessage != nullptr ? callbackData->pMessage : "";
			if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
			{
				s_ValidationErrorCount++;
				ST_CORE_ERROR("[Vulkan] {}", message);
			}
			else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0 &&
			         (types & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) != 0)
			{
				ST_CORE_WARN("[Vulkan] {}", message);
			}
			else
			{
				ST_CORE_TRACE("[Vulkan] {}", message);
			}
			return VK_FALSE;
		}

		struct DeviceCandidate
		{
			VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
			AdapterInfo Info;
			uint32_t GraphicsQueueFamily = 0;
			int32_t Score = 0;
		};

		struct GraphicsDeviceData
		{
			GraphicsDeviceSpecification Specification;
			Scope<vk::detail::DynamicLoader> Loader;
			Vulkan::Context Context;
			VkDebugUtilsMessengerEXT DebugMessenger = VK_NULL_HANDLE;
			bool ValidationEnabled = false;

			// Extension name storage referenced by NVRHI's DeviceDesc for the lifetime of the device.
			std::vector<std::string> InstanceExtensions;
			std::vector<std::string> DeviceExtensions;
			std::vector<char const*> InstanceExtensionPointers;
			std::vector<char const*> DeviceExtensionPointers;

			NvrhiMessageCallback MessageCallback;
			nvrhi::vulkan::DeviceHandle VulkanDevice;
			nvrhi::DeviceHandle Device;

			std::vector<AdapterInfo> Adapters;
			AdapterInfo Adapter;

			std::queue<nvrhi::EventQueryHandle> FramesInFlight;
			std::vector<nvrhi::EventQueryHandle> QueryPool;
		};

		Scope<GraphicsDeviceData> s_Data;

		std::string FormatVersion(uint32_t version)
		{
			return fmt::format("{}.{}.{}", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));
		}

		AdapterType ToAdapterType(VkPhysicalDeviceType type)
		{
			switch (type)
			{
				case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
					return AdapterType::IntegratedGPU;
				case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
					return AdapterType::DiscreteGPU;
				case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
					return AdapterType::VirtualGPU;
				case VK_PHYSICAL_DEVICE_TYPE_CPU:
					return AdapterType::CPU;
				default:
					return AdapterType::Other;
			}
		}

		int32_t ScoreAdapterType(AdapterType type)
		{
			switch (type)
			{
				case AdapterType::DiscreteGPU:
					return 1000;
				case AdapterType::IntegratedGPU:
					return 500;
				case AdapterType::VirtualGPU:
					return 200;
				case AdapterType::CPU:
					return 100;
				case AdapterType::Other:
					return 50;
			}
			return 0;
		}

		bool Contains(std::vector<VkExtensionProperties> const& extensions, char const* name)
		{
			return std::any_of(extensions.begin(), extensions.end(),
			                   [name](VkExtensionProperties const& extension)
			                   {
								   return std::strcmp(extension.extensionName, name) == 0;
							   });
		}

		std::vector<VkExtensionProperties> GetInstanceExtensions()
		{
			auto const& vk = Vulkan::Dispatch();
			uint32_t count = 0;
			vk.vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
			std::vector<VkExtensionProperties> extensions(count);
			vk.vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
			extensions.resize(count);
			return extensions;
		}

		std::vector<VkExtensionProperties> GetDeviceExtensions(VkPhysicalDevice physicalDevice)
		{
			auto const& vk = Vulkan::Dispatch();
			uint32_t count = 0;
			vk.vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr);
			std::vector<VkExtensionProperties> extensions(count);
			vk.vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, extensions.data());
			extensions.resize(count);
			return extensions;
		}

		bool IsLayerAvailable(char const* name)
		{
			auto const& vk = Vulkan::Dispatch();
			uint32_t count = 0;
			vk.vkEnumerateInstanceLayerProperties(&count, nullptr);
			std::vector<VkLayerProperties> layers(count);
			vk.vkEnumerateInstanceLayerProperties(&count, layers.data());
			layers.resize(count);
			return std::any_of(layers.begin(), layers.end(),
			                   [name](VkLayerProperties const& layer)
			                   {
								   return std::strcmp(layer.layerName, name) == 0;
							   });
		}

		VkDebugUtilsMessengerCreateInfoEXT MakeDebugMessengerInfo()
		{
			VkDebugUtilsMessengerCreateInfoEXT info{};
			info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
			info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
			info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
			                   VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
			info.pfnUserCallback = DebugUtilsCallback;
			return info;
		}

		Result<void> LoadVulkan(GraphicsDeviceData& data)
		{
			try
			{
				data.Loader = CreateScope<vk::detail::DynamicLoader>();
			}
			catch (std::exception const& exception)
			{
				return MakeError("Failed to load the Vulkan loader library ({}). Install a Vulkan 1.3 capable GPU driver.",
				                 exception.what());
			}

			auto const getInstanceProcAddr = data.Loader->getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
			if (getInstanceProcAddr == nullptr)
			{
				return Error{"The Vulkan loader does not export vkGetInstanceProcAddr"};
			}
			VULKAN_HPP_DEFAULT_DISPATCHER.init(getInstanceProcAddr);
			return {};
		}

		Result<void> CreateInstance(GraphicsDeviceData& data)
		{
			auto const& vk = Vulkan::Dispatch();

			uint32_t loaderVersion = VK_API_VERSION_1_0;
			if (vk.vkEnumerateInstanceVersion != nullptr)
			{
				vk.vkEnumerateInstanceVersion(&loaderVersion);
			}
			if (loaderVersion < VK_API_VERSION_1_3)
			{
				return MakeError("Vulkan 1.3 is required, but the installed Vulkan loader only supports {}", FormatVersion(loaderVersion));
			}

			std::vector<VkExtensionProperties> const available = GetInstanceExtensions();
			std::vector<std::string>& extensions = data.InstanceExtensions;

			if (!data.Specification.Headless)
			{
				uint32_t glfwExtensionCount = 0;
				char const** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
				if (glfwExtensions == nullptr)
				{
					return Error{"The window system does not support Vulkan surfaces (glfwGetRequiredInstanceExtensions failed)"};
				}
				for (uint32_t i = 0; i < glfwExtensionCount; i++)
				{
					extensions.emplace_back(glfwExtensions[i]);
				}
			}

			// Debug utils enables object names in RenderDoc/Nsight and the validation messenger.
			bool const hasDebugUtils = Contains(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
			if (hasDebugUtils)
			{
				extensions.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
			}

			// MoltenVK (macOS) is a portability implementation that is only enumerated when this extension is enabled.
			bool const hasPortabilityEnumeration = Contains(available, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
			if (hasPortabilityEnumeration)
			{
				extensions.emplace_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
			}

			for (std::string const& extension : extensions)
			{
				if (!Contains(available, extension.c_str()))
				{
					return MakeError("Required Vulkan instance extension {} is not available", extension);
				}
			}

			std::vector<char const*> layers;
			data.ValidationEnabled = false;
			if (data.Specification.EnableValidation)
			{
				if (IsLayerAvailable(ValidationLayerName) && hasDebugUtils)
				{
					layers.push_back(ValidationLayerName);
					data.ValidationEnabled = true;
				}
				else
				{
					ST_CORE_WARN("Vulkan validation was requested but {} is not available (install the Vulkan SDK)", ValidationLayerName);
				}
			}

			data.InstanceExtensionPointers.clear();
			for (std::string const& extension : extensions)
			{
				data.InstanceExtensionPointers.push_back(extension.c_str());
			}

			VkApplicationInfo applicationInfo{};
			applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
			applicationInfo.pApplicationName = data.Specification.ApplicationName.c_str();
			applicationInfo.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
			applicationInfo.pEngineName = "Strada";
			applicationInfo.engineVersion = VK_MAKE_API_VERSION(0, EngineVersion::Major, EngineVersion::Minor, EngineVersion::Patch);
			applicationInfo.apiVersion = VK_API_VERSION_1_3;

			VkDebugUtilsMessengerCreateInfoEXT debugInfo = MakeDebugMessengerInfo();

			VkInstanceCreateInfo createInfo{};
			createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
			createInfo.pApplicationInfo = &applicationInfo;
			createInfo.enabledExtensionCount = static_cast<uint32_t>(data.InstanceExtensionPointers.size());
			createInfo.ppEnabledExtensionNames = data.InstanceExtensionPointers.data();
			createInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
			createInfo.ppEnabledLayerNames = layers.data();
			if (hasPortabilityEnumeration)
			{
				createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
			}
			if (data.ValidationEnabled)
			{
				// Also reports problems during instance creation and destruction.
				createInfo.pNext = &debugInfo;
			}

			VkResult const result = vk.vkCreateInstance(&createInfo, nullptr, &data.Context.Instance);
			if (result != VK_SUCCESS)
			{
				return MakeError("vkCreateInstance failed: {}", Vulkan::ResultToString(result));
			}
			VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Instance(data.Context.Instance));

			if (data.ValidationEnabled)
			{
				VkResult const messengerResult =
					vk.vkCreateDebugUtilsMessengerEXT(data.Context.Instance, &debugInfo, nullptr, &data.DebugMessenger);
				if (messengerResult != VK_SUCCESS)
				{
					ST_CORE_WARN("Failed to create the Vulkan debug messenger: {}", Vulkan::ResultToString(messengerResult));
				}
			}
			return {};
		}

		DeviceCandidate EvaluatePhysicalDevice(GraphicsDeviceData const& data, VkPhysicalDevice physicalDevice, uint32_t index)
		{
			auto const& vk = Vulkan::Dispatch();

			VkPhysicalDeviceVulkan12Properties properties12{};
			properties12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
			VkPhysicalDeviceProperties2 properties{};
			properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
			properties.pNext = &properties12;
			vk.vkGetPhysicalDeviceProperties2(physicalDevice, &properties);

			DeviceCandidate candidate;
			candidate.PhysicalDevice = physicalDevice;
			candidate.Info.Index = index;
			candidate.Info.Name = properties.properties.deviceName;
			candidate.Info.Type = ToAdapterType(properties.properties.deviceType);
			candidate.Info.VendorID = properties.properties.vendorID;
			candidate.Info.DeviceID = properties.properties.deviceID;
			candidate.Info.ApiVersion = FormatVersion(properties.properties.apiVersion);
			candidate.Info.Driver = fmt::format("{} {}", properties12.driverName, properties12.driverInfo);
			candidate.Score = ScoreAdapterType(candidate.Info.Type);

			auto const reject = [&candidate](std::string reason)
			{
				candidate.Info.IsSupported = false;
				candidate.Info.UnsupportedReason = std::move(reason);
				return candidate;
			};

			if (properties.properties.apiVersion < VK_API_VERSION_1_3)
			{
				return reject(fmt::format("Vulkan 1.3 is required (device supports {})", candidate.Info.ApiVersion));
			}

			VkPhysicalDeviceVulkan13Features features13{};
			features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
			VkPhysicalDeviceVulkan12Features features12{};
			features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
			features12.pNext = &features13;
			VkPhysicalDeviceFeatures2 features{};
			features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
			features.pNext = &features12;
			vk.vkGetPhysicalDeviceFeatures2(physicalDevice, &features);

			if (features13.dynamicRendering != VK_TRUE)
			{
				return reject("dynamicRendering is not supported");
			}
			if (features13.synchronization2 != VK_TRUE)
			{
				return reject("synchronization2 is not supported");
			}
			if (features12.timelineSemaphore != VK_TRUE)
			{
				return reject("timelineSemaphore is not supported");
			}

			std::vector<VkExtensionProperties> const extensions = GetDeviceExtensions(physicalDevice);
			if (!data.Specification.Headless && !Contains(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
			{
				return reject("VK_KHR_swapchain is not supported");
			}

			uint32_t familyCount = 0;
			vk.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
			std::vector<VkQueueFamilyProperties> families(familyCount);
			vk.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());

			bool foundFamily = false;
			for (uint32_t family = 0; family < familyCount; family++)
			{
				VkQueueFlags const required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
				if ((families[family].queueFlags & required) != required || families[family].queueCount == 0)
				{
					continue;
				}
				// Presentation happens on the graphics queue, so the family must be able to present.
				if (!data.Specification.Headless &&
				    glfwGetPhysicalDevicePresentationSupport(data.Context.Instance, physicalDevice, family) != GLFW_TRUE)
				{
					continue;
				}
				candidate.GraphicsQueueFamily = family;
				foundFamily = true;
				break;
			}
			if (!foundFamily)
			{
				return reject(data.Specification.Headless ? "no graphics + compute queue family"
				                                          : "no graphics + compute queue family that can present");
			}

			candidate.Info.IsSupported = true;
			return candidate;
		}

		Result<DeviceCandidate> SelectPhysicalDevice(GraphicsDeviceData& data)
		{
			auto const& vk = Vulkan::Dispatch();
			uint32_t count = 0;
			vk.vkEnumeratePhysicalDevices(data.Context.Instance, &count, nullptr);
			std::vector<VkPhysicalDevice> physicalDevices(count);
			vk.vkEnumeratePhysicalDevices(data.Context.Instance, &count, physicalDevices.data());
			physicalDevices.resize(count);
			if (physicalDevices.empty())
			{
				return Error{"No Vulkan devices found"};
			}

			std::vector<DeviceCandidate> candidates;
			for (uint32_t i = 0; i < physicalDevices.size(); i++)
			{
				candidates.push_back(EvaluatePhysicalDevice(data, physicalDevices[i], i));
				data.Adapters.push_back(candidates.back().Info);
			}

			int32_t const requested = data.Specification.AdapterIndex;
			if (requested >= 0)
			{
				if (static_cast<size_t>(requested) >= candidates.size())
				{
					return MakeError("Requested GPU index {} does not exist ({} adapter(s) found)", requested, candidates.size());
				}
				DeviceCandidate const& candidate = candidates[static_cast<size_t>(requested)];
				if (!candidate.Info.IsSupported)
				{
					return MakeError("Requested GPU {} ({}) is not supported: {}", requested, candidate.Info.Name,
					                 candidate.Info.UnsupportedReason);
				}
				return candidate;
			}

			DeviceCandidate const* best = nullptr;
			for (DeviceCandidate const& candidate : candidates)
			{
				if (candidate.Info.IsSupported && (best == nullptr || candidate.Score > best->Score))
				{
					best = &candidate;
				}
			}
			if (best == nullptr)
			{
				std::string reasons;
				for (DeviceCandidate const& candidate : candidates)
				{
					reasons += fmt::format("\n  {}: {}", candidate.Info.Name, candidate.Info.UnsupportedReason);
				}
				return MakeError("No supported Vulkan device found:{}", reasons);
			}
			return *best;
		}

		Result<void> CreateLogicalDevice(GraphicsDeviceData& data, DeviceCandidate const& candidate, bool& outBufferDeviceAddress)
		{
			auto const& vk = Vulkan::Dispatch();
			VkPhysicalDevice const physicalDevice = candidate.PhysicalDevice;

			VkPhysicalDeviceVulkan13Features supported13{};
			supported13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
			VkPhysicalDeviceVulkan12Features supported12{};
			supported12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
			supported12.pNext = &supported13;
			VkPhysicalDeviceVulkan11Features supported11{};
			supported11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
			supported11.pNext = &supported12;
			VkPhysicalDeviceFeatures2 supported{};
			supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
			supported.pNext = &supported11;
			vk.vkGetPhysicalDeviceFeatures2(physicalDevice, &supported);

			// Enable only what the device supports; the renderer queries NVRHI for optional capabilities.
			VkPhysicalDeviceFeatures core{};
			core.samplerAnisotropy = supported.features.samplerAnisotropy;
			core.imageCubeArray = supported.features.imageCubeArray;
			core.fillModeNonSolid = supported.features.fillModeNonSolid;
			core.depthClamp = supported.features.depthClamp;
			core.depthBiasClamp = supported.features.depthBiasClamp;
			core.independentBlend = supported.features.independentBlend;
			core.fragmentStoresAndAtomics = supported.features.fragmentStoresAndAtomics;
			core.shaderStorageImageWriteWithoutFormat = supported.features.shaderStorageImageWriteWithoutFormat;
			core.shaderStorageImageReadWithoutFormat = supported.features.shaderStorageImageReadWithoutFormat;
			core.textureCompressionBC = supported.features.textureCompressionBC;
			core.multiDrawIndirect = supported.features.multiDrawIndirect;
			core.drawIndirectFirstInstance = supported.features.drawIndirectFirstInstance;
			core.shaderImageGatherExtended = supported.features.shaderImageGatherExtended;

			VkPhysicalDeviceVulkan11Features features11{};
			features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
			features11.shaderDrawParameters = supported11.shaderDrawParameters;

			VkPhysicalDeviceVulkan12Features features12{};
			features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
			features12.timelineSemaphore = VK_TRUE;
			// DXC's DirectX memory layout (-fvk-use-dx-layout) relies on relaxed/scalar block layouts.
			features12.scalarBlockLayout = supported12.scalarBlockLayout;
			features12.uniformBufferStandardLayout = supported12.uniformBufferStandardLayout;
			features12.bufferDeviceAddress = supported12.bufferDeviceAddress;
			features12.hostQueryReset = supported12.hostQueryReset;
			features12.samplerFilterMinmax = supported12.samplerFilterMinmax;
			features12.descriptorIndexing = supported12.descriptorIndexing;
			features12.runtimeDescriptorArray = supported12.runtimeDescriptorArray;
			features12.descriptorBindingPartiallyBound = supported12.descriptorBindingPartiallyBound;
			features12.descriptorBindingVariableDescriptorCount = supported12.descriptorBindingVariableDescriptorCount;
			features12.shaderSampledImageArrayNonUniformIndexing = supported12.shaderSampledImageArrayNonUniformIndexing;
			features12.descriptorBindingSampledImageUpdateAfterBind = supported12.descriptorBindingSampledImageUpdateAfterBind;
			features12.descriptorBindingStorageImageUpdateAfterBind = supported12.descriptorBindingStorageImageUpdateAfterBind;
			features12.descriptorBindingStorageBufferUpdateAfterBind = supported12.descriptorBindingStorageBufferUpdateAfterBind;
			features12.descriptorBindingUniformTexelBufferUpdateAfterBind = supported12.descriptorBindingUniformTexelBufferUpdateAfterBind;
			features12.descriptorBindingStorageTexelBufferUpdateAfterBind = supported12.descriptorBindingStorageTexelBufferUpdateAfterBind;
			features12.descriptorBindingUpdateUnusedWhilePending = supported12.descriptorBindingUpdateUnusedWhilePending;
			features11.pNext = &features12;

			VkPhysicalDeviceVulkan13Features features13{};
			features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
			features13.dynamicRendering = VK_TRUE;
			features13.synchronization2 = VK_TRUE;
			features13.maintenance4 = supported13.maintenance4;
			features13.shaderDemoteToHelperInvocation = supported13.shaderDemoteToHelperInvocation;
			features13.shaderTerminateInvocation = supported13.shaderTerminateInvocation;
			features12.pNext = &features13;

			if (supported12.scalarBlockLayout != VK_TRUE)
			{
				ST_CORE_WARN("{} does not support scalarBlockLayout; shaders rely on DirectX-style constant buffer packing",
				             candidate.Info.Name);
			}

			std::vector<VkExtensionProperties> const available = GetDeviceExtensions(physicalDevice);
			std::vector<std::string>& extensions = data.DeviceExtensions;
			if (!data.Specification.Headless)
			{
				extensions.emplace_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
			}
			// Portability implementations (MoltenVK) require this extension to be enabled when exposed.
			if (Contains(available, "VK_KHR_portability_subset"))
			{
				extensions.emplace_back("VK_KHR_portability_subset");
			}

			data.DeviceExtensionPointers.clear();
			for (std::string const& extension : extensions)
			{
				data.DeviceExtensionPointers.push_back(extension.c_str());
			}

			float const queuePriority = 1.0f;
			VkDeviceQueueCreateInfo queueInfo{};
			queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
			queueInfo.queueFamilyIndex = candidate.GraphicsQueueFamily;
			queueInfo.queueCount = 1;
			queueInfo.pQueuePriorities = &queuePriority;

			VkDeviceCreateInfo createInfo{};
			createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
			createInfo.pNext = &features11;
			createInfo.queueCreateInfoCount = 1;
			createInfo.pQueueCreateInfos = &queueInfo;
			createInfo.enabledExtensionCount = static_cast<uint32_t>(data.DeviceExtensionPointers.size());
			createInfo.ppEnabledExtensionNames = data.DeviceExtensionPointers.data();
			createInfo.pEnabledFeatures = &core;

			VkResult const result = vk.vkCreateDevice(physicalDevice, &createInfo, nullptr, &data.Context.Device);
			if (result != VK_SUCCESS)
			{
				return MakeError("vkCreateDevice failed: {}", Vulkan::ResultToString(result));
			}
			VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Device(data.Context.Device));

			data.Context.PhysicalDevice = physicalDevice;
			data.Context.GraphicsQueueFamily = candidate.GraphicsQueueFamily;
			vk.vkGetDeviceQueue(data.Context.Device, candidate.GraphicsQueueFamily, 0, &data.Context.GraphicsQueue);

			outBufferDeviceAddress = features12.bufferDeviceAddress == VK_TRUE;
			return {};
		}

		void DestroyVulkanObjects(GraphicsDeviceData& data)
		{
			auto const& vk = Vulkan::Dispatch();
			if (data.Context.Device != VK_NULL_HANDLE)
			{
				vk.vkDestroyDevice(data.Context.Device, nullptr);
				data.Context.Device = VK_NULL_HANDLE;
			}
			if (data.DebugMessenger != VK_NULL_HANDLE)
			{
				vk.vkDestroyDebugUtilsMessengerEXT(data.Context.Instance, data.DebugMessenger, nullptr);
				data.DebugMessenger = VK_NULL_HANDLE;
			}
			if (data.Context.Instance != VK_NULL_HANDLE)
			{
				vk.vkDestroyInstance(data.Context.Instance, nullptr);
				data.Context.Instance = VK_NULL_HANDLE;
			}
		}
	}

	namespace Vulkan
	{
		Context const& GetContext()
		{
			ST_CORE_ASSERT(s_Data, "GraphicsDevice is not initialized");
			return s_Data->Context;
		}

		nvrhi::vulkan::IDevice* GetNvrhiVulkanDevice()
		{
			ST_CORE_ASSERT(s_Data, "GraphicsDevice is not initialized");
			return s_Data->VulkanDevice.Get();
		}

		char const* ResultToString(VkResult result)
		{
			return nvrhi::vulkan::resultToString(result);
		}
	}

	Result<void> GraphicsDevice::Init(GraphicsDeviceSpecification const& specification)
	{
		ST_CORE_ASSERT(!s_Data, "GraphicsDevice is already initialized");

		auto data = CreateScope<GraphicsDeviceData>();
		data->Specification = specification;
		data->Specification.MaxFramesInFlight = std::max(1u, specification.MaxFramesInFlight);

		auto const fail = [&data](std::string const& message) -> Result<void>
		{
			DestroyVulkanObjects(*data);
			return Error{message};
		};

		if (Result<void> result = LoadVulkan(*data); !result)
		{
			return Error{result.GetError()};
		}
		if (Result<void> result = CreateInstance(*data); !result)
		{
			return fail(result.GetError());
		}

		Result<DeviceCandidate> candidate = SelectPhysicalDevice(*data);
		if (!candidate)
		{
			return fail(candidate.GetError());
		}

		bool bufferDeviceAddress = false;
		if (Result<void> result = CreateLogicalDevice(*data, candidate.GetValue(), bufferDeviceAddress); !result)
		{
			return fail(result.GetError());
		}
		data->Adapter = candidate.GetValue().Info;

		nvrhi::vulkan::DeviceDesc deviceDesc;
		deviceDesc.errorCB = &data->MessageCallback;
		deviceDesc.instance = data->Context.Instance;
		deviceDesc.physicalDevice = data->Context.PhysicalDevice;
		deviceDesc.device = data->Context.Device;
		deviceDesc.graphicsQueue = data->Context.GraphicsQueue;
		deviceDesc.graphicsQueueIndex = static_cast<int>(data->Context.GraphicsQueueFamily);
		deviceDesc.instanceExtensions = data->InstanceExtensionPointers.data();
		deviceDesc.numInstanceExtensions = data->InstanceExtensionPointers.size();
		deviceDesc.deviceExtensions = data->DeviceExtensionPointers.data();
		deviceDesc.numDeviceExtensions = data->DeviceExtensionPointers.size();
		deviceDesc.bufferDeviceAddressSupported = bufferDeviceAddress;

		data->VulkanDevice = nvrhi::vulkan::createDevice(deviceDesc);
		if (!data->VulkanDevice)
		{
			return fail("Failed to create the NVRHI device");
		}
		data->Device = data->VulkanDevice;
		if (data->ValidationEnabled)
		{
			data->Device = nvrhi::validation::createValidationLayer(data->VulkanDevice);
		}

		s_ValidationErrorCount = 0;
		s_Data = std::move(data);

		AdapterInfo const& adapter = s_Data->Adapter;
		ST_CORE_INFO("GPU: {} ({}, Vulkan {}, {}){}", adapter.Name, AdapterTypeToString(adapter.Type), adapter.ApiVersion, adapter.Driver,
		             s_Data->ValidationEnabled ? " with validation" : "");
		return {};
	}

	void GraphicsDevice::Shutdown()
	{
		if (!s_Data)
		{
			return;
		}

		s_Data->Device->waitForIdle();
		s_Data->Device->runGarbageCollection();
		while (!s_Data->FramesInFlight.empty())
		{
			s_Data->FramesInFlight.pop();
		}
		s_Data->QueryPool.clear();
		s_Data->Device = nullptr;
		s_Data->VulkanDevice = nullptr;

		DestroyVulkanObjects(*s_Data);
		s_Data.reset();
	}

	bool GraphicsDevice::IsInitialized()
	{
		return s_Data != nullptr;
	}

	nvrhi::IDevice* GraphicsDevice::GetDevice()
	{
		ST_CORE_ASSERT(s_Data, "GraphicsDevice is not initialized");
		return s_Data->Device.Get();
	}

	AdapterInfo const& GraphicsDevice::GetAdapterInfo()
	{
		ST_CORE_ASSERT(s_Data, "GraphicsDevice is not initialized");
		return s_Data->Adapter;
	}

	GraphicsDeviceSpecification const& GraphicsDevice::GetSpecification()
	{
		ST_CORE_ASSERT(s_Data, "GraphicsDevice is not initialized");
		return s_Data->Specification;
	}

	bool GraphicsDevice::IsValidationEnabled()
	{
		return s_Data && s_Data->ValidationEnabled;
	}

	std::vector<AdapterInfo> const& GraphicsDevice::GetAdapters()
	{
		ST_CORE_ASSERT(s_Data, "GraphicsDevice is not initialized");
		return s_Data->Adapters;
	}

	void GraphicsDevice::EndFrame()
	{
		ST_CORE_ASSERT(s_Data, "GraphicsDevice is not initialized");
		nvrhi::IDevice* device = s_Data->Device.Get();

		// Wait until the GPU finished the frame submitted MaxFramesInFlight frames ago.
		while (s_Data->FramesInFlight.size() >= s_Data->Specification.MaxFramesInFlight)
		{
			nvrhi::EventQueryHandle query = s_Data->FramesInFlight.front();
			s_Data->FramesInFlight.pop();
			device->waitEventQuery(query);
			s_Data->QueryPool.push_back(query);
		}

		nvrhi::EventQueryHandle query;
		if (!s_Data->QueryPool.empty())
		{
			query = s_Data->QueryPool.back();
			s_Data->QueryPool.pop_back();
		}
		else
		{
			query = device->createEventQuery();
		}
		device->resetEventQuery(query);
		device->setEventQuery(query, nvrhi::CommandQueue::Graphics);
		s_Data->FramesInFlight.push(query);

		device->runGarbageCollection();
	}

	void GraphicsDevice::WaitForIdle()
	{
		ST_CORE_ASSERT(s_Data, "GraphicsDevice is not initialized");
		s_Data->Device->waitForIdle();
	}

	uint64_t GraphicsDevice::GetValidationErrorCount()
	{
		return s_ValidationErrorCount.load();
	}

	char const* AdapterTypeToString(AdapterType type)
	{
		switch (type)
		{
			case AdapterType::IntegratedGPU:
				return "integrated GPU";
			case AdapterType::DiscreteGPU:
				return "discrete GPU";
			case AdapterType::VirtualGPU:
				return "virtual GPU";
			case AdapterType::CPU:
				return "CPU";
			case AdapterType::Other:
				return "other";
		}
		return "unknown";
	}
}
