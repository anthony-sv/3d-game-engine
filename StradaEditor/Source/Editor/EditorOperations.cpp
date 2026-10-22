#include "Editor/EditorOperations.h"

#include "Editor/AssetBrowsing.h"
#include "Editor/Commands/AssetCommands.h"
#include "Editor/Commands/ComponentCommands.h"
#include "Editor/Commands/CompositeCommand.h"
#include "Editor/Commands/PrefabCommands.h"
#include "Editor/Commands/SceneCommands.h"
#include "Editor/FileCopies.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/AssetRegistry.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/MeshImporter.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"
#include "Strada/Scene/SceneSerializer.h"

#include <algorithm>
#include <system_error>
#include <utility>

namespace Strada
{
	namespace
	{
		Result<void> RequireAssetDirectory()
		{
			if (!AssetManager::IsInitialized() || !AssetManager::HasAssetDirectory())
			{
				return Error{"no project is open"};
			}
			return {};
		}

		// A folder of the asset directory in normalized form: "" is the directory itself.
		Result<std::string> NormalizeAssetFolder(std::string_view folder)
		{
			return folder.empty() ? Result<std::string>(std::string()) : NormalizeAssetPath(folder);
		}

		// Scene paths are kept absolute so saving later does not depend on the working directory at that time.
		std::filesystem::path MakeAbsolute(std::filesystem::path const& path)
		{
			std::error_code errorCode;
			std::filesystem::path const absolute = std::filesystem::absolute(path, errorCode);
			return errorCode ? path.lexically_normal() : absolute.lexically_normal();
		}
	}

	EditorOperations::EditorOperations(EditorContext& context)
		: m_Context(context)
	{
	}

	Entity EditorOperations::FindEntity(UUID id)
	{
		return m_Context.GetScene().GetEntityByUUID(id);
	}

	Result<void> EditorOperations::CreateProject(std::filesystem::path const& directory, std::string const& name, Scene const& startScene)
	{
		if (Result<void> notPlaying = CheckNotPlaying(); !notPlaying)
		{
			return notPlaying;
		}
		Result<Ref<Project>> created = Project::Create(directory, name, startScene);
		if (!created)
		{
			KeepProjectIfOpen();
			return Error{created.GetError()};
		}
		Result<std::vector<std::string>> opened = UseProject(created.TakeValue());
		return opened ? Result<void>() : Result<void>(Error{opened.GetError()});
	}

	Result<std::vector<std::string>> EditorOperations::OpenProject(std::filesystem::path const& file)
	{
		if (Result<void> notPlaying = CheckNotPlaying(); !notPlaying)
		{
			return Error{notPlaying.GetError()};
		}
		std::vector<std::string> warnings;
		Result<Ref<Project>> opened = Project::Open(file, &warnings);
		if (!opened)
		{
			KeepProjectIfOpen();
			return Error{opened.GetError()};
		}
		for (std::string const& warning : warnings)
		{
			ST_WARN("'{}': {}", FileSystem::PathToUtf8(opened.GetValue()->GetFilePath()), warning);
		}
		Result<std::vector<std::string>> used = UseProject(opened.TakeValue());
		if (used)
		{
			warnings.insert(warnings.end(), used.GetValue().begin(), used.GetValue().end());
		}
		return used ? Result<std::vector<std::string>>(std::move(warnings)) : used;
	}

	Result<void> EditorOperations::CloseProject()
	{
		if (Result<void> notPlaying = CheckNotPlaying(); !notPlaying)
		{
			return notPlaying;
		}
		if (m_Context.GetProject() != nullptr && AssetManager::IsInitialized())
		{
			AssetManager::CloseAssetDirectory();
		}
		m_Context.SetProject(nullptr);
		m_Context.SelectAsset(AssetHandle());
		ResetScene("Untitled", false);
		return {};
	}

	Result<void> EditorOperations::ApplyProjectSettings(Json const& patch)
	{
		Project* project = m_Context.GetProject();
		if (project == nullptr)
		{
			return Error{"no project is open"};
		}
		return project->ApplySettings(patch);
	}

	Result<void> EditorOperations::SaveProject()
	{
		Project const* project = m_Context.GetProject();
		if (project == nullptr)
		{
			return Error{"no project is open"};
		}
		return project->Save();
	}

