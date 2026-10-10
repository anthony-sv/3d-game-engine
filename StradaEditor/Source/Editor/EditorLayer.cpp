#include "Editor/EditorLayer.h"

#include "Editor/Automation/AutomationInstance.h"
#include "Editor/Automation/EditorCommands.h"
#include "Editor/Automation/ScriptCommands.h"
#include "Editor/DefaultScene.h"
#include "Editor/EntityPresets.h"
#include "Editor/FileDialogs.h"
#include "Editor/UI/EditorUI.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/Application.h"
#include "Strada/Core/Events/ApplicationEvent.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Platform.h"
#include "Strada/Core/Version.h"
#include "Strada/Core/Window.h"
#include "Strada/Scene/Entity.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <array>

namespace Strada
{
	namespace
	{
		// Bumping the version rebuilds the default layout once for users with a saved layout of an older panel set.
		constexpr char const* DockspaceName = "Strada.Dockspace.v4";
		constexpr char const* UnsavedChangesPopup = "Unsaved Changes";
		constexpr char const* NewProjectPopup = "New Project";
		constexpr char const* NewScriptPopup = "New Script";
		constexpr char const* SceneExtension = ".sscene";

		std::array<FileDialogFilter, 1> const& GetSceneFilters()
		{
			static std::array<FileDialogFilter, 1> const s_Filters = {{{"Strada Scene", "sscene"}}};
			return s_Filters;
		}

		std::array<FileDialogFilter, 1> const& GetProjectFilters()
		{
			static std::array<FileDialogFilter, 1> const s_Filters = {{{"Strada Project", "sproj"}}};
			return s_Filters;
		}

		// The default file name for saving a scene.
		std::string MakeSceneFileName(std::string const& sceneName)
		{
			std::string const fileName = FileSystem::MakePortableFileName(sceneName);
			return (fileName.empty() ? std::string("Untitled") : fileName) + SceneExtension;
		}
	}

	EditorLayer::EditorLayer(EditorLayerSpecification specification)
		: Layer("EditorLayer"),
		  m_Specification(std::move(specification)),
		  m_Operations(m_Context),
		  m_Scripts(m_Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"),
		  m_AutomationServer(m_Commands),
		  m_RecentProjects(FileSystem::GetUserDataDirectory() / "Editor" / "RecentProjects.json")
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
		environment.ProjectOpened = [this]
		{
			RememberProject();
		};
		Result<void> registered = RegisterEditorCommands(m_Commands, m_Operations, std::move(environment));
		registered = registered ? RegisterScriptCommands(m_Commands, m_Scripts, m_Context) : registered;
		ST_ASSERT(registered.IsOk(), "The built-in editor commands must register");
		(void)registered;

		if (application.GetImGuiLayer() != nullptr)
		{
			m_ViewportPanel = CreateScope<ViewportPanel>();
			m_HierarchyPanel.SetFocusCallback(
				[this](UUID entity)
				{
					std::array<UUID, 1> const entities = {entity};
					m_ViewportPanel->FocusEntities(m_Context.GetScene(), entities);
				});
			m_HierarchyPanel.SetSpawnPositionProvider(
				[this]
				{
					return GetSpawnPosition();
				});
			auto const openScene = [this](std::filesystem::path const& path)
			{
				RequestSceneAction(SceneAction::OpenScene, path);
			};
			m_ViewportPanel->SetOpenSceneCallback(openScene);
			m_ContentBrowserPanel.SetOpenSceneCallback(openScene);
		}

		if (Result<void> loaded = m_RecentProjects.Load(); !loaded)
		{
			ST_WARN("The recent projects list cannot be read: {}", loaded.GetError());
		}

		bool sceneOpened = false;
		if (!m_Specification.ProjectPath.empty())
		{
			Result<std::vector<std::string>> opened = m_Operations.OpenProject(m_Specification.ProjectPath);
			sceneOpened = opened.IsOk();
			if (opened)
			{
				RememberProject();
			}
			else
			{
				ST_ERROR("Could not open the project '{}': {}", FileSystem::PathToUtf8(m_Specification.ProjectPath), opened.GetError());
			}
		}
		if (!m_Specification.ScenePath.empty())
		{
			// A scene that cannot be opened keeps the project's start scene.
			Result<std::vector<std::string>> opened = m_Operations.OpenScene(m_Specification.ScenePath);
			sceneOpened = sceneOpened || opened.IsOk();
			if (!opened)
			{
				ST_ERROR("Could not open '{}': {}", FileSystem::PathToUtf8(m_Specification.ScenePath), opened.GetError());
			}
		}
		// Without a project the editor starts with a sample scene made of built-in assets.
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
		m_ProjectSettingsPanel.Flush(m_Operations);
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
		m_Scripts.Update();
		if (!m_ScreenshotPath.empty() && Application::Get().GetFrameCount() == m_ScreenshotFrame)
		{
			Application::Get().RequestScreenshot(m_ScreenshotPath);
			m_ScreenshotPath.clear();
		}
	}

