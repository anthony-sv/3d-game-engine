#include "Editor/Automation/ScriptCommands.h"

#include "Editor/Automation/JsonSchema.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/ScriptFieldSerialization.h"

#include <string>

namespace Strada
{
	namespace
	{
		Json DescribeDiagnostic(ScriptDiagnostic const& diagnostic)
		{
			return Json::object({{"severity", diagnostic.Severity == ScriptDiagnosticSeverity::Error ? "error" : "warning"},
			                     {"file", diagnostic.File.empty() ? Json() : Json(FileSystem::PathToUtf8(diagnostic.File))},
			                     {"line", diagnostic.Line},
			                     {"column", diagnostic.Column},
			                     {"code", diagnostic.Code},
			                     {"message", diagnostic.Message}});
		}

		Json DescribeReport(ScriptBuildReport const& report)
		{
			Json diagnostics = Json::array();
			for (ScriptDiagnostic const& diagnostic : report.Diagnostics)
			{
				diagnostics.push_back(DescribeDiagnostic(diagnostic));
			}
			return Json::object({{"succeeded", report.Succeeded},
			                     {"error", report.Error.empty() ? Json() : Json(report.Error)},
			                     {"loaded", report.Loaded},
			                     {"seconds", report.Seconds},
			                     {"errors", report.CountDiagnostics(ScriptDiagnosticSeverity::Error)},
			                     {"warnings", report.CountDiagnostics(ScriptDiagnosticSeverity::Warning)},
			                     {"diagnostics", std::move(diagnostics)}});
		}

		Json DescribeField(ScriptFieldInfo const& field)
		{
			Json description = Json::object({{"name", field.Name},
			                                 {"type", ScriptFieldTypeToString(field.Type)},
			                                 {"default", ScriptFieldValueToJson(field.DefaultValue)}});
			if (field.Hidden)
			{
				description["hidden"] = true;
			}
			if (!field.Tooltip.empty())
			{
				description["tooltip"] = field.Tooltip;
			}
			if (field.Range)
			{
				description["range"] = Json::array({field.Range->x, field.Range->y});
			}
			if (field.AcceptedAssetType != AssetType::None)
			{
				description["assetType"] = AssetTypeToString(field.AcceptedAssetType);
			}
			if (!field.Enumerators.empty())
			{
				Json enumerators = Json::array();
				for (ScriptEnumerator const& enumerator : field.Enumerators)
				{
					enumerators.push_back(Json::object({{"name", enumerator.Name}, {"value", ScriptFieldValueToJson(enumerator.Value)}}));
				}
				description["enumerators"] = std::move(enumerators);
				if (field.IsFlags)
				{
					description["flags"] = true;
				}
			}
			return description;
		}

		Result<void> Add(CommandRegistry& registry, std::string name, std::string description, Json parameters, bool readOnly,
		                 CommandHandler handler)
		{
			CommandDefinition definition;
			definition.Name = std::move(name);
			definition.Description = std::move(description);
			definition.Parameters = std::move(parameters);
			definition.ReadOnly = readOnly;
			definition.Handler = std::move(handler);
			return registry.Register(std::move(definition));
		}
	}

