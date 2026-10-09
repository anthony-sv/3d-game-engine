#include "Editor/EditorLayer.h"

#include "Editor/Automation/AutomationInstance.h"
#include "Editor/Automation/EditorCommands.h"
#include "Editor/DefaultScene.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/Application.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Platform.h"
#include "Strada/Core/Version.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace Strada
{
	EditorLayer::EditorLayer(EditorLayerSpecification specification)
		: Layer("EditorLayer"),
		  m_Specification(std::move(specification)),
		  m_Operations(m_Context),
		  m_AutomationServer(m_Commands)
	{
	}

	void EditorLayer::OnAttach()
	{
		if (AssetManager::IsInitialized())
		{
			m_Context.SetAssetReferenceResolver(
				[](std::string_view reference)
				{
					return AssetManager::ResolveReference(reference);
				});
		}

		Application& application = Application::Get();
		EditorCommandEnvironment environment;
		if (application.GetWindow() != nullptr)
		{
			environment.CaptureScreenshot = [](EditorCommandEnvironment::ScreenshotCallback callback)
			{
				Application::Get().RequestScreenshotImage(std::move(callback));
			};
		}
		environment.RequestQuit = []
		{
			Application::Get().Close();
		};
		Result<void> registered = RegisterEditorCommands(m_Commands, m_Operations, std::move(environment));
		ST_ASSERT(registered.IsOk(), "The built-in editor commands must register");
		(void)registered;

		if (application.GetImGuiLayer() != nullptr)
		{
			m_ViewportPanel = CreateScope<ViewportPanel>();
		}

		bool sceneOpened = false;
		if (!m_Specification.ScenePath.empty())
		{
			Result<std::vector<std::string>> opened = m_Operations.OpenScene(m_Specification.ScenePath);
			sceneOpened = opened.IsOk();
			if (!opened)
			{
				ST_ERROR("Could not open '{}': {}", FileSystem::PathToUtf8(m_Specification.ScenePath), opened.GetError());
			}
		}
		if (!sceneOpened)
		{
			m_Context.SetScene(CreateDefaultScene(), {});
		}

		if (!m_Specification.EnableAutomation)
		{
			return;
		}
		AutomationServerSpecification serverSpecification;
		serverSpecification.Port = m_Specification.AutomationPort;
		if (Result<void> started = m_AutomationServer.Start(serverSpecification); !started)
		{
			ST_ERROR("Automation is unavailable: {}", started.GetError());
			return;
		}

		AutomationInstanceInfo info;
		info.ProcessID = Platform::GetProcessID();
		info.Port = m_AutomationServer.GetPort();
		info.Token = m_AutomationServer.GetToken();
		info.Version = EngineVersion::String;
		Result<std::filesystem::path> written = AutomationInstance::Write(info);
		if (written)
		{
			m_InstanceFileWritten = true;
			ST_INFO("Automation instance file: {}", FileSystem::PathToUtf8(written.GetValue()));
		}
		else
		{
			ST_ERROR("Could not write the automation instance file: {}", written.GetError());
		}
	}

	void EditorLayer::OnDetach()
	{
		m_AutomationServer.Stop();
		if (m_InstanceFileWritten)
		{
			AutomationInstance::Remove(Platform::GetProcessID());
			m_InstanceFileWritten = false;
		}
	}

	void EditorLayer::OnUpdate(Timestep timestep)
	{
		(void)timestep;
		m_AutomationServer.ProcessRequests();
		if (!m_ScreenshotPath.empty() && Application::Get().GetFrameCount() == m_ScreenshotFrame)
		{
			Application::Get().RequestScreenshot(m_ScreenshotPath);
			m_ScreenshotPath.clear();
		}
	}

	void EditorLayer::RequestScreenshotAtFrame(std::filesystem::path path, uint64_t frame)
	{
		m_ScreenshotPath = std::move(path);
		m_ScreenshotFrame = frame;
	}

	void EditorLayer::OnImGuiRender()
	{
		ImGuiID const dockspace = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
		// First run (no saved layout): viewport in the center, statistics on the right, console at the bottom.
		ImGuiDockNode const* root = ImGui::DockBuilderGetNode(dockspace);
		if (root == nullptr || (root->IsLeafNode() && root->Windows.empty()))
		{
			ImGui::DockBuilderRemoveNode(dockspace);
			ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
			ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
			ImGuiID center = dockspace;
			ImGuiID const bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, nullptr, &center);
			ImGuiID const right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.22f, nullptr, &center);
			ImGui::DockBuilderDockWindow("Viewport", center);
			ImGui::DockBuilderDockWindow("Statistics", right);
			ImGui::DockBuilderDockWindow("Console", bottom);
			ImGui::DockBuilderFinish(dockspace);
		}
		DrawMenuBar();

		if (m_ViewportPanel && m_ShowViewport)
		{
			m_ViewportPanel->OnImGuiRender(m_Operations, m_ShowViewport);
		}
		if (m_ShowConsole)
		{
			m_ConsolePanel.OnImGuiRender(m_ShowConsole);
		}
		if (m_ShowStatistics)
		{
			m_StatisticsPanel.OnImGuiRender(m_ShowStatistics);
		}
		if (m_ShowImGuiDemo)
		{
			ImGui::ShowDemoWindow(&m_ShowImGuiDemo);
		}
	}

	void EditorLayer::DrawMenuBar()
	{
		if (!ImGui::BeginMainMenuBar())
		{
			return;
		}

		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("Exit", "Alt+F4"))
			{
				Application::Get().Close();
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("View"))
		{
			ImGui::MenuItem("Viewport", nullptr, &m_ShowViewport);
			ImGui::MenuItem("Console", nullptr, &m_ShowConsole);
			ImGui::MenuItem("Statistics", nullptr, &m_ShowStatistics);
			ImGui::Separator();
			ImGui::MenuItem("ImGui Demo", nullptr, &m_ShowImGuiDemo);
			ImGui::EndMenu();
		}

		std::string const status = m_AutomationServer.IsRunning()
		                               ? fmt::format("Automation: 127.0.0.1:{} ({} connected)", m_AutomationServer.GetPort(),
		                                             m_AutomationServer.GetConnectionCount())
		                               : std::string("Automation: off");
		ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(status.c_str()).x - ImGui::GetStyle().ItemSpacing.x * 2.0f);
		ImGui::TextDisabled("%s", status.c_str());

		ImGui::EndMainMenuBar();
	}
}
