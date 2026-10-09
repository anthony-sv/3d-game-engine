#pragma once

#include "Editor/EditorOperations.h"
#include "Editor/UI/EditorUI.h"
#include "Editor/UI/FieldEditor.h"

#include <vector>

namespace Strada
{
	// Edits the scene name and settings (physics, rendering), generated from the settings' field tables. Every change is
	// undoable; one widget interaction is one undo step. Main thread only.
	class SceneSettingsPanel
	{
	public:
		SceneSettingsPanel();

		void OnImGuiRender(EditorOperations& operations, bool& open);

	private:
		FieldEditor m_FieldEditor;
		EditSession m_EditSession;
		std::vector<FieldDescriptor> m_Fields;
	};
}