	Result<void> RegisterScriptCommands(CommandRegistry& registry, EditorScripts& scripts, EditorContext& context)
	{
		Result<void> result =
			Add(registry, "script.status",
		        "The scripting state: whether scripting is available (.NET found), whether the project has a C# project, a build is "
		        "running or its sources changed since the last build, whether the game assembly is loaded, the number of script "
		        "classes and the last build ({ succeeded, error, loaded, seconds, errors, warnings, diagnostics }; null before the "
		        "first).",
		        Json(), true,
		        [&scripts](Json const&) -> CommandResult
		        {
					bool const available = ScriptEngine::IsInitialized();
					ScriptBuildReport const* const lastBuild = scripts.GetLastBuild();
					return Json::object({{"available", available},
			                             {"scriptProject", scripts.HasScriptProject()},
			                             {"building", scripts.IsBuilding()},
			                             {"outOfDate", scripts.IsOutOfDate()},
			                             {"loaded", available && ScriptEngine::HasGameAssembly()},
			                             {"classCount", available ? ScriptEngine::GetClasses().size() : 0},
			                             {"lastBuild", lastBuild != nullptr ? DescribeReport(*lastBuild) : Json()}});
				});

		result = result ? Add(registry, "script.classes",
		                      "The script classes of the loaded game assembly, sorted by name, with their serialized fields: "
		                      "[{ name, fields: [{ name, type, default, hidden?, tooltip?, range?, assetType?, enumerators?, flags? }] }].",
		                      Json(), true,
		                      [](Json const&) -> CommandResult
		                      {
								  if (!ScriptEngine::IsInitialized())
								  {
									  return MakeCommandError(AutomationErrorCode::Unavailable,
				                                              "scripting is unavailable: .NET 10 was not found");
								  }
								  Json classes = Json::array();
								  for (ScriptClassInfo const& scriptClass : ScriptEngine::GetClasses())
								  {
									  Json fields = Json::array();
									  for (ScriptFieldInfo const& field : scriptClass.Fields)
									  {
										  fields.push_back(DescribeField(field));
									  }
									  classes.push_back(Json::object({{"name", scriptClass.Name}, {"fields", std::move(fields)}}));
								  }
								  return Json::object({{"classes", std::move(classes)}});
							  })
		                : result;

		result =
			result
				? Add(registry, "script.create",
		              "Creates a script class from the template in Scripts/Source/<className>.cs (and the project's C# project when "
		              "it has none). The scripts build by themselves shortly after; script.build waits for a build. Returns the "
		              "file (relative to the project directory) and the class's full name.",
		              SchemaBuilder::Object()
		                  .Property("className", SchemaBuilder::String("C# class name: letters, digits and underscores").MinLength(1), true)
		                  .Build(),
		              false,
		              [&scripts, &context](Json const& params) -> CommandResult
		              {
						  Project const* const project = context.GetProject();
						  if (project == nullptr)
						  {
							  return MakeCommandError(AutomationErrorCode::InvalidOperation, "no project is open");
						  }
						  std::string const className = params.at("className").get<std::string>();
						  if (!ScriptProject::IsValidClassName(className))
						  {
							  return MakeCommandError(AutomationErrorCode::InvalidOperation,
				                                      "'{}' is not a valid script class name: use letters, digits and underscores, "
				                                      "starting with a letter",
				                                      className);
						  }
						  Result<std::filesystem::path> created = scripts.CreateScript(className);
						  if (!created)
						  {
							  return MakeCommandError(AutomationErrorCode::FileError, "{}", created.GetError());
						  }
						  return Json::object(
							  {{"path", FileSystem::PathToUtf8(FileSystem::GetRelativePath(created.GetValue(), project->GetDirectory()))},
			                   {"class", ScriptProject::GetNamespace(*project) + "." + className}});
					  })
				: result;
		if (!result)
		{
			return result;
		}

		CommandDefinition build;
		build.Name = "script.build";
		build.Description = "Builds the project's scripts with the .NET SDK (joining a build that is running) and loads the built "
							"assembly; asynchronous. Returns { succeeded, error, loaded, seconds, errors, warnings, diagnostics: [{ "
							"severity, file, line, column, code, message }] }; a build with compile errors succeeds as a command "
							"with succeeded false. Fails when there is no project or script yet, or the .NET SDK is missing.";
		build.AsyncHandler = [&scripts](Json const&, CommandCompletion completion)
		{
			scripts.Build(
				[completion](ScriptBuildReport const& report)
				{
					if (!report.Error.empty())
					{
						completion.Complete(MakeCommandError(AutomationErrorCode::InvalidOperation, "{}", report.Error));
						return;
					}
					completion.Complete(DescribeReport(report));
				});
		};
		return registry.Register(std::move(build));
	}
}