	Result<std::vector<std::string>> EditorOperations::UseProject(Ref<Project> project)
	{
		ST_INFO("Opened project '{}'", FileSystem::PathToUtf8(project->GetFilePath()));
		AssetHandle const startScene = project->GetSettings().StartScene;
		m_Context.SetProject(std::move(project));

		// The steps of asset files in the history name the assets of the project before.
		std::vector<std::string> warnings;
		if (!startScene.IsValid())
		{
			ResetScene("Untitled", false);
			return warnings;
		}
		std::filesystem::path const scenePath = AssetManager::GetAbsolutePath(startScene);
		Result<std::vector<std::string>> opened = scenePath.empty()
		                                              ? Result<std::vector<std::string>>(Error{"it is not a scene file of the project"})
		                                              : LoadScene(scenePath, false);
		if (!opened)
		{
			// The project stays usable: its settings can point at another scene.
			std::string warning = fmt::format("the start scene could not be opened: {}", opened.GetError());
			ST_WARN("{}", warning);
			warnings.push_back(std::move(warning));
			ResetScene("Untitled", false);
			return warnings;
		}
		return opened.TakeValue();
	}

	void EditorOperations::KeepProjectIfOpen()
	{
		// A failed open may have closed the previous project's asset directory; the project cannot stay open without it.
		Project const* project = m_Context.GetProject();
		bool const assetsOpen = project != nullptr && AssetManager::IsInitialized() && AssetManager::HasAssetDirectory() &&
		                        AssetManager::GetAssetDirectory() == project->GetAssetDirectory();
		if (project != nullptr && !assetsOpen)
		{
			m_Context.SetProject(nullptr);
			ResetScene("Untitled", false);
		}
	}

	Result<void> EditorOperations::CheckNotPlaying() const
	{
		if (m_Context.IsPlaying())
		{
			return Error{"the editor is playing: stop playing first"};
		}
		return {};
	}

	void EditorOperations::ResetScene(std::string name, bool keepAssetSteps)
	{
		m_Context.SetScene(CreateRef<Scene>(std::move(name)), {}, keepAssetSteps);
	}

	Result<void> EditorOperations::NewScene(std::string name)
	{
		if (Result<void> notPlaying = CheckNotPlaying(); !notPlaying)
		{
			return notPlaying;
		}
		ResetScene(std::move(name), true);
		return {};
	}

	Result<std::vector<std::string>> EditorOperations::OpenScene(std::filesystem::path const& path)
	{
		if (Result<void> notPlaying = CheckNotPlaying(); !notPlaying)
		{
			return Error{notPlaying.GetError()};
		}
		return LoadScene(path, true);
	}

	Result<std::vector<std::string>> EditorOperations::LoadScene(std::filesystem::path const& path, bool keepAssetSteps)
	{
		std::vector<std::string> warnings;
		DeserializationContext context = m_Context.CreateDeserializationContext(UnknownFieldPolicy::Warn);
		context.Warnings = &warnings;
		Result<Ref<Scene>> scene = SceneSerializer::LoadFromFile(path, context);
		if (!scene)
		{
			return Error{scene.GetError()};
		}

		std::filesystem::path absolute = MakeAbsolute(path);
		for (std::string const& warning : warnings)
		{
			ST_WARN("'{}': {}", FileSystem::PathToUtf8(absolute), warning);
		}
		ST_INFO("Opened scene '{}'", FileSystem::PathToUtf8(absolute));
		m_Context.SetScene(scene.TakeValue(), std::move(absolute), keepAssetSteps);
		return warnings;
	}

	Result<void> EditorOperations::SaveScene(std::filesystem::path const& path)
	{
		if (Result<void> notPlaying = CheckNotPlaying(); !notPlaying)
		{
			return notPlaying;
		}
		std::filesystem::path target = path.empty() ? m_Context.GetScenePath() : MakeAbsolute(path);
		if (target.empty())
		{
			return Error{"the scene has never been saved, so a file path is required"};
		}
		if (Result<void> result = SceneSerializer::SaveToFile(m_Context.GetScene(), target); !result)
		{
			return result;
		}
		ST_INFO("Saved scene '{}'", FileSystem::PathToUtf8(target));
		if (AssetManager::IsInitialized() && AssetManager::HasAssetDirectory() &&
		    FileSystem::IsInside(target, AssetManager::GetAssetDirectory()))
		{
			// Registered scenes can be referenced (the project's start scene) and listed by the content browser.
			if (Result<AssetHandle> imported = AssetManager::ImportFile(target); !imported)
			{
				ST_WARN("'{}' was saved but not registered as an asset: {}", FileSystem::PathToUtf8(target), imported.GetError());
			}
		}
		m_Context.MarkSaved(std::move(target));
		return {};
	}

	Result<void> EditorOperations::SetSceneProperties(std::optional<std::string> name, Json const& settingsPatch, uint64_t mergeKey)
	{
		if (name && name->empty())
		{
			return Error{"the scene name cannot be empty"};
		}
		return m_Context.ExecuteCommand(CreateScope<SetScenePropertiesCommand>(std::move(name), settingsPatch, mergeKey));
	}

	Result<UUID> EditorOperations::CreateEntity(EntityCreateInfo info)
	{
		UUID const id = m_Context.GetScene().GenerateUniqueID();
		if (Result<void> result = m_Context.ExecuteCommand(CreateScope<CreateEntityCommand>(id, std::move(info))); !result)
		{
			return Error{result.GetError()};
		}
		return id;
	}

