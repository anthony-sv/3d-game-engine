#include "stpch.h"
#include "Strada/ImGui/ImGuiLayer.h"

#include "Strada/Core/Application.h"
#include "Strada/Core/EmbeddedResources.h"
#include "Strada/Core/Events/Event.h"
#include "Strada/Core/Events/KeyEvent.h"
#include "Strada/Core/Events/MouseEvent.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Window.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>
#include <imgui.h>

#include <array>
#include <utility>

namespace Strada
{
	namespace
	{
		// The UI font size in pixels at a content scale of 1.
		constexpr float FontSize = 15.0f;

		// What the GLFW input callbacks need while a layer is attached (there is one ImGui context, so one layer).
		struct InputCallbackData
		{
			ImGuiLayer* Layer = nullptr;
			// The application's window (whose GLFW window is MainWindow): the platform windows of panels forward the files
			// dropped onto them to it.
			Window* ApplicationWindow = nullptr;
			GLFWwindow* MainWindow = nullptr;
			// The main window's callbacks from before the layer attached (the application's Window): its input still goes
			// there first while no input filter is set.
			GLFWwindowfocusfun ApplicationFocus = nullptr;
			GLFWcursorenterfun ApplicationCursorEnter = nullptr;
			GLFWcursorposfun ApplicationCursorPos = nullptr;
			GLFWmousebuttonfun ApplicationMouseButton = nullptr;
			GLFWscrollfun ApplicationScroll = nullptr;
			GLFWkeyfun ApplicationKey = nullptr;
			GLFWcharfun ApplicationChar = nullptr;
			// The platform backend's window creation: panels dragged out of the main window get the layer's callbacks too.
			void (*CreatePlatformWindow)(ImGuiViewport* viewport) = nullptr;
		};

		InputCallbackData s_Input;

		// ImGui's coordinates of a position in a window: the desktop's with multi-viewports, the window's without (as the
		// platform backend reports positions).
		glm::vec2 ToInterfacePosition(GLFWwindow* window, double x, double y, bool desktop)
		{
			if (desktop)
			{
				int windowX = 0;
				int windowY = 0;
				glfwGetWindowPos(window, &windowX, &windowY);
				x += windowX;
				y += windowY;
			}
			return glm::vec2(static_cast<float>(x), static_cast<float>(y));
		}

		ImGuiLayer::InputContext MakeInputContext(GLFWwindow* window, glm::vec2 cursor)
		{
			ImGuiLayer::InputContext context;
			context.Cursor = cursor;
			if (ImGuiViewport const* const viewport = ImGui::FindViewportByPlatformHandle(window))
			{
				context.Viewport = viewport->ID;
			}
			// The key state GLFW keeps per window also covers the key being pressed (X11 leaves it out of the modifier bits).
			auto const held = [window](int left, int right)
			{
				return glfwGetKey(window, left) == GLFW_PRESS || glfwGetKey(window, right) == GLFW_PRESS;
			};
			context.Shift = held(GLFW_KEY_LEFT_SHIFT, GLFW_KEY_RIGHT_SHIFT);
			context.Control = held(GLFW_KEY_LEFT_CONTROL, GLFW_KEY_RIGHT_CONTROL);
			context.Alt = held(GLFW_KEY_LEFT_ALT, GLFW_KEY_RIGHT_ALT);
			context.Super = held(GLFW_KEY_LEFT_SUPER, GLFW_KEY_RIGHT_SUPER);
			return context;
		}

		ImGuiLayer::InputContext MakeInputContext(GLFWwindow* window, bool desktop)
		{
			double x = 0.0;
			double y = 0.0;
			glfwGetCursorPos(window, &x, &y);
			return MakeInputContext(window, ToInterfacePosition(window, x, y, desktop));
		}

