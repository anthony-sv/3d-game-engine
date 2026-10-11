#include "GpuTestUtilities.h"

#include "Strada/Core/Application.h"
#include "Strada/Core/Events/KeyEvent.h"
#include "Strada/Core/Events/MouseEvent.h"
#include "Strada/Core/Input.h"
#include "Strada/ImGui/ImGuiLayer.h"
#include "Strada/RHI/GraphicsDevice.h"

#include <doctest/doctest.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <imgui.h>

#include <string>
#include <vector>

using namespace Strada;

namespace
{
	// The callback installed on a window, called the way GLFW calls it when the operating system reports input.
	template<typename TCallback>
	TCallback GetInstalledCallback(GLFWwindow* window, TCallback (*setter)(GLFWwindow*, TCallback))
	{
		TCallback const callback = setter(window, nullptr);
		setter(window, callback);
		return callback;
	}

	struct InputRecord
	{
		// Events the input filter saw, from the main window and from a panel's own window, with their contexts.
		std::vector<std::string> MainEvents;
		std::vector<ImGuiLayer::InputContext> MainContexts;
		std::vector<std::string> PanelEvents;
		std::vector<ImGuiLayer::InputContext> PanelContexts;
		glm::vec2 MainWindowPosition = glm::vec2(0.0f);
		glm::vec2 PanelWindowPosition = glm::vec2(0.0f);
		bool PanelWindowFound = false;
		// The ImGui viewports of the two windows.
		uint32_t MainViewport = 0;
		uint32_t PanelViewport = 0;
		// What the application's Input had right after the input was sent.
		bool InputSpace = false;
		bool InputLeftButton = false;
		// What ImGui saw in the frames after.
		bool InterfaceSpace = false;
		bool InterfaceEscape = false;
		bool InterfaceE = false;
		bool InterfaceLeftButton = false;
		bool InterfaceCharacter = false;
		glm::vec2 InterfaceMouse = glm::vec2(-1.0f);
		bool ViewportsEnabled = false;
	};

	// Sends window input through the callbacks installed on the main window in the first frame, draws a window outside the
	// main one (ImGui gives it a window of its own when multi-viewports are enabled) and sends input to that window too.
	// With a filter, ImGui receives everything but Space.
	class InputProbeLayer : public Layer
	{
	public:
		InputProbeLayer(bool filter, InputRecord& record)
			: Layer("InputProbe"),
			  m_Filter(filter),
			  m_Record(record)
		{
		}

