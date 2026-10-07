#include "GpuTestUtilities.h"

#include "Strada/Core/Application.h"
#include "Strada/Core/Window.h"
#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/RHI/Swapchain.h"

#include <doctest/doctest.h>

using namespace Strada;

namespace
{
	struct SwapchainProbe
	{
		uint64_t PresentedFrames = 0;
		uint32_t LastWidth = 0;
		uint32_t LastHeight = 0;
	};

	// Drives the window through resizes and present-mode changes while frames are presented.
	class ResizeLayer : public Layer
	{
	public:
		explicit ResizeLayer(SwapchainProbe& probe)
			: Layer("Resize"),
			  m_Probe(probe)
		{
		}

		void OnUpdate(Timestep) override
		{
			Application& application = Application::Get();
			uint64_t const frame = application.GetFrameCount();
			if (frame == 3)
			{
				application.GetWindow()->SetSize(480, 300);
			}
			else if (frame == 6)
			{
				application.SetVSync(false);
			}
			else if (frame == 9)
			{
				application.GetWindow()->SetSize(256, 512);
			}
			else if (frame == 12)
			{
				application.SetVSync(true);
			}

			if (application.GetBackBuffer() != nullptr)
			{
				m_Probe.PresentedFrames++;
				m_Probe.LastWidth = application.GetSwapchain()->GetWidth();
				m_Probe.LastHeight = application.GetSwapchain()->GetHeight();
			}
		}

	private:
		SwapchainProbe& m_Probe;
	};

	class WindowedApplication : public Application
	{
	public:
		WindowedApplication(ApplicationSpecification const& specification, SwapchainProbe& probe)
			: Application(specification),
			  m_Probe(probe)
		{
		}

	protected:
		Result<void> OnInit() override
		{
			PushLayer<ResizeLayer>(m_Probe);
			return {};
		}

	private:
		SwapchainProbe& m_Probe;
	};

	bool IsMissingDisplayAllowed()
	{
		return Platform::ReadEnvironmentVariable("STRADA_TESTS_ALLOW_NO_DISPLAY").value_or("") == "1" ||
		       Testing::GpuTestScope::IsMissingGpuAllowed();
	}
}

TEST_CASE("RHI: swapchain survives window resizes and present mode changes without validation errors")
{
	{
		// A Vulkan device is required; the probe device is released at the end of the block.
		ST_REQUIRE_GPU();
	}

	{
		// A display is required as well (headless CI machines have none).
		WindowSpecification probeSpecification;
		probeSpecification.Visible = false;
		Result<Scope<Window>> probeWindow = Window::Create(probeSpecification);
		if (!probeWindow)
		{
			if (IsMissingDisplayAllowed())
			{
				MESSAGE("Skipping windowed test: " << probeWindow.GetError());
				return;
			}
			FAIL("No display available (set STRADA_TESTS_ALLOW_NO_DISPLAY=1 to skip): " << probeWindow.GetError());
		}
	}

	ApplicationSpecification specification;
	specification.Name = "Swapchain Test";
	specification.Window.Title = "Strada Swapchain Test";
	specification.Window.Width = 320;
	specification.Window.Height = 240;
	specification.MaxFrames = 20;
	specification.EnableValidation = true;

	SwapchainProbe probe;
	WindowedApplication application(specification, probe);
	CHECK(application.Run() == 0);
	CHECK(application.GetFrameCount() == 20);
	CHECK(probe.PresentedFrames > 0);
	CHECK(probe.LastWidth > 0);
	CHECK(probe.LastHeight > 0);
	CHECK(GraphicsDevice::GetValidationErrorCount() == 0);
}
