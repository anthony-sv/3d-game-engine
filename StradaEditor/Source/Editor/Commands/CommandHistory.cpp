#include "Editor/Commands/CommandHistory.h"

#include "Strada/Core/Assert.h"

#include <algorithm>
#include <utility>

namespace Strada
{
	CommandHistory::CommandHistory(size_t maxSize)
		: m_MaxSize(std::max<size_t>(maxSize, 1))
	{
	}

	Result<void> CommandHistory::Execute(Scope<EditorCommand> command, EditorContext& context)
	{
		ST_CORE_ASSERT(command != nullptr, "Cannot execute a null editor command");
		if (Result<void> result = command->Execute(context); !result)
		{
			return result;
		}
		if (!command->HasEffect())
		{
			return {};
		}

		m_RedoStack.clear();
		if (TryMerge(*command))
		{
			return {};
		}

		m_UndoStack.push_back({std::move(command), m_NextStateID++});
		m_MergeAllowed = true;
		TrimToMaxSize();
		return {};
	}

	bool CommandHistory::TryMerge(EditorCommand& command)
	{
		if (!m_MergeAllowed || m_UndoStack.empty())
		{
			return false;
		}

		// Merging into the save point would make the saved state unreachable while still reporting it as current.
		Entry& previous = m_UndoStack.back();
		if (previous.StateID == m_SavedStateID || !previous.Command->CanMergeWith(command))
		{
			return false;
		}

		previous.Command->MergeWith(command);
		if (!previous.Command->HasEffect())
		{
			// The edits cancelled out: the scene is back in the state before the step, so the step disappears and the next
			// edit starts a new one.
			m_UndoStack.pop_back();
			m_MergeAllowed = false;
			return true;
		}
		previous.StateID = m_NextStateID++;
		return true;
	}

	Result<void> CommandHistory::Undo(EditorContext& context)
	{
		m_MergeAllowed = false;
		if (m_UndoStack.empty())
		{
			return Error{"there is nothing to undo"};
		}

		Entry& entry = m_UndoStack.back();
		if (Result<void> result = entry.Command->Undo(context); !result)
		{
			return MakeError("cannot undo '{}': {}", entry.Command->GetDescription(), result.GetError());
		}
		m_RedoStack.push_back(std::move(entry));
		m_UndoStack.pop_back();
		return {};
	}

	Result<void> CommandHistory::Redo(EditorContext& context)
	{
		m_MergeAllowed = false;
		if (m_RedoStack.empty())
		{
			return Error{"there is nothing to redo"};
		}

		Entry& entry = m_RedoStack.back();
		if (Result<void> result = entry.Command->Execute(context); !result)
		{
			return MakeError("cannot redo '{}': {}", entry.Command->GetDescription(), result.GetError());
		}
		// The step keeps its state ID: redoing returns to exactly the state it produced before.
		m_UndoStack.push_back(std::move(entry));
		m_RedoStack.pop_back();
		TrimToMaxSize();
		return {};
	}

	std::string CommandHistory::GetUndoDescription() const
	{
		return m_UndoStack.empty() ? std::string() : m_UndoStack.back().Command->GetDescription();
	}

	std::string CommandHistory::GetRedoDescription() const
	{
		return m_RedoStack.empty() ? std::string() : m_RedoStack.back().Command->GetDescription();
	}

	void CommandHistory::Clear()
	{
		m_UndoStack.clear();
		m_RedoStack.clear();
		m_BaseStateID = m_NextStateID++;
		m_SavedStateID = m_BaseStateID;
		m_MergeAllowed = false;
	}

	void CommandHistory::SetMaxSize(size_t maxSize)
	{
		m_MaxSize = std::max<size_t>(maxSize, 1);
		TrimToMaxSize();
	}

	void CommandHistory::TrimToMaxSize()
	{
		while (m_UndoStack.size() > m_MaxSize)
		{
			m_BaseStateID = m_UndoStack.front().StateID;
			m_UndoStack.pop_front();
		}
	}
}
