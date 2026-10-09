#include "GpuTestUtilities.h"

#include "Strada/Core/Application.h"
#include "Strada/Core/Image.h"
#include "Strada/Core/Window.h"
#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/RHI/Swapchain.h"

#include <doctest/doctest.h>

#include <vector>

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
}

TEST_CASE("RHI: swapchain survives window resizes and present mode changes without validation errors")
{
	{
		// A Vulkan device is required; the probe device is released at the end of the block.
		ST_REQUIRE_GPU();
	}
	ST_REQUIRE_DISPLAY();

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

namespace
{
	// Requests one capture on the third frame and records what the callback receives.
	class ScreenshotLayer : public Layer
	{
	public:
		explicit ScreenshotLayer(std::vector<Result<Image>>& results)
			: Layer("Screenshot"),
			  m_Results(results)
		{
		}

		void OnUpdate(Timestep) override
		{
			if (Application::Get().GetFrameCount() == 2)
			{
				Application::Get().RequestScreenshotImage(
					[this](Result<Image> image)
					{
						m_Results.push_back(std::move(image));
					});
			}
		}

	private:
		std::vector<Result<Image>>& m_Results;
	};

	class ScreenshotApplication : public Application
	{
	public:
		ScreenshotApplication(ApplicationSpecification const& specification, std::vector<Result<Image>>& results)
			: Application(specification),
			  m_Results(results)
		{
		}

	protected:
		Result<void> OnInit() override
		{
			PushLayer<ScreenshotLayer>(m_Results);
			return {};
		}

	private:
		std::vector<Result<Image>>& m_Results;
	};
}

TEST_CASE("RHI: screenshot requests receive the presented window image")
{
	{
		ST_REQUIRE_GPU();
	}
	ST_REQUIRE_DISPLAY();

	ApplicationSpecification specification;
	specification.Name = "Screenshot Test";
	specification.Window.Title = "Strada Screenshot Test";
	specification.Window.Width = 160;
	specification.Window.Height = 120;
	specification.MaxFrames = 5;
	specification.EnableValidation = true;

	std::vector<Result<Image>> results;
	ScreenshotApplication application(specification, results);
	CHECK(application.Run() == 0);
	REQUIRE(results.size() == 1);
	REQUIRE_MESSAGE(results[0].IsOk(), results[0].GetError());
	Image const& image = results[0].GetValue();
	CHECK(image.GetWidth() > 0);
	CHECK(image.GetHeight() > 0);
	CHECK(image.GetChannels() == 4);
	// The back buffer is cleared to opaque black before layers draw.
	CHECK(image.GetPixel(0, 0)[3] == 255);
	CHECK(GraphicsDevice::GetValidationErrorCount() == 0);
}
