#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"

#include <nvrhi/nvrhi.h>

#include <string>
#include <vector>

namespace Strada
{
	enum class AdapterType : uint8_t
	{
		Other = 0,
		IntegratedGPU,
		DiscreteGPU,
		VirtualGPU,
		CPU
	};

	struct AdapterInfo
	{
		uint32_t Index = 0;
		std::string Name;
		AdapterType Type = AdapterType::Other;
		uint32_t VendorID = 0;
		uint32_t DeviceID = 0;
		std::string ApiVersion;
		std::string Driver;
		// The graphics queue writes timestamps that NVRHI's timer queries can measure GPU time with (at least 32 valid bits).
		bool SupportsTimerQueries = false;
		bool IsSupported = false;
		// Why the adapter cannot be used (empty when supported).
		std::string UnsupportedReason;
	};

	struct GraphicsDeviceSpecification
	{
		std::string ApplicationName = "Strada";
		// Khronos validation layer (needs the Vulkan SDK) plus NVRHI's API validation; failures are logged and counted.
		bool EnableValidation = false;
		// No presentation support: offscreen rendering for tests, CI and headless tools.
		bool Headless = false;
		// Index into EnumerateAdapters(); -1 picks the best supported adapter (discrete GPUs first).
		int32_t AdapterIndex = -1;
		uint32_t MaxFramesInFlight = 2;
	};

	// The Vulkan device wrapped by NVRHI. Static facade; main thread only unless stated otherwise.
	class GraphicsDevice
	{
	public:
		[[nodiscard]] static Result<void> Init(GraphicsDeviceSpecification const& specification);
		// All NVRHI resources must be released before shutdown.
		static void Shutdown();
		static bool IsInitialized();

		static nvrhi::IDevice* GetDevice();
		static AdapterInfo const& GetAdapterInfo();
		static GraphicsDeviceSpecification const& GetSpecification();
		static bool IsValidationEnabled();

		// Every adapter found during Init, supported or not.
		static std::vector<AdapterInfo> const& GetAdapters();

		// Call once per frame after the frame's last submission: throttles the CPU to MaxFramesInFlight frames ahead of
		// the GPU and releases resources the GPU no longer uses.
		static void EndFrame();
		static void WaitForIdle();

		// Number of validation errors reported (Vulkan layers and NVRHI) since Init. Thread-safe.
		static uint64_t GetValidationErrorCount();
	};

	char const* AdapterTypeToString(AdapterType type);
}
