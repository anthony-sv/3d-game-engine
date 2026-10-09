#pragma once

#include "Strada/Core/Result.h"

#include <string>

namespace Strada
{
	class EditorContext;

	// One undoable modification of the edited scene. Commands refer to entities by UUID (never by handle or pointer), so
	// they stay valid while undo/redo destroys and recreates entities, and they capture what they need as JSON snapshots.
	class EditorCommand
	{
	public:
		virtual ~EditorCommand() = default;

		// Applies the change: on the first execution and again for every redo. On failure nothing may have changed.
		[[nodiscard]] virtual Result<void> Execute(EditorContext& context) = 0;
		// Reverts the last Execute. On failure nothing may have changed.
		[[nodiscard]] virtual Result<void> Undo(EditorContext& context) = 0;
		// Short user-facing name such as "Rename Entity" (Edit menu, results of editor.undo).
		virtual std::string GetDescription() const = 0;

		// False when the last Execute left the scene as it was (for example setting a field to its current value). The
		// history drops such commands instead of recording an empty undo step that would mark the scene as modified.
		virtual bool HasEffect() const { return true; }

		// False for edits of other documents (asset files, which save themselves): they are undoable like scene edits but
		// do not count as unsaved scene changes.
		virtual bool ModifiesScene() const { return true; }

		// Continuous edits (dragging a value) form one undo step: returns true when `next`, which has just been executed
		// after this command, can be folded into it.
		virtual bool CanMergeWith(EditorCommand const& next) const
		{
			(void)next;
			return false;
		}
		// Absorbs `next` (only called after CanMergeWith returned true); this command then reverts both changes.
		virtual void MergeWith(EditorCommand& next) { (void)next; }
	};
}