	Result<std::vector<UUID>> EditorOperations::CreateEntityWithChildren(EntityCreateInfo root, std::vector<EntityCreateInfo> children)
	{
		Scene const& scene = m_Context.GetScene();
		std::vector<UUID> ids;
		ids.reserve(children.size() + 1);
		while (ids.size() < children.size() + 1)
		{
			UUID const id = scene.GenerateUniqueID();
			if (std::find(ids.begin(), ids.end(), id) == ids.end())
			{
				ids.push_back(id);
			}
		}
		std::vector<Scope<EditorCommand>> commands;
		commands.reserve(ids.size());
		commands.push_back(CreateScope<CreateEntityCommand>(ids[0], std::move(root)));
		for (size_t i = 0; i < children.size(); i++)
		{
			children[i].Parent = ids[0];
			commands.push_back(CreateScope<CreateEntityCommand>(ids[i + 1], std::move(children[i])));
		}
		if (Result<void> result = m_Context.ExecuteCommand(CreateScope<CompositeCommand>("Create Entity", std::move(commands))); !result)
		{
			return Error{result.GetError()};
		}
		return ids;
	}

	Result<void> EditorOperations::DeleteEntities(std::span<UUID const> entities)
	{
		return m_Context.ExecuteCommand(CreateScope<DeleteEntitiesCommand>(std::vector<UUID>(entities.begin(), entities.end())));
	}

	Result<std::vector<UUID>> EditorOperations::DuplicateEntities(std::span<UUID const> entities)
	{
		Scope<DuplicateEntitiesCommand> command =
			CreateScope<DuplicateEntitiesCommand>(std::vector<UUID>(entities.begin(), entities.end()));
		// After a successful execution the history owns the command as its newest step (duplication never merges), so
		// it is still alive when the copies are read.
		DuplicateEntitiesCommand const& executed = *command;
		if (Result<void> result = m_Context.ExecuteCommand(std::move(command)); !result)
		{
			return Error{result.GetError()};
		}
		return executed.GetCopies();
	}

	Result<void> EditorOperations::RenameEntity(UUID entity, std::string name, uint64_t mergeKey)
	{
		if (name.empty())
		{
			return Error{"the entity name cannot be empty"};
		}
		std::vector<UUID> const entities = {entity};
		return RenameEntities(entities, std::move(name), mergeKey);
	}

	Result<void> EditorOperations::RenameEntities(std::span<UUID const> entities, std::string name, uint64_t mergeKey)
	{
		if (name.empty())
		{
			return Error{"the entity name cannot be empty"};
		}
		std::vector<ComponentEdit> edits;
		edits.reserve(entities.size());
		for (UUID const entity : entities)
		{
			edits.push_back({entity, std::string(ComponentTraits<TagComponent>::Name), Json::object({{"Tag", name}})});
		}
		std::string description = edits.size() == 1 ? "Rename Entity" : "Rename Entities";
		return m_Context.ExecuteCommand(CreateScope<SetComponentsCommand>(std::move(edits), mergeKey, std::move(description)));
	}

	Result<void> EditorOperations::ReparentEntity(UUID entity, UUID newParent, std::optional<size_t> siblingIndex, bool keepWorldTransform)
	{
		return m_Context.ExecuteCommand(CreateScope<ReparentEntityCommand>(entity, newParent, siblingIndex, keepWorldTransform));
	}

	Result<void> EditorOperations::MoveEntities(std::span<UUID const> entities, UUID newParent, UUID insertBefore, bool keepWorldTransform)
	{
		return m_Context.ExecuteCommand(CreateScope<MoveEntitiesCommand>(std::vector<UUID>(entities.begin(), entities.end()), newParent,
		                                                                 insertBefore, keepWorldTransform));
	}

	Result<void> EditorOperations::AddComponent(UUID entity, std::string_view component, Json const& fields)
	{
		return m_Context.ExecuteCommand(CreateScope<AddComponentCommand>(entity, std::string(component), fields));
	}

	Result<void> EditorOperations::AddComponent(std::span<UUID const> entities, std::string_view component, Json const& fields)
	{
		std::vector<Scope<EditorCommand>> commands;
		commands.reserve(entities.size());
		for (UUID const entity : entities)
		{
			commands.push_back(CreateScope<AddComponentCommand>(entity, std::string(component), fields));
		}
		return m_Context.ExecuteCommand(CreateScope<CompositeCommand>(fmt::format("Add {} Component", component), std::move(commands)));
	}

	Result<void> EditorOperations::RemoveComponent(UUID entity, std::string_view component)
	{
		return m_Context.ExecuteCommand(CreateScope<RemoveComponentCommand>(entity, std::string(component)));
	}