		// The key the keyboard layout names a physical key, translated the way ImGui's GLFW backend translates keys for
		// shortcuts: printable keys by the character they type, the keypad and every other key as they are.
		KeyCode GetLayoutKey(int key, int scancode)
		{
			KeyCode const physical = static_cast<KeyCode>(key);
			if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_EQUAL)
			{
				return physical;
			}
			// GLFW reports keys without a name through the error callback; here that is an answer, not an error.
			GLFWerrorfun const errorCallback = glfwSetErrorCallback(nullptr);
			char const* const name = glfwGetKeyName(key, scancode);
			glfwSetErrorCallback(errorCallback);
			(void)glfwGetError(nullptr);
			if (name == nullptr || name[0] == '\0' || name[1] != '\0')
			{
				return physical;
			}

			char const character = name[0];
			if (character >= '0' && character <= '9')
			{
				return static_cast<KeyCode>(static_cast<int>(KeyCode::D0) + (character - '0'));
			}
			if (character >= 'a' && character <= 'z')
			{
				return static_cast<KeyCode>(static_cast<int>(KeyCode::A) + (character - 'a'));
			}
			if (character >= 'A' && character <= 'Z')
			{
				return static_cast<KeyCode>(static_cast<int>(KeyCode::A) + (character - 'A'));
			}
			constexpr std::array<std::pair<char, KeyCode>, 11> Punctuation = {{
				{'`', KeyCode::GraveAccent},
				{'-', KeyCode::Minus},
				{'=', KeyCode::Equal},
				{'[', KeyCode::LeftBracket},
				{']', KeyCode::RightBracket},
				{'\\', KeyCode::Backslash},
				{',', KeyCode::Comma},
				{';', KeyCode::Semicolon},
				{'\'', KeyCode::Apostrophe},
				{'.', KeyCode::Period},
				{'/', KeyCode::Slash},
			}};
			for (auto const& [punctuation, code] : Punctuation)
			{
				if (character == punctuation)
				{
					return code;
				}
			}
			return physical;
		}

		// Where ImGui wants no OS cursor (ImGuiMouseCursor_None, such as over the editor's game view while the game hides the
		// cursor, or a cursor ImGui draws itself), the cursor is hidden in every window ImGui draws into, as it can be over any
		// of them. The GLFW backend means to do that but hides it in the first window only (the main one), and in none in the
		// frame after cursor changes were off (ImGuiConfigFlags_NoMouseCursorChange): this completes it under the backend's
		// own conditions. Once ImGui wants a cursor again, the backend shows it in every window.
		void HideCursorInPlatformWindows(GLFWwindow* mainWindow)
		{
			ImGuiIO const& io = ImGui::GetIO();
			bool const hidden = ImGui::GetMouseCursor() == ImGuiMouseCursor_None || io.MouseDrawCursor;
			if (!hidden || (io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) != 0 ||
			    glfwGetInputMode(mainWindow, GLFW_CURSOR) == GLFW_CURSOR_DISABLED)
			{
				return;
			}
			for (ImGuiViewport const* viewport : ImGui::GetPlatformIO().Viewports)
			{
				if (GLFWwindow* const window = static_cast<GLFWwindow*>(viewport->PlatformHandle))
				{
					glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
				}
			}
		}

		ImVec4 Rgb(uint32_t rgb, float alpha = 1.0f)
		{
			return ImVec4(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
			              static_cast<float>(rgb & 0xFF) / 255.0f, alpha);
		}

