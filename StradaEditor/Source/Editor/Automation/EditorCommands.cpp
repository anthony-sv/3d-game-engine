#include "Editor/Automation/EditorCommands.h"

#include "Editor/Automation/JsonSchema.h"
#include "Editor/ComponentInspection.h"
#include "Editor/DefaultScene.h"
#include "Editor/EditorCamera.h"
#include "Editor/EntityBounds.h"
#include "Editor/EntityPresets.h"
#include "Editor/Panels/ContentBrowserPanel.h"
#include "Editor/Panels/VisualScriptEditorPanel.h"
#include "Editor/ProjectImport.h"
#include "Editor/Viewport/ViewportOverlays.h"
#include "Editor/VisualScripting/VisualScriptArrange.h"
#include "Editor/VisualScripting/VisualScriptBatch.h"
#include "Editor/VisualScripting/VisualScriptClipboard.h"
#include "Editor/VisualScripting/VisualScriptCommit.h"
#include "Editor/VisualScripting/VisualScriptCrossGraph.h"
#include "Editor/VisualScripting/VisualScriptEditing.h"
#include "Editor/VisualScripting/VisualScriptEditors.h"
#include "Editor/VisualScripting/VisualScriptFind.h"
#include "Editor/VisualScripting/VisualScriptHeadlessCanvas.h"
#include "Editor/VisualScripting/VisualScriptNormalization.h"
#include "Editor/VisualScripting/VisualScriptPalette.h"
#include "Editor/VisualScripting/VisualScriptPalettePopup.h"
#include "Editor/VisualScripting/VisualScriptPanes.h"
#include "Editor/VisualScripting/VisualScriptSemantics.h"
#include "Editor/VisualScripting/VisualScriptView.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/AssetRegistry.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptReferences.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/Base64.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Hash.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Version.h"
#include "Strada/Project/Project.h"
#include "Strada/Project/ScriptProject.h"
#include "Strada/Renderer/SceneRenderer.h"
#include "Strada/Scene/ComponentRegistry.h"
#include "Strada/Scene/ComponentTraits.h"
#include "Strada/Scene/SceneSerializer.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/VisualScriptEngine.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
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

		// The directory of a content browser root: the asset directory, or the C# sources of the open project's scripts.
		std::filesystem::path GetBrowserRootDirectory(ContentRoot root, Project const* project)
		{
			return root == ContentRoot::Assets || project == nullptr ? AssetManager::GetAssetDirectory()
			                                                         : ScriptProject::GetSourceDirectory(*project);
		}

		// A folder of a content browser root a command names, relative to the root's directory with forward slashes ("" for
		// the directory itself, which may not exist yet: the scripts' before the first script). Every spelling of a folder is
		// accepted ("Textures/", "./Textures"); null when it is no existing folder inside the directory.
		std::optional<std::string> FindBrowserFolder(std::filesystem::path const& directory, std::string const& requested)
		{
			std::filesystem::path const relative = FileSystem::PathFromUtf8(requested).lexically_normal();
			std::filesystem::path const absolute = (directory / relative).lexically_normal();
			std::string normalized = FileSystem::PathToUtf8(FileSystem::GetRelativePath(absolute, directory));
			std::replace(normalized.begin(), normalized.end(), '\\', '/');
			while (normalized.ends_with('/'))
			{
				normalized.pop_back();
			}
			bool const isRoot = normalized.empty() || normalized == ".";
			if (relative.is_absolute() || !FileSystem::IsInside(absolute, directory) || (!isRoot && !FileSystem::IsDirectory(absolute)))
			{
				return std::nullopt;
			}
			return isRoot ? std::string() : normalized;
		}

		// The root a command names ("Assets" or "Scripts"; the schema allows no other), else the displayed one.
		ContentRoot GetBrowserRoot(Json const& params, ContentBrowserPanel const& browser)
		{
			if (!params.contains("root"))
			{
				return browser.GetRoot();
			}
			return params["root"].get<std::string>() == "Scripts" ? ContentRoot::Scripts : ContentRoot::Assets;
		}

		// The folder of the root a command names (FindBrowserFolder), else the root's displayed one.
		CommandValue<std::string> GetBrowserFolder(Json const& params, ContentBrowserPanel const& browser, ContentRoot root,
		                                           Project const* project)
		{
			if (!params.contains("folder"))
			{
				return root == ContentRoot::Assets ? browser.GetFolder() : browser.GetScriptFolder();
			}
			std::string const requested = params["folder"].get<std::string>();
			if (std::optional<std::string> folder = FindBrowserFolder(GetBrowserRootDirectory(root, project), requested))
			{
				return std::move(*folder);
			}
			CommandError error = MakeCommandError(AutomationErrorCode::InvalidParams, "'{}' is not a folder of the {}", requested,
			                                      root == ContentRoot::Assets ? "asset directory" : "scripts");
			error.Data = Json::object({{"path", "folder"}});
			return error;
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

		// The categories of the Entity menu that are submenus (all but Core, whose presets are in the menu itself).
		std::vector<std::string_view> GetEntityMenuSubmenus()
		{
			std::vector<std::string_view> submenus;
			for (EntityPresetMenuSection const& section : EntityPresets::GetMenu())
			{
				if (section.Submenu)
				{
					submenus.push_back(section.Category);
				}
			}
			return submenus;
		}

		// graph.library and graph.types list at most this many entries per call.
		constexpr int64_t MaxListedGraphEntries = 500;

		// A count parameter whose schema allows no negative value: an integer, or a number without a fraction (3.0), which may
		// be too large for size_t.
		size_t GetCount(Json const& params, char const* key, size_t fallback)
		{
			auto const it = params.find(key);
			if (it == params.end())
			{
				return fallback;
			}
			if (it->is_number_unsigned())
			{
				return static_cast<size_t>(std::min<uint64_t>(it->get<uint64_t>(), std::numeric_limits<size_t>::max()));
			}
			if (it->is_number_integer())
			{
				return static_cast<size_t>(std::max<int64_t>(it->get<int64_t>(), 0));
			}
			double const value = it->get<double>();
			return value >= static_cast<double>(std::numeric_limits<size_t>::max()) ? std::numeric_limits<size_t>::max()
			                                                                        : static_cast<size_t>(std::max(value, 0.0));
		}

		char ToLowerAscii(char character)
		{
			return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
		}

		bool EqualIgnoringCase(char a, char b)
		{
			return ToLowerAscii(a) == ToLowerAscii(b);
		}

		// Whether the text contains the part, ignoring the case of ASCII letters.
		bool ContainsIgnoringCase(std::string_view text, std::string_view part)
		{
			return std::search(text.begin(), text.end(), part.begin(), part.end(), EqualIgnoringCase) != text.end();
		}

		// Whether a node category is the given one or lies below it ("Math" holds "Math/Operators"), ignoring the case of
		// ASCII letters. Every category lies below the empty one.
		bool IsInCategory(std::string_view category, std::string_view parent)
		{
			while (parent.ends_with('/'))
			{
				parent.remove_suffix(1);
			}
			return parent.empty() ||
			       (category.size() >= parent.size() && std::equal(parent.begin(), parent.end(), category.begin(), EqualIgnoringCase) &&
			        (category.size() == parent.size() || category[parent.size()] == '/'));
		}

		// A visual script parameter: a registered asset of that type.
		CommandValue<AssetHandle> ParseGraph(Json const& value, std::string_view parameter = "graph")
		{
			CommandValue<AssetHandle> graph = ParseAsset(value, parameter);
			if (!graph)
			{
				return graph;
			}
			if (AssetType const type = AssetManager::GetAssetType(graph.GetValue()); type != AssetType::VisualScript)
			{
				CommandError error = MakeCommandError(AutomationErrorCode::InvalidParams, "{}: asset {} is a {}, not a visual script",
				                                      parameter, graph.GetValue(), AssetTypeToString(type));
				error.Data = Json::object({{"path", std::string(parameter)}});
				return error;
			}
			return graph;
		}

		// How the graph commands name a graph: its handle, and its file relative to Assets (null without a file of its own).
		Json DescribeGraph(AssetHandle graph)
		{
			std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(graph);
			return Json::object(
				{{"asset", graph.ToString()}, {"path", metadata && metadata->IsFileAsset() ? Json(metadata->Path) : Json()}});
		}

		CommandError MakeParameterError(std::string_view parameter, std::string const& message)
		{
			CommandError error = MakeCommandError(AutomationErrorCode::InvalidParams, "{}: {}", parameter, message);
			error.Data = Json::object({{"path", std::string(parameter)}});
			return error;
		}

		// A visual script's asset as the graph windows load it: a missing file is not read.
		Result<Ref<VisualScriptAsset>> LoadGraph(AssetHandle graph)
		{
			if (AssetManager::IsMissing(graph))
			{
				return Error{"its file is missing"};
			}
			return AssetManager::TryGetAsset<VisualScriptAsset>(graph);
		}

		bool HasGraphNode(VisualScriptGraph const& graph, uint32_t id)
		{
			return std::any_of(graph.Nodes.begin(), graph.Nodes.end(),
			                   [id](VisualScriptNode const& node)
			                   {
								   return node.ID == id;
							   });
		}

		bool HasGraphElement(VisualScriptGraph const& graph, uint32_t id)
		{
			return HasGraphNode(graph, id) || std::any_of(graph.Comments.begin(), graph.Comments.end(),
			                                              [id](VisualScriptComment const& comment)
			                                              {
															  return comment.ID == id;
														  });
		}

		// An ID parameter whose schema allows 0 to VisualScriptSerializer::MaxID: an integer, or a number without a fraction.
		uint32_t GetElementId(Json const& value)
		{
			double const id = value.get<double>();
			return std::isfinite(id) && id > 0.0 ? static_cast<uint32_t>(std::min(id, static_cast<double>(VisualScriptSerializer::MaxID)))
			                                     : 0;
		}

		// A canvas point parameter ([x, y] in canvas units, the units of positions in documents): finite and within the
		// coordinates documents allow.
		std::optional<glm::vec2> ReadCanvasPoint(Json const& value)
		{
			std::array<double, 2> coordinates{};
			for (size_t i = 0; i < coordinates.size(); i++)
			{
				double const coordinate = value[i].get<double>();
				if (!std::isfinite(coordinate) || std::abs(coordinate) > VisualScriptSerializer::MaxCoordinate)
				{
					return std::nullopt;
				}
				coordinates[i] = coordinate;
			}
			return glm::vec2(static_cast<float>(coordinates[0]), static_cast<float>(coordinates[1]));
		}

		// The names graph.editors and graph.find give the states of a graph window and the fields find matches, as their
		// enumerators are named.
		char const* GetDetailLevelName(VisualScriptDetailLevel level)
		{
			switch (level)
			{
				case VisualScriptDetailLevel::Far:
					return "Far";
				case VisualScriptDetailLevel::Overview:
					return "Overview";
				case VisualScriptDetailLevel::Compact:
					return "Compact";
				case VisualScriptDetailLevel::Detail:
					return "Detail";
			}
			return "Detail";
		}

		char const* GetNoticeName(VisualScriptNotice notice)
		{
			switch (notice)
			{
				case VisualScriptNotice::Degraded:
					return "Degraded";
				case VisualScriptNotice::Normalization:
					return "Normalization";
				case VisualScriptNotice::Repaired:
					return "Repaired";
				case VisualScriptNotice::ReadOnlyMemory:
					return "ReadOnlyMemory";
				case VisualScriptNotice::ReadOnlyFile:
					return "ReadOnlyFile";
				case VisualScriptNotice::NewFunction:
					return "NewFunction";
				case VisualScriptNotice::FirstCanvas:
					return "FirstCanvas";
				case VisualScriptNotice::GestureHint:
					return "GestureHint";
			}
			return "Degraded";
		}

		char const* GetCardName(VisualScriptCard card)
		{
			switch (card)
			{
				case VisualScriptCard::None:
					return "None";
				case VisualScriptCard::Missing:
					return "Missing";
				case VisualScriptCard::Unloadable:
					return "Unloadable";
				case VisualScriptCard::EmptyGraph:
					return "EmptyGraph";
			}
			return "None";
		}

		char const* GetLiveStateName(VisualScriptLiveState live)
		{
			switch (live)
			{
				case VisualScriptLiveState::None:
					return "None";
				case VisualScriptLiveState::Live:
					return "Live";
				case VisualScriptLiveState::LastWorking:
					return "LastWorking";
				case VisualScriptLiveState::Simulating:
					return "Simulating";
			}
			return "None";
		}

		char const* GetSaveStateName(VisualScriptSaveState save)
		{
			switch (save)
			{
				case VisualScriptSaveState::Saved:
					return "Saved";
				case VisualScriptSaveState::NotSaved:
					return "NotSaved";
			}
			return "Saved";
		}

		char const* GetDrawerModeName(VisualScriptDrawerMode drawer)
		{
			switch (drawer)
			{
				case VisualScriptDrawerMode::Closed:
					return "Closed";
				case VisualScriptDrawerMode::Problems:
					return "Problems";
				case VisualScriptDrawerMode::Find:
					return "Find";
			}
			return "Closed";
		}

		char const* GetPaneStateName(VisualScriptPaneState pane)
		{
			switch (pane)
			{
				case VisualScriptPaneState::Hidden:
					return "Hidden";
				case VisualScriptPaneState::Shown:
					return "Shown";
				case VisualScriptPaneState::AutoHidden:
					return "AutoHidden";
				case VisualScriptPaneState::Overlay:
					return "Overlay";
			}
			return "Shown";
		}

		char const* GetFindFieldName(VisualScriptFindField field)
		{
			switch (field)
			{
				case VisualScriptFindField::Title:
					return "Title";
				case VisualScriptFindField::Subtitle:
					return "Subtitle";
				case VisualScriptFindField::Type:
					return "Type";
				case VisualScriptFindField::NodeComment:
					return "NodeComment";
				case VisualScriptFindField::PinLabel:
					return "PinLabel";
				case VisualScriptFindField::Value:
					return "Value";
				case VisualScriptFindField::CommentText:
					return "CommentText";
				case VisualScriptFindField::VariableName:
					return "VariableName";
				case VisualScriptFindField::FunctionName:
					return "FunctionName";
				case VisualScriptFindField::EventName:
					return "EventName";
			}
			return "Title";
		}

		// A graph window as graph.editors reports it: what its last draw saw.
		Json DescribeGraphEditor(VisualScriptEditorState const& state)
		{
			Json notices = Json::array();
			for (VisualScriptNotice const notice : state.Notices)
			{
				notices.push_back(GetNoticeName(notice));
			}
			bool const loaded = state.Document != VisualScriptDocumentState::Missing &&
			                    state.Document != VisualScriptDocumentState::Unloadable && !AssetManager::IsMissing(state.Graph) &&
			                    !AssetManager::HasLoadFailure(state.Graph);
			Json described = DescribeGraph(state.Graph);
			described["function"] = state.Function;
			described["focused"] = state.Focused;
			described["visible"] = state.Visible;
			described["readOnly"] =
				state.Document == VisualScriptDocumentState::ReadOnly || state.Document == VisualScriptDocumentState::ReadOnlyFile;
			described["loaded"] = loaded;
			described["analyzed"] = state.Analyzed;
			described["errors"] = state.Errors;
			described["warnings"] = state.Warnings;
			described["view"] = Json::object({{"center", Json::array({state.View.Center.x, state.View.Center.y})},
			                                  {"zoom", state.View.Zoom},
			                                  {"level", GetDetailLevelName(state.Level)}});
			described["selection"] = Json::object({{"nodes", state.SelectedNodes}, {"comments", state.SelectedComments}});
			described["contextEntity"] = state.ContextEntity.IsValid() ? Json(state.ContextEntity.ToString()) : Json();
			described["palette"] = Json::object({{"open", state.PaletteOpen}, {"query", state.PaletteQuery}});
			described["notices"] = std::move(notices);
			described["card"] = GetCardName(state.Card);
			described["toasts"] = state.Toasts;
			described["live"] = GetLiveStateName(state.Live);
			described["save"] = GetSaveStateName(state.Save);
			described["drawer"] = GetDrawerModeName(state.Drawer);
			described["find"] =
				Json::object({{"open", state.FindOpen}, {"text", state.FindText}, {"index", state.FindIndex}, {"count", state.FindCount}});
			described["bookmarks"] = state.Bookmarks;
			described["minimap"] = state.MinimapVisible;
			described["panes"] = Json::object({{"sidebar", GetPaneStateName(state.Sidebar)},
			                                   {"details", GetPaneStateName(state.Details)},
			                                   {"stacked", state.PanesStacked}});
			described["shortcuts"] = state.ShortcutsOpen;
			return described;
		}

		// What a graph window's palette showed in its last draw, as graph.palette reports it.
		Json DescribeGraphPalette(VisualScriptPaletteState const& state, VisualScriptPaletteIndex const& index)
		{
			std::span<VisualScriptPaletteEntry const> const entries = index.GetEntries();
			Json listed = Json::array();
			for (VisualScriptPaletteMatch const& match : state.Entries)
			{
				if (match.Entry >= entries.size())
				{
					continue;
				}
				VisualScriptPaletteEntry const& entry = entries[match.Entry];
				Json row = Json::object({{"title", entry.Title},
				                         {"subtitle", entry.Subtitle},
				                         {"category", entry.Category},
				                         {"type", entry.NodeType},
				                         {"settings", entry.Settings}});
				if (state.From)
				{
					row["connects"] = match.ConnectPin;
					row["converts"] = match.Converts;
				}
				listed.push_back(std::move(row));
			}
			return Json::object({{"open", state.Open},
			                     {"query", state.Query},
			                     {"context", state.From.has_value()},
			                     {"entries", std::move(listed)},
			                     {"total", state.Total}});
		}

		// What graph.palette does once its parameters passed their checks: open the graph window's palette with the request,
		// or close it.
		struct GraphPaletteCall
		{
			AssetHandle Graph;
			VisualScriptPaletteRequest Request;
			bool Open = true;
		};

		CommandError MakeClosedWindowError(AssetHandle graph)
		{
			return MakeCommandError(AutomationErrorCode::InvalidOperation,
			                        "the window of visual script {} was closed before its palette was drawn", graph);
		}

		// graph.palette from its checked request on: opens the window's palette once the window is shown (at once when its last
		// draw showed it, else after the next draw, which shows a window graph.open opened in this frame), then completes with
		// what the palette showed in the draw after that, which ranks its entries.
		void OpenGraphPalette(VisualScriptEditors& editors, AssetHandle graph, VisualScriptPaletteRequest request,
		                      CommandCompletion completion, bool waited)
		{
			VisualScriptEditorPanel* panel = editors.FindPanel(graph);
			if (panel == nullptr)
			{
				completion.Complete(MakeClosedWindowError(graph));
				return;
			}
			if (!panel->GetState().Visible)
			{
				if (waited)
				{
					completion.Complete(MakeCommandError(
						AutomationErrorCode::InvalidOperation,
						"the window of visual script {} is not shown (another tab is in front of it): graph.open brings it to the front",
						graph));
					return;
				}
				editors.AfterNextDraw(
					[&editors, graph, request = std::move(request), completion]() mutable
					{
						OpenGraphPalette(editors, graph, std::move(request), std::move(completion), true);
					});
				return;
			}
			bool const fromPin = request.From.has_value();
			Result<VisualScriptPaletteState> const opened = panel->OpenPalette(request);
			if (!opened)
			{
				// The library and the node were checked: what the window refuses is the pin, unless the library went meanwhile.
				completion.Complete(!editors.HasNodeLibrary() || !fromPin
				                        ? MakeCommandError(AutomationErrorCode::Unavailable, "{}", opened.GetError())
				                        : MakeParameterError("from.pin", opened.GetError()));
				return;
			}
			editors.AfterNextDraw(
				[&editors, graph, completion]
				{
					VisualScriptEditorPanel const* drawn = editors.FindPanel(graph);
					if (drawn == nullptr)
					{
						completion.Complete(MakeClosedWindowError(graph));
						return;
					}
					VisualScriptPaletteState const state = drawn->GetPaletteState();
					if (!state.Open)
					{
						completion.Complete(MakeCommandError(AutomationErrorCode::InvalidOperation,
					                                         "the palette of visual script {} closed before it was drawn", graph));
						return;
					}
					completion.Complete(DescribeGraphPalette(state, drawn->GetPalette().GetIndex()));
				});
		}

		// A document given to graph.set or graph.check, read as the loader reads files but strictly: every repair it would
		// make is an issue. Without a document when it is refused, which only a document that is no object is (the commands'
		// schemas let none through).
		struct StrictDocument
		{
			std::optional<VisualScriptDocument> Document;
			std::vector<VisualScriptIssue> Issues;
		};

		StrictDocument ReadStrictDocument(Json const& content)
		{
			DeserializationContext context;
			context.UnknownFields = UnknownFieldPolicy::Warn;
			StrictDocument read;
			Result<VisualScriptDocument> document = VisualScriptSerializer::DeserializeDocument(content, context, &read.Issues);
			if (document)
			{
				read.Document = document.TakeValue();
			}
			else
			{
				read.Issues.push_back({std::string(), document.GetError()});
			}
			return read;
		}

		// [{ path, message }], the paths relative to the document.
		Json DescribeIssues(std::vector<VisualScriptIssue> const& issues)
		{
			Json described = Json::array();
			for (VisualScriptIssue const& issue : issues)
			{
				described.push_back(Json::object({{"path", issue.Path}, {"message", issue.Message}}));
			}
			return described;
		}

		// graph.set refuses a document with issues: the message is the first, data.path the parameter it is in and data.issues
		// lists every one.
		CommandError MakeIssuesError(std::vector<VisualScriptIssue> const& issues)
		{
			VisualScriptIssue const& first = issues.front();
			CommandError error = first.Path.empty()
			                         ? MakeCommandError(AutomationErrorCode::InvalidParams, "{}", first.Message)
			                         : MakeCommandError(AutomationErrorCode::InvalidParams, "{}: {}", first.Path, first.Message);
			error.Data = Json::object(
				{{"path", first.Path.empty() ? std::string("document") : "document." + first.Path}, {"issues", DescribeIssues(issues)}});
			return error;
		}

		// The scripting runtime's analysis of a document; none without the runtime, or when the runtime fails (logged). self is
		// the visual script the document is of (invalid: none): its nodes' Graph settings that name it read the document itself
		// rather than the stored asset, as the graph window's analysis does. No default: each caller says which graph it is.
		std::optional<VisualScriptAnalysis> AnalyzeGraph(VisualScriptDocument const& document, AssetHandle self)
		{
			if (!ScriptEngine::IsInitialized())
			{
				return std::nullopt;
			}
			Result<VisualScriptAnalysis> analysis = VisualScriptEngine::Analyze(document, self);
			if (!analysis)
			{
				ST_WARN("The visual script is not analyzed: {}", analysis.GetError());
				return std::nullopt;
			}
			return analysis.TakeValue();
		}

		// The node library of the scripting runtime; null without the runtime.
		Ref<VisualScriptLibrary const> FindGraphLibrary()
		{
			return ScriptEngine::IsInitialized() ? VisualScriptEngine::GetLibrary() : nullptr;
		}

		// A revision as the graph commands write it: 16 lowercase hex digits.
		std::string FormatGraphRevision(uint64_t hash)
		{
			return fmt::format("{:016x}", hash);
		}

		// The revision of a document: of its file's text as written (VisualScriptSerializer::WriteText), so a file of an
		// older version or one the loader repaired has the revision of its document as it loaded.
		std::string GetGraphRevision(VisualScriptDocument const& document)
		{
			return FormatGraphRevision(Hash::FNV1a(VisualScriptSerializer::WriteText(document)));
		}

		// The program revision of a document: of what HaveSameVisualScriptProgram compares, so moves, comments, arranging and
		// collapsing leave it unchanged.
		std::string GetGraphProgramRevision(VisualScriptDocument const& document)
		{
			return FormatGraphRevision(
				Hash::FNV1a(VisualScriptSerializer::WriteContentText(VisualScriptEditing::StripVisualScriptLayout(document))));
		}

		bool IsGraphRevision(std::string_view text)
		{
			return text.size() == 16 && std::all_of(text.begin(), text.end(),
			                                        [](char character)
			                                        {
														return (character >= '0' && character <= '9') ||
				                                               (character >= 'a' && character <= 'f');
													});
		}

		// The most operations one graph.edit call takes.
		constexpr uint64_t MaxGraphOperations = 10000;

		// The schema of one graph.edit operation: its kind and the union of every kind's fields, each with one JSON type
		// (references take an ID or a label or name). Which fields a kind takes and requires is the executor's check
		// (ApplyVisualScriptBatch), since the schema subset has no oneOf.
		SchemaBuilder MakeGraphOperationSchema()
		{
			auto const reference = [](std::string_view description)
			{
				return SchemaBuilder::Types({"integer", "string"}, description);
			};
			auto const references = [&reference](std::string_view description)
			{
				return SchemaBuilder::Array(reference({}), description).MaxItems(MaxGraphOperations);
			};
			auto const point = [](std::string_view description)
			{
				SchemaBuilder coordinate = SchemaBuilder::Number();
				coordinate.Minimum(-VisualScriptSerializer::MaxCoordinate).Maximum(VisualScriptSerializer::MaxCoordinate);
				return SchemaBuilder::Array(coordinate, description).MinItems(2).MaxItems(2);
			};
			SchemaBuilder end = SchemaBuilder::Array(reference({}), "[node, \"Pin\"]: a node's ID or label, and a pin's name");
			end.MinItems(2).MaxItems(2);
			SchemaBuilder link = SchemaBuilder::Object("{ from: [node, \"Output\"], to: [node, \"Input\"] }");
			link.Property("from", end, true).Property("to", end, true);
			SchemaBuilder parameter = SchemaBuilder::Object();
			parameter.Property("name", SchemaBuilder::String().MinLength(1), true)
				.Property("type", SchemaBuilder::String().MinLength(1), true);
			SchemaBuilder const parameters =
				SchemaBuilder::Array(parameter, "Parameters, in order: [{ name, type }]").MaxItems(VisualScriptSerializer::MaxParameters);
			SchemaBuilder unit = SchemaBuilder::Number();
			unit.Minimum(0).Maximum(1);

			std::span<std::string_view const> const kinds = GetVisualScriptBatchOperations();
			SchemaBuilder operation = SchemaBuilder::Object("One edit: { \"op\": \"<Kind>\", ...the fields that kind takes }");
			operation.Property("op", SchemaBuilder::String("The operation's kind").Enum(kinds), true)
				.Property("type",
			              SchemaBuilder::String("AddNode: the node type; AddVariable, AddLocal, SetVariable, SetLocal: the value type"))
				.Property("function", reference("The sub-graph or function: 0 for the event graph, else a function's ID, name or label"))
				.Property("position", point("[x, y] on the canvas; AddNode without one adds the node unplaced"))
				.Property("settings", SchemaBuilder::Object("AddNode: the node's settings (Variable and Function may name elements, "
			                                                "Graph may be an asset:// path)")
			                              .AllowAdditionalProperties())
				.Property("literals", SchemaBuilder::Object("AddNode: input pin -> literal").AllowAdditionalProperties())
				.Property("collapsed", SchemaBuilder::Boolean("AddNode, SetCollapsed: show only the header"))
				.Property("enabled", SchemaBuilder::String("AddNode, SetEnabled").Enum({"Enabled", "Disabled", "DevelopmentOnly"}))
				.Property("as", SchemaBuilder::String("The label (\"$name\") of what the operation creates"))
				.Property("elements", references("IDs or labels of nodes and comment boxes"))
				.Property("bridge", SchemaBuilder::Boolean("Delete: keep the flow through removed nodes").Default(true))
				.Property("labels", SchemaBuilder::Array(SchemaBuilder::String(), "Duplicate, Paste: labels of what it adds, in order")
			                            .MaxItems(MaxGraphOperations))
				.Property("clipboard", SchemaBuilder::String("Paste: graph.copy's text"))
				.Property("at", point("Paste: where the top-left of what is pasted lands"))
				.Property("offset", point("Move: [dx, dy]"))
				.Property("element", reference("SetPosition: a node or comment box"))
				.Property("from", end)
				.Property("to", end)
				.Property("node", reference("A node's ID or label"))
				.Property("remove", SchemaBuilder::Array(link, "Relink: links to remove").MaxItems(MaxGraphOperations))
				.Property("add", SchemaBuilder::Array(link, "Relink: links to add").MaxItems(MaxGraphOperations))
				.Property("link", link)
				.Property("input", SchemaBuilder::String("Splice: the node's input (default Execute)"))
				.Property("output", SchemaBuilder::String("Splice: the node's output (default its first exec output)"))
				.Property("pin", SchemaBuilder::String("A pin's name"))
				.Property("value", SchemaBuilder::Any("SetInput, SetSetting, AddVariable, SetVariable, AddLocal, SetLocal: a value in the "
			                                          "document's form; null removes or clears"))
				.Property("name", SchemaBuilder::String("A name (setting, variable, local, function, custom event)"))
				.Property("count", SchemaBuilder::Integer("SetSequenceOutputs: how many outputs").Minimum(0).Maximum(64))
				.Property("insertAt", SchemaBuilder::Integer("SetSequenceOutputs: where the new outputs go").Minimum(0).Maximum(64))
				.Property("removeAt", SchemaBuilder::Integer("SetSequenceOutputs: where outputs are removed").Minimum(0).Maximum(64))
				.Property("cases", SchemaBuilder::Any("SetSwitchCases: an array of cases, or null"))
				.Property("parameters", parameters)
				.Property("nodes", references("IDs or labels of nodes"))
				.Property("text", SchemaBuilder::String("A note's or comment box's text"))
				.Property("direction", SchemaBuilder::String("A pin's direction").Enum({"In", "Out"}))
				.Property("size", point("AddComment, SetComment: [width, height]"))
				.Property("color", SchemaBuilder::Array(unit, "AddComment, SetComment: linear [r, g, b, a]").MinItems(4).MaxItems(4))
				.Property("comment", reference("SetComment: a comment box"))
				.Property("description", SchemaBuilder::String("SetDescription: the document's description"))
				.Property("variable", reference("A variable's ID, name or label"))
				.Property("public", SchemaBuilder::Boolean("AddVariable, SetVariable"))
				.Property("tooltip", SchemaBuilder::String("AddVariable, SetVariable"))
				.Property("range", SchemaBuilder::Any("AddVariable, SetVariable: a public number's slider range [min, max], or null"))
				.Property("formerNames", SchemaBuilder::Array(SchemaBuilder::String(), "SetVariable: earlier names")
			                                 .MaxItems(VisualScriptSerializer::MaxFormerNames))
				.Property("variables", references("ReorderVariables: every variable, in order"))
				.Property("local", reference("A local's ID, name or label"))
				.Property("pure", SchemaBuilder::Boolean("AddFunction, SetFunctionPure"))
				.Property("inputs", parameters)
				.Property("outputs", parameters)
				.Property("entryAs", SchemaBuilder::String("AddFunction: the label of its Function.Entry"))
				.Property("returnAs", SchemaBuilder::String("AddFunction: the label of its Function.Return"))
				.Property("removeNodes", SchemaBuilder::Boolean("DeleteVariable, DeleteLocal: remove their nodes too").Default(true))
				.Property("removeCalls", SchemaBuilder::Boolean("DeleteFunction: remove its calls too").Default(true))
				.Property("scope", SchemaBuilder::String("Arrange: Graph, Created (what the batch added without a position) or Nodes")
			                           .Enum({"Graph", "Created", "Nodes"})
			                           .Default("Graph"))
				.Property("extents", SchemaBuilder::String("Arrange, Align, Distribute: Measured or Uniform node sizes")
			                             .Enum({"Measured", "Uniform"})
			                             .Default("Measured"))
				.Property("alignment", SchemaBuilder::String("Align").Enum({"Left", "Right", "Top", "Bottom", "Centers", "Middles"}))
				.Property("axis", SchemaBuilder::String("Distribute").Enum({"Horizontal", "Vertical"}))
				.Property("code", SchemaBuilder::String("Fix: the diagnostic's code"))
				.Property("fix", SchemaBuilder::String("Fix: the quick fix's label"));
			return operation;
		}

		// The optimistic concurrency of the commands that change a graph: with revision or programRevision, InvalidOperation
		// unless the stored document has each given (data.path names the parameter, data.<parameter> the current one, null
		// when the file does not load: such a file has no revision).
		std::optional<CommandError> CheckGraphRevisions(Json const& params, VisualScriptDocument const* stored, std::string_view action)
		{
			for (char const* const parameter : {"revision", "programRevision"})
			{
				auto const found = params.find(parameter);
				if (found == params.end())
				{
					continue;
				}
				std::string const& given = found->get_ref<std::string const&>();
				if (!IsGraphRevision(given))
				{
					return MakeParameterError(parameter, "a revision is 16 lowercase hexadecimal digits, as the graph commands return it");
				}
				if (stored == nullptr)
				{
					CommandError error = MakeCommandError(AutomationErrorCode::InvalidOperation,
					                                      "{}: the file of the visual script does not load, so it has no {} (it had {}): "
					                                      "read it again before {} it",
					                                      parameter, parameter, given, action);
					error.Data = Json::object({{"path", parameter}, {parameter, Json()}});
					return error;
				}
				bool const program = std::string_view(parameter) == "programRevision";
				std::string current = program ? GetGraphProgramRevision(*stored) : GetGraphRevision(*stored);
				if (given != current)
				{
					CommandError error =
						MakeCommandError(AutomationErrorCode::InvalidOperation,
					                     "{}: the visual script {} since {} {}, it has {} {} now: read it again before {} it", parameter,
					                     program ? "changed its program" : "changed", parameter, given, parameter, current, action);
					error.Data = Json::object({{"path", parameter}, {parameter, std::move(current)}});
					return error;
				}
			}
			return std::nullopt;
		}

		// The other visual scripts of the project that load, each with its document, by path: what graph.interface's usedBy
		// and the referrers of graph.set and graph.edit read. Those that do not load are added to skipped ({ asset, path,
		// error }) when it is given.
		struct ProjectScripts
		{
			std::vector<Ref<VisualScriptDocument const>> Documents;
			std::vector<std::pair<AssetHandle, VisualScriptDocument const*>> Scripts;
		};

		ProjectScripts LoadOtherGraphs(AssetHandle graph, Json* skipped)
		{
			std::vector<AssetMetadata> all = AssetManager::GetAssets(AssetType::VisualScript);
			std::sort(all.begin(), all.end(),
			          [](AssetMetadata const& a, AssetMetadata const& b)
			          {
						  return a.Path != b.Path ? a.Path < b.Path : a.Name < b.Name;
					  });
			ProjectScripts loaded;
			for (AssetMetadata const& script : all)
			{
				if (script.Handle == graph)
				{
					continue;
				}
				Result<Ref<VisualScriptAsset>> const asset = LoadGraph(script.Handle);
				if (!asset)
				{
					if (skipped != nullptr)
					{
						Json described = DescribeGraph(script.Handle);
						described["error"] = asset.GetError();
						skipped->push_back(std::move(described));
					}
					continue;
				}
				// The documents are shared objects: the pointers stay valid while Documents holds them.
				loaded.Documents.push_back(asset.GetValue()->GetSharedDocument());
				loaded.Scripts.emplace_back(script.Handle, loaded.Documents.back().get());
			}
			return loaded;
		}

		// The nodes of other scripts that an edit before -> after of the graph breaks (spec-slice3 §7.6 step 7a): none when
		// the two documents offer other graphs the same, which reads no other script; else from every other script that
		// loads.
		std::vector<VisualScriptEditing::VisualScriptReferrer>
		FindBrokenGraphReferrers(AssetHandle graph, VisualScriptDocument const& before, VisualScriptDocument const& after)
		{
			if (VisualScriptEditing::HaveSameVisualScriptOffer(before, after))
			{
				return {};
			}
			ProjectScripts const scripts = LoadOtherGraphs(graph, nullptr);
			return VisualScriptEditing::FindVisualScriptBrokenReferrers(graph, before, after, scripts.Scripts);
		}

		bool IsStaleSender(VisualScriptEditing::VisualScriptReferrer const& referrer)
		{
			return referrer.Problem == VisualScriptEditing::VisualScriptReferrerProblem::Stale;
		}

		// The referrers as graph.set and graph.edit return them: [{ asset, path, nodes: [{ id, type, problem, message }] }] by
		// path, and their counts in the summary (brokenReferrers: Removed, Private and Changed; staleSenders: Stale).
		Json DescribeGraphReferrers(std::vector<VisualScriptEditing::VisualScriptReferrer> const& referrers, Json& summary)
		{
			uint32_t broken = 0;
			uint32_t stale = 0;
			std::vector<Json> graphs;
			for (VisualScriptEditing::VisualScriptReferrer const& referrer : referrers)
			{
				(IsStaleSender(referrer) ? stale : broken)++;
				if (graphs.empty() || graphs.back()["asset"] != referrer.Graph.ToString())
				{
					Json graph = DescribeGraph(referrer.Graph);
					graph["nodes"] = Json::array();
					graphs.push_back(std::move(graph));
				}
				graphs.back()["nodes"].push_back(
					Json::object({{"id", referrer.Node},
				                  {"type", referrer.Type},
				                  {"problem", std::string(VisualScriptEditing::VisualScriptReferrerProblemToString(referrer.Problem))},
				                  {"message", referrer.Message}}));
			}
			std::stable_sort(graphs.begin(), graphs.end(),
			                 [](Json const& a, Json const& b)
			                 {
								 std::string const first = a["path"].is_string() ? a["path"].get<std::string>() : std::string();
								 std::string const second = b["path"].is_string() ? b["path"].get<std::string>() : std::string();
								 return first < second;
							 });
			summary["brokenReferrers"] = broken;
			summary["staleSenders"] = stale;
			return Json(std::move(graphs));
		}

		// refuseBrokenReferrers: InvalidOperation, naming the first node the edit breaks, with data.referrers (as the result
		// would list them), when it breaks any. Stale senders are reported, never refused.
		std::optional<CommandError> RefuseBrokenReferrers(std::vector<VisualScriptEditing::VisualScriptReferrer> const& referrers,
		                                                  Json const& described)
		{
			auto const broken = std::find_if_not(referrers.begin(), referrers.end(), IsStaleSender);
			if (broken == referrers.end())
			{
				return std::nullopt;
			}
			size_t const count = static_cast<size_t>(std::count_if(referrers.begin(), referrers.end(),
			                                                       [](VisualScriptEditing::VisualScriptReferrer const& referrer)
			                                                       {
																	   return !IsStaleSender(referrer);
																   }));
			std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(broken->Graph);
			std::string const where = metadata && metadata->IsFileAsset() ? metadata->Path : broken->Graph.ToString();
			CommandError error = MakeCommandError(AutomationErrorCode::InvalidOperation,
			                                      "refuseBrokenReferrers: the edit breaks {} {} of other scripts (first: {} in {}); "
			                                      "nothing was written",
			                                      count, count == 1 ? "node" : "nodes", broken->Message, where);
			error.Data = Json::object({{"referrers", described}});
			return error;
		}

		// Whether the other graphs an analysis read still hold what it read of them (spec-slice3 §7.10, §10.8): the same shared
		// document, else one equal for the analyzed document's use of it (the whole program of a library it calls, the
		// interface of the others, events included; a graph reached only through a library counts as a library).
		bool ReadsCurrentGraphs(VisualScriptAnalysis const& analysis, VisualScriptDocument const& document)
		{
			if (analysis.Graphs.empty())
			{
				return true;
			}
			std::vector<VisualScriptGraphReference> const references = CollectVisualScriptReferences(document);
			for (auto const& [graph, read] : analysis.Graphs)
			{
				// An asset of another type is absent as the engine reads it, and is not loaded to find that out.
				Ref<VisualScriptDocument const> current;
				if (AssetManager::GetAssetType(graph) == AssetType::VisualScript)
				{
					if (Result<Ref<VisualScriptAsset>> const asset = LoadGraph(graph))
					{
						current = asset.GetValue()->GetSharedDocument();
					}
				}
				if (current == read)
				{
					continue;
				}
				if (current == nullptr || read == nullptr)
				{
					return false;
				}
				AssetHandle const handle = graph;
				auto const reference = std::find_if(references.begin(), references.end(),
				                                    [handle](VisualScriptGraphReference const& candidate)
				                                    {
														return candidate.Graph == handle;
													});
				bool const library = reference == references.end() || reference->Use == VisualScriptReferenceUse::Library;
				bool const same = library
				                      ? HaveSameVisualScriptProgram(*read, *current)
				                      : HaveSameVisualScriptProgram(MakeVisualScriptInterface(*read), MakeVisualScriptInterface(*current));
				if (!same)
				{
					return false;
				}
			}
			return true;
		}

		// An ID parameter whose schema keeps it within 0 to MaxID: an integer, or a number without a fraction (3.0).
		uint32_t ReadGraphID(Json const& value)
		{
			if (value.is_number_integer())
			{
				return static_cast<uint32_t>(std::clamp<int64_t>(value.get<int64_t>(), 0, VisualScriptSerializer::MaxID));
			}
			return static_cast<uint32_t>(std::clamp(value.get<double>(), 0.0, static_cast<double>(VisualScriptSerializer::MaxID)));
		}

		// How messages name a sub-graph: "the event graph" or "function 'Push' (7)".
		std::string DescribeSubGraph(VisualScriptDocument const& document, uint32_t function)
		{
			if (function == 0)
			{
				return "the event graph";
			}
			auto const found = std::find_if(document.Functions.begin(), document.Functions.end(),
			                                [function](VisualScriptFunction const& candidate)
			                                {
												return candidate.ID == function;
											});
			return found != document.Functions.end() ? fmt::format("function '{}' ({})", found->Name, function)
			                                         : fmt::format("function {}", function);
		}

		// The sub-graph a "function" parameter names: 0 (the event graph, also when absent) or a function's ID, or a function's
		// name. InvalidParams with data.path "function" for a function the document does not have.
		CommandValue<uint32_t> ParseSubGraph(Json const& params, VisualScriptDocument const& document)
		{
			auto const parameter = params.find("function");
			if (parameter == params.end())
			{
				return uint32_t(0);
			}
			if (parameter->is_string())
			{
				std::string const& name = parameter->get_ref<std::string const&>();
				for (VisualScriptFunction const& function : document.Functions)
				{
					if (function.Name == name)
					{
						return function.ID;
					}
				}
				return MakeParameterError("function", fmt::format("the visual script has no function named '{}'", name));
			}
			uint32_t const id = ReadGraphID(*parameter);
			if (id != 0 && FindVisualScriptGraph(document, id) == nullptr)
			{
				return MakeParameterError("function", fmt::format("the visual script has no function {} (0 is the event graph)", id));
			}
			return id;
		}

		// The elements an array parameter names, each a node (or, with comments, a comment box) of the sub-graph, in the order
		// given without repeats. InvalidParams with data.path "<parameter>[<i>]" for one the sub-graph does not have.
		CommandValue<std::vector<uint32_t>> ParseSubGraphElements(Json const& list, VisualScriptDocument const& document, uint32_t function,
		                                                          std::string_view parameter, bool comments)
		{
			VisualScriptGraph const* const graph = FindVisualScriptGraph(document, function);
			std::vector<uint32_t> elements;
			for (size_t index = 0; index < list.size(); index++)
			{
				uint32_t const id = ReadGraphID(list[index]);
				bool const node = graph != nullptr && std::any_of(graph->Nodes.begin(), graph->Nodes.end(),
				                                                  [id](VisualScriptNode const& candidate)
				                                                  {
																	  return candidate.ID == id;
																  });
				bool const comment = comments && graph != nullptr &&
				                     std::any_of(graph->Comments.begin(), graph->Comments.end(),
				                                 [id](VisualScriptComment const& candidate)
				                                 {
													 return candidate.ID == id;
												 });
				if (!node && !comment)
				{
					return MakeParameterError(fmt::format("{}[{}]", parameter, index),
					                          fmt::format("{} is not a {} of {}", id, comments ? "node or comment box" : "node",
					                                      DescribeSubGraph(document, function)));
				}
				if (std::find(elements.begin(), elements.end(), id) == elements.end())
				{
					elements.push_back(id);
				}
			}
			return elements;
		}

		// The analysis in a result: the runtime's nodes and diagnostics.
		void AddAnalysis(Json& result, VisualScriptAnalysis const& analysis)
		{
			Json const& source = analysis.Source;
			auto const nodes = source.find("Nodes");
			auto const diagnostics = source.find("Diagnostics");
			result["nodes"] = nodes != source.end() ? *nodes : Json::array();
			result["diagnostics"] = diagnostics != source.end() ? *diagnostics : Json::array();
		}

		// Whether two diagnostics are one finding at one place: the same severity, code and place (graph.edit's New and
		// refuseNewErrors compare the result's with the stored document's this way).
		bool IsSameVisualScriptFinding(VisualScriptDiagnostic const& a, VisualScriptDiagnostic const& b)
		{
			return a.Severity == b.Severity && a.Code == b.Code && a.Function == b.Function && a.Node == b.Node && a.Pin == b.Pin &&
			       a.Direction == b.Direction && a.Link == b.Link && a.Variable == b.Variable;
		}

		// Whether a diagnostic of the result is one the stored document's analysis lacks.
		bool IsNewVisualScriptFinding(VisualScriptDiagnostic const& diagnostic, VisualScriptAnalysis const& stored)
		{
			return std::none_of(stored.Diagnostics.begin(), stored.Diagnostics.end(),
			                    [&diagnostic](VisualScriptDiagnostic const& earlier)
			                    {
									return IsSameVisualScriptFinding(diagnostic, earlier);
								});
		}

		// A node without what the runtime never reads (its position, comment and collapsing), so that two compare equal exactly
		// when the node's program and its stored form are unchanged.
		VisualScriptNode WithoutNodeLayout(VisualScriptNode node)
		{
			node.Position.reset();
			node.Comment.clear();
			node.Collapsed = false;
			return node;
		}

		// Every node of a document by ID, in each sub-graph.
		std::unordered_map<uint32_t, VisualScriptNode const*> IndexGraphNodes(VisualScriptDocument const& document)
		{
			std::unordered_map<uint32_t, VisualScriptNode const*> nodes;
			for (VisualScriptNode const& node : document.Graph.Nodes)
			{
				nodes[node.ID] = &node;
			}
			for (VisualScriptFunction const& function : document.Functions)
			{
				for (VisualScriptNode const& node : function.Graph.Nodes)
				{
					nodes[node.ID] = &node;
				}
			}
			return nodes;
		}

		// The nodes of the result that an edit created (listed, or absent from the stored document) or whose program changed
		// (the stored form of its type, settings, literals, splits or state; not a move, a comment or collapsing), in the
		// result's order: graph.edit's "Changed" analysis entries and the first of the nodes its hints cover.
		std::vector<uint32_t> FindEditedGraphNodes(VisualScriptDocument const& stored, VisualScriptDocument const& result,
		                                           std::span<uint32_t const> created)
		{
			std::unordered_map<uint32_t, VisualScriptNode const*> const before = IndexGraphNodes(stored);
			std::vector<uint32_t> edited;
			auto const visit = [&](VisualScriptGraph const& graph)
			{
				for (VisualScriptNode const& node : graph.Nodes)
				{
					auto const was = before.find(node.ID);
					bool const isNew = std::find(created.begin(), created.end(), node.ID) != created.end() || was == before.end();
					if (isNew)
					{
						edited.push_back(node.ID);
						continue;
					}
					VisualScriptNode const previous = WithoutNodeLayout(*was->second);
					VisualScriptNode const current = WithoutNodeLayout(node);
					if (!(previous == current && HaveSameVisualScriptProgram(previous, current)))
					{
						edited.push_back(node.ID);
					}
				}
			};
			for (VisualScriptFunction const& function : result.Functions)
			{
				visit(function.Graph);
			}
			visit(result.Graph);
			return edited;
		}

		// The nodes at an end of a link the stored document has and the result lacks (in the same sub-graph): the links an edit
		// removed or replaced, whose nodes may now never run or be read by nothing.
		std::vector<uint32_t> FindUnlinkedGraphNodes(VisualScriptDocument const& stored, VisualScriptDocument const& result)
		{
			std::vector<uint32_t> nodes;
			auto const compare = [&nodes](VisualScriptGraph const& before, VisualScriptGraph const* after)
			{
				std::span<VisualScriptLink const> const kept =
					after != nullptr ? std::span<VisualScriptLink const>(after->Links) : std::span<VisualScriptLink const>();
				for (VisualScriptLink const& link : FindVisualScriptLinksGone(before.Links, kept))
				{
					nodes.push_back(link.From.Node);
					nodes.push_back(link.To.Node);
				}
			};
			compare(stored.Graph, &result.Graph);
			for (VisualScriptFunction const& function : stored.Functions)
			{
				compare(function.Graph, FindVisualScriptGraph(result, function.ID));
			}
			return nodes;
		}

		// What graph.check reports graph.set would do to a document besides checking it (graph.set does it through
		// PrepareVisualScriptCommit): variables renamed since the stored document (when one loads) keep their stored values
		// through FormerNames, and with the runtime, nodes resolved inexactly are stored under their canonical types. The
		// warnings say what changed; the analysis is of the result. It runs on documents with issues too (as the loader
		// repairs them), which graph.set refuses before preparing anything.
		struct PreparedDocument
		{
			std::vector<std::string> Warnings;
			std::optional<VisualScriptAnalysis> Analysis;
		};

		PreparedDocument PrepareDocument(VisualScriptDocument& document, VisualScriptDocument const* stored, AssetHandle self)
		{
			PreparedDocument prepared;
			if (stored != nullptr)
			{
				prepared.Warnings = RecordFormerNames(document, *stored);
			}
			prepared.Analysis = AnalyzeGraph(document, self);
			if (prepared.Analysis)
			{
				// As graph.set, which prepares against an empty document when none is stored: an exact node's pins are renamed
				// only on a node the stored document has as it is.
				VisualScriptDocument const none;
				std::vector<std::string> const updated =
					ApplyCanonicalNodeTypes(document, *prepared.Analysis, stored != nullptr ? stored : &none);
				if (!updated.empty())
				{
					prepared.Warnings.insert(prepared.Warnings.end(), updated.begin(), updated.end());
					prepared.Analysis = AnalyzeGraph(document, self);
				}
			}
			return prepared;
		}

		// Whether a node of the library has a pin that links to a pin of the type: an input that takes an output of the type
		// (direction In), an output that goes to an input of the type (Out), or either without a direction.
		bool HasPinLinkingTo(VisualScriptLibrary const& library, VisualScriptNodeTemplate const& node, std::string const& type,
		                     std::optional<VisualScriptPinDirection> direction)
		{
			VisualScriptPin output;
			output.Direction = VisualScriptPinDirection::Out;
			output.Type = type;
			VisualScriptPin input;
			input.Direction = VisualScriptPinDirection::In;
			input.Type = type;
			return std::any_of(node.Pins.begin(), node.Pins.end(),
			                   [&](VisualScriptPin const& pin)
			                   {
								   if (direction && pin.Direction != *direction)
								   {
									   return false;
								   }
								   return pin.Direction == VisualScriptPinDirection::In ? library.CanConnect(output, pin)
				                                                                        : library.CanConnect(pin, input);
							   });
		}

		// The component categories the Add Component menu can show (Core components are on every entity already).
		std::vector<std::string_view> GetAddableCategories()
		{
			std::vector<std::string_view> categories;
			std::copy_if(ComponentCategory::All.begin(), ComponentCategory::All.end(), std::back_inserter(categories),
			             [](std::string_view category)
			             {
							 return category != ComponentCategory::Core;
						 });
			return categories;
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
				      &CommandSet::RegisterGraphs, &CommandSet::RegisterPrefabs, &CommandSet::RegisterLogAndViewport})
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

			// Menus do not open over a dialog, as users cannot open them then either.
			std::optional<CommandError> CheckNoDialog()
			{
				if (m_Environment.IsDialogOpen && m_Environment.IsDialogOpen())
				{
					return MakeCommandError(AutomationErrorCode::InvalidOperation,
					                        "a dialog is open in the editor: no menu opens until it is answered");
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
				result =
					result
						? Add("editor.entity-menu",
				              "Opens the menu bar's Entity menu, which creates entities from presets (the hierarchy's create menus are the "
				              "same), as if the user clicked Entity and pointed at the submenu of a category, or closes it (open false). "
				              "Returns { open, submenu, sections }: the menu's sections in order, each with its category, whether it is a "
				              "submenu and the labels of its presets. Changes nothing in the scene (entity.create creates entities). "
				              "InvalidOperation while a dialog is open; Unavailable without the editor's UI (headless).",
				              SchemaBuilder::Object()
				                  .Property("submenu",
				                            SchemaBuilder::String("The category whose submenu opens").Enum(GetEntityMenuSubmenus()))
				                  .Property("open", SchemaBuilder::Boolean("False closes the menu").Default(true))
				                  .Build(),
				              false,
				              [this](Json const& params) -> CommandResult
				              {
								  if (!m_Environment.ShowEntityMenu)
								  {
									  return MakeCommandError(AutomationErrorCode::Unavailable, "the editor has no menu bar (headless)");
								  }
								  bool const open = GetBool(params, "open", true);
								  if (open)
								  {
									  if (std::optional<CommandError> dialog = CheckNoDialog())
									  {
										  return *dialog;
									  }
								  }
								  std::string const submenu = open ? GetString(params, "submenu") : std::string();
								  m_Environment.ShowEntityMenu(open, submenu);

								  Json sections = Json::array();
								  if (open)
								  {
									  for (EntityPresetMenuSection const& section : EntityPresets::GetMenu())
									  {
										  Json presets = Json::array();
										  for (EntityPreset const* preset : section.Presets)
										  {
											  presets.push_back(std::string(preset->Label));
										  }
										  sections.push_back(Json::object({{"category", std::string(section.Category)},
							                                               {"submenu", section.Submenu},
							                                               {"presets", std::move(presets)}}));
									  }
								  }
								  return Json::object({{"open", open}, {"submenu", submenu}, {"sections", std::move(sections)}});
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
				                              .Enum({"Scene", "Prefab", "Mesh", "Material", "Texture", "Environment", "AudioClip", "Font",
				                                     "VisualScript"}))
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
				              "Moves or renames a file asset (path relative to Assets). Its handle, and every reference to it, stays valid. "
				              "A model moving to another folder takes the files it reads (buffers, material libraries, textures) along "
				              "to the same paths relative to it; refused when one outside its folder would no longer be found from "
				              "the new one or another model of its folder reads one.",
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

			// Visual scripts: graph assets (.sgraph) and the node library of the scripting runtime.
			Result<void> RegisterGraphs()
			{
				SchemaBuilder const graphParameter =
					SchemaBuilder::String("Visual script asset: a handle (decimal string) or \"asset://<path>.sgraph\"").MinLength(1);
				SchemaBuilder const documentParameter =
					SchemaBuilder::Object("The document: the content of a .sgraph file without its header (Description, Variables, "
				                          "Functions, Graph), as graph.get returns it")
						.AllowAdditionalProperties();
				SchemaBuilder const limitParameter =
					SchemaBuilder::Integer("The most entries to return").Minimum(1).Maximum(MaxListedGraphEntries).Default(50);
				SchemaBuilder const offsetParameter = SchemaBuilder::Integer("How many matching entries to skip").Minimum(0).Default(0);

				Result<void> result =
					Add("graph.create",
				        "Creates a visual script file (.sgraph, path relative to Assets) from a template: Events (an Event.Create and an "
				        "Event.Update node), Empty, or Library (a function library: functions other graphs call with a Function.Call "
				        "whose Graph setting names it; it runs on no entity). Not undoable (file creation). Returns { asset (the graph's "
				        "handle), path }; a VisualScript component whose Graph is the asset runs it on its entity (a script, not a "
				        "library).",
				        SchemaBuilder::Object()
				            .Property("path", SchemaBuilder::String("File path relative to Assets, ending in .sgraph").MinLength(1), true)
				            .Property("template", SchemaBuilder::String(
													  "What the graph starts with: Events (Event.Create and Event.Update nodes), Empty, "
													  "or Library (an empty function library)")
				                                      .Enum({"Events", "Empty", "Library"})
				                                      .Default("Events"))
				            .Build(),
				        false,
				        [this](Json const& params) -> CommandResult
				        {
							if (std::optional<CommandError> error = CheckAssets(true))
							{
								return *error;
							}
							std::string const chosen = GetString(params, "template", "Events");
							VisualScriptTemplate const kind = chosen == "Empty"     ? VisualScriptTemplate::Empty
					                                          : chosen == "Library" ? VisualScriptTemplate::Library
					                                                                : VisualScriptTemplate::Events;
							Result<AssetHandle> created = m_Operations.CreateVisualScript(GetString(params, "path"), kind);
							if (!created)
							{
								CommandError error = MakeCommandError(AutomationErrorCode::InvalidParams, "{}", created.GetError());
								error.Data = Json::object({{"path", "path"}});
								return error;
							}
							return DescribeGraph(created.GetValue());
						});
				result =
					result ? Add("graph.get",
				                 "A visual script's document (the content of its .sgraph file: Description, Variables, Functions, Graph) "
				                 "and, with analyze (the default) and the scripting runtime, the runtime's analysis: nodes (every node "
				                 "with its resolved type, title and pins) and diagnostics (errors and warnings with stable codes). "
				                 "Returns { asset, path, document, revision, programRevision, analyzed, nodes?, diagnostics? }: "
				                 "revision (16 hex digits) changes with any change of the document, programRevision only with a "
				                 "change of its program (not with moves, comments, arranging or the description); graph.set, "
				                 "graph.edit and graph.arrange take them to refuse an edit when the graph changed since. analyzed is "
				                 "false without the runtime. FileError when the file does not load.",
				                 SchemaBuilder::Object()
				                     .Property("graph", graphParameter, true)
				                     .Property("analyze", SchemaBuilder::Boolean("Add the runtime's analysis").Default(true))
				                     .Build(),
				                 true,
				                 [this](Json const& params) -> CommandResult
				                 {
									 if (std::optional<CommandError> error = CheckAssets(false))
									 {
										 return *error;
									 }
									 CommandValue<AssetHandle> graph = ParseGraph(params["graph"]);
									 if (!graph)
									 {
										 return graph.TakeError();
									 }
									 Result<Ref<VisualScriptAsset>> asset = AssetManager::TryGetAsset<VisualScriptAsset>(graph.GetValue());
									 if (!asset)
									 {
										 return CommandError{AutomationErrorCode::FileError, asset.GetError(), Json()};
									 }
									 Ref<VisualScriptAsset> const script = asset.TakeValue();
									 VisualScriptDocument const& document = script->GetDocument();
									 std::optional<VisualScriptAnalysis> const analysis =
										 GetBool(params, "analyze", true) ? AnalyzeGraph(document, graph.GetValue()) : std::nullopt;
									 Json described = DescribeGraph(graph.GetValue());
									 described["document"] = VisualScriptSerializer::SerializeDocument(document);
									 described["revision"] = GetGraphRevision(document);
									 described["programRevision"] = GetGraphProgramRevision(document);
									 described["analyzed"] = analysis.has_value();
									 if (analysis)
									 {
										 AddAnalysis(described, *analysis);
									 }
									 return described;
								 })
						   : result;
				result = result
				             ? Add("graph.set",
				                   "Replaces a visual script's document (the content of its .sgraph file: Description, Variables, "
				                   "Functions, Graph, as graph.get returns it) and saves the file. The document is checked strictly: "
				                   "anything the loader would repair (a duplicate ID, an unknown key, a link to a node that does not "
				                   "exist, ...) fails with InvalidParams, data.path naming the first issue and data.issues listing every "
				                   "one { path, message }; nothing is written then (graph.check reports the same without writing). A "
				                   "variable renamed (same ID) keeps its old name in FormerNames, so values components store for it still "
				                   "apply; with the scripting runtime, nodes resolved through a redirect or by their member's name alone "
				                   "are stored under their exact types; nodes without a Position are laid out. One undo step "
				                   "(editor.undo) that does not mark the scene modified; allowed while playing, where running graphs take "
				                   "the edit at once. revision and programRevision (as graph.get returns them) make the call fail with "
				                   "InvalidOperation, writing nothing, unless the stored document still has them. referrers lists the "
				                   "nodes of other scripts the new document breaks (a library function they call gone or changed, a "
				                   "variable they read gone or private, an event they send gone or retyped, untyped senders of an event "
				                   "name that went); refuseBrokenReferrers writes nothing (InvalidOperation, data.referrers) when any is "
				                   "broken. Returns { asset, path, changed, revision, programRevision (of the document written), "
				                   "analyzed, warnings (what was changed besides the given document), summary ({ brokenReferrers, "
				                   "staleSenders }), referrers ([{ asset, path, nodes: [{ id, type, problem: Removed, Private, Changed "
				                   "or Stale, message }] }]), nodes?, diagnostics? }.",
				                   SchemaBuilder::Object()
				                       .Property("graph", graphParameter, true)
				                       .Property("document", documentParameter, true)
				                       .Property("revision", SchemaBuilder::String("A revision a graph command returned (16 lowercase "
				                                                                   "hex digits): the call fails unless the document has it")
				                                                 .MinLength(16)
				                                                 .MaxLength(16))
				                       .Property("programRevision",
				                                 SchemaBuilder::String("A programRevision a graph command returned: the call fails "
				                                                       "unless the document's program (what moves, comments and "
				                                                       "arranging leave alone) is unchanged")
				                                     .MinLength(16)
				                                     .MaxLength(16))
				                       .Property("refuseBrokenReferrers",
				                                 SchemaBuilder::Boolean("Write nothing when the document breaks nodes of other scripts "
				                                                        "(referrers whose problem is Removed, Private or Changed)")
				                                     .Default(false))
				                       .Build(),
				                   false,
				                   [this](Json const& params) -> CommandResult
				                   {
									   if (std::optional<CommandError> error = CheckAssets(true))
									   {
										   return *error;
									   }
									   CommandValue<AssetHandle> graph = ParseGraph(params["graph"]);
									   if (!graph)
									   {
										   return graph.TakeError();
									   }
									   AssetHandle const handle = graph.GetValue();
									   if (std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(handle);
					                       !metadata || !metadata->IsFileAsset())
									   {
										   CommandError error = MakeCommandError(
											   AutomationErrorCode::InvalidParams,
											   "graph: visual script {} has no file of its own and cannot be edited", handle);
										   error.Data = Json::object({{"path", "graph"}});
										   return error;
									   }
									   // The document the file has, when it loads: renamed variables are found against it, and
					                   // giving it again changes nothing.
									   Result<Ref<VisualScriptAsset>> const stored = AssetManager::TryGetAsset<VisualScriptAsset>(handle);
									   Ref<VisualScriptDocument const> const storedDocument =
										   stored ? stored.GetValue()->GetSharedDocument() : nullptr;
									   if (std::optional<CommandError> error = CheckGraphRevisions(params, storedDocument.get(), "setting"))
									   {
										   return std::move(*error);
									   }
									   StrictDocument read = ReadStrictDocument(params["document"]);
									   if (!read.Issues.empty())
									   {
										   return MakeIssuesError(read.Issues);
									   }

									   VisualScriptDocument const none;
									   std::optional<VisualScriptAnalysis> analysis = AnalyzeGraph(*read.Document, handle);
									   Result<VisualScriptCommit> prepared =
										   PrepareVisualScriptCommit(storedDocument != nullptr ? *storedDocument : none,
					                                                 std::move(*read.Document), analysis ? &*analysis : nullptr);
									   if (!prepared)
									   {
										   // The document was valid, and preparing it keeps it valid: this is the editor's fault.
										   return MakeCommandError(AutomationErrorCode::InternalError, "{}", prepared.GetError());
									   }
									   VisualScriptCommit commit = prepared.TakeValue();
									   // The analysis is of the document as written: again when preparing it changed what the
					                   // runtime reads (not when it only placed nodes).
									   if (analysis && !commit.Warnings.empty())
									   {
										   analysis = AnalyzeGraph(commit.Document, handle);
									   }
									   // The nodes of other scripts the document breaks. A file that did not load offered nothing, so
					                   // the nodes naming it were broken already and none is reported (or refused): replacing it is
					                   // how a broken file is repaired.
									   Json summary = Json::object();
									   std::vector<VisualScriptEditing::VisualScriptReferrer> const broken =
										   storedDocument != nullptr ? FindBrokenGraphReferrers(handle, *storedDocument, commit.Document)
																	 : std::vector<VisualScriptEditing::VisualScriptReferrer>();
									   Json referrers = DescribeGraphReferrers(broken, summary);
									   if (GetBool(params, "refuseBrokenReferrers", false))
									   {
										   if (std::optional<CommandError> refused = RefuseBrokenReferrers(broken, referrers))
										   {
											   return std::move(*refused);
										   }
									   }
									   bool const changed =
										   storedDocument == nullptr || !IsSameVisualScriptDocument(*storedDocument, commit.Document);
									   std::string revision = FormatGraphRevision(Hash::FNV1a(commit.Text));
									   std::string programRevision = GetGraphProgramRevision(commit.Document);
									   if (Result<void> set = m_Operations.SetVisualScriptDocument(handle, std::move(commit.Document), 0,
					                                                                               std::move(commit.Text));
					                       !set)
									   {
										   return Failure(AutomationErrorCode::FileError, set);
									   }

									   Json described = DescribeGraph(handle);
									   described["changed"] = changed;
									   described["revision"] = std::move(revision);
									   described["programRevision"] = std::move(programRevision);
									   described["analyzed"] = analysis.has_value();
									   described["warnings"] = std::move(commit.Warnings);
									   described["summary"] = std::move(summary);
									   described["referrers"] = std::move(referrers);
									   if (analysis)
									   {
										   AddAnalysis(described, *analysis);
									   }
									   return described;
								   })
				             : result;
				result =
					result
						? Add("graph.check",
				              "Checks a visual script document without writing anything. issues are what graph.set would refuse "
				              "(the strict structural check: [{ path, message }], paths relative to the document; valid when there "
				              "is none); warnings are what graph.set would change (renamed variables kept in FormerNames, found "
				              "against the stored document of graph when it is given; nodes stored under their exact types); with "
				              "the scripting runtime, nodes and diagnostics are the analysis of the document as graph.set would "
				              "write it (elements with an issue as the loader repairs them). Pass graph when the document refers "
				              "to its own graph (a Graph setting naming it: a Variable.Get or Set of another entity running it, a "
				              "typed Send, RunsGraph): those nodes are then checked against the document itself. Returns { valid, "
				              "issues, warnings, analyzed, nodes?, diagnostics? }.",
				              SchemaBuilder::Object()
				                  .Property("document", documentParameter, true)
				                  .Property("graph", SchemaBuilder::String("The visual script the document is for (renamed variables are "
				                                                           "found against its document, and Graph settings naming it read "
				                                                           "the document itself): a handle or \"asset://<path>.sgraph\"")
				                                         .MinLength(1))
				                  .Build(),
				              true,
				              [this](Json const& params) -> CommandResult
				              {
								  if (std::optional<CommandError> error = CheckAssets(false))
								  {
									  return *error;
								  }
								  Ref<VisualScriptAsset> storedScript;
								  AssetHandle self;
								  if (params.contains("graph"))
								  {
									  CommandValue<AssetHandle> graph = ParseGraph(params["graph"]);
									  if (!graph)
									  {
										  return graph.TakeError();
									  }
									  self = graph.GetValue();
									  // A file that does not load has no variables to rename.
									  Result<Ref<VisualScriptAsset>> stored =
										  AssetManager::TryGetAsset<VisualScriptAsset>(graph.GetValue());
									  storedScript = stored ? stored.TakeValue() : nullptr;
								  }
								  StrictDocument read = ReadStrictDocument(params["document"]);
								  PreparedDocument prepared;
								  if (read.Document)
								  {
									  prepared = PrepareDocument(*read.Document,
						                                         storedScript != nullptr ? &storedScript->GetDocument() : nullptr, self);
								  }
								  Json described = Json::object({{"valid", read.Issues.empty()},
					                                             {"issues", DescribeIssues(read.Issues)},
					                                             {"warnings", std::move(prepared.Warnings)},
					                                             {"analyzed", prepared.Analysis.has_value()}});
								  if (prepared.Analysis)
								  {
									  AddAnalysis(described, *prepared.Analysis);
								  }
								  return described;
							  })
						: result;
				SchemaBuilder const subGraphParameter =
					SchemaBuilder::Types({"integer", "string"}, "The sub-graph: 0 for the event graph (the default), else a function's ID "
				                                                "or name")
						.Minimum(0)
						.Maximum(VisualScriptSerializer::MaxID)
						.MinLength(1);
				SchemaBuilder const elementParameter = SchemaBuilder::Integer().Minimum(0).Maximum(VisualScriptSerializer::MaxID);
				SchemaBuilder const revisionParameter =
					SchemaBuilder::String("A revision a graph command returned (16 lowercase hex digits): the call fails unless the "
				                          "document has it")
						.MinLength(16)
						.MaxLength(16);
				result =
					result
						? Add("graph.edit",
				              "Edits a visual script with an ordered batch of operations, each the graph window's own edit, applied "
				              "atomically as one undo step \"<description> in <script>\" (nothing is written when one fails; the "
				              "error names it, data.path the field, e.g. operations[3].from, with the pins, labels or closest names "
				              "that would have been right). operations (0 to 10000, applied in order; an empty batch changes nothing "
				              "and answers the analysis): AddNode, Delete, Duplicate, Paste (graph.copy's text), Move, SetPosition, "
				              "Connect, Disconnect, Relink, Splice, InsertReroute, SetInput, SetSetting, SetSequenceOutputs, "
				              "SetSwitchCases, SetEventParameters, RenameCustomEvent, SetNodeComment, SetCollapsed, SetEnabled, SplitPin, "
				              "RecombinePin, AddComment, SetComment, SetDescription, AddVariable, SetVariable, DeleteVariable, "
				              "ReorderVariables, DuplicateVariable, AddLocal, SetLocal, DeleteLocal, AddFunction, RenameFunction, "
				              "SetFunctionSignature, SetFunctionPure, DeleteFunction, DuplicateFunction, PromoteToVariable, "
				              "PromoteToFunctionInput, Arrange (scope Graph, Created: the nodes the batch added without a position, "
				              "beside the nodes they continue, or Nodes), Align, Distribute, Straighten, Fix (applies a quick fix of a "
				              "diagnostic). References: integers are IDs of the document as the call found it; \"$label\" names an "
				              "element an earlier operation created with as (or labels); variables, locals and functions may be named "
				              "(also in Variable and Function settings); asset values may be \"asset://<path>\"; pins take graph.get's "
				              "names. Field names are camelCase; values inside settings, literals and value keep the document's "
				              "PascalCase. Also takes function (the default sub-graph), description, revision and programRevision "
				              "(fail with InvalidOperation unless the stored document has them; programRevision ignores moves, comments "
				              "and arranging), dryRun, refuseNewErrors, refuseBrokenReferrers (write nothing when the result breaks "
				              "nodes of other scripts), analyze, nodes (None, Changed or All analysis entries) and document. "
				              "Operations that need pins need the scripting runtime (Unavailable without it), except Delete "
				              "and the Delete* operations, which do not bridge flow then, and measured layout. Returns { asset, path, "
				              "changed, dryRun, revision, programRevision, analyses (how many analyses the call made), labels ({ "
				              "\"$label\": id }), results (one per operation), warnings (what the commit changed besides the "
				              "operations), analyzed, summary ({ errors?, warnings?, newErrors?, newWarnings?, brokenReferrers, "
				              "staleSenders }), referrers ([{ asset, path, nodes: [{ id, type, problem: Removed, Private, Changed or "
				              "Stale, message }] }]: the nodes of other scripts the edit breaks, and untyped senders of a custom "
				              "event name that went; follow each with a graph.edit of that script), diagnostics? (each with "
				              "Fixes: [{ label, operation }], the quick fixes as ready Fix operations to append to a next batch; New: "
				              "the stored document did not have it; Label: the batch's label of its node), nodes?, hints? ([{ node, "
				              "kind, message }]: NeverRuns, Unused or UnlinkedInput, silent mistakes in the nodes the batch created or "
				              "changed and at the ends of links it removed), document? }.",
				              SchemaBuilder::Object()
				                  .Property("graph", graphParameter, true)
				                  .Property("operations",
				                            SchemaBuilder::Array(MakeGraphOperationSchema(),
				                                                 "The operations, applied in order: { \"op\": \"<Kind>\", "
				                                                 "...fields }")
				                                .MaxItems(MaxGraphOperations),
				                            true)
				                  .Property("function", subGraphParameter)
				                  .Property("description", SchemaBuilder::String("The undo step's text, before \" in <script>\"")
				                                               .MinLength(1)
				                                               .MaxLength(128)
				                                               .Default("Edit Visual Script"))
				                  .Property("revision", revisionParameter)
				                  .Property("programRevision",
				                            SchemaBuilder::String("A programRevision a graph command returned: the call fails unless "
				                                                  "the document's program (what moves, comments and arranging leave "
				                                                  "alone) is unchanged")
				                                .MinLength(16)
				                                .MaxLength(16))
				                  .Property("dryRun", SchemaBuilder::Boolean("Answer what the call would do, writing nothing and "
				                                                             "recording no undo step")
				                                          .Default(false))
				                  .Property("refuseNewErrors", SchemaBuilder::Boolean("Write nothing when the result has an Error "
				                                                                      "diagnostic the stored document does not have")
				                                                   .Default(false))
				                  .Property("refuseBrokenReferrers",
				                            SchemaBuilder::Boolean("Write nothing when the result breaks nodes of other scripts "
				                                                   "(referrers whose problem is Removed, Private or Changed)")
				                                .Default(false))
				                  .Property("analyze", SchemaBuilder::Boolean("Analyze the result (operations that need pins analyze "
				                                                              "anyway)")
				                                           .Default(true))
				                  .Property("nodes", SchemaBuilder::String("Which analysis entries the result lists: None, Changed (the "
				                                                           "nodes the batch created or changed) or All")
				                                         .Enum({"None", "Changed", "All"})
				                                         .Default("Changed"))
				                  .Property("document",
				                            SchemaBuilder::Boolean("Include the resulting document (content form)").Default(false))
				                  .Build(),
				              false,
				              [this](Json const& params) -> CommandResult
				              {
								  return EditGraph(params);
							  })
						: result;
				result = result
				             ? Add("graph.arrange",
				                   "Lays out nodes of a visual script as the graph window's Arrange button does (links running left to "
				                   "right in columns, few crossings, groups one below the other) and saves the file as one undo step "
				                   "\"Arrange N Nodes in <script>\". Takes graph, function (0 for the event graph, the default, else a "
				                   "function's ID or name), nodes (the IDs to arrange, links between them counting; absent or empty: "
				                   "every node of the sub-graph), extents (Measured, the default: node sizes as a window draws them, "
				                   "measured without one; Uniform: 256 x 128 for every node), revision (fails with InvalidOperation, "
				                   "changing nothing, unless the stored document has this revision) and dryRun (everything but the "
				                   "write and the undo step). Comment boxes whose contents are all arranged are fitted around them "
				                   "again. Works headless and without the scripting runtime. Returns { asset, path, changed (false when "
				                   "the nodes stand arranged already: no step), revision, programRevision (of the document written, or "
				                   "that a dry run would write), arranged (how many nodes), bounds: { min: [x, y], max: [x, y] } (the "
				                   "arranged nodes' extent afterwards, null for none: where a comment box framing them goes) }.",
				                   SchemaBuilder::Object()
				                       .Property("graph", graphParameter, true)
				                       .Property("function", subGraphParameter)
				                       .Property("nodes", SchemaBuilder::Array(elementParameter, "IDs of nodes of the sub-graph to arrange "
				                                                                                 "(default: every node)")
				                                              .MaxItems(10000))
				                       .Property("extents", SchemaBuilder::String("How nodes are sized: Measured (as a window draws them) "
				                                                                  "or Uniform (256 x 128)")
				                                                .Enum({"Measured", "Uniform"})
				                                                .Default("Measured"))
				                       .Property("revision", SchemaBuilder::String("A revision a graph command returned (16 lowercase "
				                                                                   "hex digits): the call fails unless the document has it")
				                                                 .MinLength(16)
				                                                 .MaxLength(16))
				                       .Property("dryRun", SchemaBuilder::Boolean("Answer what the call would do, writing nothing "
				                                                                  "and recording no undo step")
				                                               .Default(false))
				                       .Build(),
				                   false,
				                   [this](Json const& params) -> CommandResult
				                   {
									   return ArrangeGraph(params);
								   })
				             : result;
				result = result
				             ? Add("graph.copy",
				                   "Copies nodes and comment boxes of a visual script as the graph window's Copy does: returns { text }, "
				                   "the editor's clipboard text (the elements with the links between them, the variables they use and "
				                   "the functions they call), which graph.edit's Paste takes into any graph, binding variables and "
				                   "functions by name. Takes graph, function (0 for the event graph, the default, else a function's ID "
				                   "or name) and elements (1 to 10000 IDs of nodes and comment boxes of that sub-graph). Writes nothing "
				                   "and does not touch the system clipboard; works headless. FileError when the file does not load.",
				                   SchemaBuilder::Object()
				                       .Property("graph", graphParameter, true)
				                       .Property("function", subGraphParameter)
				                       .Property("elements",
				                                 SchemaBuilder::Array(elementParameter, "IDs of nodes and comment boxes of "
				                                                                        "the sub-graph to copy")
				                                     .MinItems(1)
				                                     .MaxItems(10000),
				                                 true)
				                       .Build(),
				                   true,
				                   [this](Json const& params) -> CommandResult
				                   {
									   return CopyGraph(params);
								   })
				             : result;
				result = result
				             ? Add("graph.interface",
				                   "What a visual script offers other graphs, read from its document: the IDs and names to put in the "
				                   "Function and Variable settings of nodes whose Graph setting names it, and the events to send it. "
				                   "Returns { asset, path, kind (Script or Library), variables: [{ id, name, type, public }] (a Get or Set "
				                   "of another entity's graph reads and writes public ones), events: [{ node, name, parameters: [{ name, "
				                   "type }], enabled }] (its custom events, which a typed Event.Send or Event.Broadcast sends), functions: "
				                   "[{ id, name, pure, inputs: [{ name, type, formerNames? }], outputs, latent? }] (a library's are what "
				                   "a Function.Call with Graph calls; latent only with the scripting runtime), uses: [{ asset, path, use: "
				                   "Verification, Interface or Library, nodes }] (the graphs its nodes name), usedBy: [{ asset, path, "
				                   "nodes }] (the other scripts whose nodes name it), skipped: [{ asset, path, error }] (scripts that do "
				                   "not load, so not read for usedBy), analyzed }. Works headless; FileError when its file does not load.",
				                   SchemaBuilder::Object().Property("graph", graphParameter, true).Build(), true,
				                   [this](Json const& params) -> CommandResult
				                   {
									   return DescribeGraphInterface(params);
								   })
				             : result;
				result =
					result
						? Add("graph.library",
				              "Lists the node kinds visual scripts can use (the built-in kinds and the scripting API's members) in "
				              "library order (by category, then title): each { Type (what a node stores as its Type), Title, "
				              "Category, Description?, Keywords, Kind, Pure, Latent, Pins: [{ Name, Direction, Type, Default?, ... }], "
				              "Settings }. query keeps nodes whose title, keywords, category or type contain the text, category "
				              "keeps a category with the ones below it (both ignoring case), and pinType keeps nodes with a pin that "
				              "links to a pin of that type: an input taking it (direction In), an output giving it (Out) or either. "
				              "Paged with limit and offset; total counts every match. notes lists the game's members marked "
				              "[VisualNode] that have no node, and why ({ member, category, message }), filtered by category and by "
				              "query (in the member or the message), never paged. Unavailable without the scripting runtime.",
				              SchemaBuilder::Object()
				                  .Property("query", SchemaBuilder::String("Text the title, keywords, category or type contain"))
				                  .Property("category", SchemaBuilder::String("A category, such as \"Flow\" or \"Components/Rigid Body\": "
				                                                              "its nodes and those of the categories below it"))
				                  .Property("pinType", SchemaBuilder::String("A type ID (graph.types lists them; exec for flow): nodes "
				                                                             "with a pin that links to a pin of this type")
				                                           .MinLength(1))
				                  .Property("direction", SchemaBuilder::String("With pinType: In for nodes with an input taking the type, "
				                                                               "Out for nodes with an output giving it")
				                                             .Enum({"In", "Out"}))
				                  .Property("limit", limitParameter)
				                  .Property("offset", offsetParameter)
				                  .Build(),
				              true,
				              [this](Json const& params) -> CommandResult
				              {
								  CommandValue<Ref<VisualScriptLibrary const>> library = GetGraphLibrary();
								  if (!library)
								  {
									  return library.TakeError();
								  }
								  std::optional<VisualScriptPinDirection> direction;
								  if (params.contains("direction"))
								  {
									  direction = params["direction"].get<std::string>() == "In" ? VisualScriptPinDirection::In
						                                                                         : VisualScriptPinDirection::Out;
								  }
								  std::string const pinType = GetString(params, "pinType");
								  if (direction && pinType.empty())
								  {
									  CommandError error = MakeCommandError(AutomationErrorCode::InvalidParams,
						                                                    "direction needs pinType: the type of the pins to link to");
									  error.Data = Json::object({{"path", "direction"}});
									  return error;
								  }
								  Ref<VisualScriptLibrary const> const nodes = library.TakeValue();
								  if (!pinType.empty() && nodes->FindType(pinType) == nullptr)
								  {
									  CommandError error = MakeCommandError(
										  AutomationErrorCode::InvalidParams,
										  "pinType: the node library has no type '{}' (graph.types lists the types)", pinType);
									  error.Data = Json::object({{"path", "pinType"}});
									  return error;
								  }
								  std::string const category = GetString(params, "category");
								  std::vector<VisualScriptNodeTemplate const*> matches = nodes->Search(GetString(params, "query"));
								  std::erase_if(matches,
					                            [&](VisualScriptNodeTemplate const* node)
					                            {
													return !IsInCategory(node->Category, category) ||
						                                   (!pinType.empty() && !HasPinLinkingTo(*nodes, *node, pinType, direction));
												});
								  size_t const offset = GetCount(params, "offset", 0);
								  size_t const limit = GetCount(params, "limit", 50);
								  Json listed = Json::array();
								  for (size_t index = offset; index < matches.size() && index - offset < limit; index++)
								  {
									  listed.push_back(matches[index]->Source);
								  }
								  return Json::object(
									  {{"nodes", std::move(listed)},
					                   {"total", matches.size()},
					                   {"notes", ListVisualScriptLibraryNotes(*nodes, GetString(params, "query"), category)}});
							  })
						: result;
				result =
					result ? Add("graph.types",
				                 "Lists the types visual scripts use for variables, pins and literals, by ID: each { ID, Title, Kind, "
				                 "FieldType? (types public variables can have), AssetType?, Literal? (documents can hold values of it), "
				                 "Enumerators? and Flags (enums), Fields? (structs), Base? (classes), Element? (arrays), ImplicitTo (the "
				                 "types it converts to; every type also converts to string) }. query keeps types whose ID or title "
				                 "contains the text (ignoring case), kind one kind. Paged with limit and offset; total counts every "
				                 "match. Unavailable without the scripting runtime.",
				                 SchemaBuilder::Object()
				                     .Property("query", SchemaBuilder::String("Text the ID or title contains"))
				                     .Property("kind", SchemaBuilder::String("Only types of this kind")
				                                           .Enum({"Exec", "Bool", "Integer", "Float", "String", "Vector", "Quaternion",
				                                                  "Color", "Matrix", "Entity", "Component", "Asset", "Enum", "Struct",
				                                                  "Class", "Array", "Object"}))
				                     .Property("limit", limitParameter)
				                     .Property("offset", offsetParameter)
				                     .Build(),
				                 true,
				                 [this](Json const& params) -> CommandResult
				                 {
									 CommandValue<Ref<VisualScriptLibrary const>> library = GetGraphLibrary();
									 if (!library)
									 {
										 return library.TakeError();
									 }
									 Ref<VisualScriptLibrary const> const types = library.TakeValue();
									 std::string const query = GetString(params, "query");
									 std::string const kind = GetString(params, "kind");
									 std::vector<VisualScriptTypeInfo const*> matches;
									 for (VisualScriptTypeInfo const& type : types->GetTypes())
									 {
										 auto const typeKind = type.Source.find("Kind");
										 bool const ofKind =
											 kind.empty() || (typeKind != type.Source.end() && typeKind->is_string() && *typeKind == kind);
										 if (ofKind && (ContainsIgnoringCase(type.ID, query) || ContainsIgnoringCase(type.Title, query)))
										 {
											 matches.push_back(&type);
										 }
									 }
									 size_t const offset = GetCount(params, "offset", 0);
									 size_t const limit = GetCount(params, "limit", 50);
									 Json listed = Json::array();
									 for (size_t index = offset; index < matches.size() && index - offset < limit; index++)
									 {
										 listed.push_back(matches[index]->Source);
									 }
									 return Json::object({{"types", std::move(listed)}, {"total", matches.size()}});
								 })
						   : result;
				result = result ? RegisterGraphWindows() : result;
				return result;
			}

			// graph.arrange: ArrangeNodes as the Arrange button runs it (VisualScriptController's Arrange), its step described
			// alike and prepared by the same commit pipeline, with node sizes from a headless canvas (Measured) or uniform ones.
			CommandResult ArrangeGraph(Json const& params)
			{
				if (std::optional<CommandError> error = CheckAssets(true))
				{
					return *error;
				}
				CommandValue<AssetHandle> graph = ParseGraph(params["graph"]);
				if (!graph)
				{
					return graph.TakeError();
				}
				AssetHandle const handle = graph.GetValue();
				std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(handle);
				if (!metadata || !metadata->IsFileAsset())
				{
					return MakeParameterError(
						"graph", fmt::format("visual script {} has no file of its own and cannot be edited", handle.ToString()));
				}
				Result<Ref<VisualScriptAsset>> const asset = LoadGraph(handle);
				if (!asset)
				{
					return MakeCommandError(AutomationErrorCode::FileError,
					                        "the file of visual script '{}' does not load: {}; graph.set replaces it", metadata->Path,
					                        asset.GetError());
				}
				Ref<VisualScriptDocument const> const stored = asset.GetValue()->GetSharedDocument();
				if (std::optional<CommandError> error = CheckGraphRevisions(params, stored.get(), "arranging"))
				{
					return std::move(*error);
				}
				CommandValue<uint32_t> const function = ParseSubGraph(params, *stored);
				if (!function)
				{
					return function.GetError();
				}
				std::vector<uint32_t> nodes;
				if (auto const listed = params.find("nodes"); listed != params.end())
				{
					CommandValue<std::vector<uint32_t>> parsed =
						ParseSubGraphElements(*listed, *stored, function.GetValue(), "nodes", false);
					if (!parsed)
					{
						return parsed.TakeError();
					}
					nodes = parsed.TakeValue();
				}
				VisualScriptGraph const* const subGraph = FindVisualScriptGraph(*stored, function.GetValue());
				if (subGraph == nullptr)
				{
					return MakeParameterError("function", "the visual script has no such sub-graph");
				}
				if (nodes.empty())
				{
					for (VisualScriptNode const& node : subGraph->Nodes)
					{
						nodes.push_back(node.ID);
					}
				}

				// The nodes' sizes: what a window would draw, from the analysis with the runtime, else the library's templates,
				// else the pins the links and literals name. The analysis also gives the commit the nodes' canonical types, as
				// the window's does.
				Ref<VisualScriptLibrary const> const library = FindGraphLibrary();
				std::optional<VisualScriptAnalysis> const analysis = library != nullptr ? AnalyzeGraph(*stored, handle) : std::nullopt;
				VisualScriptHeadlessCanvas canvas;
				canvas.Sync(*stored, function.GetValue(), analysis ? &*analysis : nullptr, stored.get(), library.get());
				VisualScriptNodeExtents const measured = canvas.GetExtents();
				VisualScriptNodeExtents const extents =
					GetString(params, "extents", "Measured") == "Uniform"
						? VisualScriptNodeExtents(&VisualScriptEditing::GetDefaultVisualScriptNodeExtents)
						: measured;

				VisualScriptDocument arranged = *stored;
				if (Result<void> const done = VisualScriptEditing::ArrangeNodes(arranged, function.GetValue(), nodes, extents); !done)
				{
					// The nodes are the sub-graph's and the sizes are a canvas's: this is the editor's fault.
					return MakeCommandError(AutomationErrorCode::InternalError, "{}", done.GetError());
				}
				std::string const action = nodes.size() == 1 ? std::string("Arrange Node") : fmt::format("Arrange {} Nodes", nodes.size());
				std::string const scriptName = FileSystem::PathToUtf8(FileSystem::PathFromUtf8(metadata->Path).stem());
				Result<VisualScriptCommitRequest> prepared =
					MakeVisualScriptCommitRequest(*stored, std::move(arranged), analysis ? &*analysis : nullptr, action, scriptName, 0);
				if (!prepared)
				{
					// The arranged document was valid, and preparing it keeps it valid: this is the editor's fault.
					return MakeCommandError(AutomationErrorCode::InternalError, "{}", prepared.GetError());
				}
				VisualScriptCommitRequest request = prepared.TakeValue();
				bool const changed = !IsSameVisualScriptDocument(*request.Document, *stored);
				bool const dryRun = GetBool(params, "dryRun", false);
				Json described = DescribeGraph(handle);
				described["changed"] = changed;
				described["revision"] = FormatGraphRevision(Hash::FNV1a(request.Text));
				described["programRevision"] = GetGraphProgramRevision(*request.Document);
				described["arranged"] = nodes.size();
				described["bounds"] = Json();
				if (VisualScriptGraph const* const result = FindVisualScriptGraph(*request.Document, function.GetValue()))
				{
					std::optional<VisualScriptRect> bounds;
					for (VisualScriptNode const& node : result->Nodes)
					{
						if (!node.Position || std::find(nodes.begin(), nodes.end(), node.ID) == nodes.end())
						{
							continue;
						}
						VisualScriptRect const rect{*node.Position, *node.Position + measured(node.ID)};
						bounds = bounds ? VisualScriptRect{glm::min(bounds->Min, rect.Min), glm::max(bounds->Max, rect.Max)} : rect;
					}
					if (bounds)
					{
						described["bounds"] = Json::object(
							{{"min", Json::array({bounds->Min.x, bounds->Min.y})}, {"max", Json::array({bounds->Max.x, bounds->Max.y})}});
					}
				}
				if (changed && !dryRun)
				{
					if (Result<void> const edited = m_Operations.EditVisualScript(handle, request.Document, std::move(request.Description),
					                                                              0, std::move(request.Text));
					    !edited)
					{
						return Failure(AutomationErrorCode::FileError, edited);
					}
				}
				return described;
			}

			// graph.interface (spec-slice3 §12.3): what the graph offers others, what it uses and who uses it.
			CommandResult DescribeGraphInterface(Json const& params)
			{
				if (std::optional<CommandError> error = CheckAssets(false))
				{
					return *error;
				}
				CommandValue<AssetHandle> graph = ParseGraph(params["graph"]);
				if (!graph)
				{
					return graph.TakeError();
				}
				AssetHandle const handle = graph.GetValue();
				Result<Ref<VisualScriptAsset>> const asset = LoadGraph(handle);
				if (!asset)
				{
					return MakeCommandError(AutomationErrorCode::FileError, "graph: the visual script does not load ({})",
					                        asset.GetError());
				}
				Ref<VisualScriptDocument const> const document = asset.GetValue()->GetSharedDocument();
				std::optional<VisualScriptAnalysis> const analysis = AnalyzeGraph(*document, handle);

				Json described = DescribeGraph(handle);
				described["kind"] = std::string(VisualScriptDocumentKindToString(document->Kind));
				Json variables = Json::array();
				for (VisualScriptVariable const& variable : document->Variables)
				{
					variables.push_back(
						Json::object({{"id", variable.ID}, {"name", variable.Name}, {"type", variable.Type}, {"public", variable.Public}}));
				}
				described["variables"] = std::move(variables);

				Json events = Json::array();
				for (VisualScriptNode const& node : document->Graph.Nodes)
				{
					if (node.Type != "Event.Custom")
					{
						continue;
					}
					auto const name = node.Settings.find("Name");
					Json parameters = Json::array();
					if (auto const list = node.Settings.find("Parameters"); list != node.Settings.end() && list->is_array())
					{
						for (Json const& parameter : *list)
						{
							auto const parameterName = parameter.find("Name");
							auto const parameterType = parameter.find("Type");
							if (parameterName != parameter.end() && parameterName->is_string() && parameterType != parameter.end() &&
							    parameterType->is_string())
							{
								parameters.push_back(Json::object({{"name", *parameterName}, {"type", *parameterType}}));
							}
						}
					}
					events.push_back(Json::object({{"node", node.ID},
					                               {"name", name != node.Settings.end() && name->is_string() ? *name : Json()},
					                               {"parameters", std::move(parameters)},
					                               {"enabled", std::string(VisualScriptNodeEnabledToString(node.Enabled))}}));
				}
				described["events"] = std::move(events);

				auto const describeParameters = [](std::vector<VisualScriptParameter> const& parameters)
				{
					Json listed = Json::array();
					for (VisualScriptParameter const& parameter : parameters)
					{
						Json entry = Json::object({{"name", parameter.Name}, {"type", parameter.Type}});
						if (!parameter.FormerNames.empty())
						{
							entry["formerNames"] = parameter.FormerNames;
						}
						listed.push_back(std::move(entry));
					}
					return listed;
				};
				Json functions = Json::array();
				for (VisualScriptFunction const& function : document->Functions)
				{
					Json entry = Json::object({{"id", function.ID},
					                           {"name", function.Name},
					                           {"pure", function.Pure},
					                           {"inputs", describeParameters(function.Inputs)},
					                           {"outputs", describeParameters(function.Outputs)}});
					if (analysis)
					{
						// As the runtime decides it: a node of its graph that waits (a latent node, or a call of a latent
						// function) and is not disabled.
						entry["latent"] = std::any_of(function.Graph.Nodes.begin(), function.Graph.Nodes.end(),
						                              [&analysis](VisualScriptNode const& node)
						                              {
														  VisualScriptAnalyzedNode const* const analyzed = analysis->FindNode(node.ID);
														  return analyzed != nullptr && analyzed->Latent &&
							                                     node.Enabled != VisualScriptNodeEnabled::Disabled;
													  });
					}
					functions.push_back(std::move(entry));
				}
				described["functions"] = std::move(functions);

				Json uses = Json::array();
				for (VisualScriptGraphReference const& reference : CollectVisualScriptReferences(*document))
				{
					Json entry = DescribeGraph(reference.Graph);
					entry["use"] = std::string(reference.Use == VisualScriptReferenceUse::Library     ? "Library"
					                           : reference.Use == VisualScriptReferenceUse::Interface ? "Interface"
					                                                                                  : "Verification");
					entry["nodes"] = reference.Nodes;
					uses.push_back(std::move(entry));
				}
				described["uses"] = std::move(uses);

				Json skipped = Json::array();
				Json usedBy = Json::array();
				ProjectScripts const scripts = LoadOtherGraphs(handle, &skipped);
				for (auto const& [other, otherDocument] : scripts.Scripts)
				{
					std::vector<VisualScriptGraphReference> const references = CollectVisualScriptReferences(*otherDocument);
					auto const reference = std::find_if(references.begin(), references.end(),
					                                    [handle](VisualScriptGraphReference const& candidate)
					                                    {
															return candidate.Graph == handle;
														});
					if (reference != references.end())
					{
						Json entry = DescribeGraph(other);
						entry["nodes"] = reference->Nodes;
						usedBy.push_back(std::move(entry));
					}
				}
				described["usedBy"] = std::move(usedBy);
				described["skipped"] = std::move(skipped);
				described["analyzed"] = analysis.has_value();
				return described;
			}

			// graph.edit (spec §7.6): the batch on a copy of the stored document, its analysis, the commit pipeline and one undo
			// step through EditVisualScript, as a graph window commits its edits.
			CommandResult EditGraph(Json const& params)
			{
				if (std::optional<CommandError> error = CheckAssets(true))
				{
					return *error;
				}
				CommandValue<AssetHandle> graph = ParseGraph(params["graph"]);
				if (!graph)
				{
					return graph.TakeError();
				}
				AssetHandle const handle = graph.GetValue();
				std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(handle);
				if (!metadata || !metadata->IsFileAsset())
				{
					return MakeParameterError(
						"graph", fmt::format("visual script {} has no file of its own and cannot be edited", handle.ToString()));
				}
				Result<Ref<VisualScriptAsset>> const asset = LoadGraph(handle);
				if (!asset)
				{
					return MakeCommandError(AutomationErrorCode::FileError,
					                        "the file of visual script '{}' does not load: {}; graph.set replaces it", metadata->Path,
					                        asset.GetError());
				}
				Ref<VisualScriptDocument const> const stored = asset.GetValue()->GetSharedDocument();
				if (std::optional<CommandError> error = CheckGraphRevisions(params, stored.get(), "editing"))
				{
					return std::move(*error);
				}
				CommandValue<uint32_t> const function = ParseSubGraph(params, *stored);
				if (!function)
				{
					return function.GetError();
				}

				// Other graphs the operations name (by a Graph setting) are read from their assets, kept for the batch.
				auto const held = CreateRef<std::vector<Ref<VisualScriptDocument const>>>();
				Ref<VisualScriptLibrary const> const library = m_Semantics.GetLibrary();
				Ref<VisualScriptAnalysis const> storedAnalysis = FindStoredGraphAnalysis(handle, stored, library);
				VisualScriptBatchContext context(m_Semantics);
				context.Graph = handle;
				context.Function = function.GetValue();
				context.FindAsset = [](std::string_view reference) -> std::optional<AssetHandle>
				{
					Result<AssetHandle> const found = AssetManager::ResolveReference(reference);
					if (!found || !AssetManager::IsValid(found.GetValue()))
					{
						return std::nullopt;
					}
					return found.GetValue();
				};
				context.FindDocument = [held](AssetHandle other) -> VisualScriptDocument const*
				{
					if (AssetManager::GetAssetType(other) != AssetType::VisualScript)
					{
						return nullptr;
					}
					Result<Ref<VisualScriptAsset>> const loaded = LoadGraph(other);
					if (!loaded)
					{
						return nullptr;
					}
					held->push_back(loaded.GetValue()->GetSharedDocument());
					return held->back().get();
				};
				context.StoredAnalysis = storedAnalysis;
				context.StoredAnalysisDocument = stored;
				std::variant<VisualScriptBatchResult, VisualScriptBatchError> applied =
					ApplyVisualScriptBatch(*stored, params["operations"], context);
				if (VisualScriptBatchError const* const failed = std::get_if<VisualScriptBatchError>(&applied))
				{
					return MakeBatchError(*failed);
				}
				VisualScriptBatchResult batch = std::get<VisualScriptBatchResult>(std::move(applied));

				// A batch that leaves the document as it found it writes nothing, so nothing is prepared: preparing places the
				// stored document's unplaced nodes and stores its inexact types canonically, changes the batch did not make (an
				// empty batch inspects a graph without editing it). Its result is the stored document, whose one analysis serves
				// as both the result's and the stored one's, and is kept for the next call.
				uint32_t analyses = batch.Analyses;
				bool const refuseNewErrors = GetBool(params, "refuseNewErrors", false);
				bool const analyze = (GetBool(params, "analyze", true) || refuseNewErrors) && library != nullptr;
				Ref<VisualScriptAnalysis const> analysis;
				std::optional<VisualScriptCommit> commit;
				if (IsSameVisualScriptDocument(*stored, batch.Document))
				{
					if (analyze)
					{
						analysis = storedAnalysis != nullptr ? storedAnalysis : batch.Analysis;
						if (analysis == nullptr)
						{
							analyses++;
							analysis = AnalyzeForEdit(*stored, handle);
						}
						storedAnalysis = analysis;
					}
				}
				else
				{
					// The analysis of the result: the batch's last one when nothing changed the program since, else a new one.
					if (analyze)
					{
						if (batch.Analysis != nullptr)
						{
							analysis = batch.Analysis;
						}
						else
						{
							analyses++;
							analysis = AnalyzeForEdit(batch.Document, handle);
						}
					}
					Result<VisualScriptCommit> prepared =
						PrepareVisualScriptCommit(*stored, std::move(batch.Document), analysis != nullptr ? analysis.get() : nullptr);
					if (!prepared)
					{
						// The operations kept the document valid, and preparing it keeps it valid: this is the editor's fault.
						return MakeCommandError(AutomationErrorCode::InternalError, "{}", prepared.GetError());
					}
					commit = prepared.TakeValue();
					// The analysis is of the document as written: again when preparing it changed what the runtime reads.
					if (analysis != nullptr && !commit->Warnings.empty())
					{
						analyses++;
						analysis = AnalyzeForEdit(commit->Document, handle);
					}
					// The stored document's analysis, for New and refuseNewErrors: cached, or made once now and kept for the next
					// call.
					if (analysis != nullptr && storedAnalysis == nullptr)
					{
						analyses++;
						storedAnalysis = AnalyzeForEdit(*stored, handle);
						RememberStoredGraphAnalysis(handle, stored, library, storedAnalysis);
					}
				}
				if (refuseNewErrors)
				{
					if (std::optional<CommandError> refused = RefuseNewErrors(analysis.get(), storedAnalysis.get()))
					{
						return std::move(*refused);
					}
				}
				// The nodes of other scripts the result breaks (spec-slice3 §7.6 step 7a); none for a result that is the stored
				// document.
				Json summary = Json::object();
				std::vector<VisualScriptEditing::VisualScriptReferrer> const broken =
					commit.has_value() ? FindBrokenGraphReferrers(handle, *stored, commit->Document)
									   : std::vector<VisualScriptEditing::VisualScriptReferrer>();
				Json referrers = DescribeGraphReferrers(broken, summary);
				if (GetBool(params, "refuseBrokenReferrers", false))
				{
					if (std::optional<CommandError> refused = RefuseBrokenReferrers(broken, referrers))
					{
						return std::move(*refused);
					}
				}

				// Preparing can give the stored document back (a type the batch wrote inexactly, stored canonically again).
				bool const changed = commit.has_value() && !IsSameVisualScriptDocument(*stored, commit->Document);
				VisualScriptDocument const& result = commit.has_value() ? commit->Document : *stored;
				bool const dryRun = GetBool(params, "dryRun", false);
				Json described = DescribeGraph(handle);
				described["changed"] = changed;
				described["dryRun"] = dryRun;
				described["revision"] = commit.has_value() ? FormatGraphRevision(Hash::FNV1a(commit->Text)) : GetGraphRevision(*stored);
				described["programRevision"] = GetGraphProgramRevision(result);
				described["analyses"] = analyses;
				Json labels = Json::object();
				for (auto const& [label, id] : batch.Labels)
				{
					labels[label] = id;
				}
				described["labels"] = std::move(labels);
				described["results"] = std::move(batch.Results);
				described["warnings"] = commit.has_value() ? Json(commit->Warnings) : Json::array();
				described["analyzed"] = analysis != nullptr;
				if (analysis != nullptr)
				{
					std::vector<uint32_t> const edited = FindEditedGraphNodes(*stored, result, batch.Created);
					AddAnalysis(described, *analysis);
					DescribeEditedDiagnostics(described["diagnostics"], summary, result, *analysis, storedAnalysis.get(), *library, batch);
					described["nodes"] = SelectGraphNodes(described["nodes"], GetString(params, "nodes", "Changed"), edited);
					std::vector<uint32_t> hinted = edited;
					std::vector<uint32_t> const unlinked = FindUnlinkedGraphNodes(*stored, result);
					hinted.insert(hinted.end(), unlinked.begin(), unlinked.end());
					Json hints = Json::array();
					for (VisualScriptHint const& hint : FindVisualScriptHints(result, *analysis, hinted))
					{
						hints.push_back(Json::object({{"node", hint.Node}, {"kind", hint.Kind}, {"message", hint.Message}}));
					}
					described["hints"] = std::move(hints);
				}
				described["summary"] = std::move(summary);
				described["referrers"] = std::move(referrers);
				if (GetBool(params, "document", false))
				{
					described["document"] = VisualScriptSerializer::SerializeDocument(result);
				}
				if (!changed)
				{
					// The result is the stored document again, so its analysis is the stored one's.
					RememberStoredGraphAnalysis(handle, stored, library, analysis);
				}
				else if (!dryRun)
				{
					std::string const scriptName = FileSystem::PathToUtf8(FileSystem::PathFromUtf8(metadata->Path).stem());
					std::string description = GetString(params, "description", "Edit Visual Script") + " in " + scriptName;
					Ref<VisualScriptDocument const> const written = CreateRef<VisualScriptDocument const>(std::move(commit->Document));
					if (Result<void> const edited =
					        m_Operations.EditVisualScript(handle, written, std::move(description), 0, std::move(commit->Text));
					    !edited)
					{
						return Failure(AutomationErrorCode::FileError, edited);
					}
					// The asset holds the document written now: its analysis is the next call's stored one.
					RememberStoredGraphAnalysis(handle, written, library, analysis);
				}
				return described;
			}

			// The runtime's analysis of a document of the graph; null when the runtime fails (logged).
			Ref<VisualScriptAnalysis const> AnalyzeForEdit(VisualScriptDocument const& document, AssetHandle graph)
			{
				Result<VisualScriptAnalysis> made = m_Semantics.Analyze(document, graph);
				if (!made)
				{
					ST_WARN("The visual script is not analyzed: {}", made.GetError());
					return nullptr;
				}
				return CreateRef<VisualScriptAnalysis const>(made.TakeValue());
			}

			// The cached analysis of a graph's stored document: valid while the asset holds the same shared document, the runtime
			// the same node library as when it was made, and every other graph it read what it read of them (ReadsCurrentGraphs);
			// null when there is none (an entry that is no longer valid is dropped).
			Ref<VisualScriptAnalysis const> FindStoredGraphAnalysis(AssetHandle graph, Ref<VisualScriptDocument const> const& document,
			                                                        Ref<VisualScriptLibrary const> const& library)
			{
				auto const found = m_StoredGraphAnalyses.find(graph);
				if (found == m_StoredGraphAnalyses.end())
				{
					return nullptr;
				}
				if (library == nullptr || found->second.Document.lock() != document || found->second.Library.lock() != library ||
				    document == nullptr || !ReadsCurrentGraphs(*found->second.Analysis, *document))
				{
					m_StoredGraphAnalyses.erase(found);
					return nullptr;
				}
				return found->second.Analysis;
			}

			// Keeps the analysis of the document a graph's asset holds for the next graph.edit; nothing when there is none.
			// Entries whose document or library is gone are dropped, so the cache holds at most one analysis per live graph.
			void RememberStoredGraphAnalysis(AssetHandle graph, Ref<VisualScriptDocument const> const& document,
			                                 Ref<VisualScriptLibrary const> const& library, Ref<VisualScriptAnalysis const> analysis)
			{
				std::erase_if(m_StoredGraphAnalyses,
				              [](auto const& entry)
				              {
								  return entry.second.Document.expired() || entry.second.Library.expired();
							  });
				if (analysis == nullptr || document == nullptr || library == nullptr)
				{
					return;
				}
				m_StoredGraphAnalyses[graph] = StoredGraphAnalysis{document, library, std::move(analysis)};
			}

			// What graph.edit adds to the analysis' diagnostics (each in the order the runtime listed them): Fixes, the quick fixes
			// that change the document as ready Fix operations; New, whether the stored document's analysis lacks the finding
			// (when that analysis was made); Label, the batch's label of the node it concerns when the batch created that node.
			// The summary counts the errors and warnings, and the new ones.
			static void DescribeEditedDiagnostics(Json& diagnostics, Json& summary, VisualScriptDocument const& document,
			                                      VisualScriptAnalysis const& analysis, VisualScriptAnalysis const* stored,
			                                      VisualScriptLibrary const& library, VisualScriptBatchResult const& batch)
			{
				uint32_t errors = 0;
				uint32_t warnings = 0;
				uint32_t newErrors = 0;
				uint32_t newWarnings = 0;
				for (size_t index = 0; index < analysis.Diagnostics.size(); index++)
				{
					VisualScriptDiagnostic const& diagnostic = analysis.Diagnostics[index];
					bool const error = diagnostic.Severity == VisualScriptSeverity::Error;
					bool const isNew = stored != nullptr && IsNewVisualScriptFinding(diagnostic, *stored);
					(error ? errors : warnings)++;
					if (isNew)
					{
						(error ? newErrors : newWarnings)++;
					}
					if (!diagnostics.is_array() || index >= diagnostics.size() || !diagnostics[index].is_object())
					{
						continue;
					}
					Json& described = diagnostics[index];
					described["Fixes"] = GetVisualScriptFixOperations(document, analysis, diagnostic, library);
					if (stored != nullptr)
					{
						described["New"] = isNew;
					}
					if (diagnostic.Node && std::find(batch.Created.begin(), batch.Created.end(), *diagnostic.Node) != batch.Created.end())
					{
						auto const label = std::find_if(batch.Labels.begin(), batch.Labels.end(),
						                                [&diagnostic](auto const& entry)
						                                {
															return entry.second == *diagnostic.Node;
														});
						if (label != batch.Labels.end())
						{
							described["Label"] = label->first;
						}
					}
				}
				summary["errors"] = errors;
				summary["warnings"] = warnings;
				if (stored != nullptr)
				{
					summary["newErrors"] = newErrors;
					summary["newWarnings"] = newWarnings;
				}
			}

			// A batch's failure as the command's error: the failing operation, its kind and field in data.
			static CommandError MakeBatchError(VisualScriptBatchError const& failed)
			{
				AutomationErrorCode code = AutomationErrorCode::InvalidParams;
				switch (failed.Failure)
				{
					case VisualScriptBatchFailure::Invalid:
						code = AutomationErrorCode::InvalidParams;
						break;
					case VisualScriptBatchFailure::NeedsRuntime:
					case VisualScriptBatchFailure::AnalysisFailed:
						code = AutomationErrorCode::Unavailable;
						break;
					case VisualScriptBatchFailure::AssetNotFound:
						code = AutomationErrorCode::AssetNotFound;
						break;
				}
				CommandError error{code, failed.Message, Json::object()};
				if (failed.Data.is_object())
				{
					error.Data = failed.Data;
				}
				error.Data["path"] = failed.Path;
				error.Data["operation"] = failed.Operation;
				if (!failed.Kind.empty())
				{
					error.Data["op"] = failed.Kind;
				}
				return error;
			}

			// refuseNewErrors: InvalidOperation, with data.diagnostics, when the result has an Error the stored document's
			// analysis does not (the same severity, code and place); Unavailable when either analysis is missing.
			static std::optional<CommandError> RefuseNewErrors(VisualScriptAnalysis const* analysis, VisualScriptAnalysis const* stored)
			{
				if (analysis == nullptr || stored == nullptr)
				{
					return MakeCommandError(AutomationErrorCode::Unavailable,
					                        "refuseNewErrors needs the analysis of the visual script before and after the edit, which "
					                        "the scripting runtime did not give");
				}
				// The runtime's JSON lists the diagnostics in the order they were read.
				Json const& source = analysis->Source;
				auto const sourceDiagnostics = source.find("Diagnostics");
				Json found = Json::array();
				std::string first;
				for (size_t index = 0; index < analysis->Diagnostics.size(); index++)
				{
					VisualScriptDiagnostic const& diagnostic = analysis->Diagnostics[index];
					if (diagnostic.Severity != VisualScriptSeverity::Error || !IsNewVisualScriptFinding(diagnostic, *stored))
					{
						continue;
					}
					bool const described =
						sourceDiagnostics != source.end() && sourceDiagnostics->is_array() && index < sourceDiagnostics->size();
					found.push_back(described ? (*sourceDiagnostics)[index] : Json::object({{"Message", diagnostic.Message}}));
					first = first.empty() ? diagnostic.Message : first;
				}
				if (found.empty())
				{
					return std::nullopt;
				}
				CommandError error = MakeCommandError(AutomationErrorCode::InvalidOperation,
				                                      "refuseNewErrors: the edit would add {} error(s) the visual script does not have; "
				                                      "first: {}",
				                                      found.size(), first);
				error.Data = Json::object({{"path", "refuseNewErrors"}, {"diagnostics", std::move(found)}});
				return error;
			}

			// The analysis entries graph.edit lists: none, all, or those of the nodes the edit created or changed (edited, from
			// FindEditedGraphNodes).
			static Json SelectGraphNodes(Json const& nodes, std::string const& selection, std::span<uint32_t const> edited)
			{
				if (selection == "All")
				{
					return nodes;
				}
				Json selected = Json::array();
				if (selection == "None" || !nodes.is_array())
				{
					return selected;
				}
				for (Json const& entry : nodes)
				{
					auto const id = entry.is_object() ? entry.find("ID") : entry.end();
					if (id != entry.end() && id->is_number_unsigned() &&
					    std::find(edited.begin(), edited.end(), id->get<uint32_t>()) != edited.end())
					{
						selected.push_back(entry);
					}
				}
				return selected;
			}

			// graph.copy: the clipboard text of the elements, as the graph window's Copy writes it.
			CommandResult CopyGraph(Json const& params)
			{
				if (std::optional<CommandError> error = CheckAssets(false))
				{
					return *error;
				}
				CommandValue<AssetHandle> graph = ParseGraph(params["graph"]);
				if (!graph)
				{
					return graph.TakeError();
				}
				AssetHandle const handle = graph.GetValue();
				Result<Ref<VisualScriptAsset>> const asset = LoadGraph(handle);
				if (!asset)
				{
					std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(handle);
					return MakeCommandError(AutomationErrorCode::FileError, "the file of visual script '{}' does not load: {}",
					                        metadata && !metadata->Path.empty() ? metadata->Path : handle.ToString(), asset.GetError());
				}
				VisualScriptDocument const& document = asset.GetValue()->GetDocument();
				CommandValue<uint32_t> const function = ParseSubGraph(params, document);
				if (!function)
				{
					return function.GetError();
				}
				CommandValue<std::vector<uint32_t>> elements =
					ParseSubGraphElements(params["elements"], document, function.GetValue(), "elements", true);
				if (!elements)
				{
					return elements.TakeError();
				}
				Ref<VisualScriptLibrary const> const library = FindGraphLibrary();
				return Json::object(
					{{"text", WriteVisualScriptClipboard(document, function.GetValue(), elements.GetValue(), handle, library.get())}});
			}

			// The graph windows: Unavailable without the editor's UI, and the asset checks (a project for the commands that
			// open something).
			std::optional<CommandError> CheckGraphWindows(bool needsProject) const
			{
				if (!m_Environment.GraphEditors)
				{
					return MakeCommandError(AutomationErrorCode::Unavailable, "the editor has no graph windows (headless)");
				}
				return CheckAssets(needsProject);
			}

			// graph.open, graph.close, graph.editors, graph.palette (the editor's graph windows) and graph.find.
			Result<void> RegisterGraphWindows()
			{
				SchemaBuilder const graphParameter =
					SchemaBuilder::String("Visual script asset: a handle (decimal string) or \"asset://<path>.sgraph\"").MinLength(1);
				SchemaBuilder const idParameter = SchemaBuilder::Integer().Minimum(0).Maximum(VisualScriptSerializer::MaxID);
				SchemaBuilder const pointParameter = SchemaBuilder::Array(SchemaBuilder::Number(), "").MinItems(2).MaxItems(2);
				// The zoom's bounds as the decimals clients write: the float bounds widened to double (0.10000000149...) would
				// refuse 0.1, the farthest zoom. Every number they admit narrows to a zoom within the float bounds.
				constexpr double MinZoom = 0.1;
				constexpr double MaxZoom = 2.0;
				static_assert(static_cast<float>(MinZoom) == VisualScriptMinZoom && static_cast<float>(MaxZoom) == VisualScriptMaxZoom);

				Result<void> result = Add(
					"graph.open",
					"Opens a visual script in its graph window, or focuses the window open already, as double-clicking it in the content "
					"browser does; with the parameters, shows a sub-graph (function), selects nodes and comments (select, the first "
					"the primary selection), frames them or the whole sub-graph (frame), sets the zoom and the view's centre, picks "
					"the context entity whose live values the window shows while playing, opens the problems drawer or the find bar "
					"(find, going to the first match, which becomes the selection). The view parameters apply in order: the framing, "
					"then zoom around its centre, then center. Applied when the window is next drawn: graph.editors a frame later "
					"shows the result. Works while playing. A file that is missing or does not load opens on its card (loaded false): "
					"function, select, frame and find are then ignored, which warnings says. Returns { asset, path, function (the "
					"sub-graph shown), selection (the select given, as applied: empty without one; an empty select keeps the window's "
					"selection), loaded, warnings }. Unavailable without the editor's UI "
					"(headless); InvalidOperation while a dialog is open.",
					SchemaBuilder::Object()
						.Property("graph", graphParameter, true)
						.Property("function", SchemaBuilder::Integer("The sub-graph to show: 0 for the event graph, else a function's ID "
				                                                     "(default: the one shown last)")
				                                  .Minimum(0)
				                                  .Maximum(VisualScriptSerializer::MaxID))
						.Property("select", SchemaBuilder::Array(idParameter, "IDs of nodes and comments of that sub-graph to select")
				                                .MaxItems(static_cast<int64_t>(VisualScriptSerializer::MaxNodesPerGraph)))
						.Property("frame", SchemaBuilder::String("What the view frames: Keep (the view the sub-graph had), Selection "
				                                                 "(default with select) or All (the whole sub-graph)")
				                               .Enum({"Keep", "Selection", "All"}))
						.Property(
							"zoom",
							SchemaBuilder::Number("Screen pixels per canvas unit at a UI scale of 1").Minimum(MinZoom).Maximum(MaxZoom))
						.Property("center", SchemaBuilder::Array(SchemaBuilder::Number()
				                                                     .Minimum(-VisualScriptSerializer::MaxCoordinate)
				                                                     .Maximum(VisualScriptSerializer::MaxCoordinate),
				                                                 "The canvas point [x, y] at the centre of the view, in canvas units")
				                                .MinItems(2)
				                                .MaxItems(2))
						.Property("entity", EntityIdSchema("The context entity: an entity of the scene that runs the graph"))
						.Property("problems", SchemaBuilder::Boolean("Opens the problems drawer"))
						.Property("find", SchemaBuilder::String("Opens the find bar with this text and goes to the first match"))
						.Build(),
					false,
					[this](Json const& params) -> CommandResult
					{
						if (std::optional<CommandError> error = CheckGraphWindows(true))
						{
							return *error;
						}
						if (std::optional<CommandError> dialog = CheckNoDialog())
						{
							return *dialog;
						}
						CommandValue<AssetHandle> graph = ParseGraph(params["graph"]);
						if (!graph)
						{
							return graph.TakeError();
						}
						VisualScriptEditors& editors = m_Environment.GraphEditors();
						VisualScriptOpenRequest request;
						request.Graph = graph.GetValue();
						if (params.contains("entity"))
						{
							CommandValue<UUID> entity = ParseEntityId(params["entity"], "entity");
							if (!entity)
							{
								return entity.TakeError();
							}
							if (!m_Operations.FindEntity(entity.GetValue()))
							{
								return MakeParameterError("entity", fmt::format("the scene has no entity {}", entity.GetValue()));
							}
							request.ContextEntity = entity.GetValue();
						}
						if (params.contains("zoom"))
						{
							double const zoom = params["zoom"].get<double>();
							if (!std::isfinite(zoom))
							{
								return MakeParameterError("zoom", "expected a finite number");
							}
							request.Zoom = static_cast<float>(zoom);
						}
						if (params.contains("center"))
						{
							std::optional<glm::vec2> const center = ReadCanvasPoint(params["center"]);
							if (!center)
							{
								return MakeParameterError("center", fmt::format("expected two finite numbers of at most {} in magnitude",
							                                                    VisualScriptSerializer::MaxCoordinate));
							}
							request.Center = *center;
						}
						std::vector<uint32_t> select;
						if (params.contains("select"))
						{
							for (Json const& id : params["select"])
							{
								select.push_back(GetElementId(id));
							}
						}
						std::string const frame = GetString(params, "frame", select.empty() ? "Keep" : "Selection");
						std::string const find = GetString(params, "find");

						Result<Ref<VisualScriptAsset>> const asset = LoadGraph(request.Graph);
						Json warnings = Json::array();
						if (asset)
						{
							// Checked against the document: the sub-graph asked for, else the one the window shows (a function that
						    // is gone shows the event graph), holds what is selected.
							Ref<VisualScriptDocument const> const document = asset.GetValue()->GetSharedDocument();
							uint32_t function =
								params.contains("function") ? GetElementId(params["function"]) : editors.GetShownFunction(request.Graph);
							VisualScriptGraph const* shown = FindVisualScriptGraph(*document, function);
							if (shown == nullptr)
							{
								if (params.contains("function"))
								{
									return MakeParameterError(
										"function", fmt::format("the visual script has no function {}", params["function"].dump()));
								}
								function = 0;
								shown = &document->Graph;
							}
							for (size_t i = 0; i < select.size(); i++)
							{
								if (!HasGraphElement(*shown, select[i]))
								{
									return MakeParameterError(fmt::format("select[{}]", i),
								                              fmt::format("the sub-graph shown has no node or comment {}", select[i]));
								}
							}
							if (params.contains("function"))
							{
								request.Function = function;
							}
							request.Select = select;
							request.Frame = frame == "All"         ? VisualScriptFrame::All
						                    : frame == "Selection" ? VisualScriptFrame::Selection
						                                           : VisualScriptFrame::Keep;
							request.Find = find;
						}
						else
						{
							// The window shows the file's card: nothing of the document can be shown or checked.
							for (char const* const ignored : {"function", "select", "frame", "find"})
							{
								if (params.contains(ignored))
								{
									warnings.push_back(
										fmt::format("{} is ignored: the visual script does not load ({})", ignored, asset.GetError()));
								}
							}
							select.clear();
						}
						request.ShowProblems = GetBool(params, "problems", false);
						Result<VisualScriptEditorState> const opened = editors.Open(request);
						if (!opened)
						{
							return MakeCommandError(AutomationErrorCode::InvalidOperation, "{}", opened.GetError());
						}
						Json described = DescribeGraph(request.Graph);
						described["function"] = opened.GetValue().Function;
						described["selection"] = std::move(select);
						described["loaded"] = asset.IsOk();
						described["warnings"] = std::move(warnings);
						return described;
					});
				result =
					result ? Add("graph.close",
				                 "Closes the window of a visual script, or every graph window without graph, as its close button does: "
				                 "edits still in a field are kept first, and the per-user view state is saved. Closing a graph whose "
				                 "window is not open (or a deleted one named by its handle, whose window closed with it) is no "
				                 "error. Returns { closed (the assets whose windows closed, in tab order) }. Unavailable without the "
				                 "editor's UI (headless).",
				                 SchemaBuilder::Object()
				                     .Property("graph", SchemaBuilder::String("The visual script whose window closes: a handle or "
				                                                              "\"asset://<path>.sgraph\" (default: every window)")
				                                            .MinLength(1))
				                     .Build(),
				                 false,
				                 [this](Json const& params) -> CommandResult
				                 {
									 if (std::optional<CommandError> error = CheckGraphWindows(false))
									 {
										 return *error;
									 }
									 std::optional<AssetHandle> target;
									 if (params.contains("graph"))
									 {
										 // A handle that is no longer registered names a deleted graph: its window closed with it.
										 std::optional<UUID> const handle = UUID::FromString(params["graph"].get_ref<std::string const&>());
										 if (handle && handle->IsValid() && !AssetManager::IsValid(AssetHandle(*handle)))
										 {
											 return Json::object({{"closed", Json::array()}});
										 }
										 CommandValue<AssetHandle> graph = ParseGraph(params["graph"]);
										 if (!graph)
										 {
											 return graph.TakeError();
										 }
										 target = graph.GetValue();
									 }
									 Json closed = Json::array();
									 for (AssetHandle const handle : m_Environment.GraphEditors().Close(target))
									 {
										 closed.push_back(handle.ToString());
									 }
									 return Json::object({{"closed", std::move(closed)}});
								 })
						   : result;
				result =
					result
						? Add("graph.editors",
				              "The open graph windows in tab order, each as its last draw showed it: { asset, path, function (the sub-graph "
				              "shown: 0 is the event graph), focused, visible (its tab is in front), readOnly, loaded, analyzed, errors, "
				              "warnings (the analysis' problems), view { center, zoom, level (Detail, Compact, Overview or Far) }, "
				              "selection { nodes, comments }, contextEntity, palette { open, query }, notices, card, toasts, live, save, "
				              "drawer, find { open, text, index, count }, bookmarks (the slots stored), minimap (shown), panes { sidebar, "
				              "details }, shortcuts (the shortcut sheet is open) }. A window opened since the last frame reports its "
				              "state before its first draw (not visible, nothing selected). Unavailable without the editor's UI "
				              "(headless).",
				              Json(), true,
				              [this](Json const&) -> CommandResult
				              {
								  if (std::optional<CommandError> error = CheckGraphWindows(false))
								  {
									  return *error;
								  }
								  Json editors = Json::array();
								  for (VisualScriptEditorState const& state : m_Environment.GraphEditors().Describe())
								  {
									  editors.push_back(DescribeGraphEditor(state));
								  }
								  return Json::object({{"editors", std::move(editors)}});
							  })
						: result;
				if (!result)
				{
					return result;
				}

				CommandDefinition palette;
				palette.Name = "graph.palette";
				palette.Description =
					"Opens the node palette of an open graph window as if the user pressed Tab at a canvas point (at, default the "
					"view's centre) and typed the query, or, with from, as if a wire dragged from that pin was released there (the "
					"palette then lists what connects to the pin), or closes it (open false). Changes nothing in the document "
					"(graph.set adds nodes). Completes once the palette was drawn: { open, query, context (from was given), entries "
					"(in rank order, at most limit: without query or from, the recent ones), total (the entries before the limit) }, "
					"each entry { title, subtitle, category, type (the node type it adds; empty for actions that add none), settings, "
					"connects (with from: the entry's pin that the wire links to), converts (with from: the link converts its value) }. "
					"Unavailable without the editor's UI (headless) and without the scripting runtime (the palette lists the node "
					"library); InvalidOperation when the graph's window is not open or not shown, its file does not load, or a dialog "
					"is open; InvalidParams for a from node the sub-graph shown does not have, a pin it does not have and an at "
					"beyond the canvas.";
				palette.Parameters =
					SchemaBuilder::Object()
						.Property("graph", graphParameter, true)
						.Property("query", SchemaBuilder::String("The text typed into the palette's search field"))
						.Property("at", SchemaBuilder::Array(SchemaBuilder::Number(), "The canvas point [x, y] where the node would go, in "
				                                                                      "canvas units (default: the view's centre)")
				                            .MinItems(2)
				                            .MaxItems(2))
						.Property("from",
				                  SchemaBuilder::Object("Context mode: the pin a wire is dragged from")
				                      .Property("node", idParameter, true)
				                      .Property("pin",
				                                SchemaBuilder::String("The pin, as the canvas names it (\"Hit.Point\" for a "
				                                                      "field of a split pin)"),
				                                true)
				                      .Property("direction",
				                                SchemaBuilder::String("Whether the pin is an input or an output").Enum({"In", "Out"}),
				                                true))
						.Property("limit", SchemaBuilder::Integer("The most entries to return").Minimum(1).Maximum(200).Default(50))
						.Property("open", SchemaBuilder::Boolean("False closes the palette").Default(true))
						.Build();
				palette.AsyncHandler = [this, self = shared_from_this()](Json const& params, CommandCompletion completion)
				{
					std::variant<CommandError, GraphPaletteCall> checked = CheckPaletteRequest(params);
					if (CommandError* error = std::get_if<CommandError>(&checked))
					{
						completion.Complete(std::move(*error));
						return;
					}
					GraphPaletteCall& call = std::get<GraphPaletteCall>(checked);
					VisualScriptEditors& editors = m_Environment.GraphEditors();
					if (!call.Open)
					{
						if (VisualScriptEditorPanel* panel = editors.FindPanel(call.Graph))
						{
							panel->ClosePalette();
						}
						completion.Complete(
							Json::object({{"open", false}, {"query", ""}, {"context", false}, {"entries", Json::array()}, {"total", 0}}));
						return;
					}
					OpenGraphPalette(editors, call.Graph, std::move(call.Request), std::move(completion), false);
				};
				result = m_Registry.Register(std::move(palette));

				result =
					result
						? Add("graph.find",
				              "Finds text in visual scripts, as the graph window's find drawer does, or lists the references of a variable "
				              "or local, a function or a custom event. Text is looked for in node titles, subtitles, types, comments, pin "
				              "labels and stored values, comment boxes, and the names of variables, functions and custom events (ASCII "
				              "letters in any case). References are the Get and Set nodes of a variable or local and the Call nodes of a "
				              "function (in its own graph), and the Event.Custom and Event.Send nodes of a custom event's name (in every "
				              "graph). Searches every visual script of the project by path, or one (graph); results come in document "
				              "order: { results: [{ asset, path, function (0 for the event graph), node?, comment?, variable?, field "
				              "(Title, Subtitle, Type, NodeComment, PinLabel, Value, CommentText, VariableName, FunctionName or "
				              "EventName), text }], total (before paging), analyzed (titles, subtitles and pin labels are the scripting "
				              "runtime's; without it titles are derived from the documents and subtitles and pin labels not searched), "
				              "skipped (the scripts that do not load: [{ asset, path, error }]) }. Works headless and changes nothing.",
				              SchemaBuilder::Object()
				                  .Property("query", SchemaBuilder::String("The text to find"))
				                  .Property(
									  "references",
									  SchemaBuilder::Object("The element whose references to list")
										  .Property(
											  "kind",
											  SchemaBuilder::String("What the element is").Enum({"Variable", "Function", "CustomEvent"}),
											  true)
										  .Property("graph", SchemaBuilder::String("Variable and Function: the visual script that has the "
				                                                                   "element (a handle or \"asset://<path>.sgraph\")")
				                                                 .MinLength(1))
										  .Property("id", SchemaBuilder::Integer("Variable and Function: the element's ID")
				                                              .Minimum(0)
				                                              .Maximum(VisualScriptSerializer::MaxID))
										  .Property("name", SchemaBuilder::String("CustomEvent: the event's name").MinLength(1)))
				                  .Property(
									  "graph",
									  SchemaBuilder::String("Only this visual script: a handle or \"asset://<path>.sgraph\"").MinLength(1))
				                  .Property("limit",
				                            SchemaBuilder::Integer("The most results to return").Minimum(1).Maximum(500).Default(50))
				                  .Property("offset", SchemaBuilder::Integer("How many results to skip").Minimum(0).Default(0))
				                  .Build(),
				              true,
				              [this](Json const& params) -> CommandResult
				              {
								  return FindInGraphs(params);
							  })
						: result;
				return result;
			}

			// graph.palette's checks, in the order the documentation gives (so its parameters are checked, and fuzzed, without the
			// runtime too): what to do when they pass.
			std::variant<CommandError, GraphPaletteCall> CheckPaletteRequest(Json const& params) const
			{
				if (std::optional<CommandError> error = CheckGraphWindows(true))
				{
					return *error;
				}
				CommandValue<AssetHandle> graph = ParseGraph(params["graph"]);
				if (!graph)
				{
					return graph.TakeError();
				}
				AssetHandle const handle = graph.GetValue();
				VisualScriptEditors& editors = m_Environment.GraphEditors();
				if (editors.FindPanel(handle) == nullptr)
				{
					return MakeCommandError(AutomationErrorCode::InvalidOperation,
					                        "the window of visual script {} is not open: graph.open opens it", handle);
				}
				GraphPaletteCall call;
				call.Graph = handle;
				call.Open = GetBool(params, "open", true);
				if (!call.Open)
				{
					return call;
				}
				if (m_Environment.IsDialogOpen && m_Environment.IsDialogOpen())
				{
					return MakeCommandError(AutomationErrorCode::InvalidOperation,
					                        "a dialog is open in the editor: no menu opens until it is answered");
				}
				Result<Ref<VisualScriptAsset>> const asset = LoadGraph(handle);
				if (!asset)
				{
					return MakeCommandError(AutomationErrorCode::InvalidOperation,
					                        "the visual script does not load ({}): its window shows no graph", asset.GetError());
				}

				VisualScriptPaletteRequest& request = call.Request;
				request.Query = GetString(params, "query");
				request.Limit = static_cast<uint32_t>(std::min<size_t>(GetCount(params, "limit", 50), 200));
				if (params.contains("from"))
				{
					Json const& from = params["from"];
					Ref<VisualScriptDocument const> const document = asset.GetValue()->GetSharedDocument();
					VisualScriptGraph const* shown = FindVisualScriptGraph(*document, editors.GetShownFunction(handle));
					if (shown == nullptr)
					{
						shown = &document->Graph;
					}
					uint32_t const node = GetElementId(from["node"]);
					if (!HasGraphNode(*shown, node))
					{
						return MakeParameterError("from.node", fmt::format("the sub-graph shown has no node {}", from["node"].dump()));
					}
					std::string const& pin = from["pin"].get_ref<std::string const&>();
					if (!VisualScriptSerializer::IsValidPinPath(pin))
					{
						return MakeParameterError("from.pin", "expected a pin name, or a field path of a split pin (\"Hit.Point\")");
					}
					request.From = VisualScriptPinReference{node, pin};
					request.Direction =
						from["direction"].get<std::string>() == "In" ? VisualScriptPinDirection::In : VisualScriptPinDirection::Out;
				}
				if (params.contains("at"))
				{
					std::optional<glm::vec2> const at = ReadCanvasPoint(params["at"]);
					if (!at)
					{
						return MakeParameterError("at", fmt::format("expected two finite numbers of at most {} in magnitude",
						                                            VisualScriptSerializer::MaxCoordinate));
					}
					request.At = *at;
				}
				if (!editors.HasNodeLibrary())
				{
					return MakeCommandError(AutomationErrorCode::Unavailable, "the node palette needs the scripting runtime");
				}
				return call;
			}

			// graph.find: the scripts searched, then FindInVisualScript in each.
			CommandResult FindInGraphs(Json const& params)
			{
				if (std::optional<CommandError> error = CheckAssets(false))
				{
					return *error;
				}
				bool const hasQuery = params.contains("query");
				if (hasQuery == params.contains("references"))
				{
					CommandError error = MakeCommandError(AutomationErrorCode::InvalidParams,
					                                      "give either query (the text to find) or references (the element whose "
					                                      "references to list)");
					error.Data = Json::object({{"path", hasQuery ? "references" : "query"}});
					return error;
				}
				std::optional<AssetHandle> only;
				if (params.contains("graph"))
				{
					CommandValue<AssetHandle> graph = ParseGraph(params["graph"]);
					if (!graph)
					{
						return graph.TakeError();
					}
					only = graph.GetValue();
				}

				VisualScriptFindQuery query;
				// Variables and functions are referred to in their own script only.
				std::optional<AssetHandle> owner;
				if (hasQuery)
				{
					query.Text = params["query"].get<std::string>();
				}
				else
				{
					Json const& references = params["references"];
					std::string const kind = references["kind"].get<std::string>();
					VisualScriptReference reference;
					if (kind == "CustomEvent")
					{
						if (!references.contains("name"))
						{
							return MakeParameterError("references.name", "a custom event's references need its name");
						}
						reference.ReferenceKind = VisualScriptReference::Kind::CustomEvent;
						reference.Name = references["name"].get<std::string>();
					}
					else
					{
						reference.ReferenceKind =
							kind == "Function" ? VisualScriptReference::Kind::Function : VisualScriptReference::Kind::Variable;
						if (!references.contains("graph"))
						{
							return MakeParameterError("references.graph", fmt::format("a {}'s references need the visual script that "
							                                                          "has it",
							                                                          kind == "Function" ? "function" : "variable"));
						}
						if (!references.contains("id"))
						{
							return MakeParameterError("references.id", fmt::format("a {}'s references need its ID",
							                                                       kind == "Function" ? "function" : "variable"));
						}
						CommandValue<AssetHandle> graph = ParseGraph(references["graph"], "references.graph");
						if (!graph)
						{
							return graph.TakeError();
						}
						Result<Ref<VisualScriptAsset>> const asset = LoadGraph(graph.GetValue());
						if (!asset)
						{
							return MakeCommandError(AutomationErrorCode::FileError,
							                        "references.graph: the visual script does not load ({})", asset.GetError());
						}
						reference.ID = GetElementId(references["id"]);
						Ref<VisualScriptDocument const> const document = asset.GetValue()->GetSharedDocument();
						if (!HasReferencedElement(*document, reference))
						{
							return MakeParameterError("references.id", fmt::format("the visual script has no {} {}",
							                                                       kind == "Function" ? "function" : "variable or local",
							                                                       references["id"].dump()));
						}
						owner = graph.GetValue();
					}
					query.References = std::move(reference);
				}

				// The scripts searched, by path (memory assets, which have none, first).
				std::vector<AssetHandle> scripts;
				if (owner)
				{
					if (!only || *only == *owner)
					{
						scripts.push_back(*owner);
					}
				}
				else if (only)
				{
					scripts.push_back(*only);
				}
				else
				{
					std::vector<AssetMetadata> all = AssetManager::GetAssets(AssetType::VisualScript);
					std::sort(all.begin(), all.end(),
					          [](AssetMetadata const& a, AssetMetadata const& b)
					          {
								  return a.Path != b.Path ? a.Path < b.Path : a.Name < b.Name;
							  });
					for (AssetMetadata const& script : all)
					{
						scripts.push_back(script.Handle);
					}
				}

				size_t const offset = GetCount(params, "offset", 0);
				size_t const limit = GetCount(params, "limit", 50);
				bool analyzed = ScriptEngine::IsInitialized();
				size_t total = 0;
				Json results = Json::array();
				Json skipped = Json::array();
				for (AssetHandle const script : scripts)
				{
					Result<Ref<VisualScriptAsset>> const asset = LoadGraph(script);
					if (!asset)
					{
						// A script asked for by name must be read; among every script, one that does not load is reported.
						if (only)
						{
							return MakeCommandError(AutomationErrorCode::FileError, "graph: the visual script does not load ({})",
							                        asset.GetError());
						}
						Json described = DescribeGraph(script);
						described["error"] = asset.GetError();
						skipped.push_back(std::move(described));
						continue;
					}
					Ref<VisualScriptDocument const> const document = asset.GetValue()->GetSharedDocument();
					std::optional<VisualScriptAnalysis> const analysis = AnalyzeGraph(*document, script);
					analyzed = analyzed && analysis.has_value();
					std::vector<VisualScriptFindResult> const found = FindInVisualScript(*document, analysis ? &*analysis : nullptr, query);
					for (VisualScriptFindResult const& match : found)
					{
						if (total >= offset && total - offset < limit)
						{
							Json row = DescribeGraph(script);
							row["function"] = match.Function;
							if (match.Node != 0)
							{
								row["node"] = match.Node;
							}
							if (match.Comment != 0)
							{
								row["comment"] = match.Comment;
							}
							if (match.Variable != 0)
							{
								row["variable"] = match.Variable;
							}
							row["field"] = GetFindFieldName(match.Field);
							row["text"] = match.Text;
							results.push_back(std::move(row));
						}
						total++;
					}
				}
				return Json::object(
					{{"results", std::move(results)}, {"total", total}, {"analyzed", analyzed}, {"skipped", std::move(skipped)}});
			}

			// Whether the document has the variable or local, or the function, whose references graph.find lists.
			static bool HasReferencedElement(VisualScriptDocument const& document, VisualScriptReference const& reference)
			{
				auto const hasID = [&reference](auto const& elements)
				{
					return std::any_of(elements.begin(), elements.end(),
					                   [&reference](auto const& element)
					                   {
										   return element.ID == reference.ID;
									   });
				};
				if (reference.ReferenceKind == VisualScriptReference::Kind::Function)
				{
					return hasID(document.Functions);
				}
				return hasID(document.Variables) || std::any_of(document.Functions.begin(), document.Functions.end(),
				                                                [&hasID](VisualScriptFunction const& function)
				                                                {
																	return hasID(function.Locals);
																});
			}

			// The node library for graph.library and graph.types: the asset checks apply, and the library needs the runtime.
			CommandValue<Ref<VisualScriptLibrary const>> GetGraphLibrary() const
			{
				if (std::optional<CommandError> error = CheckAssets(false))
				{
					return *error;
				}
				if (!ScriptEngine::IsInitialized())
				{
					return MakeCommandError(AutomationErrorCode::Unavailable,
					                        "the visual script node library needs the scripting runtime (.NET 10), which is not running");
				}
				Ref<VisualScriptLibrary const> library = VisualScriptEngine::GetLibrary();
				if (library == nullptr)
				{
					return MakeCommandError(AutomationErrorCode::Unavailable,
					                        "the scripting runtime cannot describe the visual script node library (see the log)");
				}
				return library;
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
									   // While playing, script fields are the values the running script holds, and visual script
					                   // variables the values the running graph holds.
									   (void)scene.ReadScriptFields(entity);
									   (void)scene.ReadVisualScriptVariables(entity);
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
				        "Every component type users can add, with its category (Core, Rendering, Physics, Audio, Scripting or UI) and its "
				        "fields (name, type, default value, valid range, value names, referenced asset type and description).",
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
									description["Category"] = std::string(info.Category);
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
					result
						? Add("asset.browse",
				              "Shows a folder in the editor's content browser and sets the size of its tiles: a folder of the asset "
				              "directory (root Assets, the default) or of the project's C# scripts (root Scripts: Scripts/Source), "
				              "relative to the root with forward slashes (\"\" for the root itself), or what a search of the root finds; "
				              "returns { root, folder, search, tileScale }. Changes nothing in the project. Unavailable without the "
				              "editor's UI (headless).",
				              SchemaBuilder::Object()
				                  .Property("root", SchemaBuilder::String("Which files to browse").Enum({"Assets", "Scripts"}))
				                  .Property("folder", SchemaBuilder::String("Folder relative to the root"))
				                  .Property("search",
				                            SchemaBuilder::String("Shows the root's files whose paths contain this text, ignoring "
				                                                  "case, instead of the folder; without it a search ends"))
				                  .Property("tileScale", SchemaBuilder::Number("Tile size as a multiple of the default")
				                                             .Minimum(ContentBrowserPanel::MinTileScale)
				                                             .Maximum(ContentBrowserPanel::MaxTileScale))
				                  .Build(),
				              false,
				              [this](Json const& params) -> CommandResult
				              {
								  if (!m_Environment.ContentBrowser)
								  {
									  return MakeCommandError(AutomationErrorCode::Unavailable,
						                                      "the editor has no content browser (headless)");
								  }
								  if (std::optional<CommandError> error = CheckAssets(true))
								  {
									  return *error;
								  }
								  ContentBrowserPanel& browser = m_Environment.ContentBrowser();
								  ContentRoot const root = GetBrowserRoot(params, browser);
								  CommandValue<std::string> found =
									  GetBrowserFolder(params, browser, root, m_Operations.GetContext().GetProject());
								  if (!found)
								  {
									  return found.TakeError();
								  }
								  std::string const folder = found.TakeValue();
								  if (root == ContentRoot::Assets)
								  {
									  browser.SetFolder(folder);
								  }
								  else
								  {
									  browser.SetScriptFolder(folder);
								  }
								  if (params.contains("search"))
								  {
									  browser.SetSearch(params["search"].get<std::string>());
								  }
								  if (params.contains("tileScale"))
								  {
									  browser.SetTileScale(params["tileScale"].get<float>());
								  }
								  browser.RequestFocus();
								  return Json::object({{"root", root == ContentRoot::Assets ? "Assets" : "Scripts"},
					                                   {"folder", folder},
					                                   {"search", browser.GetSearch()},
					                                   {"tileScale", browser.GetTileScale()}});
							  })
						: result;

				result =
					result
						? Add("asset.drop",
				              "Imports files and folders (absolute paths) into the project as if they were dropped onto the editor's "
				              "content browser from the OS, onto a folder of the asset directory (root Assets) or of the project's C# "
				              "scripts (root Scripts: Scripts/Source); by default the displayed one. Assets are copied into the asset "
				              "folder and registered (taken names get a number), models with the files they read (glTF buffers and "
				              "images, OBJ material libraries and their textures); C# sources go to the script folder, refused when "
				              "their file name or a type they declare is taken in the scripts; folders come whole (their C# sources to "
				              "the scripts, the rest to the assets); other files only with a model. One failure does not stop the "
				              "others. The browser shows and selects what came in and says under its toolbar what did not. Returns { "
				              "assets, scripts (relative to the project directory), folders (made in Assets), scriptFolders (made in "
				              "Scripts/Source), keptFiles (copied files that are not assets), skipped, notes, errors }. Not undoable. "
				              "Unavailable without the editor's UI (headless).",
				              SchemaBuilder::Object()
				                  .Property("paths",
				                            SchemaBuilder::Array(SchemaBuilder::String("Absolute path of a file or folder").MinLength(1),
				                                                 "Files and folders to drop")
				                                .MinItems(1),
				                            true)
				                  .Property("root", SchemaBuilder::String("Where the drop lands").Enum({"Assets", "Scripts"}))
				                  .Property("folder", SchemaBuilder::String("Folder of the root the drop lands on, relative to it"))
				                  .Build(),
				              false,
				              [this](Json const& params) -> CommandResult
				              {
								  if (!m_Environment.ContentBrowser)
								  {
									  return MakeCommandError(AutomationErrorCode::Unavailable,
						                                      "the editor has no content browser (headless)");
								  }
								  if (std::optional<CommandError> error = CheckAssets(true))
								  {
									  return *error;
								  }
								  std::vector<std::filesystem::path> paths;
								  for (Json const& path : params["paths"])
								  {
									  std::filesystem::path file = FileSystem::PathFromUtf8(path.get<std::string>());
									  // Relative to the editor's working directory, a path would name whatever happens to be there.
									  if (!file.is_absolute())
									  {
										  CommandError error = MakeCommandError(AutomationErrorCode::InvalidParams,
							                                                    "'{}' is not an absolute path", path.get<std::string>());
										  error.Data = Json::object({{"path", "paths"}});
										  return error;
									  }
									  paths.push_back(std::move(file));
								  }
								  ContentBrowserPanel& browser = m_Environment.ContentBrowser();
								  Project const* const project = m_Operations.GetContext().GetProject();
								  ContentRoot const root = GetBrowserRoot(params, browser);
								  CommandValue<std::string> folder = GetBrowserFolder(params, browser, root, project);
								  if (!folder)
								  {
									  return folder.TakeError();
								  }

								  ProjectImport::Report const report = browser.ImportDropped(m_Operations, paths, root, folder.GetValue());
								  Json scripts = Json::array();
								  for (std::filesystem::path const& script : report.Scripts)
								  {
									  scripts.push_back(project != nullptr ? FileSystem::PathToUtf8(FileSystem::GetRelativePath(
																				 script, project->GetDirectory()))
						                                                   : FileSystem::PathToUtf8(script));
								  }
								  return Json::object({{"assets", DescribeAssets(report.Assets)},
					                                   {"scripts", std::move(scripts)},
					                                   {"folders", report.Folders},
					                                   {"scriptFolders", report.ScriptFolders},
					                                   {"keptFiles", report.KeptFiles},
					                                   {"skipped", report.Skipped},
					                                   {"notes", report.Notes},
					                                   {"errors", report.Errors}});
							  })
						: result;

				result =
					result
						? Add("inspector.add-component-menu",
				              "Opens the inspector's Add Component menu for the selected entities, as if the user clicked Add Component "
				              "and typed the search text (without one the menu shows the component categories, and category opens one "
				              "of their submenus as if the user pointed at it), or closes it (open false). Returns { open, search, "
				              "category, components }: the components the menu offers, in its order (those of the open submenu with a "
				              "category). Changes nothing in the scene (component.add adds components). InvalidParams for a search with "
				              "a category; InvalidOperation without selected entities, for a category the menu offers no components of "
				              "and while a dialog is open; Unavailable without the editor's UI (headless).",
				              SchemaBuilder::Object()
				                  .Property("search", SchemaBuilder::String("Text typed into the menu's search field"))
				                  .Property("category", SchemaBuilder::String("Without search text: the category whose submenu opens")
				                                            .Enum(GetAddableCategories()))
				                  .Property("open", SchemaBuilder::Boolean("False closes the menu").Default(true))
				                  .Build(),
				              false,
				              [this](Json const& params) -> CommandResult
				              {
								  if (!m_Environment.ShowAddComponentMenu)
								  {
									  return MakeCommandError(AutomationErrorCode::Unavailable, "the editor has no inspector (headless)");
								  }
								  bool const open = GetBool(params, "open", true);
								  std::string const search = open ? GetString(params, "search") : std::string();
								  std::string const category = open ? GetString(params, "category") : std::string();
								  bool const searching = search.find_first_not_of(' ') != std::string::npos;
								  if (searching && !category.empty())
								  {
									  CommandError error = MakeCommandError(
										  AutomationErrorCode::InvalidParams,
										  "a search lists the matching components instead of the categories: give search or category");
									  error.Data = Json::object({{"path", "category"}});
									  return error;
								  }
								  EditorContext& context = m_Operations.GetContext();
								  std::vector<UUID> const& entities = context.GetSelection().GetEntities();
								  if (open && entities.empty())
								  {
									  return MakeCommandError(
										  AutomationErrorCode::InvalidOperation,
										  "no entities are selected: the menu adds components to the selected entities");
								  }
								  if (open)
								  {
									  if (std::optional<CommandError> dialog = CheckNoDialog())
									  {
										  return *dialog;
									  }
								  }

								  // What the menu lists: the categories' components in order (one category's with its submenu
					              // open), or the matches of the search.
								  Json components = Json::array();
								  if (open)
								  {
									  std::vector<ComponentInfo const*> const addable =
										  ComponentInspection::GetAddableComponents(context.GetScene(), entities);
									  if (!searching)
									  {
										  for (ComponentInspection::ComponentGroup const& group :
							                   ComponentInspection::GroupByCategory(addable))
										  {
											  if (!category.empty() && group.Category != category)
											  {
												  continue;
											  }
											  for (ComponentInfo const* component : group.Components)
											  {
												  components.push_back(std::string(component->Name));
											  }
										  }
										  if (!category.empty() && components.empty())
										  {
											  return MakeCommandError(
												  AutomationErrorCode::InvalidOperation,
												  "the menu offers no {} components: the selected entities have them all", category);
										  }
									  }
									  else
									  {
										  for (ComponentInfo const* component : ComponentInspection::SearchComponents(addable, search))
										  {
											  components.push_back(std::string(component->Name));
										  }
									  }
								  }
								  m_Environment.ShowAddComponentMenu(open, search, category);
								  return Json::object(
									  {{"open", open}, {"search", search}, {"category", category}, {"components", std::move(components)}});
							  })
						: result;

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
				              "What the scene view's renderer drew in the last frame it rendered (the scene through the editor "
				              "camera): gpuMilliseconds (the GPU time of its latest finished frame; null before one finished or when the "
				              "GPU cannot measure time), drawCalls (shadow-map draws included), triangles, shadowDrawCalls, shadowMapViews, "
				              "shadowsDropped (shadow-casting lights beyond the shadow-map budget), lights, culledLights (point and spot "
				              "lights out of the view's reach), culled (submeshes outside the view), quads (sprites, text glyphs and UI "
				              "rectangles). Unavailable without a viewport (headless) or a GPU.",
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
				result =
					result
						? Add("viewport.frame",
				              "Points the editor viewport's camera at entities, framing their meshes (canvases and UI elements: their "
				              "rectangles where the scene view draws the UI; other entities without a mesh: their origins) from the "
				              "current direction, as Focus in the hierarchy does. Returns the view like viewport.camera. Not undoable; "
				              "Unavailable without a viewport (headless).",
				              SchemaBuilder::Object()
				                  .Property("entities", SchemaBuilder::Array(EntityIdSchema("Entity ID"), "Entities to frame").MinItems(1),
				                            true)
				                  .Build(),
				              false,
				              [this](Json const& params) -> CommandResult
				              {
								  if (!m_Environment.ViewportCamera)
								  {
									  return MakeCommandError(AutomationErrorCode::Unavailable, "the editor has no viewport (headless)");
								  }
								  CommandValue<std::vector<UUID>> ids = ParseExistingEntities(m_Operations, params["entities"], "entities");
								  if (!ids)
								  {
									  return ids.TakeError();
								  }
								  EditorCamera& camera = m_Environment.ViewportCamera();
								  camera.Focus(ComputeEntityBounds(Context().GetScene(), ids.GetValue()));
								  return DescribeCamera(camera);
							  })
						: result;
				auto const shapeSchema = [](std::string_view description)
				{
					return SchemaBuilder::String(description).Enum({"off", "selected", "all"});
				};
				return result
				           ? Add("viewport.overlays",
				                 "What the editor viewport draws over the scene: sets the given overlays (the others stay) and returns "
				                 "all of them: { grid, selectionOutline, icons (light and camera gizmos), cameras (frustums), lights "
				                 "(directional light rays, point light ranges, spot light cones), colliders (with character capsules), "
				                 "joints, volumes (boxes of local post-process volumes), debugDrawing (lines scripts draw), canvases (the "
				                 "UI of each canvas in its entity's plane, with its outline) }; the shape kinds are \"off\", \"selected\" "
				                 "(for the selected entities) or \"all\" (for every entity). Changes nothing in the scene. Not undoable; "
				                 "Unavailable without a viewport (headless).",
				                 SchemaBuilder::Object()
				                     .Property("grid", SchemaBuilder::Boolean("The ground grid"))
				                     .Property("selectionOutline", SchemaBuilder::Boolean("The outline of selected meshes"))
				                     .Property("icons", SchemaBuilder::Boolean("Billboard icons of cameras and lights"))
				                     .Property("cameras", shapeSchema("Camera frustums"))
				                     .Property("lights", shapeSchema("Directional light rays, point light ranges and spot light cones"))
				                     .Property("colliders", shapeSchema("Collider outlines and character capsules"))
				                     .Property("joints", shapeSchema("Joint anchors, axes and connections"))
				                     .Property("volumes", shapeSchema("Boxes of local post-process volumes and where their blending ends"))
				                     .Property("debugDrawing", SchemaBuilder::Boolean("Lines scripts draw (Debug.DrawLine)"))
				                     .Property("canvases",
				                               SchemaBuilder::Boolean("The UI of each canvas in its entity's plane, with its outline"))
				                     .Build(),
				                 false,
				                 [this](Json const& params) -> CommandResult
				                 {
									 if (!m_Environment.ViewportOverlays)
									 {
										 return MakeCommandError(AutomationErrorCode::Unavailable, "the editor has no viewport (headless)");
									 }
									 SceneViewOverlays& overlays = m_Environment.ViewportOverlays();
									 auto const setFlag = [&params](char const* name, bool& value)
									 {
										 if (params.contains(name))
										 {
											 value = params[name].get<bool>();
										 }
									 };
									 auto const setShapes = [&params](char const* name, OverlayShapeSetting& setting)
									 {
										 if (params.contains(name))
										 {
											 std::string const value = params[name].get<std::string>();
											 setting.Enabled = value != "off";
											 // "off" keeps the choice between the selection and every entity for when it is on again.
											 if (setting.Enabled)
											 {
												 setting.AllEntities = value == "all";
											 }
										 }
									 };
									 setFlag("grid", overlays.Grid);
									 setFlag("selectionOutline", overlays.SelectionOutline);
									 setFlag("icons", overlays.Icons);
									 setShapes("cameras", overlays.Cameras);
									 setShapes("lights", overlays.Lights);
									 setShapes("colliders", overlays.Colliders);
									 setShapes("joints", overlays.Joints);
									 setShapes("volumes", overlays.Volumes);
									 setFlag("debugDrawing", overlays.DebugLines);
									 setFlag("canvases", overlays.Canvases);
									 return DescribeOverlays(overlays);
								 })
				           : result;
			}

			static Json DescribeOverlays(SceneViewOverlays const& overlays)
			{
				auto const shapes = [](OverlayShapeSetting const& setting)
				{
					return !setting.Enabled ? "off" : (setting.AllEntities ? "all" : "selected");
				};
				return Json::object({{"grid", overlays.Grid},
				                     {"selectionOutline", overlays.SelectionOutline},
				                     {"icons", overlays.Icons},
				                     {"cameras", shapes(overlays.Cameras)},
				                     {"lights", shapes(overlays.Lights)},
				                     {"colliders", shapes(overlays.Colliders)},
				                     {"joints", shapes(overlays.Joints)},
				                     {"volumes", shapes(overlays.Volumes)},
				                     {"debugDrawing", overlays.DebugLines},
				                     {"canvases", overlays.Canvases}});
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
				Entity const entity = m_Operations.FindEntity(id);
				// While playing, script fields are the values the running script holds, and visual script variables the values
				// the running graph holds.
				if (info.Name == ComponentTraits<ScriptComponent>::Name)
				{
					(void)scene.ReadScriptFields(entity);
				}
				else if (info.Name == ComponentTraits<VisualScriptComponent>::Name)
				{
					(void)scene.ReadVisualScriptVariables(entity);
				}
				return Json::object(
					{{"component", std::string(info.Name)}, {"fields", info.Serialize(scene.GetRegistry(), entity.GetHandle())}});
			}

			CommandRegistry& m_Registry;
			EditorOperations& m_Operations;
			EditorCommandEnvironment m_Environment;
			// The scripting runtime's library and analyses, for graph.edit's batches.
			EngineVisualScriptSemantics m_Semantics;
			// The analysis of a graph's stored document, kept for graph.edit's New, refuseNewErrors and a Fix before any change,
			// so a chain of edits analyzes each document it writes once.
			struct StoredGraphAnalysis
			{
				std::weak_ptr<VisualScriptDocument const> Document;
				std::weak_ptr<VisualScriptLibrary const> Library;
				Ref<VisualScriptAnalysis const> Analysis;
			};
			std::unordered_map<AssetHandle, StoredGraphAnalysis> m_StoredGraphAnalyses;
		};
	}

	Result<void> RegisterEditorCommands(CommandRegistry& registry, EditorOperations& operations, EditorCommandEnvironment environment)
	{
		return CreateRef<CommandSet>(registry, operations, std::move(environment))->RegisterAll();
	}

	Json ListVisualScriptLibraryNotes(VisualScriptLibrary const& library, std::string_view query, std::string_view category)
	{
		Json notes = Json::array();
		for (VisualScriptLibraryNote const& note : library.GetNotes())
		{
			if (IsInCategory(note.Category, category) &&
			    (query.empty() || ContainsIgnoringCase(note.Member, query) || ContainsIgnoringCase(note.Message, query)))
			{
				notes.push_back(Json::object({{"member", note.Member}, {"category", note.Category}, {"message", note.Message}}));
			}
		}
		return notes;
	}
}
