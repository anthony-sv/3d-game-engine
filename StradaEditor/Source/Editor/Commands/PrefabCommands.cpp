#include "Editor/Commands/PrefabCommands.h"

#include "Editor/EditorContext.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/PrefabAsset.h"
#include "Strada/Math/Math.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/SceneSerializer.h"

#include <glm/gtc/quaternion.hpp>

namespace Strada
{
	InstantiatePrefabCommand::InstantiatePrefabCommand(AssetHandle prefab, UUID parent, std::optional<glm::vec3> position)
		: m_Prefab(prefab),
		  m_Parent(parent),
		  m_Position(position)
	{
	}

	Result<void> InstantiatePrefabCommand::Execute(EditorContext& context)
	{
		Scene& scene = context.GetScene();
		if (m_Snapshot)
		{
			if (Result<Entity> restored = m_Snapshot->Restore(scene, context.CreateDeserializationContext()); !restored)
			{
				return Error{restored.GetError()};
			}
			return {};
		}

		Entity parent;
		if (m_Parent.IsValid())
		{
			parent = scene.GetEntityByUUID(m_Parent);
			if (!parent)
			{
				return MakeError("parent entity {} does not exist", m_Parent);
			}
		}
		Result<Ref<PrefabAsset>> prefab = AssetManager::TryGetAsset<PrefabAsset>(m_Prefab);
		if (!prefab)
		{
			return Error{prefab.GetError()};
		}
		// Prefabs saved by newer versions keep what this one knows.
		Result<Entity> instance = PrefabSerializer::Instantiate(scene, prefab.GetValue()->GetDocument(), m_Prefab, parent,
		                                                        context.CreateDeserializationContext(UnknownFieldPolicy::Warn));
		if (!instance)
		{
			return Error{instance.GetError()};
		}

		Entity const root = instance.GetValue();
		if (m_Position)
		{
			glm::vec3 translation(0.0f);
			glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
			glm::vec3 scale(1.0f);
			Math::DecomposeTransform(scene.GetWorldTransform(root), translation, rotation, scale);
			scene.SetWorldTransform(root, Math::ComposeTransform(*m_Position, rotation, scale));
		}
		m_Root = root.GetUUID();
		m_Snapshot = EntitySnapshot::Capture(scene, root);
		return {};
	}

	Result<void> InstantiatePrefabCommand::Undo(EditorContext& context)
	{
		Scene& scene = context.GetScene();
		Entity const root = scene.GetEntityByUUID(m_Root);
		if (!root)
		{
			return MakeError("entity {} no longer exists", m_Root);
		}
		scene.DestroyEntity(root);
		return {};
	}
}