	void EditorLayer::OnEvent(Event& event)
	{
		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<WindowCloseEvent>(
			[this](WindowCloseEvent&)
			{
				// Handling the event vetoes closing until the user decided about the unsaved changes.
				if (!m_Context.IsDirty() || Application::Get().GetImGuiLayer() == nullptr)
				{
					return false;
				}
				RequestSceneAction(SceneAction::Quit);
				return true;
			});
	}

	void EditorLayer::RequestScreenshotAtFrame(std::filesystem::path path, uint64_t frame)
	{
		m_ScreenshotPath = std::move(path);
		m_ScreenshotFrame = frame;
	}

	void EditorLayer::OnImGuiRender()
	{
		DrawDockspace();
		DrawMenuBar();
		HandleShortcuts();

		if (m_ViewportPanel && m_ShowViewport)
		{
			m_ViewportPanel->OnImGuiRender(m_Operations, m_ShowViewport);
		}
		if (m_ShowHierarchy)
		{
			m_HierarchyPanel.OnImGuiRender(m_Operations, m_ShowHierarchy);
		}
		if (m_ShowInspector)
		{
			m_InspectorPanel.OnImGuiRender(m_Operations, m_ShowInspector);
		}
		if (m_ShowSceneSettings)
		{
			m_SceneSettingsPanel.OnImGuiRender(m_Operations, m_ShowSceneSettings);
		}
		if (m_ShowProjectSettings)
		{
			m_ProjectSettingsPanel.OnImGuiRender(m_Operations, m_ShowProjectSettings);
		}
		if (m_ShowContentBrowser)
		{
			m_ContentBrowserPanel.OnImGuiRender(m_Operations, m_ShowContentBrowser);
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

		DrawUnsavedChangesPopup();
		DrawNewProjectPopup();
		DrawNewScriptPopup();
		UpdateWindowTitle();
	}

	void EditorLayer::DrawDockspace()
	{
		ImGuiID const dockspace = ImGui::DockSpaceOverViewport(ImHashStr(DockspaceName), ImGui::GetMainViewport());
		// First run (no saved layout) or View > Reset Layout.
		ImGuiDockNode const* root = ImGui::DockBuilderGetNode(dockspace);
		if (m_ResetLayout || root == nullptr || (root->IsLeafNode() && root->Windows.empty()))
		{
			BuildDefaultLayout(dockspace);
			m_ResetLayout = false;
		}
	}

	void EditorLayer::BuildDefaultLayout(uint32_t dockspace)
	{
		// Viewport in the center, hierarchy on the left, inspector and settings on the right with statistics below, content
		// browser and console at the bottom.
		ImGui::DockBuilderRemoveNode(dockspace);
		ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
		ImGuiID center = dockspace;
		ImGuiID const left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
		ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
		ImGuiID const bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
		ImGuiID const rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.25f, nullptr, &right);
		ImGui::DockBuilderDockWindow("Viewport", center);
		ImGui::DockBuilderDockWindow("Scene Hierarchy", left);
		ImGui::DockBuilderDockWindow("Inspector", right);
		ImGui::DockBuilderDockWindow("Scene Settings", right);
		ImGui::DockBuilderDockWindow("Project Settings", right);
		ImGui::DockBuilderDockWindow("Statistics", rightBottom);
		ImGui::DockBuilderDockWindow("Content Browser", bottom);
		ImGui::DockBuilderDockWindow("Console", bottom);
		ImGui::DockBuilderFinish(dockspace);
	}

