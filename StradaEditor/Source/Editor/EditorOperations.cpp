#include "Editor/EditorOperations.h"

#include "Editor/Commands/ComponentCommands.h"
#include "Editor/Commands/SceneCommands.h"

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
		Json patch = Json::object();
		patch["Tag"] = std::move(name);
		return m_Context.ExecuteCommand(CreateScope<SetComponentCommand>(entity, std::string(ComponentTraits<TagComponent>::Name),
		                                                                 std::move(patch), mergeKey, "Rename Entity"));
	}

	Result<void> EditorOperations::ReparentEntity(UUID entity, UUID newParent, std::optional<size_t> siblingIndex, bool keepWorldTransform)
	{
		return m_Context.ExecuteCommand(CreateScope<ReparentEntityCommand>(entity, newParent, siblingIndex, keepWorldTransform));
	}

	Result<void> EditorOperations::AddComponent(UUID entity, std::string_view component, Json const& fields)
	{
		return m_Context.ExecuteCommand(CreateScope<AddComponentCommand>(entity, std::string(component), fields));
	}

	Result<void> EditorOperations::RemoveComponent(UUID entity, std::string_view component)
	{
		return m_Context.ExecuteCommand(CreateScope<RemoveComponentCommand>(entity, std::string(component)));
	}

	Result<void> EditorOperations::SetComponentFields(UUID entity, std::string_view component, Json const& patch, uint64_t mergeKey)
	{
		return m_Context.ExecuteCommand(CreateScope<SetComponentCommand>(entity, std::string(component), patch, mergeKey));
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