	Result<void> EditorOperations::RemoveComponent(std::span<UUID const> entities, std::string_view component)
	{
		std::vector<Scope<EditorCommand>> commands;
		commands.reserve(entities.size());
		for (UUID const entity : entities)
		{
			commands.push_back(CreateScope<RemoveComponentCommand>(entity, std::string(component)));
		}
		return m_Context.ExecuteCommand(CreateScope<CompositeCommand>(fmt::format("Remove {} Component", component), std::move(commands)));
	}

	Result<void> EditorOperations::SetComponentFields(UUID entity, std::string_view component, Json const& patch, uint64_t mergeKey)
	{
		std::vector<ComponentEdit> edits;
		edits.push_back({entity, std::string(component), patch});
		return SetComponentFields(std::move(edits), mergeKey);
	}

	Result<void> EditorOperations::SetComponentFields(std::vector<ComponentEdit> edits, uint64_t mergeKey)
	{
		// While scripts run, edits of their fields (and of the variables of visual scripts) start from the values the scripts
		// hold: a value set always reaches the script, and undoing the edit gives back what the script had.
		Scene& scene = m_Context.GetScene();
		for (ComponentEdit const& edit : edits)
		{
			// Entities that run no script keep their stored values.
			if (edit.Component == ComponentTraits<ScriptComponent>::Name)
			{
				(void)scene.ReadScriptFields(scene.GetEntityByUUID(edit.Entity));
			}
			else if (edit.Component == ComponentTraits<VisualScriptComponent>::Name)
			{
				(void)scene.ReadVisualScriptVariables(scene.GetEntityByUUID(edit.Entity));
			}
		}
		return m_Context.ExecuteCommand(CreateScope<SetComponentsCommand>(std::move(edits), mergeKey));
	}

	Result<void> EditorOperations::SetScriptClass(std::span<UUID const> entities, std::string_view className)
	{
		if (className.empty())
		{
			return Error{"a script class name is required"};
		}
		Scene& scene = m_Context.GetScene();
		Json const fields = Json::object({{"ClassName", std::string(className)}});
		std::string const component(ComponentTraits<ScriptComponent>::Name);
		std::vector<Scope<EditorCommand>> commands;
		std::vector<ComponentEdit> replaced;
		for (UUID const entity : entities)
		{
			if (!scene.HasEntity(entity))
			{
				return MakeError("entity {} does not exist", entity);
			}
			Entity const target = scene.GetEntityByUUID(entity);
			if (!target.HasComponent<ScriptComponent>())
			{
				commands.push_back(CreateScope<AddComponentCommand>(entity, component, fields));
			}
			else if (target.GetComponent<ScriptComponent>().ClassName != className)
			{
				replaced.push_back({entity, component, fields});
			}
		}
		if (!replaced.empty())
		{
			commands.push_back(CreateScope<SetComponentsCommand>(std::move(replaced)));
		}
		if (commands.empty())
		{
			return {};
		}
		return m_Context.ExecuteCommand(CreateScope<CompositeCommand>(fmt::format("Set Script {}", className), std::move(commands)));
	}

	Result<void> EditorOperations::SetVisualScriptGraph(std::span<UUID const> entities, AssetHandle graph)
	{
		if (!graph.IsValid())
		{
			return Error{"a visual script is required"};
		}
		Scene& scene = m_Context.GetScene();
		Json const fields = Json::object({{"Graph", graph.ToString()}});
		std::string const component(ComponentTraits<VisualScriptComponent>::Name);
		std::vector<Scope<EditorCommand>> commands;
		std::vector<ComponentEdit> replaced;
		for (UUID const entity : entities)
		{
			if (!scene.HasEntity(entity))
			{
				return MakeError("entity {} does not exist", entity);
			}
			Entity const target = scene.GetEntityByUUID(entity);
			if (!target.HasComponent<VisualScriptComponent>())
			{
				commands.push_back(CreateScope<AddComponentCommand>(entity, component, fields));
			}
			else if (target.GetComponent<VisualScriptComponent>().Graph != graph)
			{
				// Only the graph changes: the values the entity gives the variables stay for the variables of the same names.
				replaced.push_back({entity, component, fields});
			}
		}
		if (!replaced.empty())
		{
			commands.push_back(CreateScope<SetComponentsCommand>(std::move(replaced)));
		}
		if (commands.empty())
		{
			return {};
		}
		std::optional<AssetMetadata> const metadata = AssetManager::IsInitialized() ? AssetManager::GetMetadata(graph) : std::nullopt;
		std::string const name = metadata ? metadata->GetDisplayName() : graph.ToString();
		return m_Context.ExecuteCommand(CreateScope<CompositeCommand>(fmt::format("Set Visual Script {}", name), std::move(commands)));
	}