	void EditorLayer::DrawMenuBar()
	{
		if (!ImGui::BeginMainMenuBar())
		{
			return;
		}

		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("New Project..."))
			{
				m_NewProjectName = "New Game";
				std::vector<std::filesystem::path> const& recent = m_RecentProjects.GetProjects();
				m_NewProjectLocation = recent.empty() ? std::string() : FileSystem::PathToUtf8(recent.front().parent_path().parent_path());
				m_OpenNewProjectPopup = true;
			}
			if (ImGui::MenuItem("Open Project..."))
			{
				ShowOpenProjectDialog();
			}
			DrawRecentProjectsMenu();
			if (ImGui::MenuItem("Close Project", nullptr, false, m_Context.GetProject() != nullptr))
			{
				RequestSceneAction(SceneAction::CloseProject);
			}
			ImGui::Separator();
			if (ImGui::MenuItem("New Scene", "Ctrl+N"))
			{
				RequestSceneAction(SceneAction::NewScene);
			}
			if (ImGui::MenuItem("Open Scene...", "Ctrl+O"))
			{
				ShowOpenSceneDialog();
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Save Scene", "Ctrl+S"))
			{
				SaveScene();
			}
			if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S"))
			{
				SaveSceneAs();
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Exit", "Alt+F4"))
			{
				RequestSceneAction(SceneAction::Quit);
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Edit"))
		{
			CommandHistory const& history = m_Context.GetHistory();
			std::string const undo = history.CanUndo() ? "Undo " + history.GetUndoDescription() : std::string("Undo");
			std::string const redo = history.CanRedo() ? "Redo " + history.GetRedoDescription() : std::string("Redo");
			if (ImGui::MenuItem(undo.c_str(), "Ctrl+Z", false, history.CanUndo()))
			{
				UI::ReportFailure(m_Operations.Undo(), "Undo");
			}
			if (ImGui::MenuItem(redo.c_str(), "Ctrl+Y", false, history.CanRedo()))
			{
				UI::ReportFailure(m_Operations.Redo(), "Redo");
			}
			ImGui::Separator();
			bool const hasSelection = !m_Context.GetSelection().IsEmpty();
			if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, hasSelection))
			{
				DuplicateSelection();
			}
			if (ImGui::MenuItem("Delete", "Delete", false, hasSelection))
			{
				DeleteSelection();
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Select All", "Ctrl+A"))
			{
				SelectAll();
			}
			if (ImGui::MenuItem("Deselect All", nullptr, false, hasSelection))
			{
				m_Context.ClearSelection();
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Entity"))
		{
			if (EntityPreset const* preset = UI::DrawEntityPresetMenuItems())
			{
				Result<UUID> created = EntityPresets::Create(m_Operations, *preset, UUID::Invalid(), GetSpawnPosition());
				UI::ReportFailure(created, "Creating the entity");
				if (created)
				{
					std::array<UUID, 1> const selection = {created.GetValue()};
					UI::ReportFailure(m_Operations.Select(selection), "Selecting the new entity");
				}
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Scripts"))
		{
			if (ImGui::MenuItem("New Script...", nullptr, false, m_Context.GetProject() != nullptr))
			{
				m_NewScriptName = "NewScript";
				m_OpenNewScriptPopup = true;
			}
			if (ImGui::MenuItem("Build Scripts", "Ctrl+B", false, m_Scripts.HasScriptProject() && !m_Scripts.IsBuilding()))
			{
				m_Scripts.Build();
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("View"))
		{
			ImGui::MenuItem("Viewport", nullptr, &m_ShowViewport, m_ViewportPanel != nullptr);
			ImGui::MenuItem("Scene Hierarchy", nullptr, &m_ShowHierarchy);
			ImGui::MenuItem("Inspector", nullptr, &m_ShowInspector);
			ImGui::MenuItem("Scene Settings", nullptr, &m_ShowSceneSettings);
			ImGui::MenuItem("Project Settings", nullptr, &m_ShowProjectSettings);
			ImGui::MenuItem("Content Browser", nullptr, &m_ShowContentBrowser);
			ImGui::MenuItem("Console", nullptr, &m_ShowConsole);
			ImGui::MenuItem("Statistics", nullptr, &m_ShowStatistics);
			ImGui::Separator();
			if (ImGui::MenuItem("Reset Layout"))
			{
				m_ShowViewport = m_ShowHierarchy = m_ShowInspector = m_ShowSceneSettings = m_ShowProjectSettings = m_ShowContentBrowser =
					m_ShowConsole = m_ShowStatistics = true;
				m_ResetLayout = true;
			}
			ImGui::MenuItem("ImGui Demo", nullptr, &m_ShowImGuiDemo);
			ImGui::EndMenu();
		}

		std::string const automation = m_AutomationServer.IsRunning()
		                                   ? fmt::format("Automation: 127.0.0.1:{} ({} connected)", m_AutomationServer.GetPort(),
		                                                 m_AutomationServer.GetConnectionCount())
		                                   : std::string("Automation: off");
		std::string const scripts = GetScriptStatus();
		std::string const status = scripts.empty() ? automation : scripts + "    " + automation;
		ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(status.c_str()).x - ImGui::GetStyle().ItemSpacing.x * 2.0f);
		ImGui::TextDisabled("%s", status.c_str());
		ImGui::EndMainMenuBar();
	}

	void EditorLayer::HandleShortcuts()
	{
		// Global routes: a focused text field keeps its own keys (Ctrl+Z inside a text field undoes typing).
		ImGuiInputFlags const route = ImGuiInputFlags_RouteGlobal;
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, route))
		{
			RequestSceneAction(SceneAction::NewScene);
		}
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, route))
		{
			ShowOpenSceneDialog();
		}
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, route))
		{
			SaveSceneAs();
		}
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, route))
		{
			SaveScene();
		}
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_B, route) && m_Scripts.HasScriptProject())
		{
			m_Scripts.Build();
		}

		if (ImGui::GetIO().WantTextInput)
		{
			return;
		}
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, route | ImGuiInputFlags_Repeat) && m_Context.GetHistory().CanUndo())
		{
			UI::ReportFailure(m_Operations.Undo(), "Undo");
		}
		if ((ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, route | ImGuiInputFlags_Repeat) ||
		     ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, route | ImGuiInputFlags_Repeat)) &&
		    m_Context.GetHistory().CanRedo())
		{
			UI::ReportFailure(m_Operations.Redo(), "Redo");
		}
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, route) && !m_Context.GetSelection().IsEmpty())
		{
			DuplicateSelection();
		}
		// Cmd+Backspace on macOS keyboards, which have no forward-delete key.
		bool const deletePressed = ImGui::Shortcut(ImGuiKey_Delete, route) || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Backspace, route);
		if (deletePressed && !m_Context.GetSelection().IsEmpty())
		{
			DeleteSelection();
		}
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A, route))
		{
			SelectAll();
		}
	}

	void EditorLayer::DrawUnsavedChangesPopup()
	{
		if (m_OpenUnsavedChangesPopup)
		{
			ImGui::OpenPopup(UnsavedChangesPopup);
			m_OpenUnsavedChangesPopup = false;
		}
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		if (!ImGui::BeginPopupModal(UnsavedChangesPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
		{
			return;
		}

		ImGui::Text("Save the changes to '%s'?", m_Context.GetScene().GetName().c_str());
		ImGui::TextDisabled("Your changes are lost if you do not save them.");
		ImGui::Spacing();
		float const buttonWidth = ImGui::CalcTextSize("Don't Save").x + ImGui::GetStyle().FramePadding.x * 4.0f;
		SceneAction const action = m_PendingAction;
		std::filesystem::path const path = m_PendingPath;
		if (ImGui::Button("Save", ImVec2(buttonWidth, 0.0f)))
		{
			ImGui::CloseCurrentPopup();
			m_PendingAction = SceneAction::None;
			if (SaveScene())
			{
				PerformSceneAction(action, path);
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Don't Save", ImVec2(buttonWidth, 0.0f)))
		{
			ImGui::CloseCurrentPopup();
			m_PendingAction = SceneAction::None;
			PerformSceneAction(action, path);
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			ImGui::CloseCurrentPopup();
			m_PendingAction = SceneAction::None;
		}
		ImGui::EndPopup();
	}

	void EditorLayer::UpdateWindowTitle()
	{
		Window* window = Application::Get().GetWindow();
		if (window == nullptr)
		{
			return;
		}
		Project const* project = m_Context.GetProject();
		std::string const title = fmt::format("{}{} - {}Strada Editor", m_Context.GetScene().GetName(), m_Context.IsDirty() ? "*" : "",
		                                      project != nullptr ? project->GetSettings().Name + " - " : std::string());
		if (title != m_WindowTitle)
		{
			window->SetTitle(title);
			m_WindowTitle = title;
		}
	}

	void EditorLayer::RequestSceneAction(SceneAction action, std::filesystem::path path)
	{
		if (!m_Context.IsDirty())
		{
			PerformSceneAction(action, path);
			return;
		}
		m_PendingAction = action;
		m_PendingPath = std::move(path);
		m_OpenUnsavedChangesPopup = true;
	}

	void EditorLayer::DrawNewProjectPopup()
	{
		if (m_OpenNewProjectPopup)
		{
			ImGui::OpenPopup(NewProjectPopup);
			m_OpenNewProjectPopup = false;
		}
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal(NewProjectPopup, nullptr, ImGuiWindowFlags_NoSavedSettings))
		{
			return;
		}

		ImGui::TextUnformatted("Name");
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputText("##name", &m_NewProjectName);
		ImGui::TextUnformatted("Location");
		float const browseWidth = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2.0f;
		ImGui::SetNextItemWidth(-(browseWidth + ImGui::GetStyle().ItemSpacing.x));
		ImGui::InputText("##location", &m_NewProjectLocation);
		ImGui::SameLine();
		if (ImGui::Button("Browse..."))
		{
			Result<std::optional<std::filesystem::path>> chosen = FileDialogs::PickFolder(FileSystem::PathFromUtf8(m_NewProjectLocation));
			if (!chosen)
			{
				ST_ERROR("{}", chosen.GetError());
			}
			else if (chosen.GetValue())
			{
				m_NewProjectLocation = FileSystem::PathToUtf8(*chosen.GetValue());
			}
		}

		// The directory is named after the project, restricted to characters valid in file names everywhere.
		std::string const directoryName = FileSystem::MakePortableFileName(m_NewProjectName);
		std::filesystem::path const directory = FileSystem::PathFromUtf8(m_NewProjectLocation) / FileSystem::PathFromUtf8(directoryName);
		bool const valid = !directoryName.empty() && !m_NewProjectLocation.empty();
		if (valid)
		{
			ImGui::TextDisabled("Creates %s", FileSystem::PathToUtf8(directory).c_str());
		}
		ImGui::Spacing();
		ImGui::BeginDisabled(!valid);
		if (ImGui::Button("Create"))
		{
			ImGui::CloseCurrentPopup();
			m_PendingProjectName = m_NewProjectName;
			RequestSceneAction(SceneAction::NewProject, directory);
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	void EditorLayer::DrawNewScriptPopup()
	{
		if (m_OpenNewScriptPopup)
		{
			ImGui::OpenPopup(NewScriptPopup);
			m_OpenNewScriptPopup = false;
		}
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal(NewScriptPopup, nullptr, ImGuiWindowFlags_NoSavedSettings))
		{
			return;
		}

		ImGui::TextUnformatted("Class name");
		if (ImGui::IsWindowAppearing())
		{
			ImGui::SetKeyboardFocusHere();
		}
		ImGui::SetNextItemWidth(-FLT_MIN);
		bool const submitted = ImGui::InputText("##className", &m_NewScriptName, ImGuiInputTextFlags_EnterReturnsTrue);
		bool const valid = ScriptProject::IsValidClassName(m_NewScriptName);
		Project const* const project = m_Context.GetProject();
		if (valid && project != nullptr)
		{
			ImGui::TextDisabled("Creates Scripts/Source/%s.cs (class %s.%s)", m_NewScriptName.c_str(),
			                    ScriptProject::GetNamespace(*project).c_str(), m_NewScriptName.c_str());
		}
		else
		{
			ImGui::TextDisabled("Letters, digits and underscores, starting with a letter.");
		}
		ImGui::Spacing();
		ImGui::BeginDisabled(!valid);
		if (ImGui::Button("Create") || (submitted && valid))
		{
			ImGui::CloseCurrentPopup();
			Result<std::filesystem::path> created = m_Scripts.CreateScript(m_NewScriptName);
			UI::ReportFailure(created, "Creating the script");
			if (created)
			{
				ST_INFO("Created {}", FileSystem::PathToUtf8(created.GetValue()));
			}
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	std::string EditorLayer::GetScriptStatus() const
	{
		if (m_Scripts.IsBuilding())
		{
			return "Scripts: building...";
		}
		ScriptBuildReport const* const build = m_Scripts.GetLastBuild();
		if (build == nullptr || build->Succeeded)
		{
			return {};
		}
		uint32_t const errors = build->CountDiagnostics(ScriptDiagnosticSeverity::Error);
		return errors > 0 ? fmt::format("Scripts: {} error{}", errors, errors == 1 ? "" : "s") : std::string("Scripts: build failed");
	}

	void EditorLayer::DrawRecentProjectsMenu()
	{
		std::vector<std::filesystem::path> const recent = m_RecentProjects.GetProjects();
		if (!ImGui::BeginMenu("Recent Projects", !recent.empty()))
		{
			return;
		}
		for (std::filesystem::path const& project : recent)
		{
			std::string const path = FileSystem::PathToUtf8(project);
			if (ImGui::MenuItem(FileSystem::PathToUtf8(project.stem()).c_str()))
			{
				RequestSceneAction(SceneAction::OpenProject, project);
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
			{
				ImGui::SetTooltip("%s", path.c_str());
			}
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Clear Recent Projects"))
		{
			m_RecentProjects.Clear();
			UI::ReportFailure(m_RecentProjects.Save(), "Saving the recent projects");
		}
		ImGui::EndMenu();
	}

	void EditorLayer::PerformSceneAction(SceneAction action, std::filesystem::path const& path)
	{
		switch (action)
		{
			case SceneAction::None:
				break;
			case SceneAction::NewScene:
				m_Operations.NewScene();
				break;
			case SceneAction::OpenScene:
			{
				Result<std::vector<std::string>> opened = m_Operations.OpenScene(path);
				if (!opened)
				{
					ST_ERROR("Could not open '{}': {}", FileSystem::PathToUtf8(path), opened.GetError());
				}
				break;
			}
			case SceneAction::NewProject:
			{
				Result<void> created = m_Operations.CreateProject(path, m_PendingProjectName, *CreateDefaultScene());
				if (created)
				{
					RememberProject();
				}
				UI::ReportFailure(created, "Creating the project");
				break;
			}
			case SceneAction::OpenProject:
				OpenProject(path);
				break;
			case SceneAction::CloseProject:
				m_Operations.CloseProject();
				break;
			case SceneAction::Quit:
				Application::Get().Close();
				break;
		}
	}

	void EditorLayer::ShowOpenProjectDialog()
	{
		Result<std::optional<std::filesystem::path>> chosen = FileDialogs::OpenFile(GetProjectFilters());
		if (!chosen)
		{
			ST_ERROR("{}", chosen.GetError());
			return;
		}
		if (chosen.GetValue())
		{
			RequestSceneAction(SceneAction::OpenProject, *chosen.GetValue());
		}
	}

	void EditorLayer::OpenProject(std::filesystem::path const& file)
	{
		Result<std::vector<std::string>> opened = m_Operations.OpenProject(file);
		if (opened)
		{
			RememberProject();
			return;
		}
		ST_ERROR("Could not open the project '{}': {}", FileSystem::PathToUtf8(file), opened.GetError());
		if (!FileSystem::Exists(file))
		{
			m_RecentProjects.Remove(file);
			UI::ReportFailure(m_RecentProjects.Save(), "Saving the recent projects");
		}
	}

	void EditorLayer::RememberProject()
	{
		if (Project const* project = m_Context.GetProject())
		{
			m_RecentProjects.Add(project->GetFilePath());
			UI::ReportFailure(m_RecentProjects.Save(), "Saving the recent projects");
		}
	}

	std::filesystem::path EditorLayer::GetSceneDialogDirectory() const
	{
		std::filesystem::path const& current = m_Context.GetScenePath();
		if (!current.empty())
		{
			return current.parent_path();
		}
		Project const* project = m_Context.GetProject();
		return project != nullptr ? project->GetAssetDirectory() : std::filesystem::path();
	}

	void EditorLayer::ShowOpenSceneDialog()
	{
		Result<std::optional<std::filesystem::path>> chosen = FileDialogs::OpenFile(GetSceneFilters(), GetSceneDialogDirectory());
		if (!chosen)
		{
			ST_ERROR("{}", chosen.GetError());
			return;
		}
		if (chosen.GetValue())
		{
			RequestSceneAction(SceneAction::OpenScene, *chosen.GetValue());
		}
	}

	bool EditorLayer::SaveScene()
	{
		if (m_Context.GetScenePath().empty())
		{
			return SaveSceneAs();
		}
		Result<void> saved = m_Operations.SaveScene();
		UI::ReportFailure(saved, "Saving the scene");
		return saved.IsOk();
	}

	bool EditorLayer::SaveSceneAs()
	{
		Result<std::optional<std::filesystem::path>> chosen =
			FileDialogs::SaveFile(GetSceneFilters(), GetSceneDialogDirectory(), MakeSceneFileName(m_Context.GetScene().GetName()));
		if (!chosen)
		{
			ST_ERROR("{}", chosen.GetError());
			return false;
		}
		if (!chosen.GetValue())
		{
			return false;
		}
		std::filesystem::path path = *chosen.GetValue();
		if (path.extension() != SceneExtension)
		{
			path += SceneExtension;
		}
		Result<void> saved = m_Operations.SaveScene(path);
		UI::ReportFailure(saved, "Saving the scene");
		return saved.IsOk();
	}

	void EditorLayer::DuplicateSelection()
	{
		std::vector<UUID> const selection = m_Context.GetSelection().GetEntities();
		Result<std::vector<UUID>> copies = m_Operations.DuplicateEntities(selection);
		UI::ReportFailure(copies, "Duplicating entities");
		if (copies)
		{
			UI::ReportFailure(m_Operations.Select(copies.GetValue()), "Selecting the copies");
		}
	}

	void EditorLayer::DeleteSelection()
	{
		std::vector<UUID> const selection = m_Context.GetSelection().GetEntities();
		UI::ReportFailure(m_Operations.DeleteEntities(selection), "Deleting entities");
	}

	void EditorLayer::SelectAll()
	{
		std::vector<UUID> entities;
		m_Context.GetScene().ForEachEntityInHierarchyOrder(
			[&entities](Entity entity)
			{
				entities.push_back(entity.GetUUID());
			});
		UI::ReportFailure(m_Operations.Select(entities), "Selecting all entities");
	}

	glm::vec3 EditorLayer::GetSpawnPosition() const
	{
		return m_ViewportPanel ? m_ViewportPanel->GetCamera().GetFocalPoint() : glm::vec3(0.0f);
	}
}
