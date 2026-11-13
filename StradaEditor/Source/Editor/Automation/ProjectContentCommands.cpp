#include "Editor/Automation/ProjectContentCommands.h"

#include "Editor/Automation/EditorCommands.h"
#include "Editor/Automation/JsonSchema.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		Json DescribePath(std::filesystem::path const& path)
		{
			return path.empty() ? Json() : Json(FileSystem::PathToUtf8(path));
		}

		CommandError MakeDirectoryError(std::string const& message)
		{
			CommandError error = MakeCommandError(AutomationErrorCode::InvalidParams, "directory: {}", message);
			error.Data = Json::object({{"path", "directory"}});
			return error;
		}

		// project.copy-example's directory: absolute, or relative to the open project's directory (as project.export's is).
		CommandValue<std::filesystem::path> ResolveDirectory(std::string const& text, Project const* project)
		{
			bool const control = std::ranges::any_of(text,
			                                         [](char character)
			                                         {
														 auto const byte = static_cast<uint8_t>(character);
														 return byte < 0x20 || byte == 0x7F;
													 });
			if (control)
			{
				return MakeDirectoryError("control characters are not allowed in a directory");
			}
			std::filesystem::path const path = FileSystem::PathFromUtf8(text);
			if (path.is_absolute())
			{
				return path.lexically_normal();
			}
			if (path.has_root_name() || path.has_root_directory())
			{
				return MakeDirectoryError(fmt::format("'{}' is neither absolute nor relative to a folder", text));
			}
			if (project == nullptr)
			{
				return MakeDirectoryError(
					fmt::format("'{}' is relative to the open project's directory, but no project is open: use an absolute path", text));
			}
			return (project->GetDirectory() / path).lexically_normal();
		}

		// What the commands share: the engine content and the callback of a copy that opened.
		struct ContentCommands
		{
			EditorOperations& Operations;
			ExampleCopyTask& Copies;
			EditorLocations Locations;
			std::function<void()> ProjectOpened;

			EditorContext& Context() { return Operations.GetContext(); }

			// The checks of project.copy-example before anything is copied; the example's catalog and the target.
			struct CopyRequest
			{
				ExampleCatalog Catalog;
				ExampleEntry Example;
				std::filesystem::path Directory;
			};

			CommandValue<CopyRequest> CheckCopy(Json const& params, bool open, bool discardChanges)
			{
				std::string const id = params["example"].get<std::string>();
				Result<ExampleCatalog> catalog = EditorOperations::LoadExampleCatalog(Locations.Layout);
				ExampleEntry const* example = catalog ? catalog.GetValue().Find(id) : nullptr;
				if (example == nullptr)
				{
					CommandError error = catalog ? MakeCommandError(AutomationErrorCode::InvalidParams,
					                                                "example: there is no example '{}' (project.examples lists them)", id)
					                             : MakeCommandError(AutomationErrorCode::InvalidParams,
					                                                "example: there is no example '{}': {}", id, catalog.GetError());
					error.Data = Json::object({{"path", "example"}});
					return error;
				}

				CommandValue<std::filesystem::path> directory =
					ResolveDirectory(params["directory"].get<std::string>(), Context().GetProject());
				if (!directory)
				{
					return directory.GetError();
				}
				std::filesystem::path const& target = directory.GetValue();
				std::filesystem::path const source = catalog.GetValue().GetDirectory(*example);
				if (FileSystem::IsInside(target, source) || FileSystem::IsInside(source, target))
				{
					return MakeDirectoryError(
						fmt::format("'{}' and the example's folder lie inside one another", FileSystem::PathToUtf8(target)));
				}
				if (AssetManager::IsInitialized() && AssetManager::HasAssetDirectory() &&
				    FileSystem::IsInside(target, AssetManager::GetAssetDirectory()))
				{
					return MakeDirectoryError(
						fmt::format("'{}' lies in the open project's asset directory, where a project does not belong",
					                FileSystem::PathToUtf8(target)));
				}
				if (Result<void> available = CheckNewProjectDirectory(target); !available)
				{
					return MakeDirectoryError(available.GetError());
				}

				if (!catalog.GetValue().IsAvailable(*example))
				{
					CommandError error =
						MakeCommandError(AutomationErrorCode::InvalidOperation, "the example '{}' is not available: {}", id,
					                     example->NeedsHint.empty() ? std::string("its files are missing") : example->NeedsHint);
					error.Data = Json::object({{"hint", example->NeedsHint.empty() ? Json() : Json(example->NeedsHint)}});
					return error;
				}
				if (open)
				{
					if (Context().IsPlaying())
					{
						return MakeCommandError(AutomationErrorCode::InvalidOperation,
						                        "the editor is playing: stop playing first, or pass \"open\": false");
					}
					if (Context().IsDirty() && !discardChanges)
					{
						return MakeCommandError(AutomationErrorCode::UnsavedChanges,
						                        "scene '{}' has unsaved changes; save it first or pass \"discardChanges\": true",
						                        Context().GetScene().GetName());
					}
				}
				if (Copies.IsRunning())
				{
					return MakeCommandError(AutomationErrorCode::InvalidOperation, "an example is being copied: wait until it is done");
				}
				ExampleEntry entry = *example;
				return CopyRequest{catalog.TakeValue(), std::move(entry), target};
			}

			// The copy's outcome, opened when asked: the editor may have started playing or the scene changed while it copied.
			CommandResult Finish(Result<ExampleCopyOutcome> copied, bool cancelled, bool open, bool discardChanges)
			{
				if (!copied)
				{
					return cancelled ? MakeCommandError(AutomationErrorCode::Cancelled, "{}", copied.GetError())
					                 : MakeCommandError(AutomationErrorCode::FileError, "{}", copied.GetError());
				}
				ExampleCopyOutcome const& outcome = copied.GetValue();
				Json result = Json::object({{"directory", FileSystem::PathToUtf8(outcome.Directory)},
				                            {"bytes", outcome.Bytes},
				                            {"files", outcome.Files},
				                            {"warnings", outcome.Warnings}});
				if (!open)
				{
					return result;
				}
				// The copy stays: it is a complete project, which project.open opens later.
				std::string const copy = FileSystem::PathToUtf8(outcome.ProjectFile);
				if (Context().IsPlaying())
				{
					return MakeCommandError(
						AutomationErrorCode::InvalidOperation,
						"the example was copied to '{}', but the editor started playing: stop and open it with project.open", copy);
				}
				if (Context().IsDirty() && !discardChanges)
				{
					return MakeCommandError(
						AutomationErrorCode::UnsavedChanges,
						"the example was copied to '{}', but scene '{}' has unsaved changes now; save it first, then open the "
						"copy with project.open",
						copy, Context().GetScene().GetName());
				}
				Result<std::vector<std::string>> opened = Operations.OpenProject(outcome.ProjectFile);
				if (!opened)
				{
					return MakeCommandError(AutomationErrorCode::FileError, "the example was copied to '{}', but it does not open: {}",
					                        copy, opened.GetError());
				}
				if (ProjectOpened)
				{
					ProjectOpened();
				}
				for (std::string const& warning : opened.GetValue())
				{
					result["warnings"].push_back(warning);
				}
				result["project"] = DescribeEditorProject(Context());
				result["scene"] = DescribeEditorScene(Context());
				return result;
			}
		};
	}

	Json DescribeProjectTemplate(ProjectTemplate const& projectTemplate)
	{
		return Json::object({{"id", projectTemplate.Id},
		                     {"name", projectTemplate.Name},
		                     {"description", projectTemplate.Description},
		                     {"order", projectTemplate.Order},
		                     {"tags", projectTemplate.Tags},
		                     {"requires", Json::object({{"sdk", projectTemplate.RequiresSdk}})},
		                     {"thumbnail", DescribePath(projectTemplate.GetThumbnailPath())}});
	}

	Json DescribeExample(ExampleCatalog const& catalog, ExampleEntry const& example)
	{
		bool const available = catalog.IsAvailable(example);
		Json description = Json::object({{"id", example.Id},
		                                 {"name", example.Name},
		                                 {"description", example.Description},
		                                 {"order", example.Order},
		                                 {"requires", Json::object({{"sdk", example.RequiresSdk}})},
		                                 {"thumbnail", DescribePath(catalog.GetThumbnailPath(example))},
		                                 {"available", available}});
		if (!available)
		{
			description["hint"] = example.NeedsHint.empty() ? Json() : Json(example.NeedsHint);
		}
		return description;
	}

	Result<void> RegisterProjectContentCommands(CommandRegistry& registry, EditorOperations& operations, ExampleCopyTask& copies,
	                                            EditorLocations locations, std::function<void()> projectOpened)
	{
		Ref<ContentCommands> const commands =
			CreateRef<ContentCommands>(ContentCommands{operations, copies, std::move(locations), std::move(projectOpened)});

		CommandDefinition templates;
		templates.Name = "project.templates";
		templates.Description =
			"Lists the templates project.create can start a project from, sorted by order: { templates: [{ id, name, description, "
			"order, tags, requires: { sdk } (the .NET 10 SDK is needed before the project plays: it has C# scripts), thumbnail (an "
			"absolute path to a PNG, or null) }], warnings (templates left out, and the editor's default scene standing in for "
			"empty-3d when the engine has none) }. Changes nothing.";
		templates.ReadOnly = true;
		templates.Handler = [commands](Json const&) -> CommandResult
		{
			ProjectTemplateList const list = EditorOperations::ListProjectTemplates(commands->Locations.Layout);
			Json described = Json::array();
			for (ProjectTemplate const& projectTemplate : list.Templates)
			{
				described.push_back(DescribeProjectTemplate(projectTemplate));
			}
			return Json::object({{"templates", std::move(described)}, {"warnings", list.Warnings}});
		};
		Result<void> result = registry.Register(std::move(templates));

		CommandDefinition examples;
		examples.Name = "project.examples";
		examples.Description =
			"Lists the example games project.copy-example copies, sorted by order: { examples: [{ id, name, description, order, "
			"requires: { sdk }, thumbnail (an absolute path to a PNG, or null), available (the files it needs exist), hint (what to "
			"do when it is not available, or null; only then) }], warnings (examples left out, or why there is no catalog) }. "
			"Changes nothing.";
		examples.ReadOnly = true;
		examples.Handler = [commands](Json const&) -> CommandResult
		{
			Json described = Json::array();
			Json warnings = Json::array();
			Result<ExampleCatalog> catalog = EditorOperations::LoadExampleCatalog(commands->Locations.Layout);
			if (!catalog)
			{
				warnings.push_back(catalog.GetError());
				return Json::object({{"examples", std::move(described)}, {"warnings", std::move(warnings)}});
			}
			for (ExampleEntry const& example : catalog.GetValue().GetExamples())
			{
				described.push_back(DescribeExample(catalog.GetValue(), example));
			}
			for (std::string const& warning : catalog.GetValue().GetWarnings())
			{
				warnings.push_back(warning);
			}
			return Json::object({{"examples", std::move(described)}, {"warnings", std::move(warnings)}});
		};
		result = result ? registry.Register(std::move(examples)) : result;

		CommandDefinition copyExample;
		copyExample.Name = "project.copy-example";
		copyExample.Description =
			"Copies an example game (project.examples lists them) into a new or empty directory, leaving out its tools and build "
			"output, and opens the copy with its start scene unless open is false: examples are only ever opened as copies. "
			"Asynchronous: the copy runs in the background. Not an undo step: it writes files. Returns { directory, bytes, files, "
			"warnings } and, when it opened, project and scene as project.open gives them. InvalidParams for an unknown example "
			"or a directory that is not new or empty (absolute, or relative to the open project's directory); InvalidOperation "
			"when the example is not available (the message carries its hint), while playing (with open) or while another example "
			"is copied; UnsavedChanges (with open) when the scene has unsaved changes, before anything is copied, unless "
			"discardChanges is true; FileError when the copy fails (it leaves nothing behind) or does not open (the copy stays); "
			"Cancelled when the editor closes first.";
		copyExample.Parameters =
			SchemaBuilder::Object()
				.Property("example", SchemaBuilder::String("The example's id, as project.examples lists it").MinLength(1), true)
				.Property("directory",
		                  SchemaBuilder::String("Where the copy goes: a new or empty directory, absolute or relative to the open "
		                                        "project's directory")
		                      .MinLength(1),
		                  true)
				.Property("open", SchemaBuilder::Boolean("Open the copy when it is made").Default(true))
				.Property("discardChanges", SchemaBuilder::Boolean("Discard unsaved scene changes (with open)").Default(false))
				.Build();
		copyExample.AsyncHandler = [commands](Json const& params, CommandCompletion completion)
		{
			bool const open = params.value("open", true);
			bool const discardChanges = params.value("discardChanges", false);
			CommandValue<ContentCommands::CopyRequest> request = commands->CheckCopy(params, open, discardChanges);
			if (!request)
			{
				completion.Complete(request.GetError());
				return;
			}
			ContentCommands::CopyRequest checked = request.TakeValue();
			Result<void> started =
				commands->Copies.Start(std::move(checked.Catalog), std::move(checked.Example), std::move(checked.Directory),
			                           [commands, completion, open, discardChanges](Result<ExampleCopyOutcome> outcome, bool cancelled)
			                           {
										   completion.Complete(commands->Finish(std::move(outcome), cancelled, open, discardChanges));
									   });
			if (!started)
			{
				completion.Complete(MakeCommandError(AutomationErrorCode::InvalidOperation, "{}", started.GetError()));
			}
		};
		return result ? registry.Register(std::move(copyExample)) : result;
	}
}