	Result<void> EditorOperations::CreateAssetFolder(std::string_view folder)
	{
		if (Result<void> available = RequireAssetDirectory(); !available)
		{
			return available;
		}
		if (folder.empty())
		{
			return Error{"a folder name is required"};
		}
		Result<std::string> normalized = NormalizeAssetPath(folder);
		if (!normalized)
		{
			return Error{normalized.GetError()};
		}
		std::filesystem::path const path = AssetManager::GetAssetDirectory() / FileSystem::PathFromUtf8(normalized.GetValue());
		if (FileSystem::Exists(path))
		{
			return MakeError("'{}' already exists", normalized.GetValue());
		}
		return FileSystem::CreateDirectories(path);
	}

	Result<AssetHandle> EditorOperations::CreateMaterial(std::string_view path, Json const& fields)
	{
		if (Result<void> available = RequireAssetDirectory(); !available)
		{
			return Error{available.GetError()};
		}
		Result<std::string> normalized = NormalizeAssetPath(path);
		if (!normalized)
		{
			return Error{normalized.GetError()};
		}
		std::filesystem::path const file = AssetManager::GetAssetDirectory() / FileSystem::PathFromUtf8(normalized.GetValue());
		if (GetAssetTypeForExtension(FileSystem::PathToUtf8(file.extension())) != AssetType::Material)
		{
			return MakeError("material files end with .smat, got '{}'", normalized.GetValue());
		}
		if (FileSystem::Exists(file))
		{
			return MakeError("'{}' already exists", normalized.GetValue());
		}
		MaterialData data;
		if (Result<void> read =
		        DeserializeFields<StructTraits<MaterialData>>(fields, data, AssetManager::CreateDeserializationContext(), "material");
		    !read)
		{
			return Error{read.GetError()};
		}
		if (Result<void> created = FileSystem::CreateDirectories(file.parent_path()); !created)
		{
			return Error{created.GetError()};
		}
		if (Result<void> saved = MaterialSerializer::SaveToFile(data, file); !saved)
		{
			return Error{saved.GetError()};
		}
		Result<AssetHandle> imported = AssetManager::ImportFile(file);
		if (!imported)
		{
			(void)FileSystem::Remove(file);
		}
		return imported;
	}

	Result<AssetHandle> EditorOperations::CreateVisualScript(std::string_view path, VisualScriptTemplate kind)
	{
		if (Result<void> available = RequireAssetDirectory(); !available)
		{
			return Error{available.GetError()};
		}
		Result<std::string> normalized = NormalizeAssetPath(path);
		if (!normalized)
		{
			return Error{normalized.GetError()};
		}
		std::filesystem::path const file = AssetManager::GetAssetDirectory() / FileSystem::PathFromUtf8(normalized.GetValue());
		if (GetAssetTypeForExtension(FileSystem::PathToUtf8(file.extension())) != AssetType::VisualScript)
		{
			return MakeError("visual script files end with .sgraph, got '{}'", normalized.GetValue());
		}
		if (FileSystem::Exists(file))
		{
			return MakeError("'{}' already exists", normalized.GetValue());
		}
		if (Result<void> created = FileSystem::CreateDirectories(file.parent_path()); !created)
		{
			return Error{created.GetError()};
		}
		if (Result<void> saved = VisualScriptSerializer::SaveToFile(MakeVisualScriptTemplate(kind), file); !saved)
		{
			return Error{saved.GetError()};
		}
		Result<AssetHandle> imported = AssetManager::ImportFile(file);
		if (!imported)
		{
			(void)FileSystem::Remove(file);
		}
		return imported;
	}

	Result<AssetHandle> EditorOperations::CreatePrefab(UUID entity, std::string_view path)
	{
		if (Result<void> available = RequireAssetDirectory(); !available)
		{
			return Error{available.GetError()};
		}
		Entity const root = m_Context.GetScene().GetEntityByUUID(entity);
		if (!root)
		{
			return MakeError("entity {} does not exist", entity);
		}
		Result<std::string> normalized = NormalizeAssetPath(path);
		if (!normalized)
		{
			return Error{normalized.GetError()};
		}
		std::filesystem::path const file = AssetManager::GetAssetDirectory() / FileSystem::PathFromUtf8(normalized.GetValue());
		if (GetAssetTypeForExtension(FileSystem::PathToUtf8(file.extension())) != AssetType::Prefab)
		{
			return MakeError("prefab files end with .sprefab, got '{}'", normalized.GetValue());
		}
		if (FileSystem::Exists(file))
		{
			return MakeError("'{}' already exists", normalized.GetValue());
		}
		if (Result<void> saved = PrefabSerializer::SaveToFile(m_Context.GetScene(), root, file); !saved)
		{
			return Error{saved.GetError()};
		}
		Result<AssetHandle> imported = AssetManager::ImportFile(file);
		if (!imported)
		{
			(void)FileSystem::Remove(file);
		}
		return imported;
	}

