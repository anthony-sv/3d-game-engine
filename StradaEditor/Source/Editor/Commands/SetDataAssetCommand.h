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
	// keeps the step working. Steps of the same asset with the same non-zero merge key (one drag in the inspector) merge.
	class SetDataAssetCommand final : public EditorCommand
	{
	public:
		SetDataAssetCommand(AssetHandle asset, Json patch, uint64_t mergeKey = 0);

		[[nodiscard]] Result<void> Execute(EditorContext& context) override;
		[[nodiscard]] Result<void> Undo(EditorContext& context) override;
		// "Edit <type name>" ("Edit Curve"), recorded by the first execution.
		std::string GetDescription() const override;
		bool HasEffect() const override;
		// False: the edit is of an asset file, which saves itself.
		bool ModifiesScene() const override;
		// The same asset and the same non-zero merge key.
		bool CanMergeWith(EditorCommand const& next) const override;
		// Takes next's state after; keeps its own state before.
		void MergeWith(EditorCommand& next) override;
		AssetHandle GetEditedAsset() const override;

	private:
		AssetHandle m_Asset;
		Json m_Patch;
		uint64_t m_MergeKey = 0;
		std::string m_Description;
		// Every field before and after, recorded by the first execution.
		Json m_Before;
		Json m_After;
	};
}
