#pragma once

#include "Editor/EditorOperations.h"
#include "Editor/UI/FieldEditor.h"

#include <vector>

namespace Strada
{
	// Edits the open project's settings (name, start scene, script module, game window, physics timestep and layers),
	// generated from the settings' field tables, plus the layer collision matrix. Changes apply at once and are written to
	// the project file when the widget interaction ends. Project settings are not part of the scene's undo history.
	// Main thread only.
	class ProjectSettingsPanel
	{
	public:
		ProjectSettingsPanel();

		void OnImGuiRender(EditorOperations& operations, bool& open);
		// Writes changes that wait for the end of an interaction (call before the editor closes).
		void Flush(EditorOperations& operations);

	private:
		void DrawCollisionMatrix(EditorOperations& operations, ProjectSettings const& settings);
		void Apply(EditorOperations& operations, Json patch);

		FieldEditor m_FieldEditor;
		// The project fields without the asset directory (fixed while open) and the ignored collisions (drawn as a matrix).
		std::vector<FieldDescriptor> m_Fields;
		bool m_SavePending = false;
	};
}
