#include "Editor/StartScreen/StartScreenModel.h"

#include "Editor/Automation/AgentCommands.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Project/Project.h"
#include "Strada/Project/ProjectCopy.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <utility>

namespace Strada
{
	namespace
	{
		// The header type of a project file (Project.h: { "Strada": { "Version": 1, "Type": "Project" }, "Project": {...} }).
		constexpr std::string_view ProjectHeaderType = "Project";
		// What project files end with, in lower case.
		constexpr std::string_view ProjectFileExtension = ".sproj";

		struct GuideLink
		{
			std::string_view Title;
			std::string_view FileName;
		};

		constexpr std::array<GuideLink, 5> GuideLinks = {{
			{"Getting started", "GettingStarted.md"},
			{"The editor", "Editor.md"},
			{"Scripting", "Scripting.md"},
			{"Visual scripting", "VisualScripting.md"},
			{"Agents", "Agents.md"},
		}};

		bool IsFree(std::filesystem::path const& directory)
		{
			return CheckNewProjectDirectory(directory).IsOk();
		}
	}

	std::string_view GetRecentProjectStateName(RecentProjectState state)
	{
		switch (state)
		{
			case RecentProjectState::Ok:
				return "Ok";
			case RecentProjectState::Missing:
				return "Missing";
			case RecentProjectState::Unreadable:
				return "Unreadable";
		}
		return "Unreadable";
	}

	NewProjectAgentSetup SetUpNewProjectForAgents(AgentProject const& project, EngineLayout const& layout, AgentToolLookup const& findTool,
	                                              bool gitIgnore)
	{
		AgentSetupOptions options;
		options.GitIgnore = gitIgnore;
		AgentApplyResult applied = AgentKit::Apply(project, layout, findTool, options);
		NewProjectAgentSetup setup{std::move(applied.Plan.Files), std::move(applied.Plan.Warnings)};
		if (!applied.Succeeded())
		{
			setup.Warnings.push_back(
				applied.FailedPath.empty()
					? fmt::format("the project was not set up for coding agents: {}", applied.Error)
					: fmt::format("setting the project up for coding agents stopped at {}: {}", applied.FailedPath, applied.Error));
		}
		return setup;
	}

	Result<NewProjectOutcome> CreateProjectFromRequest(EditorOperations& operations, NewProjectRequest const& request,
	                                                   ProjectTemplateList const& templates, EngineLayout const& layout,
	                                                   AgentToolLookup const& findTool)
	{
		ProjectTemplate const* const projectTemplate = templates.Find(request.Form.TemplateId);
		if (projectTemplate == nullptr)
		{
			return MakeError("there is no template '{}'", request.Form.TemplateId);
		}
		TemplateOptions options;
		options.GitIgnore = request.Form.GitIgnore;
		if (Result<void> created = operations.CreateProject(request.Directory, request.Form.Name, *projectTemplate, options); !created)
		{
			return Error{created.GetError()};
		}
		Project const* const project = operations.GetContext().GetProject();
		if (project == nullptr)
		{
			return MakeError("the new project in '{}' is not open", FileSystem::PathToUtf8(request.Directory));
		}
		NewProjectOutcome outcome;
		outcome.ProjectFile = project->GetFilePath();
		if (std::optional<AgentProject> const agentProject = GetAgentProject(operations.GetContext());
		    request.Form.SetUpAgents && agentProject)
		{
			outcome.Agents = SetUpNewProjectForAgents(*agentProject, layout, findTool, request.Form.GitIgnore);
		}
		return outcome;
	}

	std::optional<std::filesystem::path> FindDroppedProject(std::span<std::filesystem::path const> paths)
	{
		for (std::filesystem::path const& path : paths)
		{
			std::string extension = FileSystem::PathToUtf8(path.extension());
			std::ranges::transform(extension, extension.begin(),
			                       [](char character)
			                       {
									   return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
								   });
			if (extension == ProjectFileExtension)
			{
				return path;
			}
		}
		return std::nullopt;
	}