		ImFont* AddEmbeddedFont(std::string_view name)
		{
			std::span<uint8_t const> const data = EmbeddedResources::Get(name);
			if (data.empty())
			{
				ST_CORE_WARN("The embedded font '{}' is missing", name);
				return nullptr;
			}
			ImFontConfig config;
			// The data lives in the binary; ImGui only reads it.
			config.FontDataOwnedByAtlas = false;
			return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<uint8_t*>(data.data()), static_cast<int>(data.size()), FontSize,
			                                                  &config);
		}
	}

	// The platform backend is initialized without its own callbacks; these take their place on the main window and on the
	// platform windows of panels, offer each input event to the input filter (or, without one, to the application, as the
	// backend's chained callbacks did), and pass what ImGui receives on to the backend.
	struct ImGuiLayer::InputCallbacks
	{
		static void Install(ImGuiLayer& layer, Window& applicationWindow)
		{
			GLFWwindow* const window = applicationWindow.GetNativeWindow();
			s_Input.Layer = &layer;
			s_Input.ApplicationWindow = &applicationWindow;
			s_Input.MainWindow = window;
			s_Input.ApplicationFocus = glfwSetWindowFocusCallback(window, OnFocus);
			s_Input.ApplicationCursorEnter = glfwSetCursorEnterCallback(window, OnCursorEnter);
			s_Input.ApplicationCursorPos = glfwSetCursorPosCallback(window, OnCursorPos);
			s_Input.ApplicationMouseButton = glfwSetMouseButtonCallback(window, OnMouseButton);
			s_Input.ApplicationScroll = glfwSetScrollCallback(window, OnScroll);
			s_Input.ApplicationKey = glfwSetKeyCallback(window, OnKey);
			s_Input.ApplicationChar = glfwSetCharCallback(window, OnChar);
			ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
			if (platform.Platform_CreateWindow != nullptr)
			{
				s_Input.CreatePlatformWindow = platform.Platform_CreateWindow;
				platform.Platform_CreateWindow = OnCreatePlatformWindow;
			}
		}

		// Gives the main window its application callbacks back. The platform windows keep the layer's until the backend
		// destroys them.
		static void RestoreMainWindow()
		{
			GLFWwindow* const window = s_Input.MainWindow;
			glfwSetWindowFocusCallback(window, s_Input.ApplicationFocus);
			glfwSetCursorEnterCallback(window, s_Input.ApplicationCursorEnter);
			glfwSetCursorPosCallback(window, s_Input.ApplicationCursorPos);
			glfwSetMouseButtonCallback(window, s_Input.ApplicationMouseButton);
			glfwSetScrollCallback(window, s_Input.ApplicationScroll);
			glfwSetKeyCallback(window, s_Input.ApplicationKey);
			glfwSetCharCallback(window, s_Input.ApplicationChar);
		}

		static void OnCreatePlatformWindow(ImGuiViewport* viewport)
		{
			s_Input.CreatePlatformWindow(viewport);
			// The backend installed its input callbacks on the new window; these replace them and pass events on to them.
			GLFWwindow* const window = static_cast<GLFWwindow*>(viewport->PlatformHandle);
			glfwSetCursorPosCallback(window, OnCursorPos);
			glfwSetMouseButtonCallback(window, OnMouseButton);
			glfwSetScrollCallback(window, OnScroll);
			glfwSetKeyCallback(window, OnKey);
			glfwSetCharCallback(window, OnChar);
			// Files dropped onto a panel dragged out of the main window reach the application like files dropped onto it.
			s_Input.ApplicationWindow->ForwardFileDrops(window);
		}

		static bool IsFiltered() { return static_cast<bool>(s_Input.Layer->m_InputFilter); }

		static bool Filter(Event const& event, InputContext const& context) { return s_Input.Layer->m_InputFilter(event, context); }

		static bool IsDesktopCoordinates() { return s_Input.Layer->m_ViewportsEnabled; }

		// Window focus is not input: the application always learns about the main window's, and so does ImGui.
		static void OnFocus(GLFWwindow* window, int focused)
		{
			if (s_Input.ApplicationFocus != nullptr)
			{
				s_Input.ApplicationFocus(window, focused);
			}
			ImGui_ImplGlfw_WindowFocusCallback(window, focused);
		}

		static void OnCursorEnter(GLFWwindow* window, int entered)
		{
			if (s_Input.ApplicationCursorEnter != nullptr)
			{
				s_Input.ApplicationCursorEnter(window, entered);
			}
			ImGui_ImplGlfw_CursorEnterCallback(window, entered);
		}

		static void OnCursorPos(GLFWwindow* window, double x, double y)
		{
			bool interfaceReceives = true;
			if (IsFiltered())
			{
				InputContext const context = MakeInputContext(window, ToInterfacePosition(window, x, y, IsDesktopCoordinates()));
				MouseMovedEvent const event(context.Cursor.x, context.Cursor.y);
				interfaceReceives = Filter(event, context);
			}
			else if (window == s_Input.MainWindow && s_Input.ApplicationCursorPos != nullptr)
			{
				s_Input.ApplicationCursorPos(window, x, y);
			}
			if (interfaceReceives)
			{
				ImGui_ImplGlfw_CursorPosCallback(window, x, y);
			}
		}

		static void OnMouseButton(GLFWwindow* window, int button, int action, int mods)
		{
			bool interfaceReceives = true;
			if (IsFiltered())
			{
				if (button >= 0 && button < MouseButtonCount && (action == GLFW_PRESS || action == GLFW_RELEASE))
				{
					InputContext const context = MakeInputContext(window, IsDesktopCoordinates());
					MouseButton const mouseButton = static_cast<MouseButton>(button);
					if (action == GLFW_PRESS)
					{
						interfaceReceives = Filter(MouseButtonPressedEvent(mouseButton), context);
					}
					else
					{
						interfaceReceives = Filter(MouseButtonReleasedEvent(mouseButton), context);
					}
				}
			}
			else if (window == s_Input.MainWindow && s_Input.ApplicationMouseButton != nullptr)
			{
				s_Input.ApplicationMouseButton(window, button, action, mods);
			}
			if (interfaceReceives)
			{
				ImGui_ImplGlfw_MouseButtonCallback(window, button, action, mods);
			}
		}

		static void OnScroll(GLFWwindow* window, double x, double y)
		{
			bool interfaceReceives = true;
			if (IsFiltered())
			{
				interfaceReceives = Filter(MouseScrolledEvent(static_cast<float>(x), static_cast<float>(y)),
				                           MakeInputContext(window, IsDesktopCoordinates()));
			}
			else if (window == s_Input.MainWindow && s_Input.ApplicationScroll != nullptr)
			{
				s_Input.ApplicationScroll(window, x, y);
			}
			if (interfaceReceives)
			{
				ImGui_ImplGlfw_ScrollCallback(window, x, y);
			}
		}

		static void OnKey(GLFWwindow* window, int key, int scancode, int action, int mods)
		{
			bool interfaceReceives = true;
			if (IsFiltered())
			{
				// Keys Strada has no code for (GLFW_KEY_UNKNOWN) only go to ImGui, which ignores them too.
				if (key >= 0 && key < KeyCodeCount && (action == GLFW_PRESS || action == GLFW_REPEAT || action == GLFW_RELEASE))
				{
					InputContext context = MakeInputContext(window, IsDesktopCoordinates());
					context.LayoutKey = GetLayoutKey(key, scancode);
					KeyCode const keyCode = static_cast<KeyCode>(key);
					if (action == GLFW_RELEASE)
					{
						interfaceReceives = Filter(KeyReleasedEvent(keyCode), context);
					}
					else
					{
						interfaceReceives = Filter(KeyPressedEvent(keyCode, action == GLFW_REPEAT), context);
					}
				}
			}
			else if (window == s_Input.MainWindow && s_Input.ApplicationKey != nullptr)
			{
				s_Input.ApplicationKey(window, key, scancode, action, mods);
			}
			if (interfaceReceives)
			{
				ImGui_ImplGlfw_KeyCallback(window, key, scancode, action, mods);
			}
		}

		static void OnChar(GLFWwindow* window, unsigned int codepoint)
		{
			bool interfaceReceives = true;
			if (IsFiltered())
			{
				interfaceReceives = Filter(KeyTypedEvent(codepoint), MakeInputContext(window, IsDesktopCoordinates()));
			}
			else if (window == s_Input.MainWindow && s_Input.ApplicationChar != nullptr)
			{
				s_Input.ApplicationChar(window, codepoint);
			}
			if (interfaceReceives)
			{
				ImGui_ImplGlfw_CharCallback(window, codepoint);
			}
		}
	};

	void ImGuiLayer::ApplyTheme()
	{
		ImGuiStyle& style = ImGui::GetStyle();
		style = ImGuiStyle();
		style.FontSizeBase = FontSize;
		style.WindowPadding = ImVec2(8.0f, 8.0f);
		style.FramePadding = ImVec2(8.0f, 4.0f);
		style.CellPadding = ImVec2(6.0f, 3.0f);
		style.ItemSpacing = ImVec2(8.0f, 6.0f);
		style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
		style.IndentSpacing = 16.0f;
		style.ScrollbarSize = 12.0f;
		style.GrabMinSize = 10.0f;
		style.WindowBorderSize = 1.0f;
		style.ChildBorderSize = 1.0f;
		style.PopupBorderSize = 1.0f;
		style.FrameBorderSize = 0.0f;
		style.TabBorderSize = 0.0f;
		style.TabBarBorderSize = 1.0f;
		style.TabBarOverlineSize = 2.0f;
		style.WindowRounding = 6.0f;
		style.ChildRounding = 4.0f;
		style.FrameRounding = 4.0f;
		style.PopupRounding = 4.0f;
		style.ScrollbarRounding = 6.0f;
		style.GrabRounding = 4.0f;
		style.TabRounding = 4.0f;
		style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
		// Docked windows have their tab bar; no menu triangle in the corner.
		style.WindowMenuButtonPosition = ImGuiDir_None;
		style.SeparatorTextBorderSize = 1.0f;
		style.SeparatorTextPadding = ImVec2(0.0f, 3.0f);
		style.DockingSeparatorSize = 2.0f;

		ImVec4 const accent = Rgb(0x3D8BFD);
		ImVec4* colors = style.Colors;
		colors[ImGuiCol_Text] = Rgb(0xE4E7EC);
		colors[ImGuiCol_TextDisabled] = Rgb(0x868C98);
		colors[ImGuiCol_WindowBg] = Rgb(0x262A31);
		colors[ImGuiCol_ChildBg] = Rgb(0x262A31, 0.0f);
		colors[ImGuiCol_PopupBg] = Rgb(0x2C3038, 0.98f);
		colors[ImGuiCol_Border] = Rgb(0x16181C);
		colors[ImGuiCol_BorderShadow] = Rgb(0x000000, 0.0f);
		colors[ImGuiCol_FrameBg] = Rgb(0x33373F);
		colors[ImGuiCol_FrameBgHovered] = Rgb(0x3C414B);
		colors[ImGuiCol_FrameBgActive] = Rgb(0x464C58);
		colors[ImGuiCol_TitleBg] = Rgb(0x1C1E23);
		colors[ImGuiCol_TitleBgActive] = Rgb(0x1C1E23);
		colors[ImGuiCol_TitleBgCollapsed] = Rgb(0x1C1E23);
		colors[ImGuiCol_MenuBarBg] = Rgb(0x1F2227);
		colors[ImGuiCol_ScrollbarBg] = Rgb(0x1F2227, 0.6f);
		colors[ImGuiCol_ScrollbarGrab] = Rgb(0x4A505C);
		colors[ImGuiCol_ScrollbarGrabHovered] = Rgb(0x5A6170);
		colors[ImGuiCol_ScrollbarGrabActive] = Rgb(0x6A7284);
		colors[ImGuiCol_CheckMark] = Rgb(0x5A9DFF);
		colors[ImGuiCol_SliderGrab] = Rgb(0x5A9DFF);
		colors[ImGuiCol_SliderGrabActive] = Rgb(0x7AB0FF);
		colors[ImGuiCol_Button] = Rgb(0x3A3F49);
		colors[ImGuiCol_ButtonHovered] = Rgb(0x474D59);
		colors[ImGuiCol_ButtonActive] = Rgb(0x2F6FDB);
		colors[ImGuiCol_Header] = Rgb(0x363B45);
		colors[ImGuiCol_HeaderHovered] = Rgb(0x404653);
		colors[ImGuiCol_HeaderActive] = Rgb(0x2F5FB0);
		colors[ImGuiCol_Separator] = Rgb(0x16181C);
		colors[ImGuiCol_SeparatorHovered] = Rgb(0x3D8BFD, 0.6f);
		colors[ImGuiCol_SeparatorActive] = accent;
		colors[ImGuiCol_ResizeGrip] = Rgb(0x3D8BFD, 0.2f);
		colors[ImGuiCol_ResizeGripHovered] = Rgb(0x3D8BFD, 0.6f);
		colors[ImGuiCol_ResizeGripActive] = accent;
		colors[ImGuiCol_TabHovered] = Rgb(0x3A3F49);
		colors[ImGuiCol_Tab] = Rgb(0x1F2227);
		colors[ImGuiCol_TabSelected] = Rgb(0x262A31);
		colors[ImGuiCol_TabSelectedOverline] = accent;
		colors[ImGuiCol_TabDimmed] = Rgb(0x1F2227);
		colors[ImGuiCol_TabDimmedSelected] = Rgb(0x262A31);
		colors[ImGuiCol_TabDimmedSelectedOverline] = Rgb(0x262A31, 0.0f);
		colors[ImGuiCol_DockingPreview] = Rgb(0x3D8BFD, 0.5f);
		colors[ImGuiCol_DockingEmptyBg] = Rgb(0x1C1E23);
		colors[ImGuiCol_PlotLines] = Rgb(0x5A9DFF);
		colors[ImGuiCol_PlotLinesHovered] = Rgb(0xFFA14A);
		colors[ImGuiCol_PlotHistogram] = Rgb(0x5A9DFF);
		colors[ImGuiCol_PlotHistogramHovered] = Rgb(0xFFA14A);
		colors[ImGuiCol_TableHeaderBg] = Rgb(0x2E323A);
		colors[ImGuiCol_TableBorderStrong] = Rgb(0x16181C);
		colors[ImGuiCol_TableBorderLight] = Rgb(0x30343C);
		colors[ImGuiCol_TableRowBg] = Rgb(0x000000, 0.0f);
		colors[ImGuiCol_TableRowBgAlt] = Rgb(0xFFFFFF, 0.03f);
		colors[ImGuiCol_TextLink] = Rgb(0x5A9DFF);
		colors[ImGuiCol_TextSelectedBg] = Rgb(0x3D8BFD, 0.35f);
		colors[ImGuiCol_DragDropTarget] = Rgb(0xFFA14A);
		colors[ImGuiCol_NavCursor] = accent;
		colors[ImGuiCol_NavWindowingHighlight] = Rgb(0xFFFFFF, 0.7f);
		colors[ImGuiCol_NavWindowingDimBg] = Rgb(0x000000, 0.4f);
		colors[ImGuiCol_ModalWindowDimBg] = Rgb(0x000000, 0.45f);
	}

	ImGuiLayer::ImGuiLayer(ImGuiLayerSpecification specification)
		: Layer("ImGuiLayer"),
		  m_Specification(std::move(specification))
	{
	}

	void ImGuiLayer::OnAttach()
	{
		Window* window = Application::Get().GetWindow();
		ST_CORE_ASSERT(window != nullptr, "ImGuiLayer requires a window");

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();

		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		if (m_Specification.EnableDocking)
		{
			io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		}

		// ImGui cannot position platform windows on Wayland, so viewports stay inside the main window there.
		m_ViewportsEnabled = m_Specification.EnableViewports && !Window::IsWayland();
		if (m_Specification.EnableViewports && !m_ViewportsEnabled)
		{
			ST_CORE_WARN("ImGui multi-viewports are unavailable on Wayland; set STRADA_GLFW_PLATFORM=x11 to enable them");
		}
		if (m_ViewportsEnabled)
		{
			io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
		}

		if (m_Specification.IniFilePath.empty())
		{
			io.IniFilename = nullptr;
		}
		else
		{
			Result<void> created = FileSystem::CreateDirectories(m_Specification.IniFilePath.parent_path());
			if (!created)
			{
				ST_CORE_WARN("ImGui layout will not be saved: {}", created.GetError());
			}
			m_IniFilePath = FileSystem::PathToUtf8(m_Specification.IniFilePath);
			io.IniFilename = m_IniFilePath.c_str();
		}

		ApplyTheme();
		// The first font added is the default.
		AddEmbeddedFont(EmbeddedResources::DefaultFont);
		m_MonospaceFont = AddEmbeddedFont(EmbeddedResources::MonospaceFont);
		ImGuiStyle& style = ImGui::GetStyle();
		if (m_ViewportsEnabled)
		{
			// Platform windows look identical to regular ones.
			style.WindowRounding = 0.0f;
			style.Colors[ImGuiCol_WindowBg].w = 1.0f;
		}

		float const contentScale = ImGui_ImplGlfw_GetContentScaleForWindow(window->GetNativeWindow());
		if (contentScale > 0.0f && contentScale != 1.0f)
		{
			style.ScaleAllSizes(contentScale);
			style.FontScaleDpi = contentScale;
		}

		// The layer installs the input callbacks itself (InputCallbacks) so that an input filter can keep events from ImGui.
		if (!ImGui_ImplGlfw_InitForVulkan(window->GetNativeWindow(), false))
		{
			ST_CORE_ERROR("Failed to initialize the ImGui GLFW backend");
			ImGui::DestroyContext();
			return;
		}
		InputCallbacks::Install(*this, *window);

		if (Result<void> result = m_Renderer.Init(); !result)
		{
			ST_CORE_ERROR("Failed to initialize the ImGui renderer: {}", result.GetError());
			InputCallbacks::RestoreMainWindow();
			ImGui_ImplGlfw_Shutdown();
			ImGui::DestroyContext();
			s_Input = {};
			return;
		}

		m_Attached = true;
	}

	void ImGuiLayer::OnDetach()
	{
		if (!m_Attached)
		{
			return;
		}

		if (m_FrameStarted)
		{
			ImGui::EndFrame();
			m_FrameStarted = false;
		}
		InputCallbacks::RestoreMainWindow();
		m_Renderer.Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
		s_Input = {};
		m_InputFilter = {};
		m_MonospaceFont = nullptr;
		m_Attached = false;
	}

	void ImGuiLayer::OnEvent(Event& event)
	{
		// Input events arrive here only without an input filter: the application gets them first, as before ImGui, and those
		// ImGui wants to capture stop here.
		if (!m_Attached)
		{
			return;
		}

		ImGuiIO const& io = ImGui::GetIO();
		event.Handled |= event.IsInCategory(EventCategoryMouse) && io.WantCaptureMouse;
		event.Handled |= event.IsInCategory(EventCategoryKeyboard) && io.WantCaptureKeyboard;
	}

	void ImGuiLayer::Begin()
	{
		if (!m_Attached)
		{
			return;
		}

		// The backend applies the cursor the last frame asked for, which ImGui keeps until NewFrame.
		ImGui_ImplGlfw_NewFrame();
		HideCursorInPlatformWindows(s_Input.MainWindow);
		ImGui::NewFrame();
		m_FrameStarted = true;
	}

	void ImGuiLayer::End(nvrhi::IFramebuffer* mainFramebuffer)
	{
		if (!m_Attached || !m_FrameStarted)
		{
			return;
		}
		m_FrameStarted = false;

		ImGui::Render();
		m_Renderer.UpdateTextures();
		if (mainFramebuffer != nullptr)
		{
			m_Renderer.RenderDrawData(ImGui::GetDrawData(), mainFramebuffer, false);
		}

		if (m_ViewportsEnabled)
		{
			ImGui::UpdatePlatformWindows();
			ImGui::RenderPlatformWindowsDefault();
		}
	}
}
