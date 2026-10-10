#include "Editor/EditorScripts.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"
#include "Strada/Script/ScriptEngine.h"

#include <algorithm>
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

	Result<std::filesystem::path> EditorScripts::CreateScript(std::string_view className)
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
		if (Result<void> generated = ScriptProject::Generate(*project, m_ScriptCoreAssembly); !generated)
		{
			return Error{generated.GetError()};
		}
		return ScriptProject::CreateScript(*project, className);
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