	StartScreenModel::StartScreenModel(EditorLocations locations, StartScreenServices services)
		: m_Locations(std::move(locations)),
		  m_Services(std::move(services)),
		  m_RecentProjects(m_Locations.GetUserDataFile(RecentProjectsFileName))
	{
		ResetForm();
	}

	Result<void> StartScreenModel::OnProjectOpened(std::filesystem::path const& projectFile)
	{
		m_Visible = false;
		m_RecentProjects.Add(projectFile);
		return m_RecentProjects.Save();
	}

	Result<void> StartScreenModel::RemoveRecentProject(std::filesystem::path const& projectFile)
	{
		m_RecentProjects.Remove(projectFile);
		std::filesystem::path const normalized = RecentProjects::NormalizePath(projectFile);
		std::erase_if(m_Recent,
		              [&normalized](RecentProjectInfo const& recent)
		              {
						  return RecentProjects::NormalizePath(recent.File) == normalized;
					  });
		return m_RecentProjects.Save();
	}

	Result<void> StartScreenModel::ClearRecentProjects()
	{
		m_RecentProjects.Clear();
		m_Recent.clear();
		return m_RecentProjects.Save();
	}

	std::vector<RecentProjectInfo> StartScreenModel::DescribeRecentProjects() const
	{
		std::vector<RecentProjectInfo> described;
		for (std::filesystem::path const& project : m_RecentProjects.GetProjects())
		{
			described.push_back(DescribeRecentProject(project));
		}
		return described;
	}

	RecentProjectInfo StartScreenModel::DescribeRecentProject(std::filesystem::path const& projectFile)
	{
		RecentProjectInfo info;
		info.File = projectFile;
		info.Name = FileSystem::PathToUtf8(projectFile.stem());
		if (!FileSystem::Exists(projectFile))
		{
			info.State = RecentProjectState::Missing;
			return info;
		}
		info.State = RecentProjectState::Unreadable;
		std::optional<uint64_t> const size = FileSystem::IsRegularFile(projectFile) ? FileSystem::GetFileSize(projectFile) : std::nullopt;
		if (!size || *size > MaxProjectFileBytes)
		{
			return info;
		}
		Result<std::string> text = FileSystem::ReadTextFile(projectFile);
		if (!text)
		{
			return info;
		}
		Result<Json> document = ParseJson(text.GetValue());
		if (!document || !ReadFileHeader(document.GetValue(), ProjectHeaderType, Project::FormatVersion))
		{
			return info;
		}
		info.State = RecentProjectState::Ok;
		Json const& root = document.GetValue();
		auto const settings = root.find(std::string(ProjectHeaderType));
		if (settings != root.end() && settings->is_object())
		{
			auto const name = settings->find("Name");
			if (name != settings->end() && name->is_string() && !name->get_ref<std::string const&>().empty())
			{
				info.Name = name->get<std::string>();
			}
		}
		return info;
	}

	void StartScreenModel::Refresh()
	{
		m_Templates = EditorOperations::ListProjectTemplates(m_Locations.Layout);
		Result<ExampleCatalog> catalog = EditorOperations::LoadExampleCatalog(m_Locations.Layout);
		if (catalog)
		{
			m_Examples = catalog.TakeValue();
			m_ExamplesError.clear();
		}
		else
		{
			m_Examples.reset();
			m_ExamplesError = catalog.GetError();
		}
		m_Guides = ListGuides(m_Locations.Layout);
		m_Environment = m_Services.DescribeEnvironment ? m_Services.DescribeEnvironment() : StartScreenEnvironment{};
		m_Recent = DescribeRecentProjects();
	}

