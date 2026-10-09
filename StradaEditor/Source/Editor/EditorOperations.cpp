#include "Editor/EditorOperations.h"

#include "Editor/AssetBrowsing.h"
#include "Editor/Commands/AssetCommands.h"
#include "Editor/Commands/ComponentCommands.h"
#include "Editor/Commands/CompositeCommand.h"
#include "Editor/Commands/SceneCommands.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/AssetRegistry.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"
#include "Strada/Scene/SceneSerializer.h"

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

	void EditorOperations::CloseProject()
	{
		if (m_Context.GetProject() != nullptr && AssetManager::IsInitialized())
		{
			AssetManager::CloseAssetDirectory();
		}
		m_Context.SetProject(nullptr);
		m_Context.SelectAsset(AssetHandle());
		NewScene();
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

		std::vector<std::string> warnings;
		if (!startScene.IsValid())
		{
			NewScene();
			return warnings;
		}
		std::filesystem::path const scenePath = AssetManager::GetAbsolutePath(startScene);
		Result<std::vector<std::string>> opened =
			scenePath.empty() ? Result<std::vector<std::string>>(Error{"it is not a scene file of the project"}) : OpenScene(scenePath);
		if (!opened)
		{
			// The project stays usable: its settings can point at another scene.
			std::string warning = fmt::format("the start scene could not be opened: {}", opened.GetError());
			ST_WARN("{}", warning);
			warnings.push_back(std::move(warning));
			NewScene();
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
			NewScene();
		}
	}

	void EditorOperations::NewScene(std::string name)
	{
		m_Context.SetScene(CreateRef<Scene>(std::move(name)), {});
	}

	Result<std::vector<std::string>> EditorOperations::OpenScene(std::filesystem::path const& path)
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
		m_Context.SetScene(scene.TakeValue(), std::move(absolute));
		return warnings;
	}

	Result<void> EditorOperations::SaveScene(std::filesystem::path const& path)
	{
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
		return m_Context.ExecuteCommand(CreateScope<SetComponentsCommand>(std::move(edits), mergeKey));
	}

	Result<void> EditorOperations::SetComponentFields(std::vector<ComponentEdit> edits, uint64_t mergeKey)
	{
		return m_Context.ExecuteCommand(CreateScope<SetComponentsCommand>(std::move(edits), mergeKey));
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
			bool const inside = FileSystem::IsInside(file, AssetManager::GetAssetDirectory());
			std::string const name = AssetBrowsing::MakeUniqueName(destination.GetValue(), FileSystem::PathToUtf8(file.stem()),
			                                                       FileSystem::PathToUtf8(file.extension()));
			std::filesystem::path const target = inside ? file : destinationPath / FileSystem::PathFromUtf8(name);
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
		return AssetManager::MoveAsset(asset, newPath);
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
		ForgetDeletedSelection();
		return deleted;
	}

	Result<void> EditorOperations::DeleteAssetFolder(std::string_view folder)
	{
		if (Result<void> available = RequireAssetDirectory(); !available)
		{
			return available;
		}
		Result<void> deleted = AssetManager::DeleteFolder(folder);
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

	Result<void> EditorOperations::SetMaterialFields(AssetHandle material, Json const& patch, uint64_t mergeKey)
	{
		return m_Context.ExecuteCommand(CreateScope<SetMaterialCommand>(material, patch, mergeKey));
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
