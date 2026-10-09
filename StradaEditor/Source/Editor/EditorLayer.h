#pragma once

#include "Editor/Automation/AutomationServer.h"
#include "Editor/Automation/CommandRegistry.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/Panels/ConsolePanel.h"
#include "Editor/Panels/StatisticsPanel.h"

#include "Strada/Core/Layer.h"

#include <cstdint>
#include <filesystem>

namespace Strada
{
	struct EditorLayerSpecification
	{
		// Start the automation server (JSON-RPC on 127.0.0.1) and write the instance file clients use to find it.
		bool EnableAutomation = true;
		// 0 picks a free port.
		uint16_t AutomationPort = 0;
		// Scene file opened at startup; empty starts with an empty scene.
		std::filesystem::path ScenePath;
	};

	// Root of the editor: owns the document state (scene, undo history, selection), the automation command registry and
	// server, and the UI (dockspace, menu bar and panels) when ImGui is available.
	class EditorLayer : public Layer
	{
	public:
		explicit EditorLayer(EditorLayerSpecification specification = {});

		void OnAttach() override;
		void OnDetach() override;
		void OnUpdate(Timestep timestep) override;
		void OnImGuiRender() override;

		// Saves a screenshot of the editor window when the given frame (0-based) is rendered.
		void RequestScreenshotAtFrame(std::filesystem::path path, uint64_t frame);

		EditorContext& GetContext() { return m_Context; }
		CommandRegistry const& GetCommands() const { return m_Commands; }
		AutomationServer const& GetAutomationServer() const { return m_AutomationServer; }

	private:
		void DrawMenuBar();

		EditorLayerSpecification m_Specification;
		EditorContext m_Context;
		EditorOperations m_Operations;
		CommandRegistry m_Commands;
		AutomationServer m_AutomationServer;
		bool m_InstanceFileWritten = false;

		ConsolePanel m_ConsolePanel;
		StatisticsPanel m_StatisticsPanel;
		bool m_ShowConsole = true;
		bool m_ShowStatistics = true;
		bool m_ShowImGuiDemo = false;

		std::filesystem::path m_ScreenshotPath;
		uint64_t m_ScreenshotFrame = 0;
	};
}
