#include "Editor/EditorExport.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"

#include <exception>
#include <utility>

namespace Strada
{
	EditorExport::EditorExport(EditorContext& context, EditorScripts& scripts)
		: m_Context(context),
		  m_Scripts(scripts)
	{
	}

	EditorExport::~EditorExport()
	{
		if (m_Export.valid())
		{
			m_Cancel = true;
			m_Export.wait();
			m_Scripts.SetBuildBlocker({});
		}
	}

	Result<void> EditorExport::Start(std::filesystem::path const& directory, CompletionCallback callback)
	{
		Project const* const project = m_Context.GetProject();
		if (project == nullptr)
		{
			return Error{"no project is open"};
		}
		if (m_Export.valid())
		{
			return Error{"the game is being exported: wait for the export to finish"};
		}
		if (m_Context.IsPlaying())
		{
			return Error{"the editor is playing: stop playing first"};
		}
		if (m_Scripts.IsBuilding())
		{
			return Error{"the scripts are being built: export once the build finished"};
		}
		Result<GameExportSettings> prepared = GameExporter::Prepare(*project, directory);
		if (!prepared)
		{
			return Error{prepared.GetError()};
		}

		m_Pending = GameExportReport();
		m_Pending.Directory = directory;
		m_Pending.UnsavedChanges = m_Context.IsDirty();
		m_ProjectDirectory = project->GetDirectory();
		m_Callback = std::move(callback);
		m_Cancel = false;
		m_Start = std::chrono::steady_clock::now();
		{
			std::lock_guard<std::mutex> const lock(m_ProgressMutex);
			m_Progress = "Starting";
		}
		ST_INFO("Exporting the game to '{}'...", FileSystem::PathToUtf8(directory));
		try
		{
			// The worker touches no engine state: it copies files and runs dotnet.
			m_Export = std::async(std::launch::async,
			                      [this, settings = prepared.TakeValue()]()
			                      {
									  return GameExporter::Export(
										  settings,
										  [this](std::string_view step)
										  {
											  std::lock_guard<std::mutex> const lock(m_ProgressMutex);
											  m_Progress = std::string(step);
										  },
										  &m_Cancel);
								  });
		}
		catch (std::exception const& exception)
		{
			m_Callback = {};
			return MakeError("the export cannot start: {}", exception.what());
		}
		m_Scripts.SetBuildBlocker("the game is being exported: wait for the export to finish");
		return {};
	}

	std::string EditorExport::GetProgress() const
	{
		std::lock_guard<std::mutex> const lock(m_ProgressMutex);
		return m_Progress;
	}

	void EditorExport::Cancel()
	{
		if (m_Export.valid())
		{
			m_Cancel = true;
		}
	}

	void EditorExport::Update()
	{
		if (!m_Export.valid() || m_Export.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		{
			return;
		}
		Result<GameExportResult> exported = m_Export.get();
		m_Scripts.SetBuildBlocker({});

		GameExportReport report = std::move(m_Pending);
		report.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_Start).count();
		if (!exported)
		{
			report.Error = exported.GetError();
			report.Cancelled = m_Cancel;
		}
		else
		{
			GameExportResult const& result = exported.GetValue();
			if (result.ScriptBuild)
			{
				report.ScriptsBuilt = result.ScriptBuild->Succeeded;
				report.ScriptBuildFailed = !result.ScriptBuild->Succeeded;
				report.Diagnostics = result.ScriptBuild->Diagnostics;
				LogScriptDiagnostics(report.Diagnostics, m_ProjectDirectory);
			}
			report.Succeeded = result.Succeeded;
			report.Executable = result.Executable;
			if (!result.Succeeded)
			{
				report.Error = "the scripts did not build: fix their errors to export the game";
			}
		}

		if (report.Succeeded)
		{
			ST_INFO("Exported the game to '{}' in {:.1f} s", FileSystem::PathToUtf8(report.Executable), report.Seconds);
		}
		else
		{
			ST_ERROR("The game was not exported: {}", report.Error);
		}
		CompletionCallback const callback = std::move(m_Callback);
		m_Callback = {};
		if (callback)
		{
			callback(report);
		}
	}
}
