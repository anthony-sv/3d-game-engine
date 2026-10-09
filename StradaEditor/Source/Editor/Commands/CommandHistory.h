#pragma once

#include "Editor/Commands/EditorCommand.h"

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
	// changes, and the number of undo steps is bounded (the oldest are dropped). Main thread only.
	class CommandHistory
	{
	public:
		static constexpr size_t DefaultMaxSize = 256;

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

	private:
		struct Entry
		{
			Scope<EditorCommand> Command;
			// Identifies the scene state right after this step.
			uint64_t StateID = 0;
		};

		uint64_t GetCurrentStateID() const { return m_UndoStack.empty() ? m_BaseStateID : m_UndoStack.back().StateID; }
		bool TryMerge(EditorCommand& command);
		void TrimToMaxSize();

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
