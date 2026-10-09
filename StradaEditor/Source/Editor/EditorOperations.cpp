#include "Editor/EditorOperations.h"

#include "Editor/Commands/ComponentCommands.h"
#include "Editor/Commands/CompositeCommand.h"
#include "Editor/Commands/SceneCommands.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"
#include "Strada/Scene/SceneSerializer.h"

#include <system_error>
#include <utility>

namespace Strada
{
	namespace
	{
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
