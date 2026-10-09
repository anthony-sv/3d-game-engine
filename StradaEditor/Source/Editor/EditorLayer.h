#pragma once

#include "Editor/Automation/AutomationServer.h"
#include "Editor/Automation/CommandRegistry.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/Panels/ConsolePanel.h"
#include "Editor/Panels/InspectorPanel.h"
#include "Editor/Panels/SceneHierarchyPanel.h"
#include "Editor/Panels/SceneSettingsPanel.h"
#include "Editor/Panels/StatisticsPanel.h"
#include "Editor/Panels/ViewportPanel.h"

#include "Strada/Core/Layer.h"

#include <cstdint>
#include <filesystem>
#include <string>

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
	// server, and the UI (dockspace, menu bar, shortcuts and panels) when ImGui is available. Replacing the scene or closing
	// the editor with unsaved changes asks to save them first.
	class EditorLayer : public Layer
	{
	public:
		explicit EditorLayer(EditorLayerSpecification specification = {});

		void OnAttach() override;
		void OnDetach() override;
		void OnUpdate(Timestep timestep) override;
		void OnImGuiRender() override;
		void OnEvent(Event& event) override;

		// Saves a screenshot of the editor window when the given frame (0-based) is rendered.
		void RequestScreenshotAtFrame(std::filesystem::path path, uint64_t frame);

		EditorContext& GetContext() { return m_Context; }
		CommandRegistry const& GetCommands() const { return m_Commands; }
		AutomationServer const& GetAutomationServer() const { return m_AutomationServer; }

	private:
		// Actions that discard the edited scene; they ask about unsaved changes first.
		enum class SceneAction : uint8_t
		{
			None = 0,
			NewScene,
			OpenScene,
			Quit
		};

		void DrawDockspace();
		void BuildDefaultLayout(uint32_t dockspace);
		void DrawMenuBar();
		void HandleShortcuts();
		void DrawUnsavedChangesPopup();
		void UpdateWindowTitle();

		// Runs the action now, or after the unsaved-changes prompt when the scene has unsaved changes.
		void RequestSceneAction(SceneAction action, std::filesystem::path path = {});
		void PerformSceneAction(SceneAction action, std::filesystem::path const& path);
		void ShowOpenSceneDialog();
		// Saves to the scene's file, or asks for one when the scene has never been saved. False when cancelled or failed.
		bool SaveScene();
		bool SaveSceneAs();

		void DuplicateSelection();
		void DeleteSelection();
		void SelectAll();
		// Where new root entities are placed: the viewport's focal point.
		glm::vec3 GetSpawnPosition() const;

		EditorLayerSpecification m_Specification;
		EditorContext m_Context;
		EditorOperations m_Operations;
		CommandRegistry m_Commands;
		AutomationServer m_AutomationServer;
		bool m_InstanceFileWritten = false;

		ConsolePanel m_ConsolePanel;
		StatisticsPanel m_StatisticsPanel;
		SceneHierarchyPanel m_HierarchyPanel;
		InspectorPanel m_InspectorPanel;
		SceneSettingsPanel m_SceneSettingsPanel;
		// Created when ImGui is available.
		Scope<ViewportPanel> m_ViewportPanel;
		bool m_ShowViewport = true;
		bool m_ShowHierarchy = true;
		bool m_ShowInspector = true;
		bool m_ShowSceneSettings = true;
		bool m_ShowConsole = true;
		bool m_ShowStatistics = true;
		bool m_ShowImGuiDemo = false;
		bool m_ResetLayout = false;

		SceneAction m_PendingAction = SceneAction::None;
		std::filesystem::path m_PendingPath;
		bool m_OpenUnsavedChangesPopup = false;
		std::string m_WindowTitle;

		std::filesystem::path m_ScreenshotPath;
		uint64_t m_ScreenshotFrame = 0;
	};
}
