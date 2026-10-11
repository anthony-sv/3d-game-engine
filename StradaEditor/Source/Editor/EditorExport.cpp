#include "Editor/EditorExport.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"
#include "Strada/Platform/Process.h"

#include <algorithm>
#include <exception>
#include <string>
#include <system_error>
#include <utility>

namespace Strada
{
	EditorExport::EditorExport(EditorContext& context, EditorScripts& scripts)
		: m_Context(context),
		  m_Scripts(scripts)
	{
	}

	namespace
	{
		// The last lines of a program's output (the smoke test's failure reason).
		std::string Tail(std::string const& output, size_t lines)
		{
			size_t start = output.size();
			for (size_t found = 0; found <= lines && start > 0;)
			{
				start = output.rfind('\n', start - 1);
				if (start == std::string::npos)
				{
					return output;
				}
				found++;
			}
			return output.substr(start + 1);
		}
	}

	EditorExport::~EditorExport()
	{
		if (m_Export.valid())
		{
			m_Cancel = true;
			m_Export.wait();
			m_Scripts.SetBuildBlocker({});
		}
		StopGame();
	}

	std::filesystem::path EditorExport::GetPlayerBinary(std::filesystem::path const& executable)
	{
		if (executable.extension() == ".app")
		{
			return executable / "Contents" / "MacOS" / executable.stem();
		}
		return executable;
	}

	Result<void> EditorExport::Start(std::filesystem::path const& directory, CompletionCallback callback, EditorExportOptions options)
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
			                      [this, settings = prepared.TakeValue(), options]()
			                      {
									  auto const progress = [this](std::string_view step)
									  {
										  std::lock_guard<std::mutex> const lock(m_ProgressMutex);
										  m_Progress = std::string(step);
									  };
									  WorkerResult worker;
									  worker.Export = GameExporter::Export(settings, progress, &m_Cancel);
									  if (!worker.Export || !worker.Export.GetValue().Succeeded)
									  {
										  return worker;
									  }
									  std::error_code error;
									  for (std::filesystem::recursive_directory_iterator it(settings.OutputDirectory, error), end;
				                           !error && it != end; it.increment(error))
									  {
										  if (it->is_regular_file(error))
										  {
											  worker.Bytes += static_cast<uint64_t>(it->file_size(error));
											  worker.Files++;
										  }
									  }
									  if (options.SmokeTest && !m_Cancel)
									  {
										  progress("Testing the game");
										  ProcessSpecification run;
										  run.Executable = GetPlayerBinary(worker.Export.GetValue().Executable);
										  run.Arguments = {"--headless", "--frames", std::to_string(std::max(options.SmokeTestFrames, 1u))};
										  run.WorkingDirectory = settings.OutputDirectory;
										  run.Timeout = std::chrono::minutes(2);
										  Result<ProcessResult> const ran = Process::Run(run, &m_Cancel);
										  worker.SmokeTestPassed = ran && ran.GetValue().Succeeded();
										  worker.SmokeTestOutput = ran ? Tail(ran.GetValue().Output, 12) : ran.GetError();
									  }
									  return worker;
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

	Result<void> EditorExport::RunGame(std::filesystem::path const& executable)
	{
		if (IsGameRunning())
		{
			return Error{"the game is already running"};
		}
		if (m_Game.valid())
		{
			m_Game.get();
		}
		std::filesystem::path const binary = GetPlayerBinary(executable);
		if (!FileSystem::IsRegularFile(binary))
		{
			return MakeError("'{}' does not exist", FileSystem::PathToUtf8(binary));
		}
		m_StopGame = false;
		try
		{
			m_Game = std::async(std::launch::async,
			                    [this, binary]
			                    {
									ProcessSpecification run;
									run.Executable = binary;
									run.WorkingDirectory = binary.parent_path();
									if (Result<ProcessResult> const ran = Process::Run(run, &m_StopGame); !ran)
									{
										ST_ERROR("The game could not run: {}", ran.GetError());
									}
								});
		}
		catch (std::exception const& exception)
		{
			return MakeError("the game cannot start: {}", exception.what());
		}
		return {};
	}

	bool EditorExport::IsGameRunning() const
	{
		return m_Game.valid() && m_Game.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
	}

	void EditorExport::StopGame()
	{
		if (m_Game.valid())
		{
			m_StopGame = true;
			m_Game.wait();
		}
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
		WorkerResult worker = m_Export.get();
		Result<GameExportResult>& exported = worker.Export;
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
			report.Bytes = worker.Bytes;
			report.Files = worker.Files;
			report.SmokeTestPassed = worker.SmokeTestPassed;
			report.SmokeTestOutput = std::move(worker.SmokeTestOutput);
			if (!result.Succeeded)
			{
				report.Error = "the scripts did not build: fix their errors to export the game";
			}
		}

		if (report.Succeeded)
		{
			ST_INFO("Exported the game to '{}' in {:.1f} s ({} files, {:.1f} MB)", FileSystem::PathToUtf8(report.Executable),
			        report.Seconds, report.Files, static_cast<double>(report.Bytes) / (1024.0 * 1024.0));
			if (report.SmokeTestPassed == std::optional<bool>(true))
			{
				ST_INFO("The exported game started and exited cleanly");
			}
			else if (report.SmokeTestPassed)
			{
				ST_ERROR("The exported game failed its smoke test:\n{}", report.SmokeTestOutput);
			}
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