	Result<AssetHandle> EditorOperations::CreatePrefabInFolder(UUID entity, std::string_view folder)
	{
		if (Result<void> available = RequireAssetDirectory(); !available)
		{
			return Error{available.GetError()};
		}
		Entity const root = m_Context.GetScene().GetEntityByUUID(entity);
		if (!root)
		{
			return MakeError("entity {} does not exist", entity);
		}
		std::string name = FileSystem::MakePortableFileName(root.GetName());
		if (name.empty())
		{
			name = "Prefab";
		}
		return CreatePrefab(entity, AssetBrowsing::JoinPath(folder, AssetBrowsing::MakeUniqueName(folder, name, ".sprefab")));
	}

	Result<UUID> EditorOperations::InstantiatePrefab(AssetHandle prefab, UUID parent, std::optional<glm::vec3> position)
	{
		if (AssetManager::GetAssetType(prefab) != AssetType::Prefab)
		{
			return MakeError("asset {} is not a prefab", prefab);
		}
		Scope<InstantiatePrefabCommand> command = CreateScope<InstantiatePrefabCommand>(prefab, parent, position);
		// After a successful execution the history owns the command as its newest step (instantiation never merges).
		InstantiatePrefabCommand const& executed = *command;
		if (Result<void> result = m_Context.ExecuteCommand(std::move(command)); !result)
		{
			return Error{result.GetError()};
		}
		return executed.GetRoot();
	}

	Result<std::vector<AssetHandle>> EditorOperations::ImportAssets(std::span<std::filesystem::path const> files, std::string_view folder)
	{
		if (Result<void> available = RequireAssetDirectory(); !available)
		{
			return Error{available.GetError()};
		}
		Result<std::string> destination = NormalizeAssetFolder(folder);
		if (!destination)
		{
			return Error{destination.GetError()};
		}
		std::filesystem::path const destinationPath = AssetManager::GetAssetDirectory() / FileSystem::PathFromUtf8(destination.GetValue());
		for (std::filesystem::path const& file : files)
		{
			if (!FileSystem::IsRegularFile(file))
			{
				return MakeError("'{}' is not a file", FileSystem::PathToUtf8(file));
			}
			if (GetAssetTypeForExtension(FileSystem::PathToUtf8(file.extension())) == AssetType::None)
			{
				return MakeError("'{}' is not a supported asset type", FileSystem::PathToUtf8(file.filename()));
			}
		}
		if (Result<void> created = FileSystem::CreateDirectories(destinationPath); !created)
		{
			return Error{created.GetError()};
		}

		// All or nothing: copies made before a failure are removed again.
		std::vector<AssetHandle> handles;
		std::vector<AssetHandle> copies;
		auto const rollBack = [&copies]
		{
			for (AssetHandle const copy : copies)
			{
				(void)AssetManager::DeleteAsset(copy);
			}
		};
		for (std::filesystem::path const& file : files)
		{
			// A file of the asset directory is registered where it is, also when it is spelled otherwise (file systems that
			// ignore letter case take any spelling of its names).
			std::optional<std::filesystem::path> const inProject = FileSystem::FindInside(file, AssetManager::GetAssetDirectory());
			bool const inside = inProject.has_value();
			std::string const name = AssetBrowsing::MakeUniqueName(destination.GetValue(), FileSystem::PathToUtf8(file.stem()),
			                                                       FileSystem::PathToUtf8(file.extension()));
			std::filesystem::path const target = inside ? *inProject : destinationPath / FileSystem::PathFromUtf8(name);
			if (!inside)
			{
				if (Result<void> copied = FileSystem::Copy(file, target, false); !copied)
				{
					rollBack();
					return Error{copied.GetError()};
				}
			}
			Result<AssetHandle> imported = AssetManager::ImportFile(target);
			if (!imported)
			{
				if (!inside)
				{
					(void)FileSystem::Remove(target);
				}
				rollBack();
				return Error{imported.GetError()};
			}
			if (!inside)
			{
				copies.push_back(imported.GetValue());
			}
			handles.push_back(imported.GetValue());
		}
		return handles;
	}

	Result<void> EditorOperations::MoveAsset(AssetHandle asset, std::string_view newPath)
	{
		if (Result<void> available = RequireAssetDirectory(); !available)
		{
			return available;
		}
		// A model that moves to another folder takes the files it reads along.
		std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(asset);
		Result<std::string> const normalized = NormalizeAssetPath(newPath);
		if (metadata && metadata->IsFileAsset() && metadata->Type == AssetType::Mesh && normalized &&
		    AssetBrowsing::GetParentFolder(normalized.GetValue()) != AssetBrowsing::GetParentFolder(metadata->Path))
		{
			return MoveModelWithFiles(asset, *metadata, normalized.GetValue());
		}
		return AssetManager::MoveAsset(asset, newPath);
	}

