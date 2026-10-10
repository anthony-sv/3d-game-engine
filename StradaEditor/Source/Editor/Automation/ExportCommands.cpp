#include "Editor/Automation/ExportCommands.h"

#include "Editor/Automation/JsonSchema.h"
#include "Editor/Automation/ScriptCommands.h"

#include "Strada/Core/FileSystem.h"

#include <algorithm>
#include <string>

namespace Strada
{
	namespace
	{
		Json DescribeExport(GameExportReport const& report)
		{
			Json diagnostics = Json::array();
			for (ScriptDiagnostic const& diagnostic : report.Diagnostics)
			{
				diagnostics.push_back(DescribeScriptDiagnostic(diagnostic));
			}
			auto const count = [&report](ScriptDiagnosticSeverity severity)
			{
				return std::count_if(report.Diagnostics.begin(), report.Diagnostics.end(),
				                     [severity](ScriptDiagnostic const& diagnostic)
				                     {
										 return diagnostic.Severity == severity;
									 });
			};
			return Json::object({{"succeeded", report.Succeeded},
			                     {"error", report.Error.empty() ? Json() : Json(report.Error)},
			                     {"directory", FileSystem::PathToUtf8(report.Directory)},
			                     {"executable", report.Executable.empty() ? Json() : Json(FileSystem::PathToUtf8(report.Executable))},
			                     {"unsavedChanges", report.UnsavedChanges},
			                     {"seconds", report.Seconds},
			                     {"scripts", Json::object({{"built", report.ScriptsBuilt},
			                                               {"errors", count(ScriptDiagnosticSeverity::Error)},
			                                               {"warnings", count(ScriptDiagnosticSeverity::Warning)},
			                                               {"diagnostics", std::move(diagnostics)}})}});
		}
	}

	Result<void> RegisterExportCommands(CommandRegistry& registry, EditorExport& gameExport, EditorContext& context)
	{
		CommandDefinition exportGame;
		exportGame.Name = "project.export";
		exportGame.Description =
			"Exports the open project as a game for this platform (the player renamed after the game, Game.sgame, the assets, "
			"the scripts built in Release and the engine files it needs), replacing an earlier export in the directory; "
			"asynchronous. The game uses the saved files. Returns { succeeded, error, directory, executable, unsavedChanges, "
			"seconds, scripts: { built, errors, warnings, diagnostics } }; scripts with compile errors end it as a command with "
			"succeeded false. Fails when no project is open, the editor plays, the project has no start scene, or the game "
			"cannot be written.";
		exportGame.Parameters =
			SchemaBuilder::Object()
				.Property("directory", SchemaBuilder::String("Where the game goes: a new or empty directory, or an earlier export "
		                                                     "(absolute, or relative to the project directory; default Build)")
		                                   .MinLength(1))
				.Build();
		exportGame.AsyncHandler = [&gameExport, &context](Json const& params, CommandCompletion completion)
		{
			Project const* const project = context.GetProject();
			if (project == nullptr)
			{
				completion.Complete(MakeCommandError(AutomationErrorCode::InvalidOperation, "no project is open"));
				return;
			}
			auto const directory = params.find("directory");
			std::filesystem::path const path =
				project->GetDirectory() / FileSystem::PathFromUtf8(directory != params.end() ? directory->get<std::string>() : "Build");
			Result<void> started = gameExport.Start(
				path.lexically_normal(),
				[completion](GameExportReport const& report)
				{
					if (report.Succeeded || report.ScriptBuildFailed)
					{
						completion.Complete(DescribeExport(report));
					}
					else
					{
						completion.Complete(MakeCommandError(
							report.Cancelled ? AutomationErrorCode::Cancelled : AutomationErrorCode::FileError, "{}", report.Error));
					}
				});
			if (!started)
			{
				completion.Complete(MakeCommandError(AutomationErrorCode::InvalidOperation, "{}", started.GetError()));
			}
		};
		return registry.Register(std::move(exportGame));
	}
}
