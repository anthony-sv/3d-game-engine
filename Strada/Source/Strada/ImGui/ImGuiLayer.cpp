#include "stpch.h"
#include "Strada/ImGui/ImGuiLayer.h"

#include "Strada/Core/Application.h"
#include "Strada/Core/Events/Event.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Window.h"

#include <backends/imgui_impl_glfw.h>
#include <imgui.h>

namespace Strada
{
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

		ImGui::StyleColorsDark();
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
