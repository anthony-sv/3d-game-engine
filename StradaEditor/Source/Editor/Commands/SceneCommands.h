#pragma once

#include "Editor/Commands/EditorCommand.h"

#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <cstdint>
#include <optional>
#include <string>

namespace Strada
{
	// Renames the scene and/or applies a partial patch to its settings (the "Settings" object of scene files, validated
	// before anything changes). Commands with the same non-zero merge key merge into one undo step.
	class SetScenePropertiesCommand final : public EditorCommand
	{
	public:
		SetScenePropertiesCommand(std::optional<std::string> name, Json settingsPatch, uint64_t mergeKey = 0);

		Result<void> Execute(EditorContext& context) override;
		Result<void> Undo(EditorContext& context) override;
		std::string GetDescription() const override;
		bool HasEffect() const override;
		bool CanMergeWith(EditorCommand const& next) const override;
		void MergeWith(EditorCommand& next) override;

	private:
		struct State
		{
			std::string Name;
			Json Settings;
		};

		static Result<void> Apply(EditorContext& context, State const& state);

		std::optional<std::string> m_Name;
		Json m_SettingsPatch;
		uint64_t m_MergeKey;
		std::optional<State> m_Before;
		std::optional<State> m_After;
	};
}
