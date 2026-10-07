#pragma once

#include "Strada/Core/Platform.h"
#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/RHI/ShaderLibrary.h"

#include <doctest/doctest.h>

#include <string>

namespace Strada::Testing
{
	// Initializes a headless GraphicsDevice (with validation when the SDK layer is available) and the shader library
	// for the duration of a test, and checks on exit that no validation error was reported.
	class GpuTestScope
	{
	public:
		GpuTestScope()
		{
			GraphicsDeviceSpecification specification;
			specification.ApplicationName = "StradaTests";
			specification.EnableValidation = true;
			specification.Headless = true;
			Result<void> result = GraphicsDevice::Init(specification);
			if (!result)
			{
				m_Reason = result.GetError();
				return;
			}
			ShaderLibrary::Init();
			m_Available = true;
		}

		~GpuTestScope()
		{
			if (!m_Available)
			{
				return;
			}
			GraphicsDevice::WaitForIdle();
			CHECK_MESSAGE(GraphicsDevice::GetValidationErrorCount() == 0, "Vulkan/NVRHI validation reported errors (see log)");
			ShaderLibrary::Shutdown();
			GraphicsDevice::Shutdown();
		}

		GpuTestScope(GpuTestScope const&) = delete;
		GpuTestScope& operator=(GpuTestScope const&) = delete;

		bool IsAvailable() const { return m_Available; }
		std::string const& GetReason() const { return m_Reason; }

		// CI machines without any Vulkan device set STRADA_TESTS_ALLOW_NO_GPU=1 to skip GPU tests; everywhere else a
		// missing device is a failure so broken device creation can never pass silently.
		static bool IsMissingGpuAllowed() { return Platform::ReadEnvironmentVariable("STRADA_TESTS_ALLOW_NO_GPU").value_or("") == "1"; }

	private:
		bool m_Available = false;
		std::string m_Reason;
	};
}

// Starts a GPU test: initializes the device for the rest of the test case or skips/fails when none is available.
#define ST_REQUIRE_GPU()                                                                                                     \
	::Strada::Testing::GpuTestScope stGpuTestScope;                                                                          \
	if (!stGpuTestScope.IsAvailable())                                                                                       \
	{                                                                                                                        \
		if (::Strada::Testing::GpuTestScope::IsMissingGpuAllowed())                                                          \
		{                                                                                                                    \
			MESSAGE("Skipping GPU test: " << stGpuTestScope.GetReason());                                                    \
			return;                                                                                                          \
		}                                                                                                                    \
		FAIL("No usable Vulkan device (set STRADA_TESTS_ALLOW_NO_GPU=1 to skip GPU tests): " << stGpuTestScope.GetReason()); \
	}
