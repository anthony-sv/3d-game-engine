#include "Editor/EditorScripts.h"

#include "Editor/AssetBrowsing.h"
#include "Editor/FileCopies.h"
#include "Editor/ScriptBrowsing.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"
#include "Strada/Project/Project.h"
#include "Strada/Script/ScriptEngine.h"

#include <algorithm>
#include <optional>
#include <system_error>
#include <utility>

namespace Strada
{
	namespace
	{
		// Where a diagnostic is, relative to the project directory when inside it: "Scripts/Source/Player.cs(7,13)".
		std::string DescribeLocation(ScriptDiagnostic const& diagnostic, std::filesystem::path const& projectDirectory)
		{
			if (diagnostic.File.empty())
			{
				return "Scripts";
			}
			std::filesystem::path file = diagnostic.File;
			if (FileSystem::IsInside(diagnostic.File, projectDirectory))
			{
				file = FileSystem::GetRelativePath(diagnostic.File, projectDirectory);
			}
			std::string location = FileSystem::PathToUtf8(file);
			if (diagnostic.Line > 0)
			{
				location += diagnostic.Column > 0 ? fmt::format("({},{})", diagnostic.Line, diagnostic.Column)
				                                  : fmt::format("({})", diagnostic.Line);
			}
			return location;
		}

		// The types the C# project's sources declare, each with its source as the user knows it.
		struct DeclaredSources
		{
			// "Scripts/Source/AI/Brain.cs".
			std::string Description;
			std::vector<ScriptBrowsing::DeclaredType> Types;
		};

		std::string DescribeProjectFile(Project const& project, std::filesystem::path const& file)
		{
			return FileSystem::PathToUtf8(FileSystem::GetRelativePath(file, project.GetDirectory()));
		}

		std::vector<DeclaredSources> ReadDeclaredSources(Project const& project)
		{
			std::vector<DeclaredSources> sources;
			for (std::filesystem::path const& file : ScriptProject::ListSources(project))
			{
				// A source that cannot be read cannot be checked; its build reports what is wrong with it.
				if (Result<std::string> const text = FileSystem::ReadTextFile(file))
				{
					sources.push_back({DescribeProjectFile(project, file), ScriptBrowsing::FindDeclaredTypes(text.GetValue())});
				}
			}
			return sources;
		}

		// Refuses C# sources that would keep the C# project from building (the files of one assembly may not declare a type
		// twice): a name that a file of the project or another of the sources has, which gives the same class in the
		// common case of an updated copy, or a type that the project or another of the sources declares.
		Result<void> CheckNewSources(Project const& project, std::span<ScriptFileCopy const> files)
		{
			std::optional<std::vector<DeclaredSources>> declared;
			std::vector<std::string> names;
			for (ScriptFileCopy const& file : files)
			{
				if (!ScriptBrowsing::IsScriptFile(FileSystem::PathFromUtf8(file.Target)))
				{
					continue;
				}
				std::string const name = AssetBrowsing::GetFileName(file.Target);
				if (std::optional<std::filesystem::path> const existing = ScriptProject::FindSource(project, name))
				{
					return MakeError("'{}' already exists in {}: replace it there or rename the class", name,
					                 DescribeProjectFile(project, existing->parent_path()));
				}
				if (std::find(names.begin(), names.end(), name) != names.end())
				{
					return MakeError("two of the C# sources are named '{}': rename one of them", name);
				}
				names.push_back(name);
				Result<std::string> const text = FileSystem::ReadTextFile(file.Source);
				if (!text)
				{
					return Error{text.GetError()};
				}
				if (!declared)
				{
					declared = ReadDeclaredSources(project);
				}
				std::vector<ScriptBrowsing::DeclaredType> types = ScriptBrowsing::FindDeclaredTypes(text.GetValue());
				for (ScriptBrowsing::DeclaredType const& type : types)
				{
					for (DeclaredSources const& source : *declared)
					{
						for (ScriptBrowsing::DeclaredType const& other : source.Types)
						{
							if (ScriptBrowsing::IsDuplicateDeclaration(other, type))
							{
								return MakeError("'{}' declares {}, which {} declares already: rename one of them", name,
								                 ScriptBrowsing::GetTypeDisplayName(type.Name), source.Description);
							}
						}
					}
				}
				declared->push_back({fmt::format("{} (added with it)", file.Target), std::move(types)});
			}
			return {};
		}
	}

