#include "stpch.h"
#include "Strada/ImGui/ImGuiLayer.h"

#include "Strada/Core/Application.h"
#include "Strada/Core/EmbeddedResources.h"
#include "Strada/Core/Events/Event.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Window.h"

#include <backends/imgui_impl_glfw.h>
#include <imgui.h>

namespace Strada
{
	namespace
	{
		// The UI font size in pixels at a content scale of 1.
		constexpr float FontSize = 15.0f;

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

		if (!ImGui_ImplGlfw_InitForVulkan(window->GetNativeWindow(), true))
		{
			ST_CORE_ERROR("Failed to initialize the ImGui GLFW backend");
			ImGui::DestroyContext();
			return;
		}
		if (m_ViewportsEnabled)
		{
			// Files dropped onto a panel dragged out of the main window reach the application like files dropped onto the
			// main window: the backend's platform windows forward their drops to it.
			ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
			m_CreatePlatformWindow = platformIO.Platform_CreateWindow;
			platformIO.Platform_CreateWindow = [](ImGuiViewport* viewport)
			{
				// Platform windows are created while frames end, when the application holds the attached layer.
				Application const& application = Application::Get();
				ImGuiLayer const* const layer = application.GetImGuiLayer();
				ST_CORE_ASSERT(layer != nullptr && layer->m_CreatePlatformWindow != nullptr, "ImGui platform windows need the ImGui layer");
				layer->m_CreatePlatformWindow(viewport);
				if (Window* const mainWindow = application.GetWindow())
				{
					mainWindow->ForwardFileDrops(static_cast<GLFWwindow*>(viewport->PlatformHandle));
				}
			};
		}

		if (Result<void> result = m_Renderer.Init(); !result)
		{
			ST_CORE_ERROR("Failed to initialize the ImGui renderer: {}", result.GetError());
			ImGui_ImplGlfw_Shutdown();
			ImGui::DestroyContext();
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
		m_Renderer.Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
		m_MonospaceFont = nullptr;
		m_Attached = false;
	}

	void ImGuiLayer::OnEvent(Event& event)
	{
		if (!m_Attached || !m_BlockEvents)
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

		ImGui_ImplGlfw_NewFrame();
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
