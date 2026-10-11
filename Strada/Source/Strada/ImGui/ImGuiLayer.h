#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Layer.h"
#include "Strada/ImGui/ImGuiRenderer.h"

#include <nvrhi/nvrhi.h>

#include <filesystem>
#include <string>

struct ImFont;
struct ImGuiViewport;

namespace Strada
{
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
	class ImGuiLayer : public Layer
	{
	public:
		explicit ImGuiLayer(ImGuiLayerSpecification specification = {});
		~ImGuiLayer() override = default;

		void OnAttach() override;
		void OnDetach() override;
		void OnEvent(Event& event) override;

		void Begin();
		// Renders the main viewport into the framebuffer (if any) and updates/renders the platform windows.
		void End(nvrhi::IFramebuffer* mainFramebuffer);

		// When enabled (default), mouse/keyboard events ImGui wants to capture are marked handled.
		void SetBlockEvents(bool block) { m_BlockEvents = block; }
		bool AreViewportsEnabled() const { return m_ViewportsEnabled; }
		bool IsAttached() const { return m_Attached; }
		// The fixed-width font (for logs and code); null before OnAttach.
		ImFont* GetMonospaceFont() const { return m_MonospaceFont; }

		// Strada's theme (colors, rounding, spacing) at a content scale of 1, applied to the current ImGui context.
		static void ApplyTheme();

	private:
		ImGuiLayerSpecification m_Specification;
		std::string m_IniFilePath;
		ImGuiRenderer m_Renderer;
		// The GLFW backend's Platform_CreateWindow, which the layer's own wraps (multi-viewports only).
		void (*m_CreatePlatformWindow)(ImGuiViewport* viewport) = nullptr;
		bool m_BlockEvents = true;
		bool m_ViewportsEnabled = false;
		bool m_Attached = false;
		bool m_FrameStarted = false;
		ImFont* m_MonospaceFont = nullptr;
	};
}
