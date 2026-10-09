#include "Editor/Commands/SceneCommands.h"

#include "Editor/EditorContext.h"

#include "Strada/Scene/SceneSerializer.h"

#include <utility>

namespace Strada
{
	SetScenePropertiesCommand::SetScenePropertiesCommand(std::optional<std::string> name, Json settingsPatch, uint64_t mergeKey)
		: m_Name(std::move(name)),
		  m_SettingsPatch(settingsPatch.is_null() ? Json::object() : std::move(settingsPatch)),
		  m_MergeKey(mergeKey)
	{
	}

	Result<void> SetScenePropertiesCommand::Execute(EditorContext& context)
	{
		if (m_After)
		{
			return Apply(context, *m_After);
		}

		Scene& scene = context.GetScene();
		SceneSettings settings = scene.GetSettings();
		if (Result<void> result = SceneSerializer::DeserializeSettings(m_SettingsPatch, settings, context.CreateDeserializationContext());
		    !result)
		{
			return result;
		}

		m_Before = State{scene.GetName(), SceneSerializer::SerializeSettings(scene.GetSettings())};
		if (m_Name)
		{
			scene.SetName(*m_Name);
		}
		scene.GetSettings() = settings;
		m_After = State{scene.GetName(), SceneSerializer::SerializeSettings(scene.GetSettings())};
		return {};
	}

	Result<void> SetScenePropertiesCommand::Undo(EditorContext& context)
	{
		if (!m_Before)
		{
			return Error{"the scene properties were never changed"};
		}
		return Apply(context, *m_Before);
	}

	std::string SetScenePropertiesCommand::GetDescription() const
	{
		return m_Name && m_SettingsPatch.empty() ? "Rename Scene" : "Edit Scene Settings";
	}

	bool SetScenePropertiesCommand::HasEffect() const
	{
		return m_Before && m_After && (m_Before->Name != m_After->Name || m_Before->Settings != m_After->Settings);
	}

	bool SetScenePropertiesCommand::CanMergeWith(EditorCommand const& next) const
	{
		if (m_MergeKey == 0)
		{
			return false;
		}
		auto const* other = dynamic_cast<SetScenePropertiesCommand const*>(&next);
		return other != nullptr && other->m_MergeKey == m_MergeKey;
	}

	void SetScenePropertiesCommand::MergeWith(EditorCommand& next)
	{
		auto& other = static_cast<SetScenePropertiesCommand&>(next);
		m_After = std::move(other.m_After);
	}

	Result<void> SetScenePropertiesCommand::Apply(EditorContext& context, State const& state)
	{
		Scene& scene = context.GetScene();
		SceneSettings settings = scene.GetSettings();
		if (Result<void> result = SceneSerializer::DeserializeSettings(state.Settings, settings, context.CreateDeserializationContext());
		    !result)
		{
			return result;
		}
		scene.SetName(state.Name);
		scene.GetSettings() = settings;
		return {};
	}
}
