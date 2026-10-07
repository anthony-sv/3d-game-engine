#include "Editor/EditorLayer.h"

#include "Strada/Core/Application.h"

#include <imgui.h>

namespace Strada
{
	EditorLayer::EditorLayer()
		: Layer("EditorLayer")
	{
	}

	void EditorLayer::OnUpdate(Timestep timestep)
	{
		(void)timestep;
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
		ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
		DrawMenuBar();

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
			ImGui::MenuItem("Console", nullptr, &m_ShowConsole);
			ImGui::MenuItem("Statistics", nullptr, &m_ShowStatistics);
			ImGui::Separator();
			ImGui::MenuItem("ImGui Demo", nullptr, &m_ShowImGuiDemo);
			ImGui::EndMenu();
		}

		ImGui::EndMainMenuBar();
	}
}
