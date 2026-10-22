#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/KeyCodes.h"
#include "Strada/Core/Layer.h"
#include "Strada/ImGui/ImGuiRenderer.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <filesystem>
#include <functional>
#include <string>
#include <utility>

struct ImFont;

namespace Strada
{
	class Event;

	struct ImGuiLayerSpecification
	{
		bool EnableDocking = true;
		// Lets ImGui windows leave the main window (disabled automatically where unsupported, e.g. Wayland).
		bool EnableViewports = true;
		// Where window layout is persisted; empty disables persistence.
		std::filesystem::path IniFilePath;
	};

	// Owns the ImGui context, the GLFW platform backend and the NVRHI renderer. Application drives Begin/End around the
	// layers' OnImGuiRender calls. Requires a window and an initialized GraphicsDevice. Sets up Strada's look: the embedded
	// Roboto as the UI font (Cousine for fixed-width text) and a dark slate theme with a blue accent.
	//
	// The layer hands the keyboard and mouse of the windows ImGui draws into (the main window and the platform windows of
	// panels dragged out of it) to the platform backend itself. Without an input filter the application receives the main
	// window's input as before ImGui (events ImGui wants to capture are marked handled); an input filter decides instead,
	// event by event, whether ImGui receives it. Where ImGui wants no cursor (ImGuiMouseCursor_None), it is hidden in
	// whichever of those windows it is over. Files dropped onto the platform window of a panel reach the application as
	// WindowFileDropEvents, like files dropped onto the main window.
	class ImGuiLayer : public Layer
	{
	public:
		// What an input filter learns about a keyboard or mouse event besides the event itself.
		struct InputContext
		{
			// Where the cursor is, in ImGui's coordinates: the desktop's with multi-viewports, the main window's without.
			// MouseMovedEvent positions are given in these coordinates too.
			glm::vec2 Cursor = glm::vec2(0.0f);
			// The ImGui viewport (ImGuiViewport::ID) of the window the event comes from: the main window's, or the one of a
			// panel dragged out of it. Windows of different viewports can overlap on the desktop, and the operating system
			// sends mouse buttons to the one on top.
			uint32_t Viewport = 0;
			// For key events, the key the keyboard layout names the physical key (on any layout, the key that types 'p' is
			// P), as ImGui reads keys for shortcuts; KeyEvent::GetKeyCode is the physical key games read. Unknown otherwise.
			KeyCode LayoutKey = KeyCode::Unknown;
			// Modifier keys held.
			bool Shift = false;
			bool Control = false;
			bool Alt = false;
			bool Super = false;
		};

		// Sees every keyboard and mouse event of the windows ImGui draws into before ImGui does (KeyPressedEvent with
		// repeats, KeyReleasedEvent, KeyTypedEvent, MouseButtonPressedEvent, MouseButtonReleasedEvent, MouseMovedEvent and
		// MouseScrolledEvent) and returns whether ImGui receives it. While a filter is set these events reach neither the
		// layers nor Input through the application: the filter passes on what the application needs itself.
		using InputFilter = std::function<bool(Event const& event, InputContext const& context)>;

		explicit ImGuiLayer(ImGuiLayerSpecification specification = {});
		~ImGuiLayer() override = default;

		void OnAttach() override;
		void OnDetach() override;
		void OnEvent(Event& event) override;

		void Begin();
		// Renders the main viewport into the framebuffer (if any) and updates/renders the platform windows.
		void End(nvrhi::IFramebuffer* mainFramebuffer);

		// Installs (or with an empty function removes) the input filter. Main thread only.
		void SetInputFilter(InputFilter filter) { m_InputFilter = std::move(filter); }
		bool AreViewportsEnabled() const { return m_ViewportsEnabled; }
		bool IsAttached() const { return m_Attached; }
		// The fixed-width font (for logs and code); null before OnAttach.
		ImFont* GetMonospaceFont() const { return m_MonospaceFont; }

		// Strada's theme (colors, rounding, spacing) at a content scale of 1, applied to the current ImGui context.
		static void ApplyTheme();
		// Adds the editor's fonts to the current ImGui context's atlas at the UI font size: the embedded Roboto, which
		// becomes the default font, and the fixed-width Cousine, which is returned (null when its data is missing, logged).
		// OnAttach calls it; headless tests call it to measure and draw with the editor's fonts.
		static ImFont* AddEditorFonts();

	private:
		// The GLFW callbacks that share the windows' input between the application, the input filter and ImGui
		// (ImGuiLayer.cpp).
		struct InputCallbacks;

		ImGuiLayerSpecification m_Specification;
		std::string m_IniFilePath;
		ImGuiRenderer m_Renderer;
		InputFilter m_InputFilter;
		bool m_ViewportsEnabled = false;
		bool m_Attached = false;
		bool m_FrameStarted = false;
		ImFont* m_MonospaceFont = nullptr;
	};
}
