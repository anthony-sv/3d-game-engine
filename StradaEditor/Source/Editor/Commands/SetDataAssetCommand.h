#pragma once

#include "Editor/Commands/EditorCommand.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <cstdint>
#include <string>

namespace Strada
{
	// Applies a partial patch to the fields of a data asset file (any registered data asset type, DataAsset.h) and writes the
	// file: what SetMaterialCommand does for materials, for every data asset type. Undoable like scene edits, but the scene
	// does not count as modified. The patch is read strictly (unknown fields and values outside their ranges refuse it) and
	// the result must pass the type's Validate; a refused patch changes nothing. Only data assets with a file of their own
	// can be edited, and a read-only file is refused, by undo too (which then stays on the stack until the file is writable
	// again), on every platform.
	//
	// The first execution records every field before and after (the content object as DataAsset::SerializeFields gives
	// it): undo writes back the state before, not the file's bytes, so undoing the first edit of a file written in an older
	// format version, or holding fields this engine does not know, leaves it in the current version with the same values,
	// as materials do. Every write is recorded (AssetManager::RecordFileWrite), so a refresh keeps the loaded asset object
	// and its users see the change through its version. The asset is looked up by its handle each time, so moving the file
	// keeps the step working. Steps of the same asset with the same non-zero merge key merge: one interaction of the inspector
	// (its EditSession's key), or a run of edits a drawer keys itself (a curve editor's arrow-key nudges, merged by time
	// while no ImGui item is active).
	class SetDataAssetCommand final : public EditorCommand
	{
	public:
		// The description names the edit in the undo history ("Set Length on Timeline (moves 2 keys)"); empty for the default,
		// "Edit <type name>".
		SetDataAssetCommand(AssetHandle asset, Json patch, std::string description = {}, uint64_t mergeKey = 0);

		// Whether edits of the asset can be kept: it is registered, of a registered data asset type, and has a file of its own
		// that is present and not read-only. The error says why not. Whether the file loads is not checked. Main thread only.
		[[nodiscard]] static Result<void> CheckEditable(AssetHandle asset);

		[[nodiscard]] Result<void> Execute(EditorContext& context) override;
		[[nodiscard]] Result<void> Undo(EditorContext& context) override;
		// The description given, or "Edit <type name>" ("Edit Curve") recorded by the first execution.
		std::string GetDescription() const override;
		bool HasEffect() const override;
		// False: the edit is of an asset file, which saves itself.
		bool ModifiesScene() const override;
		// The same asset and the same non-zero merge key.
		bool CanMergeWith(EditorCommand const& next) const override;
		// Takes next's state after and description; keeps its own state before.
		void MergeWith(EditorCommand& next) override;
		AssetHandle GetEditedAsset() const override;

	private:
		AssetHandle m_Asset;
		Json m_Patch;
		uint64_t m_MergeKey = 0;
		// The description given, else "Edit <type name>" from the first execution.
		std::string m_Description;
		// Every field before and after, recorded by the first execution.
		Json m_Before;
		Json m_After;
	};
}
