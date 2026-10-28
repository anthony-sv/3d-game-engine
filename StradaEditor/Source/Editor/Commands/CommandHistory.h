#pragma once

#include "Editor/Commands/EditorCommand.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace Strada
{
	// Undo and redo stacks of executed editor commands. Continuous edits merge into one step, a save point tracks unsaved
	// changes, and the undo steps are bounded in number and in the memory they keep (the oldest are dropped). Main thread
	// only.
	class CommandHistory
	{
	public:
		static constexpr size_t DefaultMaxSize = 256;
		// The memory the undo steps may keep beyond the documents they edit (EditorCommand::GetMemoryUse: snapshots of asset
		// files) before the oldest are dropped, which never leaves fewer than MinStepsKept steps.
		static constexpr size_t DefaultMaxMemory = 256u * 1024u * 1024u;
		static constexpr size_t MinStepsKept = 16;

		explicit CommandHistory(size_t maxSize = DefaultMaxSize);

		// Executes the command and records it as the newest undo step, or merges it into the previous step (see
		// EditorCommand::CanMergeWith) unless BreakMerge, Undo or Redo was called in between or that step is the save
		// point. A merged step that ends where it started is removed. A command that changed nothing (HasEffect() is
		// false) is dropped without touching either stack. Recording a step clears the redo stack. On failure nothing is
		// recorded and both stacks are kept.
		[[nodiscard]] Result<void> Execute(Scope<EditorCommand> command, EditorContext& context);
		// On failure the history is unchanged.
		[[nodiscard]] Result<void> Undo(EditorContext& context);
		[[nodiscard]] Result<void> Redo(EditorContext& context);

		bool CanUndo() const { return !m_UndoStack.empty(); }
		bool CanRedo() const { return !m_RedoStack.empty(); }
		// Descriptions of the steps Undo and Redo would apply; empty when there is none.
		std::string GetUndoDescription() const;
		std::string GetRedoDescription() const;
		// The asset whose file the step Undo (Redo) would apply edits (EditorCommand::GetEditedAsset); invalid when that step
		// edits only the scene, and when there is none.
		AssetHandle GetUndoEditedAsset() const;
		AssetHandle GetRedoEditedAsset() const;
		// The step Undo would apply, to inspect what it recorded (the performance tests read a commit's timings); null when
		// there is none.
		EditorCommand const* GetUndoCommand() const;
		size_t GetUndoCount() const { return m_UndoStack.size(); }
		size_t GetRedoCount() const { return m_RedoStack.size(); }

		// Ends the current run of mergeable edits (for example when a drag ends): the next command starts a new step.
		void BreakMerge() { m_MergeAllowed = false; }

		// True when the scene differs from the state recorded by the last MarkSaved (or Clear).
		bool IsDirty() const { return GetCurrentStateID() != m_SavedStateID; }
		void MarkSaved() { m_SavedStateID = GetCurrentStateID(); }

		// Forgets every step; the current state counts as saved (used when another scene is opened).
		void Clear();

		size_t GetMaxSize() const { return m_MaxSize; }
		// Drops the oldest undo steps beyond maxSize (at least 1).
		void SetMaxSize(size_t maxSize);

		// --- Steps of asset files ---

		// Removes the undo steps that leave the scene alone (ModifiesScene() false: edits of asset files, which keep their
		// changes on disk) and returns them, oldest first, for another history to take (AppendAssetSteps) before this one is
		// dropped. The redo stack is cleared; the steps that stay still lead to the scene's current state.
		[[nodiscard]] std::vector<Scope<EditorCommand>> TakeAssetSteps();
		// Appends steps taken from another history (TakeAssetSteps) as the newest undo steps. They leave the scene in its
		// current state, so whether it has unsaved changes does not change. As when a step is recorded, the redo stack is
		// cleared, the next command starts a new step and the oldest steps beyond the bounds are dropped. No steps change
		// nothing.
		void AppendAssetSteps(std::vector<Scope<EditorCommand>> steps);
		// The asset was deleted: the steps of both stacks that edit it (GetEditedAsset) are removed when they leave the scene
		// alone; steps that also changed the scene stay, so every state of the scene can still be reached, and forget the
		// asset (EditorCommand::ForgetEditedAsset). The next command starts a new step.
		void DiscardStepsOf(AssetHandle asset);

	private:
		struct Entry
		{
			Scope<EditorCommand> Command;
			// Identifies the scene state right after this step.
			uint64_t StateID = 0;
		};

		uint64_t GetCurrentStateID() const { return m_UndoStack.empty() ? m_BaseStateID : m_UndoStack.back().StateID; }
		bool TryMerge(EditorCommand& command);
		// Drops the oldest undo steps beyond the maximum size, then while they keep more than DefaultMaxMemory and more than
		// MinStepsKept remain.
		void TrimToMaxSize();
		void DropOldestStep();

		std::deque<Entry> m_UndoStack;
		std::vector<Entry> m_RedoStack;
		size_t m_MaxSize;
		uint64_t m_NextStateID = 1;
		// The state before the oldest undo step; advances when old steps are dropped.
		uint64_t m_BaseStateID = 0;
		uint64_t m_SavedStateID = 0;
		bool m_MergeAllowed = false;
	};
}
