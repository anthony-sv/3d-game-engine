#include "GpuTestUtilities.h"

#include "Strada/Core/Application.h"
#include "Strada/Core/Events/ApplicationEvent.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/ImGui/ImGuiLayer.h"
#include "Strada/RHI/GraphicsDevice.h"

#include <doctest/doctest.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <imgui.h>

#include <array>
#include <filesystem>
#include <vector>

using namespace Strada;

namespace
{
	// The drop callback installed on a window, called the way GLFW calls it when files are dropped onto the window.
	GLFWdropfun GetDropCallback(GLFWwindow* window)
	{
		GLFWdropfun const callback = glfwSetDropCallback(window, nullptr);
		glfwSetDropCallback(window, callback);
		return callback;
	}

	struct DropRecord
	{
		bool ViewportsEnabled = false;
		// The platform window ImGui made for the window drawn outside the main one, and whether it had a drop callback.
		bool PanelWindowFound = false;
		bool PanelHasDropCallback = false;
		// The paths of the WindowFileDropEvents the application's layers received, in order.
		std::vector<std::vector<std::filesystem::path>> Drops;
	};

	// Drops a file onto the main window in the first frame, draws a window outside the main one (ImGui gives it a window of
	// its own when multi-viewports are enabled) and drops files onto that window once it exists.
	class DropProbeLayer : public Layer
	{
	public:
		explicit DropProbeLayer(DropRecord& record)
			: Layer("DropProbe"),
			  m_Record(record)
		{
		}

		void OnUpdate(Timestep) override
		{
			GLFWwindow* const window = Application::Get().GetWindow()->GetNativeWindow();
			if (m_Frame == 0)
			{
				m_Record.ViewportsEnabled = Application::Get().GetImGuiLayer()->AreViewportsEnabled();
				std::array<char const*, 1> paths = {"C:/Downloads/Crate.glb"};
				GetDropCallback(window)(window, static_cast<int>(paths.size()), paths.data());
			}
			else if (m_Record.ViewportsEnabled && !m_Record.PanelWindowFound)
			{
				for (ImGuiViewport const* viewport : ImGui::GetPlatformIO().Viewports)
				{
					GLFWwindow* const platformWindow = static_cast<GLFWwindow*>(viewport->PlatformHandle);
					if (platformWindow == nullptr || platformWindow == window)
					{
						continue;
					}
					m_Record.PanelWindowFound = true;
					GLFWdropfun const drop = GetDropCallback(platformWindow);
					m_Record.PanelHasDropCallback = drop != nullptr;
					if (drop != nullptr)
					{
						std::array<char const*, 2> paths = {"/home/J\xC3\xBCrgen/Ambience.ogg", "/home/J\xC3\xBCrgen/Pack"};
						drop(platformWindow, static_cast<int>(paths.size()), paths.data());
					}
					break;
				}
			}
			m_Frame++;
		}

		void OnImGuiRender() override
		{
			ImGuiViewport const* const main = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(ImVec2(main->Pos.x + main->Size.x + 40.0f, main->Pos.y));
			ImGui::SetNextWindowSize(ImVec2(160.0f, 120.0f));
			ImGui::Begin("Floating");
			ImGui::End();
		}

		void OnEvent(Event& event) override
		{
			EventDispatcher dispatcher(event);
			dispatcher.Dispatch<WindowFileDropEvent>(
				[this](WindowFileDropEvent& dropped)
				{
					m_Record.Drops.push_back(dropped.GetPaths());
					return true;
				});
		}

	private:
		DropRecord& m_Record;
		uint64_t m_Frame = 0;
	};

	class DropProbeApplication : public Application
	{
	public:
		DropProbeApplication(ApplicationSpecification const& specification, DropRecord& record)
			: Application(specification),
			  m_Record(record)
		{
		}

	protected:
		Result<void> OnInit() override
		{
			PushLayer<DropProbeLayer>(m_Record);
			return {};
		}

	private:
		DropRecord& m_Record;
	};
}

TEST_CASE("ImGui: files dropped onto the main window or the window of a panel dragged out of it reach the application")
{
	{
		// A Vulkan device is required; the probe device is released at the end of the block.
		ST_REQUIRE_GPU();
	}
	ST_REQUIRE_DISPLAY();

	DropRecord record;
	ApplicationSpecification specification;
	specification.Name = "ImGui File Drop Test";
	specification.Window.Title = "Strada ImGui File Drop Test";
	specification.Window.Width = 320;
	specification.Window.Height = 240;
	specification.Window.Visible = false;
	specification.EnableImGui = true;
	specification.EnableScripting = false;
	specification.EnableValidation = true;
	specification.MaxFrames = 8;
	{
		DropProbeApplication application(specification, record);
		CHECK(application.Run() == 0);
	}
	CHECK(GraphicsDevice::GetValidationErrorCount() == 0);

	REQUIRE_FALSE(record.Drops.empty());
	REQUIRE(record.Drops[0].size() == 1);
	CHECK(FileSystem::PathToUtf8(record.Drops[0][0]) == "C:/Downloads/Crate.glb");
	if (record.ViewportsEnabled)
	{
		REQUIRE(record.PanelWindowFound);
		CHECK(record.PanelHasDropCallback);
		REQUIRE(record.Drops.size() == 2);
		REQUIRE(record.Drops[1].size() == 2);
		CHECK(FileSystem::PathToUtf8(record.Drops[1][0].filename()) == "Ambience.ogg");
		CHECK(FileSystem::PathToUtf8(record.Drops[1][1]) == "/home/J\xC3\xBCrgen/Pack");
	}
	else
	{
		MESSAGE("Multi-viewports are unavailable (Wayland): only the main window's drops are checked");
		CHECK(record.Drops.size() == 1);
	}
}