	std::vector<StartScreenGuide> StartScreenModel::ListGuides(EngineLayout const& layout)
	{
		std::vector<StartScreenGuide> guides;
		std::filesystem::path const docs = layout.GetContentPath(EngineContent::Docs);
		if (docs.empty())
		{
			return guides;
		}
		for (GuideLink const& link : GuideLinks)
		{
			std::filesystem::path const file = docs / FileSystem::PathFromUtf8(link.FileName);
			if (FileSystem::IsRegularFile(file))
			{
				guides.push_back(StartScreenGuide{std::string(link.Title), file});
			}
		}
		return guides;
	}

	void StartScreenModel::ResetForm()
	{
		m_Form = NewProjectForm{};
		m_Form.Location = FileSystem::PathToUtf8(m_Locations.DefaultProjectLocation);
	}

	NewProjectCheck StartScreenModel::CheckForm() const
	{
		NewProjectCheck check;
		check.DirectoryName = FileSystem::MakePortableFileName(m_Form.Name);
		std::filesystem::path const location = FileSystem::PathFromUtf8(m_Form.Location);
		if (!check.DirectoryName.empty() && !m_Form.Location.empty())
		{
			check.Directory = location / FileSystem::PathFromUtf8(check.DirectoryName);
		}
		ProjectTemplate const* const projectTemplate = m_Templates.Find(m_Form.TemplateId);
		if (projectTemplate != nullptr && projectTemplate->RequiresSdk && !m_Environment.DotNetSdk)
		{
			check.Warning =
				fmt::format("{} has C# scripts: install the .NET 10 SDK to build them before the game plays.", projectTemplate->Name);
		}

		if (check.DirectoryName.empty())
		{
			check.Problem = NewProjectProblem::NoName;
			check.Reason = "Enter a name with a letter or a digit.";
		}
		else if (m_Form.Location.empty())
		{
			check.Problem = NewProjectProblem::NoLocation;
			check.Reason = "Choose a location.";
		}
		else if (!location.is_absolute())
		{
			check.Problem = NewProjectProblem::RelativeLocation;
			check.Reason = "The location must be a full path.";
		}
		else if (projectTemplate == nullptr)
		{
			check.Problem = NewProjectProblem::UnknownTemplate;
			check.Reason = "Choose a template.";
		}
		else if (!IsFree(check.Directory))
		{
			check.Problem = NewProjectProblem::TargetTaken;
			check.Reason = fmt::format("{} already exists and is not an empty folder.", FileSystem::PathToUtf8(check.Directory));
		}
		return check;
	}

	std::vector<std::string> StartScreenModel::ListAgentFiles() const
	{
		AgentSetupOptions options;
		options.GitIgnore = m_Form.GitIgnore;
		return AgentKit::ListFiles(m_Locations.Layout, options);
	}

	std::optional<std::filesystem::path> StartScreenModel::GetExampleCopyTarget(ExampleEntry const& example) const
	{
		std::filesystem::path const location = FileSystem::PathFromUtf8(m_Form.Location);
		std::string const name = FileSystem::MakePortableFileName(example.Name);
		if (m_Form.Location.empty() || !location.is_absolute() || name.empty())
		{
			return std::nullopt;
		}
		for (uint32_t suffix = 1; suffix <= MaxCopySuffix; suffix++)
		{
			std::filesystem::path const candidate =
				location / FileSystem::PathFromUtf8(suffix == 1 ? name : fmt::format("{} {}", name, suffix));
			if (IsFree(candidate))
			{
				return candidate;
			}
		}
		return std::nullopt;
	}

	Result<void> StartScreenModel::OpenWithSystem(std::filesystem::path const& path) const
	{
		if (!m_Services.OpenWithSystem)
		{
			return MakeError("'{}' cannot be opened: the editor has no handler for files", FileSystem::PathToUtf8(path));
		}
		return m_Services.OpenWithSystem(path);
	}

	Result<void> StartScreenModel::ShowInFolder(std::filesystem::path const& projectFile) const
	{
		return OpenWithSystem(projectFile.parent_path());
	}
}
