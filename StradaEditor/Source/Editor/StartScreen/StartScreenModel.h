#pragma once

#include "Editor/Agents/AgentKit.h"
#include "Editor/EditorLocations.h"
#include "Editor/EditorOperations.h"
#include "Editor/RecentProjects.h"

#include "Strada/Core/Result.h"
#include "Strada/Project/ExampleCatalog.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	// What a recent project's file is now.
	enum class RecentProjectState : uint8_t
	{
		// It reads as a project file.
		Ok = 0,
		// Nothing is there any more.
		Missing,
		// Something is there that does not read as a project file's header (a folder, another kind of file, a newer
		// version, garbage).
		Unreadable
	};

	// "Ok", "Missing", "Unreadable": the names start.info and editor.recent report.
	[[nodiscard]] std::string_view GetRecentProjectStateName(RecentProjectState state);

	// A recent project as the start screen lists it.
	struct RecentProjectInfo
	{
		// The project's name (Project.Name) when its file reads, else the file's name without its extension.
		std::string Name;
		// The project file, as the recent projects list holds it.
		std::filesystem::path File;
		RecentProjectState State = RecentProjectState::Missing;
	};

	// What the editor knows of the machine, for the start screen's environment strip. Nothing here is detected for it: each
	// value comes from what the editor already found (StartScreenServices::DescribeEnvironment). Empty when unknown.
	struct StartScreenEnvironment
	{
		// The .NET runtime scripting started with (".NET 10"); none when scripting is off.
		std::optional<std::string> DotNetRuntime;
		// The .NET SDK's driver (ScriptProject::FindDotNet), which builds C# scripts; none when it was not found.
		std::optional<std::filesystem::path> DotNetSdk;
		// The graphics adapter's name and the Vulkan version it offers; none without a graphics device.
		std::optional<std::string> Gpu;
		std::optional<std::string> Vulkan;
	};

	// A guide of the engine's documentation that the start screen links to.
	struct StartScreenGuide
	{
		std::string Title;
		// An existing file of EngineContent::Docs.
		std::filesystem::path File;
	};

	// Why the New Project form cannot create its project.
	enum class NewProjectProblem : uint8_t
	{
		None = 0,
		// The name gives no portable file name (FileSystem::MakePortableFileName is empty: the name is empty or spaces).
		NoName,
		NoLocation,
		// The location is not an absolute path.
		RelativeLocation,
		// The target exists and is not an empty directory (CheckNewProjectDirectory).
		TargetTaken,
		// The template the form names is not one the editor offers.
		UnknownTemplate
	};

	// The New Project form's fields, as the user left them. Nothing of it is remembered between editor sessions.
	struct NewProjectForm
	{
		static constexpr std::string_view DefaultName = "New Game";

		std::string Name = std::string(DefaultName);
		// The folder the project's folder goes into, as typed (UTF-8).
		std::string Location;
		std::string TemplateId = std::string(ProjectTemplate::DefaultId);
		// Set the new project up for coding agents (AgentKit).
		bool SetUpAgents = true;
		// Write the root .gitignore (TemplateOptions::GitIgnore and AgentSetupOptions::GitIgnore).
		bool GitIgnore = true;

		bool operator==(NewProjectForm const& other) const = default;
	};

	// What the form would create, and why it cannot.
	struct NewProjectCheck
	{
		// The name in a form every platform can store; empty when nothing usable remains.
		std::string DirectoryName;
		// <location>/<DirectoryName>; empty without a name or a location.
		std::filesystem::path Directory;
		NewProjectProblem Problem = NewProjectProblem::None;
		// Why Create is disabled, for the user; empty when it is not.
		std::string Reason;
		// What the user should know but does not stop the creation (the template needs a .NET SDK the machine lacks); empty
		// when nothing.
		std::string Warning;

		bool CanCreate() const { return Problem == NewProjectProblem::None; }
	};

	// What setting a new project up for coding agents did (SetUpNewProjectForAgents).
	struct NewProjectAgentSetup
	{
		// The files the setup handled, in AgentKit::ListFiles order: all of them, or those before the one that failed.
		std::vector<AgentFilePlan> Files;
		// What the user should know, the failure that stopped the setup included.
		std::vector<std::string> Warnings;
	};

	// Sets a project that was just created up for coding agents (AgentKit::Apply with the .gitignore choice of its creation),
	// as project.create's agents and the New Project form's "Set up for coding agents" do. A setup that fails does not undo
	// the project: the failure is a warning, and the files written before it stay.
	[[nodiscard]] NewProjectAgentSetup SetUpNewProjectForAgents(AgentProject const& project, EngineLayout const& layout,
	                                                            AgentToolLookup const& findTool, bool gitIgnore);

	// What the New Project form asks to create, as the form was when Create was clicked: the editor may ask about unsaved
	// changes before it creates anything, and the form may change meanwhile.
	struct NewProjectRequest
	{
		NewProjectForm Form;
		// <location>/<portable name> (NewProjectCheck::Directory).
		std::filesystem::path Directory;
	};

	// What creating a project from the New Project form did.
	struct NewProjectOutcome
	{
		// The new project's file.
		std::filesystem::path ProjectFile;
		// What setting it up for coding agents did; none when the form left that off.
		std::optional<NewProjectAgentSetup> Agents;
	};

	// Creates the project a New Project form asked for in its directory and opens it (EditorOperations::CreateProject with the
	// form's template and .gitignore choice), then sets it up for coding agents when the form says so
	// (SetUpNewProjectForAgents), as project.create does. A setup that fails is in the outcome's warnings and leaves the
	// project created. Fails for a template the list does not hold and when the project cannot be created or opened. Main
	// thread only.
	[[nodiscard]] Result<NewProjectOutcome> CreateProjectFromRequest(EditorOperations& operations, NewProjectRequest const& request,
	                                                                 ProjectTemplateList const& templates, EngineLayout const& layout,
	                                                                 AgentToolLookup const& findTool);

	// The project file a drop of files onto the editor's window opens while the start screen shows: the first file whose
	// extension is .sproj in any letter case; none when the drop holds none (the start screen imports nothing). Pure.
	[[nodiscard]] std::optional<std::filesystem::path> FindDroppedProject(std::span<std::filesystem::path const> paths);

	// "<doing> failed: <error>": how the start screen's error line words a failure ("Opening the project '<file>'"). Pure.
	[[nodiscard]] std::string DescribeStartScreenFailure(std::string_view doing, std::string_view error);

	// What the start screen needs from the editor besides its folders. Each may be unset; the model never starts a process
	// or detects anything itself.
	struct StartScreenServices
	{
		// What the editor knows of the machine. Unset: nothing is known (tests that set nothing never read the machine's).
		std::function<StartScreenEnvironment()> DescribeEnvironment;
		// Opens a file or folder with the system's handler (Show in Folder, the guides): the editor's is Desktop::Open. Unset:
		// opening fails.
		std::function<Result<void>(std::filesystem::path const& path)> OpenWithSystem;
	};

	// The state behind the editor's start screen, free of ImGui: the recent projects (it owns the editor's list, which the
	// commands and the File menu reach through it), the New Project form, the templates and the examples of the engine
	// content, the guides, the environment, and whether the screen shows. Reads the engine content and the recent project
	// files only in Refresh and the Describe functions; writes nothing but the recent projects file (in the locations' user
	// data). Main thread only.
	class StartScreenModel
	{
	public:
		// Project files larger than this are not read: a recent project of such a file is Unreadable.
		static constexpr uint64_t MaxProjectFileBytes = 16ull * 1024 * 1024;
		// How far " 2", " 3"... go to find a free folder for an example's copy.
		static constexpr uint32_t MaxCopySuffix = 1000;
		static constexpr std::string_view RecentProjectsFileName = "RecentProjects.json";

		// The recent projects file is the locations' user-data file RecentProjectsFileName; it is not read until
		// GetRecentProjects().Load(). The form starts empty (ResetForm).
		explicit StartScreenModel(EditorLocations locations, StartScreenServices services = {});

		EditorLocations const& GetLocations() const { return m_Locations; }

		// --- Visibility (the panel draws the screen while it is visible; a headless editor never shows it) ---

		bool IsVisible() const { return m_Visible; }
		// Hiding the screen clears its error line.
		void SetVisible(bool visible);
		// The editor's start without a project: shows the screen, then puts why the project its command line named did not
		// open on the error line (projectFailure, as DescribeStartScreenFailure words it; empty when none failed). In that
		// order, since the line keeps a message only while the screen is visible.
		void ShowAtStart(std::string projectFailure);

		// --- What failed (the screen's error line) ---

		// An action of the screen failed (opening, creating or copying a project, Show in Folder, a guide, a dropped file): the
		// message shows on the screen's error line until another replaces it, an action of the screen starts or the screen
		// hides. Kept only while the screen is visible: a hidden screen's failures are only in the log (the Console
		// shows them). The caller logs it.
		void ReportError(std::string message);
		void ClearError() { m_Error.clear(); }
		// The error line; empty when nothing failed.
		std::string const& GetError() const { return m_Error; }

		// --- Recent projects ---

		RecentProjects& GetRecentProjects() { return m_RecentProjects; }
		RecentProjects const& GetRecentProjects() const { return m_RecentProjects; }
		// A project was created or opened, by any path: it goes to the front of the recent projects, which are saved, and the
		// screen hides. Fails when the list cannot be saved (the list in memory has the project).
		[[nodiscard]] Result<void> OnProjectOpened(std::filesystem::path const& projectFile);
		// Removes a project from the recent projects (any spelling of its path) and saves the list.
		[[nodiscard]] Result<void> RemoveRecentProject(std::filesystem::path const& projectFile);
		// Empties the recent projects and saves the list.
		[[nodiscard]] Result<void> ClearRecentProjects();
		// The recent projects with their state, read from their files now.
		[[nodiscard]] std::vector<RecentProjectInfo> DescribeRecentProjects() const;
		// One recent project's state and name, read from its file now.
		[[nodiscard]] static RecentProjectInfo DescribeRecentProject(std::filesystem::path const& projectFile);

		// --- The engine content and the environment, as Refresh last read them ---

		// Reads the templates, the examples' catalog, the guides, the environment and the recent projects' states again.
		void Refresh();
		ProjectTemplateList const& GetTemplates() const { return m_Templates; }
		// None when the catalog is missing or damaged (GetExamplesError says why).
		std::optional<ExampleCatalog> const& GetExamples() const { return m_Examples; }
		std::string const& GetExamplesError() const { return m_ExamplesError; }
		std::vector<StartScreenGuide> const& GetGuides() const { return m_Guides; }
		StartScreenEnvironment const& GetEnvironment() const { return m_Environment; }
		std::vector<RecentProjectInfo> const& GetRecent() const { return m_Recent; }

		// The start screen's guides that exist in the layout's EngineContent::Docs, in the screen's order: Getting started,
		// The editor, Scripting, Visual scripting, Agents.
		[[nodiscard]] static std::vector<StartScreenGuide> ListGuides(EngineLayout const& layout);

		// --- The New Project form ---

		NewProjectForm& GetForm() { return m_Form; }
		NewProjectForm const& GetForm() const { return m_Form; }
		// The form as it starts: the default name, the default project location, empty-3d, both checkboxes on.
		void ResetForm();
		// What the form would create, and why it cannot (the target is looked at now). The SDK warning uses the environment
		// Refresh read.
		[[nodiscard]] NewProjectCheck CheckForm() const;
		// The files a setup for coding agents writes in the new project (AgentKit::ListFiles with the form's .gitignore
		// choice): the expander under "Set up for coding agents".
		[[nodiscard]] std::vector<std::string> ListAgentFiles() const;

		// --- Examples ---

		// Where an example's copy goes: <location>/<portable name>, or the first of "<name> 2", "<name> 3"... up to
		// MaxCopySuffix that is free (missing, or an empty directory), with the form's location. None without a location, a
		// portable name, or a free folder.
		[[nodiscard]] std::optional<std::filesystem::path> GetExampleCopyTarget(ExampleEntry const& example) const;

		// --- The system's handler ---

		// Opens a file or folder through StartScreenServices::OpenWithSystem; fails when it is unset.
		[[nodiscard]] Result<void> OpenWithSystem(std::filesystem::path const& path) const;
		// Shows a recent project's folder in the file manager.
		[[nodiscard]] Result<void> ShowInFolder(std::filesystem::path const& projectFile) const;

	private:
		EditorLocations m_Locations;
		StartScreenServices m_Services;
		RecentProjects m_RecentProjects;
		bool m_Visible = false;
		std::string m_Error;
		NewProjectForm m_Form;
		ProjectTemplateList m_Templates;
		std::optional<ExampleCatalog> m_Examples;
		std::string m_ExamplesError;
		std::vector<StartScreenGuide> m_Guides;
		StartScreenEnvironment m_Environment;
		std::vector<RecentProjectInfo> m_Recent;
	};
}
