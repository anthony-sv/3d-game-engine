#pragma once

#include "Editor/Automation/AutomationServer.h"
#include "Editor/Automation/CommandRegistry.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/Panels/ConsolePanel.h"
#include "Editor/Panels/ContentBrowserPanel.h"
#include "Editor/Panels/InspectorPanel.h"
#include "Editor/Panels/ProjectSettingsPanel.h"
#include "Editor/Panels/SceneHierarchyPanel.h"
#include "Editor/Panels/SceneSettingsPanel.h"
#include "Editor/Panels/StatisticsPanel.h"
#include "Editor/Panels/ViewportPanel.h"
#include "Editor/PlayMode.h"
#include "Editor/RecentProjects.h"

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
		// Project file opened at startup (with its start scene); empty starts without a project.
		std::filesystem::path ProjectPath;
		// Scene file opened at startup, after the project; empty keeps the project's start scene or the default scene.
		std::filesystem::path ScenePath;
	};

	// Root of the editor: owns the document state (project, scene, undo history, selection), the project's scripts (builds
	// and hot reload), play mode, the automation command registry and server, and the UI (dockspace, menu bar with the
	// play controls, shortcuts and panels) when ImGui is available. Replacing the scene (new or opened scenes and projects)
	// or closing the editor with unsaved changes asks to save them first; playing stops first.
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
		EditorScripts& GetScripts() { return m_Scripts; }
		PlayMode& GetPlayMode() { return m_PlayMode; }
		CommandRegistry const& GetCommands() const { return m_Commands; }
		AutomationServer const& GetAutomationServer() const { return m_AutomationServer; }

	private:
		// Actions that discard the edited scene; they ask about unsaved changes first.
		enum class SceneAction : uint8_t
		{
			None = 0,
			NewScene,
			OpenScene,
			NewProject,
			OpenProject,
			CloseProject,
			Quit
		};

		void DrawDockspace();
		void BuildDefaultLayout(uint32_t dockspace);
		void DrawMenuBar();
		void HandleShortcuts();
		void DrawUnsavedChangesPopup();
		void DrawNewProjectPopup();
		void DrawNewScriptPopup();
		void DrawPlayControls();
		// Plays (building changed scripts first) or simulates the edited scene.
		void StartPlay(EditorPlayState state);
		// The size of the view the game renders to: the viewport, or the project's window when there is none.
		glm::uvec2 GetGameViewSize() const;
		// While the game view is focused the game gets the window's input, with the cursor in game-view pixels.
		void UpdateGameInput();
		// The scripts' state for the menu bar; empty while nothing needs attention.
		std::string GetScriptStatus() const;
		void DrawRecentProjectsMenu();
		void UpdateWindowTitle();

		// Runs the action now, or after the unsaved-changes prompt when the scene has unsaved changes.
		void RequestSceneAction(SceneAction action, std::filesystem::path path = {});
		void PerformSceneAction(SceneAction action, std::filesystem::path const& path);
		void ShowOpenSceneDialog();
		void ShowOpenProjectDialog();
		// Where file dialogs for scenes start: the scene's folder, else the project's asset directory.
		std::filesystem::path GetSceneDialogDirectory() const;
		void OpenProject(std::filesystem::path const& file);
		void RememberProject();
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
		EditorScripts m_Scripts;
		PlayMode m_PlayMode;
		CommandRegistry m_Commands;
		AutomationServer m_AutomationServer;
		bool m_InstanceFileWritten = false;

		ConsolePanel m_ConsolePanel;
		StatisticsPanel m_StatisticsPanel;
		SceneHierarchyPanel m_HierarchyPanel;
		InspectorPanel m_InspectorPanel;
		SceneSettingsPanel m_SceneSettingsPanel;
		ProjectSettingsPanel m_ProjectSettingsPanel;
		ContentBrowserPanel m_ContentBrowserPanel;
		// Created when ImGui is available.
		Scope<ViewportPanel> m_ViewportPanel;
		bool m_ShowViewport = true;
		bool m_ShowHierarchy = true;
		bool m_ShowInspector = true;
		bool m_ShowSceneSettings = true;
		bool m_ShowProjectSettings = true;
		bool m_ShowContentBrowser = true;
		bool m_ShowConsole = true;
		bool m_ShowStatistics = true;
		bool m_ShowImGuiDemo = false;
		bool m_ResetLayout = false;

		SceneAction m_PendingAction = SceneAction::None;
		std::filesystem::path m_PendingPath;
		std::string m_PendingProjectName;
		bool m_OpenUnsavedChangesPopup = false;

		RecentProjects m_RecentProjects;
		bool m_OpenNewProjectPopup = false;
		std::string m_NewProjectName;
		std::string m_NewProjectLocation;
		bool m_GameInputWasActive = false;
		bool m_OpenNewScriptPopup = false;
		std::string m_NewScriptName;
		std::string m_WindowTitle;

		std::filesystem::path m_ScreenshotPath;
		uint64_t m_ScreenshotFrame = 0;
	};
}