		void OnUpdate(Timestep) override
		{
			GLFWwindow* const window = Application::Get().GetWindow()->GetNativeWindow();
			if (m_Frame == 0)
			{
				m_Record.ViewportsEnabled = Application::Get().GetImGuiLayer()->AreViewportsEnabled();
				m_Record.MainViewport = ImGui::GetMainViewport()->ID;
				if (m_Filter)
				{
					Application::Get().GetImGuiLayer()->SetInputFilter(
						[this](Event const& event, ImGuiLayer::InputContext const& context)
						{
							bool const panel = m_PanelWindow != nullptr;
							(panel ? m_Record.PanelEvents : m_Record.MainEvents).push_back(event.ToString());
							(panel ? m_Record.PanelContexts : m_Record.MainContexts).push_back(context);
							bool const space = event.GetEventType() == EventType::KeyPressed &&
						                       static_cast<KeyPressedEvent const&>(event).GetKeyCode() == KeyCode::Space;
							return !space;
						});
				}
				int x = 0;
				int y = 0;
				glfwGetWindowPos(window, &x, &y);
				m_Record.MainWindowPosition = glm::vec2(static_cast<float>(x), static_cast<float>(y));

				GLFWkeyfun const key = GetInstalledCallback(window, glfwSetKeyCallback);
				key(window, GLFW_KEY_SPACE, glfwGetKeyScancode(GLFW_KEY_SPACE), GLFW_PRESS, 0);
				key(window, GLFW_KEY_ESCAPE, glfwGetKeyScancode(GLFW_KEY_ESCAPE), GLFW_PRESS, 0);
				GetInstalledCallback(window, glfwSetMouseButtonCallback)(window, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
				GetInstalledCallback(window, glfwSetCharCallback)(window, 'x');
				GetInstalledCallback(window, glfwSetCursorPosCallback)(window, 10.0, 20.0);
				GetInstalledCallback(window, glfwSetScrollCallback)(window, 0.0, 1.0);
				m_Record.InputSpace = Input::IsKeyDown(KeyCode::Space);
				m_Record.InputLeftButton = Input::IsMouseButtonDown(MouseButton::Left);
			}
			else if (m_Frame >= PanelInputFrame && m_PanelWindow == nullptr && m_Record.ViewportsEnabled)
			{
				for (ImGuiViewport const* viewport : ImGui::GetPlatformIO().Viewports)
				{
					GLFWwindow* const platformWindow = static_cast<GLFWwindow*>(viewport->PlatformHandle);
					if (platformWindow != nullptr && platformWindow != window)
					{
						SendToPanelWindow(platformWindow);
						break;
					}
				}
			}
			m_Frame++;
		}

		void OnImGuiRender() override
		{
			m_Record.InterfaceSpace |= ImGui::IsKeyDown(ImGuiKey_Space);
			m_Record.InterfaceEscape |= ImGui::IsKeyDown(ImGuiKey_Escape);
			m_Record.InterfaceE |= ImGui::IsKeyDown(ImGuiKey_E);
			m_Record.InterfaceLeftButton |= ImGui::IsMouseDown(ImGuiMouseButton_Left);
			m_Record.InterfaceCharacter |= !ImGui::GetIO().InputQueueCharacters.empty();
			// The first position ImGui processes is the one sent to the main window.
			if (!m_HasInterfaceMouse && ImGui::IsMousePosValid())
			{
				m_Record.InterfaceMouse = glm::vec2(ImGui::GetMousePos().x, ImGui::GetMousePos().y);
				m_HasInterfaceMouse = true;
			}

			ImGuiViewport const* const main = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(ImVec2(main->Pos.x + main->Size.x + 40.0f, main->Pos.y));
			ImGui::SetNextWindowSize(ImVec2(160.0f, 120.0f));
			ImGui::Begin("Floating");
			ImGui::End();
		}

	private:
		void SendToPanelWindow(GLFWwindow* window)
		{
			m_PanelWindow = window;
			m_Record.PanelWindowFound = true;
			if (ImGuiViewport const* const viewport = ImGui::FindViewportByPlatformHandle(window))
			{
				m_Record.PanelViewport = viewport->ID;
			}
			int x = 0;
			int y = 0;
			glfwGetWindowPos(window, &x, &y);
			m_Record.PanelWindowPosition = glm::vec2(static_cast<float>(x), static_cast<float>(y));
			GetInstalledCallback(window, glfwSetKeyCallback)(window, GLFW_KEY_E, glfwGetKeyScancode(GLFW_KEY_E), GLFW_PRESS, 0);
			GetInstalledCallback(window, glfwSetCursorPosCallback)(window, 5.0, 6.0);
		}

		// ImGui takes queued input over several frames (a button change, then text, then the cursor...).
		static constexpr uint64_t PanelInputFrame = 5;

		bool m_Filter;
		InputRecord& m_Record;
		uint64_t m_Frame = 0;
		GLFWwindow* m_PanelWindow = nullptr;
		bool m_HasInterfaceMouse = false;
	};

	class InputProbeApplication : public Application
	{
	public:
		InputProbeApplication(ApplicationSpecification const& specification, bool filter, InputRecord& record)
			: Application(specification),
			  m_Filter(filter),
			  m_Record(record)
		{
		}

	protected:
		Result<void> OnInit() override
		{
			PushLayer<InputProbeLayer>(m_Filter, m_Record);
			return {};
		}

	private:
		bool m_Filter;
		InputRecord& m_Record;
	};

	// Runs a hidden window with ImGui for a few frames.
	void RunInputProbe(bool filter, InputRecord& record)
	{
		ApplicationSpecification specification;
		specification.Name = "ImGui Input Test";
		specification.Window.Title = "Strada ImGui Input Test";
		specification.Window.Width = 320;
		specification.Window.Height = 240;
		specification.Window.Visible = false;
		specification.EnableImGui = true;
		specification.EnableScripting = false;
		specification.EnableValidation = true;
		specification.MaxFrames = 10;
		InputProbeApplication application(specification, filter, record);
		Input::Reset();
		CHECK(application.Run() == 0);
		Input::Reset();
		CHECK(GraphicsDevice::GetValidationErrorCount() == 0);
	}

