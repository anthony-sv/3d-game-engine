#pragma once

#include "Editor/Commands/EditorCommand.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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

	// Replaces the document of a visual script file (.sgraph): the file is written first, then the loaded asset takes the
	// document (running graphs take the edit at their next update). Undoable like scene edits, but the scene does not count
	// as modified. A file that does not load is replaced too: undoing writes its bytes back, and it fails to load again as it
	// did. The asset is looked up by its handle each time, so moving the file keeps the step working. Commands with the same
	// graph and non-zero merge key merge into one step.
	class SetVisualScriptCommand final : public EditorCommand
	{
	public:
		SetVisualScriptCommand(AssetHandle graph, VisualScriptDocument document, uint64_t mergeKey = 0);

		Result<void> Execute(EditorContext& context) override;
		Result<void> Undo(EditorContext& context) override;
		std::string GetDescription() const override { return "Edit Visual Script"; }
		// False when the document is the one the file had.
		bool HasEffect() const override { return !m_Before || *m_Before != m_After; }
		bool ModifiesScene() const override { return false; }
		bool CanMergeWith(EditorCommand const& next) const override;
		void MergeWith(EditorCommand& next) override;

	private:
		AssetHandle m_Graph;
		VisualScriptDocument m_After;
		uint64_t m_MergeKey;
		// Whether the first execution read what the file held.
		bool m_Recorded = false;
		// What the file held before the first execution: its document, or (without one) the bytes of a file that did not load.
		std::optional<VisualScriptDocument> m_Before;
		std::vector<uint8_t> m_BeforeBytes;
	};
}
