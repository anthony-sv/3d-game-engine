#pragma once

#include "Editor/Panels/ConsolePanel.h"
#include "Editor/Panels/StatisticsPanel.h"

#include "Strada/Core/Layer.h"

#include <filesystem>

namespace Strada
{
	// Root of the editor UI: dockspace, menu bar and panels.
	class EditorLayer : public Layer
	{
	public:
		EditorLayer();

		void OnUpdate(Timestep timestep) override;
		void OnImGuiRender() override;

		// Saves a screenshot of the editor window when the given frame (0-based) is rendered.
		void RequestScreenshotAtFrame(std::filesystem::path path, uint64_t frame);

	private:
		void DrawMenuBar();

		ConsolePanel m_ConsolePanel;
		StatisticsPanel m_StatisticsPanel;
		bool m_ShowConsole = true;
		bool m_ShowStatistics = true;
		bool m_ShowImGuiDemo = false;

		std::filesystem::path m_ScreenshotPath;
		uint64_t m_ScreenshotFrame = 0;
	};
}