	bool Contains(std::vector<std::string> const& events, std::string const& event)
	{
		for (std::string const& seen : events)
		{
			if (seen == event)
			{
				return true;
			}
		}
		return false;
	}
}

TEST_CASE("ImGui: without an input filter the application gets the main window's input, and ImGui too")
{
	{
		// A Vulkan device is required; the probe device is released at the end of the block.
		ST_REQUIRE_GPU();
	}
	ST_REQUIRE_DISPLAY();
	InputRecord record;
	RunInputProbe(false, record);
	CHECK(record.InputSpace);
	CHECK(record.InputLeftButton);
	CHECK(record.InterfaceSpace);
	CHECK(record.InterfaceEscape);
	CHECK(record.InterfaceLeftButton);
	CHECK(record.InterfaceCharacter);
	glm::vec2 const expected = glm::vec2(10.0f, 20.0f) + (record.ViewportsEnabled ? record.MainWindowPosition : glm::vec2(0.0f));
	CHECK(record.InterfaceMouse == expected);
}

TEST_CASE("ImGui: an input filter sees every window's input first and decides what ImGui receives")
{
	{
		// A Vulkan device is required; the probe device is released at the end of the block.
		ST_REQUIRE_GPU();
	}
	ST_REQUIRE_DISPLAY();
	InputRecord record;
	RunInputProbe(true, record);
	// The filter is the application's only way to the window's input.
	CHECK_FALSE(record.InputSpace);
	CHECK_FALSE(record.InputLeftButton);

	CHECK(Contains(record.MainEvents, KeyPressedEvent(KeyCode::Space, false).ToString()));
	CHECK(Contains(record.MainEvents, KeyPressedEvent(KeyCode::Escape, false).ToString()));
	CHECK(Contains(record.MainEvents, MouseButtonPressedEvent(MouseButton::Left).ToString()));
	CHECK(Contains(record.MainEvents, KeyTypedEvent('x').ToString()));
	CHECK(Contains(record.MainEvents, MouseScrolledEvent(0.0f, 1.0f).ToString()));
	// Positions are in ImGui's coordinates: the desktop's with multi-viewports.
	glm::vec2 const cursor = glm::vec2(10.0f, 20.0f) + (record.ViewportsEnabled ? record.MainWindowPosition : glm::vec2(0.0f));
	CHECK(Contains(record.MainEvents, MouseMovedEvent(cursor.x, cursor.y).ToString()));
	REQUIRE(record.MainContexts.size() == record.MainEvents.size());
	for (size_t i = 0; i < record.MainEvents.size(); i++)
	{
		CAPTURE(record.MainEvents[i]);
		// Keys without a printable character are their own layout key; no modifier is held.
		if (record.MainEvents[i] == KeyPressedEvent(KeyCode::Escape, false).ToString())
		{
			CHECK(record.MainContexts[i].LayoutKey == KeyCode::Escape);
		}
		if (record.MainEvents[i] == MouseMovedEvent(cursor.x, cursor.y).ToString())
		{
			CHECK(record.MainContexts[i].Cursor == cursor);
		}
		CHECK_FALSE(record.MainContexts[i].Shift);
		CHECK_FALSE(record.MainContexts[i].Control);
		CHECK(record.MainContexts[i].Viewport == record.MainViewport);
	}

	// ImGui received what the filter let through, and nothing else.
	CHECK_FALSE(record.InterfaceSpace);
	CHECK(record.InterfaceEscape);
	CHECK(record.InterfaceLeftButton);
	CHECK(record.InterfaceCharacter);

	// A panel's own window goes through the filter too.
	if (record.ViewportsEnabled)
	{
		REQUIRE(record.PanelWindowFound);
		CHECK(Contains(record.PanelEvents, KeyPressedEvent(KeyCode::E, false).ToString()));
		glm::vec2 const panelCursor = record.PanelWindowPosition + glm::vec2(5.0f, 6.0f);
		CHECK(Contains(record.PanelEvents, MouseMovedEvent(panelCursor.x, panelCursor.y).ToString()));
		CHECK(record.InterfaceE);
		// Events name the viewport of the window they come from.
		CHECK(record.PanelViewport != record.MainViewport);
		for (ImGuiLayer::InputContext const& context : record.PanelContexts)
		{
			CHECK(context.Viewport == record.PanelViewport);
		}
	}
}