	Result<void> EditorOperations::MoveModelWithFiles(AssetHandle model, AssetMetadata const& metadata, std::string const& newPath)
	{
		std::filesystem::path const& assets = AssetManager::GetAssetDirectory();
		auto const absolute = [&assets](std::string_view path)
		{
			return assets / FileSystem::PathFromUtf8(path);
		};
		auto const relative = [&assets](std::filesystem::path const& path)
		{
			return FileSystem::PathToUtf8(path.lexically_relative(assets));
		};
		std::filesystem::path const from = absolute(metadata.Path).parent_path();
		std::filesystem::path const to = absolute(newPath).parent_path();
		Result<std::vector<std::filesystem::path>> const referenced = MeshImporter::FindExternalFiles(absolute(metadata.Path), assets);
		// A model that cannot be read moves alone; its asset says why it does not load.
		if (!referenced)
		{
			return AssetManager::MoveAsset(model, newPath);
		}

		// The model names its files relative to itself: those in its folder move along. One outside it stays where it is and
		// must still be found from the new folder (a textures folder beside a sibling folder is).
		std::vector<std::filesystem::path> files;
		for (std::filesystem::path const& file : referenced.GetValue())
		{
			if (FileSystem::IsInside(file, from))
			{
				files.push_back(file);
			}
			else if ((to / file.lexically_relative(from)).lexically_normal() != file)
			{
				return MakeError("'{}' reads '{}', which is not in its folder and would be missing from '{}': move the folder that holds "
				                 "both instead",
				                 metadata.Path, relative(file), AssetBrowsing::GetParentFolder(newPath));
			}
		}
		if (files.empty())
		{
			return AssetManager::MoveAsset(model, newPath);
		}

		// A file that another model reads too would be missing for it: models of any folder may read files of the model's
		// folder (they read every file of the asset directory they name).
		for (AssetMetadata const& other : AssetManager::GetAssets(AssetType::Mesh))
		{
			if (other.Handle == model || !other.IsFileAsset())
			{
				continue;
			}
			Result<std::vector<std::filesystem::path>> const shared = MeshImporter::FindExternalFiles(absolute(other.Path), assets);
			for (std::filesystem::path const& file : shared ? shared.GetValue() : std::vector<std::filesystem::path>())
			{
				if (std::find(files.begin(), files.end(), file) != files.end())
				{
					return MakeError("'{}' reads '{}' too: move the folder that holds both instead", other.Path, relative(file));
				}
			}
		}

		// Every file to the same place relative to the model; registered ones (textures) keep their handles.
		struct FileMove
		{
			std::string From;
			std::string To;
			AssetHandle Asset;
		};
		std::vector<FileMove> moves;
		std::string const folder = AssetBrowsing::GetParentFolder(newPath);
		for (std::filesystem::path const& file : files)
		{
			FileMove move{relative(file), AssetBrowsing::JoinPath(folder, FileSystem::PathToUtf8(file.lexically_relative(from))),
			              AssetHandle()};
			if (FileSystem::Exists(absolute(move.To)) || AssetManager::FindByPath(move.To).IsValid())
			{
				return MakeError("'{}' already exists", move.To);
			}
			move.Asset = AssetManager::FindByPath(move.From);
			moves.push_back(std::move(move));
		}
		if (FileSystem::Exists(absolute(newPath)) || AssetManager::FindByPath(newPath).IsValid())
		{
			return MakeError("'{}' already exists", newPath);
		}

		// All or nothing: what moved goes back, with the folders made for it.
		FileCopies folders;
		size_t moved = 0;
		auto const moveFile = [&](std::string const& source, std::string const& target, AssetHandle asset) -> Result<void>
		{
			if (Result<void> created = folders.CreateFolder(absolute(target).parent_path()); !created)
			{
				return created;
			}
			return asset.IsValid() ? AssetManager::MoveAsset(asset, target) : FileSystem::Move(absolute(source), absolute(target));
		};
		auto const moveBack = [&]
		{
			for (size_t index = moved; index-- > 0;)
			{
				FileMove const& move = moves[index];
				(void)(move.Asset.IsValid() ? AssetManager::MoveAsset(move.Asset, move.From)
				                            : FileSystem::Move(absolute(move.To), absolute(move.From)));
			}
			folders.Rollback();
		};
		for (; moved < moves.size(); moved++)
		{
			FileMove const& move = moves[moved];
			if (Result<void> result = moveFile(move.From, move.To, move.Asset); !result)
			{
				moveBack();
				return result;
			}
		}
		if (Result<void> result = moveFile(metadata.Path, newPath, model); !result)
		{
			moveBack();
			return result;
		}
		// Folders of the model's folder that the files leave empty go too.
		for (FileMove const& move : moves)
		{
			for (std::filesystem::path directory = absolute(move.From).parent_path();
			     directory != from && FileSystem::IsInside(directory, from); directory = directory.parent_path())
			{
				std::error_code error;
				if (!std::filesystem::is_empty(directory, error) || error || !FileSystem::Remove(directory))
				{
					break;
				}
			}
		}
		return {};
	}

