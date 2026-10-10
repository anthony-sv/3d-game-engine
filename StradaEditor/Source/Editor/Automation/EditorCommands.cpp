#include "Editor/Automation/EditorCommands.h"

#include "Editor/Automation/JsonSchema.h"
#include "Editor/DefaultScene.h"
#include "Editor/EditorCamera.h"
#include "Editor/EntityBounds.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/AssetRegistry.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Core/Base64.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Version.h"
#include "Strada/Project/Project.h"
#include "Strada/Renderer/SceneRenderer.h"
#include "Strada/Scene/ComponentRegistry.h"
#include "Strada/Scene/SceneSerializer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		SchemaBuilder EntityIdSchema(std::string_view description)
		{
			return SchemaBuilder::String(description).MinLength(1);
		}

		SchemaBuilder EntityListSchema(std::string_view description)
		{
			return SchemaBuilder::Array(EntityIdSchema("Entity ID"), description).MinItems(1);
		}

		// Parses an entity ID parameter ("<decimal UUID>").
		CommandValue<UUID> ParseEntityId(Json const& value, std::string_view parameter)
		{
			std::optional<UUID> const id = UUID::FromString(value.get_ref<std::string const&>());
			if (!id || !id->IsValid())
			{
				CommandError error = MakeCommandError(AutomationErrorCode::InvalidParams, "'{}' is not a valid entity ID",
				                                      value.get_ref<std::string const&>());
				error.Data = Json::object({{"path", std::string(parameter)}});
				return error;
			}
			return *id;
		}

		// Parses an entity ID parameter that must name an existing entity.
		CommandValue<UUID> ParseExistingEntity(EditorOperations& operations, Json const& value, std::string_view parameter)
		{
			CommandValue<UUID> id = ParseEntityId(value, parameter);
			if (!id)
			{
				return id;
			}
			if (!operations.FindEntity(id.GetValue()))
			{
				return MakeCommandError(AutomationErrorCode::EntityNotFound, "entity {} does not exist", id.GetValue());
			}
			return id;
		}

		CommandValue<std::vector<UUID>> ParseExistingEntities(EditorOperations& operations, Json const& values, std::string_view parameter)
		{
			std::vector<UUID> ids;
			for (size_t i = 0; i < values.size(); i++)
			{
				CommandValue<UUID> id = ParseExistingEntity(operations, values[i], fmt::format("{}[{}]", parameter, i));
				if (!id)
				{
					return id.TakeError();
				}
				ids.push_back(id.GetValue());
			}
			return ids;
		}

		// A component that users may name in automation (registered and not engine-internal).
		CommandValue<ComponentInfo const*> FindPublicComponent(std::string_view name)
		{
			ComponentInfo const* info = ComponentRegistry::Find(name);
			if (info == nullptr || info->IsInternal())
			{
				return MakeCommandError(AutomationErrorCode::ComponentNotFound, "unknown component '{}' (component.types lists them)",
				                        name);
			}
			return info;
		}

		CommandError Failure(AutomationErrorCode code, Result<void> const& result)
		{
			return CommandError{code, result.GetError(), Json()};
		}

		std::string GetString(Json const& params, char const* key, std::string fallback = {})
		{
			auto const it = params.find(key);
			return it != params.end() ? it->get<std::string>() : std::move(fallback);
		}

		bool GetBool(Json const& params, char const* key, bool fallback)
		{
			auto const it = params.find(key);
			return it != params.end() ? it->get<bool>() : fallback;
		}

		// A scene file parameter: an absolute path ("/..." or "C:/..."), or one relative to the open project's asset
		// directory like every other asset path of the commands. Relative to the editor's working directory, scenes would
		// land wherever the editor happened to start.
		CommandValue<std::filesystem::path> ResolveSceneFile(Project const* project, std::string const& path)
		{
			std::filesystem::path const file = FileSystem::PathFromUtf8(path);
			if (file.has_root_directory())
			{
				return file;
			}
			Result<std::string> normalized = NormalizeAssetPath(path);
			if (normalized && project != nullptr)
			{
				return project->GetAssetDirectory() / FileSystem::PathFromUtf8(normalized.GetValue());
			}
			CommandError error =
				normalized ? MakeCommandError(AutomationErrorCode::InvalidParams,
			                                  "'{}' is relative to the asset directory, but no project is open: use an absolute path", path)
						   : MakeCommandError(AutomationErrorCode::InvalidParams, "{}", normalized.GetError());
			error.Data = Json::object({{"path", "path"}});
			return error;
		}

		Json DescribeEntity(Scene const& scene, Entity entity)
		{
			Json components = Json::array();
			entt::registry const& registry = scene.GetRegistry();
			for (ComponentInfo const& info : ComponentRegistry::GetComponents())
			{
				if (!info.IsInternal() && info.Has(registry, entity.GetHandle()))
				{
					components.push_back(std::string(info.Name));
				}
			}
			return Json::object({{"id", entity.GetUUID().ToString()}, {"name", entity.GetName()}, {"components", std::move(components)}});
		}

		Json DescribeHierarchy(Scene& scene, Entity entity)
		{
			Json node = DescribeEntity(scene, entity);
			Json children = Json::array();
			for (Entity const child : scene.GetChildren(entity))
			{
				children.push_back(DescribeHierarchy(scene, child));
			}
			node["children"] = std::move(children);
			return node;
		}

		Json DescribeScene(EditorContext const& context)
		{
			std::filesystem::path const& path = context.GetScenePath();
			return Json::object({{"name", context.GetScene().GetName()},
			                     {"path", path.empty() ? Json() : Json(FileSystem::PathToUtf8(path))},
			                     {"dirty", context.IsDirty()},
			                     {"entityCount", context.GetScene().GetEntityCount()}});
		}

		Json DescribeProject(EditorContext const& context)
		{
			Project const* project = context.GetProject();
			if (project == nullptr)
			{
				return Json();
			}
			return Json::object({{"name", project->GetSettings().Name},
			                     {"file", FileSystem::PathToUtf8(project->GetFilePath())},
			                     {"assetDirectory", FileSystem::PathToUtf8(project->GetAssetDirectory())}});
		}

		Json DescribeAsset(AssetMetadata const& asset)
		{
			return Json::object({{"id", asset.Handle.ToString()},
			                     {"name", asset.GetDisplayName()},
			                     {"type", AssetTypeToString(asset.Type)},
			                     {"path", asset.IsFileAsset() ? Json(asset.Path) : Json()},
			                     {"reference", AssetManager::GetReference(asset.Handle)},
			                     {"missing", AssetManager::IsMissing(asset.Handle)}});
		}

		Json DescribeAsset(AssetHandle handle)
		{
			std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(handle);
			return metadata ? DescribeAsset(*metadata) : Json();
		}

		Json DescribeAssets(std::vector<AssetHandle> const& handles)
		{
			Json assets = Json::array();
			for (AssetHandle const handle : handles)
			{
				assets.push_back(DescribeAsset(handle));
			}
			return assets;
		}

		// The selected entities, or the asset the inspector shows (null when none or no longer registered).
		Json DescribeSelection(EditorContext const& context)
		{
			Json entities = Json::array();
			for (UUID const id : context.GetSelection().GetEntities())
			{
				entities.push_back(id.ToString());
			}
			UUID const primary = context.GetSelection().GetPrimary();
			AssetHandle const asset = context.GetSelectedAsset();
			return Json::object({{"entities", std::move(entities)},
			                     {"primary", primary.IsValid() ? Json(primary.ToString()) : Json()},
			                     {"asset", asset.IsValid() && AssetManager::IsInitialized() ? DescribeAsset(asset) : Json()}});
		}

		// An asset parameter: a handle (decimal string), "asset://<path>" or "builtin://<name>".
		CommandValue<AssetHandle> ParseAsset(Json const& value, std::string_view parameter)
		{
			std::string const& text = value.get_ref<std::string const&>();
			Result<AssetHandle> handle = AssetManager::ResolveReference(text);
			if (!handle || !AssetManager::IsValid(handle.GetValue()))
			{
				CommandError error =
					MakeCommandError(AutomationErrorCode::AssetNotFound, "{}: no asset '{}' is registered", parameter, text);
				error.Data = Json::object({{"path", std::string(parameter)}});
				return error;
			}
			return handle.GetValue();
		}

		CommandError UnsavedChangesError(EditorContext const& context)
		{
			return MakeCommandError(AutomationErrorCode::UnsavedChanges,
			                        "scene '{}' has unsaved changes; save it first or pass \"discardChanges\": true",
			                        context.GetScene().GetName());
		}

		// Lowercase level names used by the automation API ("trace", "info", "warn", "error", "critical").
		std::string GetLevelName(LogLevel level)
		{
			std::string name = Log::LevelToString(level);
			std::transform(name.begin(), name.end(), name.begin(),
			               [](char character)
			               {
							   return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
						   });
			return name;
		}

		std::optional<LogLevel> ParseLogLevel(std::string_view name)
		{
			for (LogLevel const level : {LogLevel::Trace, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Critical})
			{
				if (name == GetLevelName(level))
				{
					return level;
				}
			}
			return std::nullopt;
		}

		class CommandSet : public std::enable_shared_from_this<CommandSet>
		{
		public:
			CommandSet(CommandRegistry& registry, EditorOperations& operations, EditorCommandEnvironment environment)
				: m_Registry(registry),
				  m_Operations(operations),
				  m_Environment(std::move(environment))
			{
			}

			Result<void> RegisterAll()
			{
				for (auto const registerDomain :
				     {&CommandSet::RegisterEditor, &CommandSet::RegisterProject, &CommandSet::RegisterScene, &CommandSet::RegisterEntities,
				      &CommandSet::RegisterComponents, &CommandSet::RegisterAssets, &CommandSet::RegisterMaterials,
				      &CommandSet::RegisterPrefabs, &CommandSet::RegisterLogAndViewport})
				{
					if (Result<void> result = (this->*registerDomain)(); !result)
					{
						return result;
					}
				}
				return {};
			}

		private:
			Result<void> Add(std::string name, std::string description, Json parameters, bool readOnly, CommandHandler handler)
			{
				CommandDefinition definition;
				definition.Name = std::move(name);
				definition.Description = std::move(description);
				definition.Parameters = std::move(parameters);
				definition.ReadOnly = readOnly;
				definition.Handler = [self = shared_from_this(), handler = std::move(handler)](Json const& params)
				{
					return handler(params);
				};
				return m_Registry.Register(std::move(definition));
			}

			EditorContext& Context() { return m_Operations.GetContext(); }

			// Scene and project files cannot change while the editor plays a copy of the scene.
			std::optional<CommandError> CheckNotPlaying()
			{
				if (Context().IsPlaying())
				{
					return MakeCommandError(AutomationErrorCode::InvalidOperation, "the editor is playing: stop playing first (play.stop)");
				}
				return std::nullopt;
			}

			Result<void> RegisterEditor()
			{
				Result<void> result =
					Add("editor.status",
				        "Editor version, the open project (null when none), the edited scene (name, file, unsaved changes, entity "
				        "count), undo/redo state and the selection.",
				        Json(), true,
				        [this](Json const&) -> CommandResult
				        {
							CommandHistory const& history = Context().GetHistory();
							return Json::object({{"version", EngineVersion::String},
					                             {"project", DescribeProject(Context())},
					                             {"scene", DescribeScene(Context())},
					                             {"undo", history.CanUndo() ? Json(history.GetUndoDescription()) : Json()},
					                             {"redo", history.CanRedo() ? Json(history.GetRedoDescription()) : Json()},
					                             {"selection", DescribeSelection(Context())}});
						});
				result = result ? Add("editor.commands", "Every automation command with its description and JSON-schema parameters.",
				                      Json(), true,
				                      [this](Json const&) -> CommandResult
				                      {
										  return m_Registry.Describe();
									  })
				                : result;
				result = result
				             ? Add("editor.undo", "Undoes the most recent scene modification. Returns the description of the undone step.",
				                   Json(), false,
				                   [this](Json const&) -> CommandResult
				                   {
									   if (!Context().GetHistory().CanUndo())
									   {
										   return MakeCommandError(AutomationErrorCode::InvalidOperation, "there is nothing to undo");
									   }
									   std::string const description = Context().GetHistory().GetUndoDescription();
									   if (Result<void> undone = m_Operations.Undo(); !undone)
									   {
										   return Failure(AutomationErrorCode::InvalidOperation, undone);
									   }
									   return Json::object({{"undone", description}, {"scene", DescribeScene(Context())}});
								   })
				             : result;
				result = result ? Add("editor.redo", "Redoes the most recently undone scene modification.", Json(), false,
				                      [this](Json const&) -> CommandResult
				                      {
										  if (!Context().GetHistory().CanRedo())
										  {
											  return MakeCommandError(AutomationErrorCode::InvalidOperation, "there is nothing to redo");
										  }
										  std::string const description = Context().GetHistory().GetRedoDescription();
										  if (Result<void> redone = m_Operations.Redo(); !redone)
										  {
											  return Failure(AutomationErrorCode::InvalidOperation, redone);
										  }
										  return Json::object({{"redone", description}, {"scene", DescribeScene(Context())}});
									  })
				                : result;
				result = result ? Add("editor.quit",
				                      "Closes the editor. Fails when the scene has unsaved changes unless discardChanges is true.",
				                      SchemaBuilder::Object()
				                          .Property("discardChanges",
				                                    SchemaBuilder::Boolean("Quit even if the scene has unsaved changes").Default(false))
				                          .Build(),
				                      false,
				                      [this](Json const& params) -> CommandResult
				                      {
										  if (!m_Environment.RequestQuit)
										  {
											  return MakeCommandError(AutomationErrorCode::Unavailable, "this editor host cannot quit");
										  }
										  if (Context().IsDirty() && !GetBool(params, "discardChanges", false))
										  {
											  return UnsavedChangesError(Context());
										  }
										  m_Environment.RequestQuit();
										  return Json::object({{"quitting", true}});
									  })
				                : result;
				return result;
			}

			Result<void> RegisterProject()
			{
				Result<void> result =
					Add("project.info", "The open project (name, project file, asset directory; null when none is open) and its settings.",
				        Json(), true,
				        [this](Json const&) -> CommandResult
				        {
							Project const* project = Context().GetProject();
							return Json::object(
								{{"project", DescribeProject(Context())},
					             {"settings",
					              project != nullptr ? SerializeFields<StructTraits<ProjectSettings>>(project->GetSettings()) : Json()}});
						});
				result =
					result
						? Add("project.create",
				              "Creates a project in an empty or new directory (the project file <name>.sproj, an Assets directory and the "
				              "start scene Assets/Scenes/Main.sscene) and opens it with its start scene. Fails on unsaved scene changes "
				              "unless discardChanges is true.",
				              SchemaBuilder::Object()
				                  .Property("directory",
				                            SchemaBuilder::String(
												"Directory of the new project; created when missing, otherwise it must be empty")
				                                .MinLength(1),
				                            true)
				                  .Property("name", SchemaBuilder::String("Project name").MinLength(1), true)
				                  .Property("discardChanges", SchemaBuilder::Boolean("Discard unsaved scene changes").Default(false))
				                  .Build(),
				              false,
				              [this](Json const& params) -> CommandResult
				              {
								  if (std::optional<CommandError> playing = CheckNotPlaying())
								  {
									  return *playing;
								  }
								  if (Context().IsDirty() && !GetBool(params, "discardChanges", false))
								  {
									  return UnsavedChangesError(Context());
								  }
								  Result<void> created =
									  m_Operations.CreateProject(FileSystem::PathFromUtf8(GetString(params, "directory")),
					                                             GetString(params, "name"), *CreateDefaultScene());
								  if (!created)
								  {
									  return Failure(AutomationErrorCode::FileError, created);
								  }
								  NotifyProjectOpened();
								  return Json::object({{"project", DescribeProject(Context())}, {"scene", DescribeScene(Context())}});
							  })
						: result;
				result = result
				             ? Add("project.open",
				                   "Opens a project file (.sproj) with its start scene. Settings unknown to this version are skipped and "
				                   "reported as warnings. Fails on unsaved scene changes unless discardChanges is true.",
				                   SchemaBuilder::Object()
				                       .Property("path", SchemaBuilder::String("Project file path").MinLength(1), true)
				                       .Property("discardChanges", SchemaBuilder::Boolean("Discard unsaved scene changes").Default(false))
				                       .Build(),
				                   false,
				                   [this](Json const& params) -> CommandResult
				                   {
									   if (std::optional<CommandError> playing = CheckNotPlaying())
									   {
										   return *playing;
									   }
									   if (Context().IsDirty() && !GetBool(params, "discardChanges", false))
									   {
										   return UnsavedChangesError(Context());
									   }
									   Result<std::vector<std::string>> opened =
										   m_Operations.OpenProject(FileSystem::PathFromUtf8(GetString(params, "path")));
									   if (!opened)
									   {
										   return CommandError{AutomationErrorCode::FileError, opened.GetError(), Json()};
									   }
									   NotifyProjectOpened();
									   return Json::object({{"project", DescribeProject(Context())},
					                                        {"scene", DescribeScene(Context())},
					                                        {"warnings", opened.GetValue()}});
								   })
				             : result;
				result =
					result
						? Add("project.close",
				              "Closes the project and its assets; the scene is replaced with an empty one. Fails on unsaved scene changes "
				              "unless discardChanges is true.",
				              SchemaBuilder::Object()
				                  .Property("discardChanges", SchemaBuilder::Boolean("Discard unsaved scene changes").Default(false))
				                  .Build(),
				              false,
				              [this](Json const& params) -> CommandResult
				              {
								  if (Context().GetProject() == nullptr)
								  {
									  return MakeCommandError(AutomationErrorCode::InvalidOperation, "no project is open");
								  }
								  if (std::optional<CommandError> playing = CheckNotPlaying())
								  {
									  return *playing;
								  }
								  if (Context().IsDirty() && !GetBool(params, "discardChanges", false))
								  {
									  return UnsavedChangesError(Context());
								  }
								  if (Result<void> closed = m_Operations.CloseProject(); !closed)
								  {
									  return Failure(AutomationErrorCode::InvalidOperation, closed);
								  }
								  return Json::object({{"closed", true}, {"scene", DescribeScene(Context())}});
							  })
						: result;
				result =
					result
						? Add("project.settings",
				              "Returns the open project's settings; optionally applies a partial patch in the project-file format (e.g. "
				              "{ \"Window\": { \"Width\": 1920 }, \"Physics\": { \"Layers\": [\"Default\", \"Player\"] } }) and saves the "
				              "project file. Project settings are not part of the undo history.",
				              SchemaBuilder::Object()
				                  .Property("settings", SchemaBuilder::Object("Partial settings patch").AllowAdditionalProperties())
				                  .Build(),
				              false,
				              [this](Json const& params) -> CommandResult
				              {
								  Project* project = Context().GetProject();
								  if (project == nullptr)
								  {
									  return MakeCommandError(AutomationErrorCode::InvalidOperation, "no project is open");
								  }
								  Json const patch = params.contains("settings") ? params["settings"] : Json::object();
								  if (!patch.empty())
								  {
									  Json const before = SerializeFields<StructTraits<ProjectSettings>>(project->GetSettings());
									  if (Result<void> applied = m_Operations.ApplyProjectSettings(patch); !applied)
									  {
										  return Failure(AutomationErrorCode::InvalidParams, applied);
									  }
									  if (Result<void> saved = m_Operations.SaveProject(); !saved)
									  {
										  // Keep memory and file consistent: the change is undone when it cannot be saved.
										  (void)m_Operations.ApplyProjectSettings(before);
										  return Failure(AutomationErrorCode::FileError, saved);
									  }
								  }
								  return Json::object(
									  {{"settings", SerializeFields<StructTraits<ProjectSettings>>(project->GetSettings())}});
							  })
						: result;
				return result;
			}

			// Asset commands need the AssetManager, and file operations an open project.
			std::optional<CommandError> CheckAssets(bool needsProject) const
			{
				if (!AssetManager::IsInitialized())
				{
					return MakeCommandError(AutomationErrorCode::Unavailable, "assets are not available in this editor host");
				}
				if (needsProject && !AssetManager::HasAssetDirectory())
				{
					return MakeCommandError(AutomationErrorCode::InvalidOperation, "no project is open");
				}
				return std::nullopt;
			}

			Result<void> RegisterAssets()
			{
				SchemaBuilder const assetParameter =
					SchemaBuilder::String("Asset: a handle (decimal string), \"asset://<path in Assets>\" or \"builtin://<name>\"")
						.MinLength(1);

				Result<void> result = Add(
					"asset.list",
					"Lists registered assets ({ id, name, type, path, reference, missing }) and the folders of the asset directory. Paths are "
					"relative to Assets with forward slashes; folder limits the listing to a folder (with its subfolders unless "
					"recursive is false). Built-in assets are included with builtIn.",
					SchemaBuilder::Object()
						.Property("folder", SchemaBuilder::String("Folder relative to Assets (empty for all)").Default(""))
						.Property("type", SchemaBuilder::String("Only assets of this type")
				                              .Enum({"Scene", "Prefab", "Mesh", "Material", "Texture", "Environment", "AudioClip", "Font"}))
						.Property("recursive", SchemaBuilder::Boolean("Include subfolders").Default(true))
						.Property("builtIn", SchemaBuilder::Boolean("Include built-in assets").Default(false))
						.Build(),
					true,
					[this](Json const& params) -> CommandResult
					{
						if (std::optional<CommandError> error = CheckAssets(false))
						{
							return *error;
						}
						std::string const folder = GetString(params, "folder");
						bool const recursive = GetBool(params, "recursive", true);
						std::string const prefix = folder.empty() ? std::string() : folder + "/";
						AssetType const type = params.contains("type")
					                               ? AssetTypeFromString(params["type"].get<std::string>()).value_or(AssetType::None)
					                               : AssetType::None;
						Json assets = Json::array();
						for (AssetMetadata const& asset : AssetManager::GetAssets(type))
						{
							bool listed = false;
							if (asset.IsBuiltIn())
							{
								listed = GetBool(params, "builtIn", false) && folder.empty();
							}
							else if (asset.IsFileAsset())
							{
								std::string_view const path = asset.Path;
								listed = path.starts_with(prefix) && (recursive || path.find('/', prefix.size()) == std::string_view::npos);
							}
							if (listed)
							{
								assets.push_back(DescribeAsset(asset));
							}
						}

						Json folders = Json::array();
						if (AssetManager::HasAssetDirectory())
						{
							std::filesystem::path const root = AssetManager::GetAssetDirectory();
							std::filesystem::path const start = root / FileSystem::PathFromUtf8(folder);
							std::error_code errorCode;
							auto const addFolder = [&](std::filesystem::directory_entry const& entry)
							{
								if (entry.is_directory(errorCode) && !FileSystem::PathToUtf8(entry.path().filename()).starts_with("."))
								{
									folders.push_back(FileSystem::PathToUtf8(FileSystem::GetRelativePath(entry.path(), root)));
								}
							};
							if (recursive)
							{
								for (auto it = std::filesystem::recursive_directory_iterator(start, errorCode);
							         !errorCode && it != std::filesystem::recursive_directory_iterator(); it.increment(errorCode))
								{
									if (FileSystem::PathToUtf8(it->path().filename()).starts_with("."))
									{
										it.disable_recursion_pending();
										continue;
									}
									addFolder(*it);
								}
							}
							else
							{
								for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(start, errorCode))
								{
									addFolder(entry);
								}
							}
						}
						return Json::object({{"assets", std::move(assets)}, {"folders", std::move(folders)}});
					});
				result = result ? Add("asset.get", "Describes one asset ({ id, name, type, path, reference, missing }).",
				                      SchemaBuilder::Object().Property("asset", assetParameter, true).Build(), true,
				                      [this](Json const& params) -> CommandResult
				                      {
										  if (std::optional<CommandError> error = CheckAssets(false))
										  {
											  return *error;
										  }
										  CommandValue<AssetHandle> asset = ParseAsset(params["asset"], "asset");
										  if (!asset)
										  {
											  return asset.TakeError();
										  }
										  return DescribeAsset(asset.GetValue());
									  })
				                : result;
				result = result ? Add("asset.select",
				                      "Shows an asset in the inspector instead of the selected entities (not undoable). Returns the "
				                      "selection.",
				                      SchemaBuilder::Object().Property("asset", assetParameter, true).Build(), false,
				                      [this](Json const& params) -> CommandResult
				                      {
										  if (std::optional<CommandError> error = CheckAssets(false))
										  {
											  return *error;
										  }
										  CommandValue<AssetHandle> asset = ParseAsset(params["asset"], "asset");
										  if (!asset)
										  {
											  return asset.TakeError();
										  }
										  Context().SelectAsset(asset.GetValue());
										  return DescribeSelection(Context());
									  })
				                : result;
				result =
					result
						? Add("asset.import",
				              "Copies files (absolute paths) into a folder of the asset directory and registers them, all or nothing. "
				              "Names that are taken get a number; files already inside the asset directory are registered in place.",
				              SchemaBuilder::Object()
				                  .Property(
									  "files",
									  SchemaBuilder::Array(SchemaBuilder::String("File path").MinLength(1), "Files to import").MinItems(1),
									  true)
				                  .Property("folder", SchemaBuilder::String("Destination folder relative to Assets").Default(""))
				                  .Build(),
				              false,
				              [this](Json const& params) -> CommandResult
				              {
								  if (std::optional<CommandError> error = CheckAssets(true))
								  {
									  return *error;
								  }
								  std::vector<std::filesystem::path> files;
								  for (Json const& file : params["files"])
								  {
									  files.push_back(FileSystem::PathFromUtf8(file.get<std::string>()));
								  }
								  Result<std::vector<AssetHandle>> imported = m_Operations.ImportAssets(files, GetString(params, "folder"));
								  if (!imported)
								  {
									  return CommandError{AutomationErrorCode::FileError, imported.GetError(), Json()};
								  }
								  return Json::object({{"assets", DescribeAssets(imported.GetValue())}});
							  })
						: result;
				result = result
				             ? Add("asset.refresh",
				                   "Rescans the asset directory: registers new files, reports missing ones (they keep their handles) and "
				                   "reloads modified ones.",
				                   Json(), false,
				                   [this](Json const&) -> CommandResult
				                   {
									   if (std::optional<CommandError> error = CheckAssets(true))
									   {
										   return *error;
									   }
									   Result<AssetRefreshResult> refreshed = m_Operations.RefreshAssets();
									   if (!refreshed)
									   {
										   return CommandError{AutomationErrorCode::FileError, refreshed.GetError(), Json()};
									   }
									   AssetRefreshResult const& changes = refreshed.GetValue();
									   return Json::object({{"added", DescribeAssets(changes.Added)},
					                                        {"missing", DescribeAssets(changes.Missing)},
					                                        {"modified", DescribeAssets(changes.Modified)},
					                                        {"warnings", changes.Warnings}});
								   })
				             : result;
				result =
					result
						? Add("asset.move",
				              "Moves or renames a file asset (path relative to Assets). Its handle, and every reference to it, stays valid.",
				              SchemaBuilder::Object()
				                  .Property("asset", assetParameter, true)
				                  .Property("path", SchemaBuilder::String("New path relative to Assets").MinLength(1), true)
				                  .Build(),
				              false,
				              [this](Json const& params) -> CommandResult
				              {
								  if (std::optional<CommandError> error = CheckAssets(true))
								  {
									  return *error;
								  }
								  CommandValue<AssetHandle> asset = ParseAsset(params["asset"], "asset");
								  if (!asset)
								  {
									  return asset.TakeError();
								  }
								  if (Result<void> moved = m_Operations.MoveAsset(asset.GetValue(), GetString(params, "path")); !moved)
								  {
									  return Failure(AutomationErrorCode::FileError, moved);
								  }
								  return DescribeAsset(asset.GetValue());
							  })
						: result;
				result = result ? Add("asset.delete",
				                      "Deletes a file asset from disk and from the registry. References to it in scenes become missing.",
				                      SchemaBuilder::Object().Property("asset", assetParameter, true).Build(), false,
				                      [this](Json const& params) -> CommandResult
				                      {
										  if (std::optional<CommandError> error = CheckAssets(true))
										  {
											  return *error;
										  }
										  CommandValue<AssetHandle> asset = ParseAsset(params["asset"], "asset");
										  if (!asset)
										  {
											  return asset.TakeError();
										  }
										  if (Result<void> deleted = m_Operations.DeleteAsset(asset.GetValue()); !deleted)
										  {
											  return Failure(AutomationErrorCode::FileError, deleted);
										  }
										  return Json::object({{"deleted", asset.GetValue().ToString()}});
									  })
				                : result;
				result = result ? Add("asset.create-folder", "Creates a folder in the asset directory.",
				                      SchemaBuilder::Object()
				                          .Property("folder", SchemaBuilder::String("Folder relative to Assets").MinLength(1), true)
				                          .Build(),
				                      false,
				                      [this](Json const& params) -> CommandResult
				                      {
										  if (std::optional<CommandError> error = CheckAssets(true))
										  {
											  return *error;
										  }
										  if (Result<void> created = m_Operations.CreateAssetFolder(GetString(params, "folder")); !created)
										  {
											  return Failure(AutomationErrorCode::FileError, created);
										  }
										  return Json::object({{"folder", GetString(params, "folder")}});
									  })
				                : result;
				result =
					result ? Add("asset.move-folder",
				                 "Moves or renames a folder of the asset directory with everything inside; asset handles stay valid.",
				                 SchemaBuilder::Object()
				                     .Property("folder", SchemaBuilder::String("Folder relative to Assets").MinLength(1), true)
				                     .Property("newFolder", SchemaBuilder::String("New folder path relative to Assets").MinLength(1), true)
				                     .Build(),
				                 false,
				                 [this](Json const& params) -> CommandResult
				                 {
									 if (std::optional<CommandError> error = CheckAssets(true))
									 {
										 return *error;
									 }
									 if (Result<void> moved =
					                         m_Operations.MoveAssetFolder(GetString(params, "folder"), GetString(params, "newFolder"));
					                     !moved)
									 {
										 return Failure(AutomationErrorCode::FileError, moved);
									 }
									 return Json::object({{"folder", GetString(params, "newFolder")}});
								 })
						   : result;
				result = result ? Add("asset.delete-folder",
				                      "Deletes a folder of the asset directory with every file inside; its assets are unregistered.",
				                      SchemaBuilder::Object()
				                          .Property("folder", SchemaBuilder::String("Folder relative to Assets").MinLength(1), true)
				                          .Build(),
				                      false,
				                      [this](Json const& params) -> CommandResult
				                      {
										  if (std::optional<CommandError> error = CheckAssets(true))
										  {
											  return *error;
										  }
										  if (Result<void> deleted = m_Operations.DeleteAssetFolder(GetString(params, "folder")); !deleted)
										  {
											  return Failure(AutomationErrorCode::FileError, deleted);
										  }
										  return Json::object({{"deleted", GetString(params, "folder")}});
									  })
				                : result;
				return result;
			}

			Result<void> RegisterMaterials()
			{
				auto const describeMaterial = [](AssetHandle handle) -> CommandResult
				{
					Result<Ref<MaterialAsset>> material = AssetManager::TryGetAsset<MaterialAsset>(handle);
					if (!material)
					{
						return CommandError{AutomationErrorCode::FileError, material.GetError(), Json()};
					}
					return Json::object({{"material", DescribeAsset(handle)},
					                     {"fields", SerializeFields<StructTraits<MaterialData>>(material.GetValue()->GetData())}});
				};
				SchemaBuilder const materialParameter =
					SchemaBuilder::String("Material asset: a handle, \"asset://<path>.smat\" or \"builtin://DefaultMaterial\"")
						.MinLength(1);

				Result<void> result = Add(
					"material.create",
					"Creates a material file (.smat, path relative to Assets) with default parameters plus the given fields (component.types "
					"style names: BaseColor, Metallic, Roughness, BaseColorTexture, ...; textures as asset references).",
					SchemaBuilder::Object()
						.Property("path", SchemaBuilder::String("File path relative to Assets, ending in .smat").MinLength(1), true)
						.Property("fields", SchemaBuilder::Object("Initial material fields").AllowAdditionalProperties())
						.Build(),
					false,
					[this, describeMaterial](Json const& params) -> CommandResult
					{
						if (std::optional<CommandError> error = CheckAssets(true))
						{
							return *error;
						}
						Result<AssetHandle> created = m_Operations.CreateMaterial(
							GetString(params, "path"), params.contains("fields") ? params["fields"] : Json::object());
						if (!created)
						{
							return CommandError{AutomationErrorCode::InvalidParams, created.GetError(), Json()};
						}
						return describeMaterial(created.GetValue());
					});
				result = result ? Add("material.get", "A material's parameters (MaterialData fields; textures as handles).",
				                      SchemaBuilder::Object().Property("material", materialParameter, true).Build(), true,
				                      [this, describeMaterial](Json const& params) -> CommandResult
				                      {
										  if (std::optional<CommandError> error = CheckAssets(false))
										  {
											  return *error;
										  }
										  CommandValue<AssetHandle> material = ParseAsset(params["material"], "material");
										  if (!material)
										  {
											  return material.TakeError();
										  }
										  return describeMaterial(material.GetValue());
									  })
				                : result;
				result =
					result
						? Add("material.set",
				              "Applies a partial patch to a material file's parameters and saves it. Undoable (editor.undo), but the scene "
				              "does not count as modified. Built-in and mesh-embedded materials cannot be edited.",
				              SchemaBuilder::Object()
				                  .Property("material", materialParameter, true)
				                  .Property("fields", SchemaBuilder::Object("Partial material patch").AllowAdditionalProperties(), true)
				                  .Build(),
				              false,
				              [this, describeMaterial](Json const& params) -> CommandResult
				              {
								  if (std::optional<CommandError> error = CheckAssets(true))
								  {
									  return *error;
								  }
								  CommandValue<AssetHandle> material = ParseAsset(params["material"], "material");
								  if (!material)
								  {
									  return material.TakeError();
								  }
								  if (Result<void> changed = m_Operations.SetMaterialFields(material.GetValue(), params["fields"]);
					                  !changed)
								  {
									  return Failure(AutomationErrorCode::InvalidParams, changed);
								  }
								  return describeMaterial(material.GetValue());
							  })
						: result;
				return result;
			}

			void NotifyProjectOpened()
			{
				if (m_Environment.ProjectOpened)
				{
					m_Environment.ProjectOpened();
				}
			}

			Result<void> RegisterScene()
			{
				Result<void> result =
					Add("scene.new", "Replaces the edited scene with an empty one. Fails on unsaved changes unless discardChanges is true.",
				        SchemaBuilder::Object()
				            .Property("name", SchemaBuilder::String("Scene name").MinLength(1).Default("Untitled"))
				            .Property("discardChanges", SchemaBuilder::Boolean("Discard unsaved changes").Default(false))
				            .Build(),
				        false,
				        [this](Json const& params) -> CommandResult
				        {
							if (std::optional<CommandError> playing = CheckNotPlaying())
							{
								return *playing;
							}
							if (Context().IsDirty() && !GetBool(params, "discardChanges", false))
							{
								return UnsavedChangesError(Context());
							}
							if (Result<void> created = m_Operations.NewScene(GetString(params, "name", "Untitled")); !created)
							{
								return Failure(AutomationErrorCode::InvalidOperation, created);
							}
							return Json::object({{"scene", DescribeScene(Context())}});
						});
				result =
					result
						? Add("scene.open",
				              "Opens a scene file (.sscene): an absolute path, or one relative to the project's Assets directory. Unknown "
				              "components or fields are skipped and reported as warnings. Fails on unsaved changes unless discardChanges "
				              "is true.",
				              SchemaBuilder::Object()
				                  .Property("path", SchemaBuilder::String("Scene file: absolute, or relative to Assets").MinLength(1), true)
				                  .Property("discardChanges", SchemaBuilder::Boolean("Discard unsaved changes").Default(false))
				                  .Build(),
				              false,
				              [this](Json const& params) -> CommandResult
				              {
								  if (std::optional<CommandError> playing = CheckNotPlaying())
								  {
									  return *playing;
								  }
								  CommandValue<std::filesystem::path> file =
									  ResolveSceneFile(Context().GetProject(), params["path"].get<std::string>());
								  if (!file)
								  {
									  return file.TakeError();
								  }
								  if (Context().IsDirty() && !GetBool(params, "discardChanges", false))
								  {
									  return UnsavedChangesError(Context());
								  }
								  Result<std::vector<std::string>> warnings = m_Operations.OpenScene(file.GetValue());
								  if (!warnings)
								  {
									  return CommandError{AutomationErrorCode::FileError, warnings.GetError(), Json()};
								  }
								  return Json::object({{"scene", DescribeScene(Context())}, {"warnings", warnings.GetValue()}});
							  })
						: result;
				result = result
				             ? Add("scene.save",
				                   "Saves the scene to path (absolute, or relative to the project's Assets directory; scenes saved inside "
				                   "Assets become assets), or to its current file when path is omitted.",
				                   SchemaBuilder::Object()
				                       .Property("path", SchemaBuilder::String("Scene file: absolute, or relative to Assets").MinLength(1))
				                       .Build(),
				                   false,
				                   [this](Json const& params) -> CommandResult
				                   {
									   if (std::optional<CommandError> playing = CheckNotPlaying())
									   {
										   return *playing;
									   }
									   std::filesystem::path path;
									   if (params.contains("path"))
									   {
										   CommandValue<std::filesystem::path> file =
											   ResolveSceneFile(Context().GetProject(), params["path"].get<std::string>());
										   if (!file)
										   {
											   return file.TakeError();
										   }
										   path = file.TakeValue();
									   }
									   if (path.empty() && Context().GetScenePath().empty())
									   {
										   return MakeCommandError(AutomationErrorCode::InvalidOperation,
						                                           "the scene has never been saved, so \"path\" is required");
									   }
									   if (Result<void> saved = m_Operations.SaveScene(path); !saved)
									   {
										   return Failure(AutomationErrorCode::FileError, saved);
									   }
									   return Json::object({{"scene", DescribeScene(Context())}});
								   })
				             : result;
				result = result ? Add("scene.hierarchy", "The entity tree: [{ id, name, components, children }] in hierarchy order.",
				                      Json(), true,
				                      [this](Json const&) -> CommandResult
				                      {
										  Json roots = Json::array();
										  for (Entity const root : Context().GetScene().GetRootEntities())
										  {
											  roots.push_back(DescribeHierarchy(Context().GetScene(), root));
										  }
										  return Json::object({{"scene", DescribeScene(Context())}, {"entities", std::move(roots)}});
									  })
				                : result;
				result = result ? Add("scene.dump", "The complete scene in the scene-file format.", Json(), true,
				                      [this](Json const&) -> CommandResult
				                      {
										  return SceneSerializer::Serialize(Context().GetScene());
									  })
				                : result;
				result =
					result ? Add("scene.settings",
				                 "Returns the scene name and settings; optionally renames the scene and/or applies a partial patch to the "
				                 "settings (scene-file format, e.g. { \"Physics\": { \"Gravity\": [0, -9.81, 0] } }).",
				                 SchemaBuilder::Object()
				                     .Property("name", SchemaBuilder::String("New scene name").MinLength(1))
				                     .Property("settings", SchemaBuilder::Object("Partial settings patch").AllowAdditionalProperties())
				                     .Build(),
				                 false,
				                 [this](Json const& params) -> CommandResult
				                 {
									 std::optional<std::string> name;
									 if (params.contains("name"))
									 {
										 name = params["name"].get<std::string>();
									 }
									 Json const patch = params.contains("settings") ? params["settings"] : Json::object();
									 if (name || !patch.empty())
									 {
										 if (Result<void> changed = m_Operations.SetSceneProperties(name, patch); !changed)
										 {
											 return Failure(AutomationErrorCode::InvalidParams, changed);
										 }
									 }
									 return Json::object(
										 {{"name", Context().GetScene().GetName()},
					                      {"settings", SceneSerializer::SerializeSettings(Context().GetScene().GetSettings())}});
								 })
						   : result;
				return result;
			}

			Result<void> RegisterEntities()
			{
				Result<void> result =
					Add("entity.create",
				        "Creates an entity, optionally under a parent and with components in the scene-file format ({ \"PointLight\": { "
				        "\"Range\": 5 }, \"Transform\": { \"Translation\": [0, 1, 0] } }). Asset fields accept \"builtin://Cube\" and "
				        "\"asset://<path>\". Returns the new entity ID.",
				        SchemaBuilder::Object()
				            .Property("name", SchemaBuilder::String("Entity name").MinLength(1).Default("Entity"))
				            .Property("parent", EntityIdSchema("Parent entity ID (omit for a root entity)"))
				            .Property("siblingIndex", SchemaBuilder::Integer("Position among the siblings (default: last)").Minimum(0))
				            .Property("components", SchemaBuilder::Object("Components to add: { \"<Component>\": { \"<Field>\": value } }")
				                                        .AdditionalProperties(SchemaBuilder::Object().AllowAdditionalProperties()))
				            .Build(),
				        false,
				        [this](Json const& params) -> CommandResult
				        {
							EntityCreateInfo info;
							info.Name = GetString(params, "name", "Entity");
							if (params.contains("parent"))
							{
								CommandValue<UUID> parent = ParseExistingEntity(m_Operations, params["parent"], "parent");
								if (!parent)
								{
									return parent.TakeError();
								}
								info.Parent = parent.GetValue();
							}
							if (params.contains("siblingIndex"))
							{
								info.SiblingIndex = params["siblingIndex"].get<size_t>();
							}
							if (params.contains("components"))
							{
								for (auto const& [component, fields] : params["components"].items())
								{
									if (CommandValue<ComponentInfo const*> known = FindPublicComponent(component); !known)
									{
										return known.TakeError();
									}
								}
								info.Components = params["components"];
							}
							Result<UUID> id = m_Operations.CreateEntity(std::move(info));
							if (!id)
							{
								return CommandError{AutomationErrorCode::InvalidParams, id.GetError(), Json()};
							}
							return Json::object({{"id", id.GetValue().ToString()}});
						});
				result =
					result
						? Add("entity.delete", "Deletes entities and their descendants (one undo step).",
				              SchemaBuilder::Object().Property("entities", EntityListSchema("Entities to delete"), true).Build(), false,
				              [this](Json const& params) -> CommandResult
				              {
								  CommandValue<std::vector<UUID>> ids = ParseExistingEntities(m_Operations, params["entities"], "entities");
								  if (!ids)
								  {
									  return ids.TakeError();
								  }
								  if (Result<void> deleted = m_Operations.DeleteEntities(ids.GetValue()); !deleted)
								  {
									  return Failure(AutomationErrorCode::InvalidOperation, deleted);
								  }
								  return Json::object({{"deleted", params["entities"]}});
							  })
						: result;
				result =
					result
						? Add("entity.duplicate",
				              "Duplicates entities with their descendants; each copy is placed after its original. Returns the copies' IDs.",
				              SchemaBuilder::Object().Property("entities", EntityListSchema("Entities to duplicate"), true).Build(), false,
				              [this](Json const& params) -> CommandResult
				              {
								  CommandValue<std::vector<UUID>> ids = ParseExistingEntities(m_Operations, params["entities"], "entities");
								  if (!ids)
								  {
									  return ids.TakeError();
								  }
								  Result<std::vector<UUID>> copies = m_Operations.DuplicateEntities(ids.GetValue());
								  if (!copies)
								  {
									  return CommandError{AutomationErrorCode::InvalidOperation, copies.GetError(), Json()};
								  }
								  Json copyIDs = Json::array();
								  for (UUID const copy : copies.GetValue())
								  {
									  copyIDs.push_back(copy.ToString());
								  }
								  return Json::object({{"ids", std::move(copyIDs)}});
							  })
						: result;
				result =
					result ? Add("entity.rename", "Renames an entity.",
				                 SchemaBuilder::Object()
				                     .Property("entity", EntityIdSchema("Entity ID"), true)
				                     .Property("name", SchemaBuilder::String("New name").MinLength(1), true)
				                     .Build(),
				                 false,
				                 [this](Json const& params) -> CommandResult
				                 {
									 CommandValue<UUID> id = ParseExistingEntity(m_Operations, params["entity"], "entity");
									 if (!id)
									 {
										 return id.TakeError();
									 }
									 if (Result<void> renamed = m_Operations.RenameEntity(id.GetValue(), params["name"].get<std::string>());
					                     !renamed)
									 {
										 return Failure(AutomationErrorCode::InvalidParams, renamed);
									 }
									 return Json::object({{"id", id.GetValue().ToString()}, {"name", params["name"]}});
								 })
						   : result;
				result =
					result
						? Add("entity.reparent",
				              "Moves an entity under a new parent (null or omitted: a root entity), keeping its world transform by default.",
				              SchemaBuilder::Object()
				                  .Property("entity", EntityIdSchema("Entity ID"), true)
				                  .Property("parent", EntityIdSchema("New parent ID (null or omitted for a root entity)").Nullable())
				                  .Property("siblingIndex",
				                            SchemaBuilder::Integer("Position among the new siblings (default: last)").Minimum(0))
				                  .Property("keepWorldTransform", SchemaBuilder::Boolean("Keep the world-space placement").Default(true))
				                  .Build(),
				              false,
				              [this](Json const& params) -> CommandResult
				              {
								  CommandValue<UUID> id = ParseExistingEntity(m_Operations, params["entity"], "entity");
								  if (!id)
								  {
									  return id.TakeError();
								  }
								  UUID parent = UUID::Invalid();
								  if (params.contains("parent") && !params["parent"].is_null())
								  {
									  CommandValue<UUID> parsed = ParseExistingEntity(m_Operations, params["parent"], "parent");
									  if (!parsed)
									  {
										  return parsed.TakeError();
									  }
									  parent = parsed.GetValue();
								  }
								  std::optional<size_t> siblingIndex;
								  if (params.contains("siblingIndex"))
								  {
									  siblingIndex = params["siblingIndex"].get<size_t>();
								  }
								  if (Result<void> moved = m_Operations.ReparentEntity(id.GetValue(), parent, siblingIndex,
					                                                                   GetBool(params, "keepWorldTransform", true));
					                  !moved)
								  {
									  return Failure(AutomationErrorCode::InvalidOperation, moved);
								  }
								  return Json::object(
									  {{"id", id.GetValue().ToString()}, {"parent", parent.IsValid() ? Json(parent.ToString()) : Json()}});
							  })
						: result;
				result =
					result
						? Add("entity.find",
				              "Finds entities whose name matches exactly and/or that have a component. Returns [{ id, name, components }] in "
				              "hierarchy order.",
				              SchemaBuilder::Object()
				                  .Property("name", SchemaBuilder::String("Exact entity name"))
				                  .Property("component", SchemaBuilder::String("Component the entities must have"))
				                  .Build(),
				              true,
				              [this](Json const& params) -> CommandResult
				              {
								  ComponentInfo const* component = nullptr;
								  if (params.contains("component"))
								  {
									  CommandValue<ComponentInfo const*> info = FindPublicComponent(params["component"].get<std::string>());
									  if (!info)
									  {
										  return info.TakeError();
									  }
									  component = info.GetValue();
								  }
								  std::optional<std::string> const name =
									  params.contains("name") ? std::optional<std::string>(params["name"].get<std::string>())
															  : std::nullopt;
								  Scene& scene = Context().GetScene();
								  Json matches = Json::array();
								  scene.ForEachEntityInHierarchyOrder(
									  [&](Entity entity)
									  {
										  if ((name && entity.GetName() != *name) ||
						                      (component != nullptr && !component->Has(scene.GetRegistry(), entity.GetHandle())))
										  {
											  return;
										  }
										  matches.push_back(DescribeEntity(scene, entity));
									  });
								  return Json::object({{"entities", std::move(matches)}});
							  })
						: result;
				result = result
				             ? Add("entity.get", "An entity's name, parent, children and every component with all fields.",
				                   SchemaBuilder::Object().Property("entity", EntityIdSchema("Entity ID"), true).Build(), true,
				                   [this](Json const& params) -> CommandResult
				                   {
									   CommandValue<UUID> id = ParseExistingEntity(m_Operations, params["entity"], "entity");
									   if (!id)
									   {
										   return id.TakeError();
									   }
									   Scene& scene = Context().GetScene();
									   Entity const entity = m_Operations.FindEntity(id.GetValue());
									   Json components = Json::object();
									   for (ComponentInfo const& info : ComponentRegistry::GetComponents())
									   {
										   if (!info.IsInternal() && info.Has(scene.GetRegistry(), entity.GetHandle()))
										   {
											   components[std::string(info.Name)] = info.Serialize(scene.GetRegistry(), entity.GetHandle());
										   }
									   }
									   Json children = Json::array();
									   for (Entity const child : scene.GetChildren(entity))
									   {
										   children.push_back(child.GetUUID().ToString());
									   }
									   Entity const parent = scene.GetParent(entity);
									   return Json::object({{"id", id.GetValue().ToString()},
					                                        {"name", entity.GetName()},
					                                        {"parent", parent ? Json(parent.GetUUID().ToString()) : Json()},
					                                        {"children", std::move(children)},
					                                        {"components", std::move(components)}});
								   })
				             : result;
				result = result
				             ? Add("entity.select", "Changes the selection (not undoable). Returns the selection.",
				                   SchemaBuilder::Object()
				                       .Property("entities", SchemaBuilder::Array(EntityIdSchema("Entity ID"), "Entities"), true)
				                       .Property("mode", SchemaBuilder::String("How to combine with the current selection")
				                                             .Enum({"replace", "add", "remove", "toggle"})
				                                             .Default("replace"))
				                       .Property("primary", EntityIdSchema("Entity shown in the inspector (must end up selected)"))
				                       .Build(),
				                   false,
				                   [this](Json const& params) -> CommandResult
				                   {
									   CommandValue<std::vector<UUID>> ids =
										   ParseExistingEntities(m_Operations, params["entities"], "entities");
									   if (!ids)
									   {
										   return ids.TakeError();
									   }
									   std::string const mode = GetString(params, "mode", "replace");
									   SelectionMode const selectionMode = mode == "add"      ? SelectionMode::Add
					                                                       : mode == "remove" ? SelectionMode::Remove
					                                                       : mode == "toggle" ? SelectionMode::Toggle
					                                                                          : SelectionMode::Replace;
									   UUID primary = UUID::Invalid();
									   if (params.contains("primary"))
									   {
										   CommandValue<UUID> parsed = ParseExistingEntity(m_Operations, params["primary"], "primary");
										   if (!parsed)
										   {
											   return parsed.TakeError();
										   }
										   primary = parsed.GetValue();
									   }
									   if (Result<void> selected = m_Operations.Select(ids.GetValue(), selectionMode, primary); !selected)
									   {
										   return Failure(AutomationErrorCode::InvalidOperation, selected);
									   }
									   return DescribeSelection(Context());
								   })
				             : result;
				return result;
			}

			Result<void> RegisterComponents()
			{
				Result<void> result =
					Add("component.types",
				        "Every component type users can add, with its fields (name, type, default value, valid range, value names, "
				        "referenced asset type and description).",
				        Json(), true,
				        [](Json const&) -> CommandResult
				        {
							Json types = Json::array();
							for (ComponentInfo const& info : ComponentRegistry::GetComponents())
							{
								if (!info.IsInternal())
								{
									Json description = info.Describe();
									description["Core"] = info.IsCore();
									types.push_back(std::move(description));
								}
							}
							return Json::object({{"components", std::move(types)}});
						});

				auto const entityAndComponent = [](std::string_view componentDescription)
				{
					return SchemaBuilder::Object()
					    .Property("entity", EntityIdSchema("Entity ID"), true)
					    .Property("component", SchemaBuilder::String(componentDescription).MinLength(1), true);
				};

				result =
					result ? Add("component.add",
				                 "Adds a component to an entity, optionally with initial field values. Fails if the entity already has it.",
				                 entityAndComponent("Component type (see component.types)")
				                     .Property("fields", SchemaBuilder::Object("Initial field values").AllowAdditionalProperties())
				                     .Build(),
				                 false,
				                 [this](Json const& params) -> CommandResult
				                 {
									 CommandValue<std::pair<UUID, ComponentInfo const*>> target = ParseTarget(params);
									 if (!target)
									 {
										 return target.TakeError();
									 }
									 auto const [id, info] = target.GetValue();
									 if (info->Has(Context().GetScene().GetRegistry(), m_Operations.FindEntity(id).GetHandle()))
									 {
										 return MakeCommandError(AutomationErrorCode::InvalidOperation,
						                                         "entity {} already has a {} component", id, info->Name);
									 }
									 Json const fields = params.contains("fields") ? params["fields"] : Json::object();
									 if (Result<void> added = m_Operations.AddComponent(id, info->Name, fields); !added)
									 {
										 return Failure(AutomationErrorCode::InvalidParams, added);
									 }
									 return ComponentFields(id, *info);
								 })
						   : result;
				result = result ? Add("component.remove", "Removes a component from an entity (core components cannot be removed).",
				                      entityAndComponent("Component type").Build(), false,
				                      [this](Json const& params) -> CommandResult
				                      {
										  CommandValue<std::pair<UUID, ComponentInfo const*>> target = ParseTarget(params);
										  if (!target)
										  {
											  return target.TakeError();
										  }
										  auto const [id, info] = target.GetValue();
										  if (!info->Has(Context().GetScene().GetRegistry(), m_Operations.FindEntity(id).GetHandle()))
										  {
											  return MakeCommandError(AutomationErrorCode::ComponentNotFound,
						                                              "entity {} has no {} component", id, info->Name);
										  }
										  if (info->IsCore())
										  {
											  return MakeCommandError(AutomationErrorCode::InvalidOperation,
						                                              "{} is a core component and cannot be removed", info->Name);
										  }
										  if (Result<void> removed = m_Operations.RemoveComponent(id, info->Name); !removed)
										  {
											  return Failure(AutomationErrorCode::InvalidOperation, removed);
										  }
										  return Json::object({{"removed", std::string(info->Name)}});
									  })
				                : result;
				result = result ? Add("component.get", "All fields of an entity's component.", entityAndComponent("Component type").Build(),
				                      true,
				                      [this](Json const& params) -> CommandResult
				                      {
										  CommandValue<std::pair<UUID, ComponentInfo const*>> target = ParseTarget(params);
										  if (!target)
										  {
											  return target.TakeError();
										  }
										  auto const [id, info] = target.GetValue();
										  if (!info->Has(Context().GetScene().GetRegistry(), m_Operations.FindEntity(id).GetHandle()))
										  {
											  return MakeCommandError(AutomationErrorCode::ComponentNotFound,
						                                              "entity {} has no {} component", id, info->Name);
										  }
										  return ComponentFields(id, *info);
									  })
				                : result;
				result =
					result
						? Add("component.set",
				              "Applies a partial patch to an entity's component (only the given fields change; invalid values change nothing). "
				              "Transform also accepts \"RotationEuler\": [x, y, z] in degrees. Returns all fields.",
				              entityAndComponent("Component type")
				                  .Property("fields", SchemaBuilder::Object("Fields to change").AllowAdditionalProperties(), true)
				                  .Build(),
				              false,
				              [this](Json const& params) -> CommandResult
				              {
								  CommandValue<std::pair<UUID, ComponentInfo const*>> target = ParseTarget(params);
								  if (!target)
								  {
									  return target.TakeError();
								  }
								  auto const [id, info] = target.GetValue();
								  if (!info->Has(Context().GetScene().GetRegistry(), m_Operations.FindEntity(id).GetHandle()))
								  {
									  return MakeCommandError(AutomationErrorCode::ComponentNotFound, "entity {} has no {} component", id,
						                                      info->Name);
								  }
								  if (Result<void> set = m_Operations.SetComponentFields(id, info->Name, params["fields"]); !set)
								  {
									  return Failure(AutomationErrorCode::InvalidParams, set);
								  }
								  return ComponentFields(id, *info);
							  })
						: result;
				return result;
			}

			Result<void> RegisterPrefabs()
			{
				Result<void> result = Add(
					"prefab.create",
					"Writes an entity with its descendants as a prefab file (.sprefab, path relative to Assets) and registers it. The scene "
					"does not change. Returns the prefab asset ({ id, name, type, path, reference, missing }).",
					SchemaBuilder::Object()
						.Property("entity", EntityIdSchema("Root entity of the prefab"), true)
						.Property("path", SchemaBuilder::String("File path relative to Assets, ending in .sprefab").MinLength(1), true)
						.Build(),
					false,
					[this](Json const& params) -> CommandResult
					{
						if (std::optional<CommandError> error = CheckAssets(true))
						{
							return *error;
						}
						CommandValue<UUID> entity = ParseExistingEntity(m_Operations, params["entity"], "entity");
						if (!entity)
						{
							return entity.TakeError();
						}
						Result<AssetHandle> created = m_Operations.CreatePrefab(entity.GetValue(), GetString(params, "path"));
						if (!created)
						{
							return CommandError{AutomationErrorCode::InvalidParams, created.GetError(), Json()};
						}
						return DescribeAsset(created.GetValue());
					});
				return result
				           ? Add("prefab.instantiate",
				                 "Creates an instance of a prefab with new entity IDs, under a parent or as a root entity; with a position "
				                 "its root moves there (world space), keeping its rotation and scale. One undo step. Returns the root's ID.",
				                 SchemaBuilder::Object()
				                     .Property(
										 "prefab",
										 SchemaBuilder::String("Prefab asset: a handle (decimal string) or \"asset://<path in Assets>\"")
											 .MinLength(1),
										 true)
				                     .Property("parent", EntityIdSchema("Parent entity ID (omit for a root entity)"))
				                     .Property("position",
				                               SchemaBuilder::Array(SchemaBuilder::Number(), "World position [x, y, z] of the root")
				                                   .MinItems(3)
				                                   .MaxItems(3))
				                     .Build(),
				                 false,
				                 [this](Json const& params) -> CommandResult
				                 {
									 if (std::optional<CommandError> error = CheckAssets(true))
									 {
										 return *error;
									 }
									 CommandValue<AssetHandle> prefab = ParseAsset(params["prefab"], "prefab");
									 if (!prefab)
									 {
										 return prefab.TakeError();
									 }
									 UUID parent = UUID::Invalid();
									 if (params.contains("parent"))
									 {
										 CommandValue<UUID> found = ParseExistingEntity(m_Operations, params["parent"], "parent");
										 if (!found)
										 {
											 return found.TakeError();
										 }
										 parent = found.GetValue();
									 }
									 std::optional<glm::vec3> position;
									 if (params.contains("position"))
									 {
										 Json const& value = params["position"];
										 position = glm::vec3(value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
										 if (!std::isfinite(position->x) || !std::isfinite(position->y) || !std::isfinite(position->z))
										 {
											 return MakeCommandError(AutomationErrorCode::InvalidParams,
							                                         "position: the coordinates must be finite");
										 }
									 }
									 Result<UUID> root = m_Operations.InstantiatePrefab(prefab.GetValue(), parent, position);
									 if (!root)
									 {
										 return CommandError{AutomationErrorCode::InvalidOperation, root.GetError(), Json()};
									 }
									 return Json::object({{"id", root.GetValue().ToString()}});
								 })
				           : result;
			}

			Result<void> RegisterLogAndViewport()
			{
				Result<void> result = Add(
					"log.read",
					"Log entries with index >= since (oldest first), at or above minLevel. Pass the returned \"next\" as since to read "
					"only new entries.",
					SchemaBuilder::Object()
						.Property("since", SchemaBuilder::Integer("First entry index").Minimum(0).Default(0))
						.Property("maxCount", SchemaBuilder::Integer("Maximum entries to return").Minimum(1).Maximum(10000).Default(500))
						.Property(
							"minLevel",
							SchemaBuilder::String("Lowest severity").Enum({"trace", "info", "warn", "error", "critical"}).Default("trace"))
						.Build(),
					true,
					[](Json const& params) -> CommandResult
					{
						uint64_t const since = params.contains("since") ? params["since"].get<uint64_t>() : 0;
						size_t const maxCount = params.contains("maxCount") ? params["maxCount"].get<size_t>() : 500;
						LogLevel const minLevel = ParseLogLevel(GetString(params, "minLevel", "trace")).value_or(LogLevel::Trace);
						Json entries = Json::array();
						uint64_t next = since;
						// Filtering by level may skip entries: keep reading until maxCount matches or the buffer is exhausted.
						while (entries.size() < maxCount)
						{
							std::vector<LogEntry> const batch = Log::GetEntries(next, maxCount);
							if (batch.empty())
							{
								break;
							}
							for (LogEntry const& entry : batch)
							{
								next = entry.Index + 1;
								if (entry.Severity >= minLevel)
								{
									entries.push_back(Json::object({{"index", entry.Index},
								                                    {"level", GetLevelName(entry.Severity)},
								                                    {"logger", entry.Logger},
								                                    {"message", entry.Message},
								                                    {"timestampMs", entry.TimestampMs}}));
									if (entries.size() == maxCount)
									{
										break;
									}
								}
							}
						}
						return Json::object(
							{{"entries", std::move(entries)}, {"next", std::max(next, std::min(since, Log::GetNextEntryIndex()))}});
					});
				if (!result)
				{
					return result;
				}

				CommandDefinition screenshot;
				screenshot.Name = "viewport.screenshot";
				screenshot.Description = "Captures the editor window as a PNG: { mimeType, width, height, data (base64) }.";
				screenshot.ReadOnly = true;
				screenshot.AsyncHandler = [this, self = shared_from_this()](Json const&, CommandCompletion completion)
				{
					if (!m_Environment.CaptureScreenshot)
					{
						completion.Complete(MakeCommandError(AutomationErrorCode::Unavailable,
						                                     "screenshots need an editor window (not available headless)"));
						return;
					}
					m_Environment.CaptureScreenshot(
						[completion](Result<Image> image)
						{
							if (!image)
							{
								completion.Complete(
									MakeCommandError(AutomationErrorCode::Unavailable, "screenshot failed: {}", image.GetError()));
								return;
							}
							Result<std::vector<uint8_t>> png = image.GetValue().EncodePNG();
							if (!png)
							{
								completion.Complete(MakeCommandError(AutomationErrorCode::InternalError, "{}", png.GetError()));
								return;
							}
							completion.Complete(Json::object({{"mimeType", "image/png"},
						                                      {"width", image.GetValue().GetWidth()},
						                                      {"height", image.GetValue().GetHeight()},
						                                      {"data", Base64Encode(png.GetValue())}}));
						});
				};
				result = m_Registry.Register(std::move(screenshot));

				result =
					result ? Add("viewport.camera",
				                 "The editor viewport's camera, which orbits a focal point: sets the given values (the others stay) and "
				                 "returns the view: { focalPoint, distance, yaw, pitch (degrees), position, forward }. Not undoable; "
				                 "Unavailable without a viewport (headless).",
				                 SchemaBuilder::Object()
				                     .Property("focalPoint",
				                               SchemaBuilder::Array(SchemaBuilder::Number(), "The point the camera orbits [x, y, z]")
				                                   .MinItems(3)
				                                   .MaxItems(3))
				                     .Property("distance", SchemaBuilder::Number("Distance from the focal point")
				                                               .Minimum(EditorCamera::MinimumDistance)
				                                               .Maximum(EditorCamera::MaximumDistance))
				                     .Property("yaw", SchemaBuilder::Number("Turn about the vertical axis in degrees (0 looks down -Z)"))
				                     .Property("pitch", SchemaBuilder::Number("Look up (positive) or down in degrees")
				                                            .Minimum(-EditorCamera::PitchLimit)
				                                            .Maximum(EditorCamera::PitchLimit))
				                     .Build(),
				                 false,
				                 [this](Json const& params) -> CommandResult
				                 {
									 if (!m_Environment.ViewportCamera)
									 {
										 return MakeCommandError(AutomationErrorCode::Unavailable, "the editor has no viewport (headless)");
									 }
									 EditorCamera& camera = m_Environment.ViewportCamera();
									 glm::vec3 focalPoint = camera.GetFocalPoint();
									 if (params.contains("focalPoint"))
									 {
										 Json const& value = params["focalPoint"];
										 focalPoint = glm::vec3(value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
									 }
									 float const distance =
										 params.contains("distance") ? params["distance"].get<float>() : camera.GetDistance();
									 float const yaw = params.contains("yaw") ? params["yaw"].get<float>() : camera.GetYaw();
									 float const pitch = params.contains("pitch") ? params["pitch"].get<float>() : camera.GetPitch();
									 if (!std::isfinite(focalPoint.x) || !std::isfinite(focalPoint.y) || !std::isfinite(focalPoint.z) ||
					                     !std::isfinite(yaw))
									 {
										 return MakeCommandError(AutomationErrorCode::InvalidParams, "the view must be finite");
									 }
									 camera.SetView(focalPoint, distance, yaw, pitch);
									 return DescribeCamera(camera);
								 })
						   : result;
				result =
					result
						? Add("viewport.statistics",
				              "What the viewport's renderer drew in the last frame it rendered (the editor view, or the game while "
				              "playing): gpuMilliseconds (the GPU time of its latest finished frame; null before one finished or when the "
				              "GPU cannot measure time), drawCalls (shadow-map draws included), triangles, shadowDrawCalls, shadowMapViews, "
				              "shadowsDropped (shadow-casting lights beyond the shadow-map budget), lights, culledLights (point and spot "
				              "lights out of the view's reach), culled (submeshes outside the view), quads (sprites and text glyphs). "
				              "Unavailable without a viewport (headless) or a GPU.",
				              Json(), true,
				              [this](Json const&) -> CommandResult
				              {
								  if (!m_Environment.ViewportStatistics)
								  {
									  return MakeCommandError(AutomationErrorCode::Unavailable, "the editor has no viewport (headless)");
								  }
								  SceneRendererStatistics const* statistics = m_Environment.ViewportStatistics();
								  if (statistics == nullptr)
								  {
									  return MakeCommandError(AutomationErrorCode::Unavailable, "the viewport has no renderer (no GPU)");
								  }
								  return Json::object(
									  {{"gpuMilliseconds", statistics->GpuMilliseconds ? Json(*statistics->GpuMilliseconds) : Json()},
					                   {"drawCalls", statistics->DrawCalls},
					                   {"triangles", statistics->Triangles},
					                   {"shadowDrawCalls", statistics->ShadowDrawCalls},
					                   {"shadowMapViews", statistics->ShadowMapViews},
					                   {"shadowsDropped", statistics->ShadowsDropped},
					                   {"lights", statistics->Lights},
					                   {"culledLights", statistics->CulledLights},
					                   {"culled", statistics->Culled},
					                   {"quads", statistics->Quads}});
							  })
						: result;
				return result
				           ? Add("viewport.frame",
				                 "Points the editor viewport's camera at entities, framing their meshes (their origins without one) from the "
				                 "current direction, as Focus in the hierarchy does. Returns the view like viewport.camera. Not undoable; "
				                 "Unavailable without a viewport (headless).",
				                 SchemaBuilder::Object()
				                     .Property("entities",
				                               SchemaBuilder::Array(EntityIdSchema("Entity ID"), "Entities to frame").MinItems(1), true)
				                     .Build(),
				                 false,
				                 [this](Json const& params) -> CommandResult
				                 {
									 if (!m_Environment.ViewportCamera)
									 {
										 return MakeCommandError(AutomationErrorCode::Unavailable, "the editor has no viewport (headless)");
									 }
									 CommandValue<std::vector<UUID>> ids =
										 ParseExistingEntities(m_Operations, params["entities"], "entities");
									 if (!ids)
									 {
										 return ids.TakeError();
									 }
									 EditorCamera& camera = m_Environment.ViewportCamera();
									 camera.Focus(ComputeEntityBounds(Context().GetScene(), ids.GetValue()));
									 return DescribeCamera(camera);
								 })
				           : result;
			}

			static Json DescribeCamera(EditorCamera const& camera)
			{
				auto const toArray = [](glm::vec3 const& value)
				{
					return Json::array({value.x, value.y, value.z});
				};
				return Json::object({{"focalPoint", toArray(camera.GetFocalPoint())},
				                     {"distance", camera.GetDistance()},
				                     {"yaw", camera.GetYaw()},
				                     {"pitch", camera.GetPitch()},
				                     {"position", toArray(camera.GetPosition())},
				                     {"forward", toArray(camera.GetForward())}});
			}

			CommandValue<std::pair<UUID, ComponentInfo const*>> ParseTarget(Json const& params)
			{
				CommandValue<UUID> id = ParseExistingEntity(m_Operations, params["entity"], "entity");
				if (!id)
				{
					return id.TakeError();
				}
				CommandValue<ComponentInfo const*> info = FindPublicComponent(params["component"].get<std::string>());
				if (!info)
				{
					return info.TakeError();
				}
				return std::pair<UUID, ComponentInfo const*>(id.GetValue(), info.GetValue());
			}

			Json ComponentFields(UUID id, ComponentInfo const& info)
			{
				Scene& scene = Context().GetScene();
				return Json::object({{"component", std::string(info.Name)},
				                     {"fields", info.Serialize(scene.GetRegistry(), m_Operations.FindEntity(id).GetHandle())}});
			}

			CommandRegistry& m_Registry;
			EditorOperations& m_Operations;
			EditorCommandEnvironment m_Environment;
		};
	}

	Result<void> RegisterEditorCommands(CommandRegistry& registry, EditorOperations& operations, EditorCommandEnvironment environment)
	{
		return CreateRef<CommandSet>(registry, operations, std::move(environment))->RegisterAll();
	}
}