	void LogScriptDiagnostics(std::vector<ScriptDiagnostic> const& diagnostics, std::filesystem::path const& projectDirectory)
	{
		for (ScriptDiagnostic const& diagnostic : diagnostics)
		{
			std::string const location = DescribeLocation(diagnostic, projectDirectory);
			if (diagnostic.Severity == ScriptDiagnosticSeverity::Error)
			{
				ST_ERROR("{}: error {}: {}", location, diagnostic.Code, diagnostic.Message);
			}
			else
			{
				ST_WARN("{}: warning {}: {}", location, diagnostic.Code, diagnostic.Message);
			}
		}
	}

	uint32_t ScriptBuildReport::CountDiagnostics(ScriptDiagnosticSeverity severity) const
	{
		return static_cast<uint32_t>(std::count_if(Diagnostics.begin(), Diagnostics.end(),
		                                           [severity](ScriptDiagnostic const& diagnostic)
		                                           {
													   return diagnostic.Severity == severity;
												   }));
	}

	EditorScripts::EditorScripts(EditorContext& context, std::filesystem::path scriptCoreAssembly)
		: m_Context(context),
		  m_ScriptCoreAssembly(std::move(scriptCoreAssembly))
	{
	}

	EditorScripts::~EditorScripts()
	{
		// Callbacks are dropped, not called: what they belong to (automation requests) may already be gone.
		m_Callbacks.clear();
		StopBuild();
	}

	void EditorScripts::Update()
	{
		FollowProject();
		if (m_Build.valid() && m_Build.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			FinishBuild();
		}
		if (m_LoadPending)
		{
			LoadAssembly();
		}
		BuildChangedSources();
	}

	void EditorScripts::Build(BuildCallback callback)
	{
		if (callback)
		{
			m_Callbacks.push_back(std::move(callback));
		}
		if (m_Build.valid())
		{
			return;
		}
		if (!m_BuildBlocker.empty())
		{
			// Refused without replacing the last build's outcome.
			ScriptBuildReport blocked;
			blocked.Error = m_BuildBlocker;
			NotifyCallbacks(blocked);
			return;
		}

		ScriptBuildReport refused;
		Project const* const project = m_Context.GetProject();
		if (project == nullptr)
		{
			refused.Error = "no project is open";
		}
		else if (!ScriptProject::Exists(*project))
		{
			refused.Error = "the project has no scripts yet: create one first";
		}
		else if (Result<void> props = ScriptProject::UpdateProps(*project, m_ScriptCoreAssembly); !props)
		{
			refused.Error = props.GetError();
		}
		if (!refused.Error.empty())
		{
			Complete(std::move(refused));
			return;
		}

		m_BuiltSources = ScriptProject::GetNewestSourceTime(*project);
		m_CancelBuild = false;
		m_BuildStart = std::chrono::steady_clock::now();
		ST_INFO("Building the game's scripts...");
		ScriptBuildSettings const settings = ScriptProject::MakeBuildSettings(*project, ScriptBuildConfiguration::Debug);
		try
		{
			// The worker touches no engine state: it runs dotnet and parses its output.
			m_Build = std::async(std::launch::async,
			                     [settings, &cancel = m_CancelBuild]()
			                     {
									 return ScriptProject::Build(settings, &cancel);
								 });
		}
		catch (std::system_error const& error)
		{
			refused.Error = fmt::format("the build thread cannot start: {}", error.what());
			Complete(std::move(refused));
		}
	}

	Result<std::filesystem::path> EditorScripts::CreateScript(std::string_view className, std::string_view folder)
	{
		Project const* const project = m_Context.GetProject();
		if (project == nullptr)
		{
			return Error{"no project is open"};
		}
		if (!ScriptProject::IsValidClassName(className))
		{
			return MakeError("'{}' is not a valid script class name: use letters, digits and underscores, starting with a letter",
			                 className);
		}
		if (Result<std::filesystem::path> const directory = ScriptProject::GetSourceFolder(*project, folder); !directory)
		{
			return Error{directory.GetError()};
		}
		// The template declares the class in the project's namespace.
		ScriptBrowsing::DeclaredType declared;
		declared.Name = ScriptProject::GetNamespace(*project) + "." + std::string(className);
		for (DeclaredSources const& source : ReadDeclaredSources(*project))
		{
			for (ScriptBrowsing::DeclaredType const& type : source.Types)
			{
				if (ScriptBrowsing::IsDuplicateDeclaration(type, declared))
				{
					return MakeError("{} declares {} already: choose another name", source.Description,
					                 ScriptBrowsing::GetTypeDisplayName(type.Name));
				}
			}
		}
		if (Result<void> generated = ScriptProject::Generate(*project, m_ScriptCoreAssembly); !generated)
		{
			return Error{generated.GetError()};
		}
		return ScriptProject::CreateScript(*project, className, folder);
	}