	Result<void> EditorOperations::MoveAssetFolder(std::string_view folder, std::string_view newFolder)
	{
		if (Result<void> available = RequireAssetDirectory(); !available)
		{
			return available;
		}
		return AssetManager::MoveFolder(folder, newFolder);
	}

	Result<void> EditorOperations::DeleteAsset(AssetHandle asset)
	{
		if (Result<void> available = RequireAssetDirectory(); !available)
		{
			return available;
		}
		Result<void> deleted = AssetManager::DeleteAsset(asset);
		ForgetDeletedAssets(std::span<AssetHandle const>(&asset, 1));
		ForgetDeletedSelection();
		return deleted;
	}

	Result<void> EditorOperations::DeleteAssetFolder(std::string_view folder)
	{
		if (Result<void> available = RequireAssetDirectory(); !available)
		{
			return available;
		}
		std::vector<AssetHandle> files;
		for (AssetMetadata const& metadata : AssetManager::GetAssets())
		{
			if (metadata.IsFileAsset())
			{
				files.push_back(metadata.Handle);
			}
		}
		Result<void> deleted = AssetManager::DeleteFolder(folder);
		ForgetDeletedAssets(files);
		ForgetDeletedSelection();
		return deleted;
	}

	Result<AssetRefreshResult> EditorOperations::RefreshAssets()
	{
		if (Result<void> available = RequireAssetDirectory(); !available)
		{
			return Error{available.GetError()};
		}
		return AssetManager::Refresh();
	}

	void EditorOperations::ForgetDeletedSelection()
	{
		AssetHandle const selected = m_Context.GetSelectedAsset();
		if (selected.IsValid() && !AssetManager::IsValid(selected))
		{
			m_Context.SelectAsset(AssetHandle());
		}
	}

	void EditorOperations::ForgetDeletedAssets(std::span<AssetHandle const> assets)
	{
		// The registry may drop an asset and still fail (saving the registry), so what is gone is read from it, not from the
		// result.
		for (AssetHandle const asset : assets)
		{
			if (!AssetManager::IsValid(asset))
			{
				m_Context.DiscardAssetSteps(asset);
				m_Context.ForgetVisualScriptText(asset);
			}
		}
	}

	Result<void> EditorOperations::SetMaterialFields(AssetHandle material, Json const& patch, uint64_t mergeKey)
	{
		return m_Context.ExecuteCommand(CreateScope<SetMaterialCommand>(material, patch, mergeKey));
	}

	Result<void> EditorOperations::EditVisualScript(AssetHandle graph, Ref<VisualScriptDocument const> document, std::string description,
	                                                uint64_t mergeKey, std::string text)
	{
		if (document == nullptr)
		{
			return Error{"there is no visual script document to write"};
		}
		return m_Context.ExecuteCommand(
			CreateScope<SetVisualScriptCommand>(graph, std::move(document), std::move(description), mergeKey, std::move(text)));
	}

	Result<void> EditorOperations::EditVisualScriptAndComponents(AssetHandle graph, Ref<VisualScriptDocument const> document,
	                                                             std::string description, std::vector<ComponentEdit> edits,
	                                                             std::string text)
	{
		if (document == nullptr)
		{
			return Error{"there is no visual script document to write"};
		}
		return m_Context.ExecuteCommand(CreateScope<SetVisualScriptAndComponentsCommand>(graph, std::move(document), std::move(description),
		                                                                                 std::move(edits), std::move(text)));
	}

	Result<void> EditorOperations::SetVisualScriptDocument(AssetHandle graph, VisualScriptDocument document, uint64_t mergeKey,
	                                                       std::string text)
	{
		return EditVisualScript(graph, CreateRef<VisualScriptDocument const>(std::move(document)), "Edit Visual Script", mergeKey,
		                        std::move(text));
	}

	Result<void> EditorOperations::Select(std::span<UUID const> entities, SelectionMode mode, UUID primary)
	{
		Scene const& scene = m_Context.GetScene();
		for (UUID const entity : entities)
		{
			if (!scene.HasEntity(entity))
			{
				return MakeError("entity {} does not exist", entity);
			}
		}

		EntitySelection preview = m_Context.GetSelection();
		preview.Apply(entities, mode);
		if (primary.IsValid() && !preview.Contains(primary))
		{
			return MakeError("entity {} is not selected, so it cannot be the primary selection", primary);
		}
		m_Context.Select(entities, mode, primary);
		return {};
	}
}
