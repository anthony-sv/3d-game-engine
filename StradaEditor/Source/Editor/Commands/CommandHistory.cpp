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
			// The merged step keeps the memory of its last change now.
			TrimToMaxSize();
			return {};
		}

		// Asset edits leave the scene in its current state.
		uint64_t const stateID = command->ModifiesScene() ? m_NextStateID++ : GetCurrentStateID();
		m_UndoStack.push_back({std::move(command), stateID});
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

		// Merging into the save point would make the saved state unreachable while still reporting it as current (steps
		// that leave the scene alone do not move the scene's state).
		Entry& previous = m_UndoStack.back();
		bool const modifiesScene = previous.Command->ModifiesScene();
		if ((modifiesScene && previous.StateID == m_SavedStateID) || !previous.Command->CanMergeWith(command))
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
		if (modifiesScene)
		{
			previous.StateID = m_NextStateID++;
		}
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

	AssetHandle CommandHistory::GetUndoEditedAsset() const
	{
		return m_UndoStack.empty() ? AssetHandle() : m_UndoStack.back().Command->GetEditedAsset();
	}

	AssetHandle CommandHistory::GetRedoEditedAsset() const
	{
		return m_RedoStack.empty() ? AssetHandle() : m_RedoStack.back().Command->GetEditedAsset();
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

	std::vector<Scope<EditorCommand>> CommandHistory::TakeAssetSteps()
	{
		// A step that leaves the scene alone has the state ID of the step before it, so taking it out leaves the chain of
		// states from the base to the current one as it was.
		std::vector<Scope<EditorCommand>> steps;
		for (Entry& entry : m_UndoStack)
		{
			if (!entry.Command->ModifiesScene())
			{
				steps.push_back(std::move(entry.Command));
			}
		}
		std::erase_if(m_UndoStack,
		              [](Entry const& entry)
		              {
						  return entry.Command == nullptr;
					  });
		m_RedoStack.clear();
		m_MergeAllowed = false;
		return steps;
	}

	void CommandHistory::AppendAssetSteps(std::vector<Scope<EditorCommand>> steps)
	{
		if (steps.empty())
		{
			return;
		}
		uint64_t const stateID = GetCurrentStateID();
		for (Scope<EditorCommand>& step : steps)
		{
			ST_CORE_ASSERT(step != nullptr && !step->ModifiesScene(), "Only steps that leave the scene alone move to another history");
			m_UndoStack.push_back({std::move(step), stateID});
		}
		m_RedoStack.clear();
		m_MergeAllowed = false;
		TrimToMaxSize();
	}

	void CommandHistory::DiscardStepsOf(AssetHandle asset)
	{
		if (!asset.IsValid())
		{
			return;
		}
		// Removing a step that changed the scene would leave its change without an undo and the states after it unreachable.
		auto const isAssetStep = [asset](Entry const& entry)
		{
			return entry.Command->GetEditedAsset() == asset && !entry.Command->ModifiesScene();
		};
		size_t const removed = std::erase_if(m_UndoStack, isAssetStep) + std::erase_if(m_RedoStack, isAssetStep);
		size_t forgotten = 0;
		auto const forget = [asset, &forgotten](Entry& entry)
		{
			if (entry.Command->GetEditedAsset() == asset)
			{
				entry.Command->ForgetEditedAsset();
				forgotten++;
			}
		};
		std::ranges::for_each(m_UndoStack, forget);
		std::ranges::for_each(m_RedoStack, forget);
		if (removed + forgotten > 0)
		{
			m_MergeAllowed = false;
		}
	}

	void CommandHistory::TrimToMaxSize()
	{
		while (m_UndoStack.size() > m_MaxSize)
		{
			DropOldestStep();
		}

		// Steps that keep snapshots of large asset files: the memory stays bounded, but the last few steps are always kept.
		size_t memory = 0;
		for (Entry const& entry : m_UndoStack)
		{
			memory += entry.Command->GetMemoryUse();
		}
		while (memory > DefaultMaxMemory && m_UndoStack.size() > MinStepsKept)
		{
			memory -= m_UndoStack.front().Command->GetMemoryUse();
			DropOldestStep();
		}
	}

	void CommandHistory::DropOldestStep()
	{
		m_BaseStateID = m_UndoStack.front().StateID;
		m_UndoStack.pop_front();
	}
}