	Result<std::vector<std::filesystem::path>> EditorScripts::AddScripts(std::span<std::filesystem::path const> files,
	                                                                     std::string_view folder)
	{
		Project const* const project = m_Context.GetProject();
		if (project == nullptr)
		{
			return Error{"no project is open"};
		}
		for (std::filesystem::path const& file : files)
		{
			if (!FileSystem::IsRegularFile(file) || !ScriptBrowsing::IsScriptFile(file))
			{
				return MakeError("'{}' is not a C# source file", FileSystem::PathToUtf8(file.filename()));
			}
		}
		// Sources of the C# project, under any spelling the file system takes, are compiled already: copies would declare
		// their types twice.
		std::filesystem::path const projectDirectory = project->GetDirectory() / ScriptProject::DirectoryName;
		std::vector<std::optional<std::filesystem::path>> inProject;
		inProject.reserve(files.size());
		std::vector<ScriptFileCopy> copies;
		for (std::filesystem::path const& file : files)
		{
			inProject.push_back(FileSystem::FindInside(file, projectDirectory));
			if (!inProject.back())
			{
				copies.push_back({file, FileSystem::PathToUtf8(file.filename())});
			}
		}
		Result<std::vector<std::filesystem::path>> copied = AddScriptFiles(copies, folder);
		if (!copied)
		{
			return Error{copied.GetError()};
		}
		std::vector<std::filesystem::path> added;
		added.reserve(files.size());
		size_t next = 0;
		for (std::optional<std::filesystem::path> const& source : inProject)
		{
			added.push_back(source ? *source : copied.GetValue()[next++]);
		}
		return added;
	}

	Result<std::vector<std::filesystem::path>> EditorScripts::AddScriptFiles(std::span<ScriptFileCopy const> files, std::string_view folder)
	{
		Project const* const project = m_Context.GetProject();
		if (project == nullptr)
		{
			return Error{"no project is open"};
		}
		Result<std::filesystem::path> const folderDirectory = ScriptProject::GetSourceFolder(*project, folder);
		if (!folderDirectory)
		{
			return Error{folderDirectory.GetError()};
		}
		std::filesystem::path const& destination = folderDirectory.GetValue();
		std::vector<std::filesystem::path> targets;
		targets.reserve(files.size());
		for (ScriptFileCopy const& file : files)
		{
			std::filesystem::path const relative = FileSystem::PathFromUtf8(file.Target);
			std::filesystem::path const target = (destination / relative).lexically_normal();
			if (relative.empty() || relative.has_root_path() || target == destination || !FileSystem::IsInside(target, destination))
			{
				return MakeError("'{}' is not a file of the scripts' folder", file.Target);
			}
			if (!FileSystem::IsRegularFile(file.Source))
			{
				return MakeError("'{}' is not a file", FileSystem::PathToUtf8(file.Source));
			}
			targets.push_back(target);
		}
		// C# sources first: their refusal says where the source of that name or type is.
		if (Result<void> checked = CheckNewSources(*project, files); !checked)
		{
			return Error{checked.GetError()};
		}
		for (size_t index = 0; index < targets.size(); index++)
		{
			std::filesystem::path const& target = targets[index];
			if (FileSystem::Exists(target) || std::find(targets.begin(), targets.begin() + static_cast<std::ptrdiff_t>(index), target) !=
			                                      targets.begin() + static_cast<std::ptrdiff_t>(index))
			{
				return MakeError("'{}' already exists",
				                 FileSystem::PathToUtf8(FileSystem::GetRelativePath(target, project->GetDirectory())));
			}
		}
		if (Result<void> generated = ScriptProject::Generate(*project, m_ScriptCoreAssembly); !generated)
		{
			return Error{generated.GetError()};
		}

		// All or nothing: what was copied, and the folders made for it, go again after a failure.
		FileCopies copies;
		for (size_t index = 0; index < files.size(); index++)
		{
			if (Result<void> copied = copies.Copy(files[index].Source, targets[index]); !copied)
			{
				copies.Rollback();
				return Error{copied.GetError()};
			}
		}
		return copies.GetCopies();
	}

	bool EditorScripts::HasScriptProject() const
	{
		Project const* const project = m_Context.GetProject();
		return project != nullptr && ScriptProject::Exists(*project);
	}

	bool EditorScripts::IsOutOfDate() const
	{
		Project const* const project = m_Context.GetProject();
		return project != nullptr && ScriptProject::Exists(*project) && ScriptProject::IsOutOfDate(*project);
	}

