#pragma once

#include "Editor/EditorCamera.h"
#include "Editor/EditorContext.h"

#include "Strada/Core/Base.h"
#include "Strada/Renderer/SceneRenderer.h"

namespace Strada
{
	// Shows the edited scene rendered from the editor camera; the camera reacts to input while the viewport is hovered
	// (right mouse: look + WASD/QE fly, Alt + left mouse: orbit, middle mouse: pan, wheel: zoom, F: focus selection).
	class ViewportPanel
	{
	public:
		ViewportPanel();

		void OnImGuiRender(EditorContext& context, bool& open);
		EditorCamera& GetCamera() { return m_Camera; }

	private:
		void FocusSelection(EditorContext& context);

		EditorCamera m_Camera;
		bool m_LookCaptured = false;
		bool m_PanCaptured = false;
		bool m_OrbitCaptured = false;
		// Null when no GPU renderer is available.
		Scope<SceneRenderer> m_Renderer;
	};
}
