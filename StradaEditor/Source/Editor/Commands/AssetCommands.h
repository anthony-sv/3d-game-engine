#pragma once

#include "Editor/Commands/EditorCommand.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <cstdint>
#include <string>

namespace Strada
{
	// Applies a partial patch to the parameters of a material file (.smat) and writes the file. Undoable like scene edits,
	// but the scene does not count as modified. Commands with the same material and non-zero merge key merge into one step.
	class SetMaterialCommand final : public EditorCommand
	{
	public:
		SetMaterialCommand(AssetHandle material, Json patch, uint64_t mergeKey = 0);

		Result<void> Execute(EditorContext& context) override;
		Result<void> Undo(EditorContext& context) override;
		std::string GetDescription() const override { return "Edit Material"; }
		bool HasEffect() const override { return m_Before != m_After; }
		bool ModifiesScene() const override { return false; }
		bool CanMergeWith(EditorCommand const& next) const override;
		void MergeWith(EditorCommand& next) override;

	private:
		AssetHandle m_Material;
		Json m_Patch;
		uint64_t m_MergeKey;
		// Every material field before and after, recorded by the first execution.
		Json m_Before;
		Json m_After;
	};
}