	void EditorScripts::FollowProject()
	{
		Project const* const project = m_Context.GetProject();
		std::filesystem::path const projectFile = project != nullptr ? project->GetFilePath() : std::filesystem::path();
		if (projectFile == m_ProjectFile)
		{
			return;
		}

		if (StopBuild())
		{
			ScriptBuildReport cancelled;
			cancelled.Error = "the build was cancelled: the project changed";
			Complete(std::move(cancelled));
		}
		m_ProjectFile = projectFile;
		m_LastBuild.reset();
		m_BuiltSources.reset();
		m_LoadPending = false;
		// The previous project's scripts go (scripts never run while projects change).
		if (ScriptEngine::IsInitialized() && ScriptEngine::HasGameAssembly() && ScriptEngine::GetInstanceCount() == 0)
		{
			ScriptEngine::UnloadGameAssembly();
		}
		if (project == nullptr || !ScriptProject::Exists(*project))
		{
			return;
		}
		if (Result<void> props = ScriptProject::UpdateProps(*project, m_ScriptCoreAssembly); !props)
		{
			ST_WARN("The script project cannot be updated: {}", props.GetError());
		}
		if (ScriptProject::IsOutOfDate(*project))
		{
			Build();
		}
		else
		{
			m_BuiltSources = ScriptProject::GetNewestSourceTime(*project);
			LoadAssembly();
		}
	}

	void EditorScripts::FinishBuild()
	{
		Result<ScriptBuildResult> result = m_Build.get();
		ScriptBuildReport report;
		report.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_BuildStart).count();
		if (!result)
		{
			report.Error = result.GetError();
			ST_ERROR("The game's scripts cannot be built: {}", report.Error);
			Complete(std::move(report));
			return;
		}

		report.Succeeded = result.GetValue().Succeeded;
		report.Diagnostics = std::move(result.GetValue().Diagnostics);
		LogScriptDiagnostics(report.Diagnostics, m_ProjectFile.parent_path());
		if (report.Succeeded)
		{
			ST_INFO("The game's scripts were built in {:.1f} s ({} warnings)", report.Seconds,
			        report.CountDiagnostics(ScriptDiagnosticSeverity::Warning));
			report.Loaded = LoadAssembly();
		}
		else
		{
			ST_ERROR("The game's scripts did not build: {} errors", report.CountDiagnostics(ScriptDiagnosticSeverity::Error));
		}
		Complete(std::move(report));
	}

	void EditorScripts::BuildChangedSources()
	{
		std::chrono::steady_clock::time_point const now = std::chrono::steady_clock::now();
		if (m_Build.valid() || !m_BuildBlocker.empty() || now - m_LastSourceCheck < SourceCheckInterval)
		{
			return;
		}
		m_LastSourceCheck = now;
		Project const* const project = m_Context.GetProject();
		if (project == nullptr || !ScriptProject::Exists(*project))
		{
			return;
		}
		std::optional<std::filesystem::file_time_type> const sources = ScriptProject::GetNewestSourceTime(*project);
		if (sources && (!m_BuiltSources || *sources > *m_BuiltSources))
		{
			Build();
		}
	}

	void EditorScripts::Complete(ScriptBuildReport report)
	{
		m_LastBuild = std::move(report);
		NotifyCallbacks(ScriptBuildReport(*m_LastBuild));
	}

	void EditorScripts::NotifyCallbacks(ScriptBuildReport const& report)
	{
		// Callbacks may start another build.
		std::vector<BuildCallback> callbacks;
		callbacks.swap(m_Callbacks);
		for (BuildCallback const& callback : callbacks)
		{
			callback(report);
		}
	}

	bool EditorScripts::LoadAssembly()
	{
		Project const* const project = m_Context.GetProject();
		if (project == nullptr || !ScriptEngine::IsInitialized())
		{
			m_LoadPending = false;
			return false;
		}
		// The running scripts keep their assembly; the new one replaces it when they stop.
		if (ScriptEngine::GetInstanceCount() > 0)
		{
			m_LoadPending = true;
			return false;
		}
		m_LoadPending = false;
		std::filesystem::path const assembly = ScriptProject::GetAssemblyPath(*project);
		std::error_code error;
		if (!std::filesystem::is_regular_file(assembly, error))
		{
			return false;
		}
		if (Result<void> loaded = ScriptEngine::LoadGameAssembly(assembly); !loaded)
		{
			ST_ERROR("The game's scripts cannot be loaded: {}", loaded.GetError());
			return false;
		}
		ST_INFO("Loaded {} script classes from {}", ScriptEngine::GetClasses().size(), FileSystem::PathToUtf8(assembly.filename()));
		return true;
	}

	bool EditorScripts::StopBuild()
	{
		if (!m_Build.valid())
		{
			return false;
		}
		m_CancelBuild = true;
		m_Build.wait();
		m_Build = {};
		return true;
	}
}
