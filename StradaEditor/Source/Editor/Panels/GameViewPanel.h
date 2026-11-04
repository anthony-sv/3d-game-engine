#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Renderer/SceneRenderer.h"

#include <glm/glm.hpp>
#include <imgui.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

struct ImGuiWindow;

namespace Strada
{
	class EditorContext;
	class GameInputRouter;
	class Project;
	class Scene;

	// What size the game view renders at.
	enum class GameViewResolution : uint8_t
	{
		// The panel's size.
		Free = 0,
		// The largest rectangle of an aspect ratio that fits the panel.
		Aspect16x9,
		Aspect16x10,
		Aspect4x3,
		Aspect21x9,
		// Fixed sizes, scaled down to fit the panel when it is smaller: the project's window size, Full HD, HD.
		ProjectWindow,
		FullHD,
		HD
	};

	struct GameViewResolutionInfo
	{
		GameViewResolution Resolution = GameViewResolution::Free;
		char const* Name = "";
	};

	// The resolutions in the order the game view lists them.
	std::span<GameViewResolutionInfo const> GetGameViewResolutions();

	// Where the game's image goes inside the panel and what size it is rendered at.
	struct GameViewLayout
	{
		// Pixels the game renders (and its scripts see as the window size).
		glm::uvec2 RenderSize = glm::uvec2(1);
		// The image's rectangle, relative to the panel's content origin; letterboxed when the aspect ratios differ.
		glm::vec2 ImageOffset = glm::vec2(0.0f);
		glm::vec2 ImageSize = glm::vec2(1.0f);
	};

	// Lays the game's image out in an area of the panel. project may be null (the project window size is then 1280x720).
	GameViewLayout ComputeGameViewLayout(GameViewResolution resolution, glm::vec2 available, Project const* project);

	// Whether the game renders for the graph windows' preview (spec-slice4 §15.6): the game view was not visible this frame
	// (so it rendered nothing) and a graph window showed the preview in the last frame. The scene renders at most once per
	// frame either way. Pure.
	[[nodiscard]] bool WantsHiddenGameRender(bool gameViewVisible, bool previewWanted);

	// Renders a scene at a size through its primary camera and gives the image's texture; none when nothing rendered (no
	// primary camera). Tests give the panel one of their own.
	using GameViewRenderFunction = std::function<std::optional<ImTextureID>(Scene& scene, glm::uvec2 size)>;

	// Shows the scene the way the game sees it: through its primary camera, at the chosen resolution, letterboxed. While
	// playing, the game view shares the window's input with the game through a GameInputRouter: it tells the router where
	// the game is shown each frame, takes the focus when the game takes the input, frames itself while the game has it and
	// shows the cursor the game asks for meanwhile (hidden over the view, or locked). The scene view keeps showing the
	// running scene through the editor camera.
	class GameViewPanel
	{
	public:
		// The view's ImGui window name, by which the editor docks it and finds it.
		static constexpr char const* WindowName = "Game View";

		// The router must outlive the panel. The scene renders through render when given (tests), else through a SceneRenderer
		// of its own when the renderer is initialized, else not at all.
		explicit GameViewPanel(GameInputRouter& input, GameViewRenderFunction render = {});
		~GameViewPanel();

		GameViewPanel(GameViewPanel const&) = delete;
		GameViewPanel& operator=(GameViewPanel const&) = delete;

		void OnImGuiRender(EditorContext& context, bool& open);
		// While the game has the input and locks the cursor, gives the editor's OS windows their cursor modes for it
		// (GameInputRouter::GetWindowCursorMode: locked in the window showing the view, hidden in the others) and keeps ImGui
		// from changing cursors (ImGuiConfigFlags_NoMouseCursorChange); afterwards it gives the cursors back to ImGui. A
		// hidden game cursor needs none of this: OnImGuiRender asks ImGui for no cursor over the view. Call once per frame
		// after the panels are drawn, also while the panel is closed.
		void UpdateWindowCursors();

		// Renders the game as the last drawn frame laid it out (GetRenderSize), for the graph windows' preview while the view is
		// hidden (WantsHiddenGameRender); the view's input and layout stay as they are. Inside an ImGui frame, after the view
		// would have been drawn.
		void RenderHidden(EditorContext& context);
		// The texture of the game's image rendered in this frame (by the view or RenderHidden); none when it did not render.
		std::optional<ImTextureID> GetPreviewTexture() const;
		// Whether the game rendered in this frame (by the view or RenderHidden), with an image or without (no camera).
		bool HasRenderedThisFrame() const;
		// Whether the view was drawn with its tab in front in this frame (open, not collapsed, not behind another tab).
		bool IsVisible() const;
		// The dock node the view was drawn in with its tab in front this frame; 0 when it was not visible, or floats.
		ImGuiID GetVisibleDockNode() const;
		// Where ImGui put the view's window when it was last drawn (screen pixels, min and max).
		std::optional<std::pair<glm::vec2, glm::vec2>> GetWindowRect() const;
		// How many times the scene rendered for the view and for RenderHidden.
		uint64_t GetRenderCount() const { return m_RenderCount; }
		// The toolbar's height as the view last drew it (pixels), else one frame height and its padding.
		float GetToolbarHeight() const;
		// The toolbar's Beside button, shown while set: places the view beside the graph window it shares its dock node with
		// (the editor sets it each frame, before the view draws).
		void SetBesideAction(std::function<void()> action) { m_BesideAction = std::move(action); }

		GameViewResolution GetResolution() const { return m_Resolution; }
		void SetResolution(GameViewResolution resolution) { m_Resolution = resolution; }

		// The size the game renders at, as of the last frame the panel was drawn.
		glm::uvec2 GetRenderSize() const { return m_RenderSize; }
		// What the game view's renderer drew in its last frame; null without a GPU.
		SceneRendererStatistics const* GetRendererStatistics() const { return m_Renderer ? &m_Renderer->GetStatistics() : nullptr; }

		// The toolbar's Beside button and its tooltip, which the glyph test reads (spec-slice4 §15).
		static constexpr std::string_view BesideLabel = "Beside";
		static constexpr std::string_view BesideTooltip = "Show the game beside the graph window it shares this dock node with";
		static constexpr std::array Texts = std::to_array<std::string_view>({BesideLabel, BesideTooltip});

	private:
		void DrawToolbar(EditorContext const& context);
		// Renders the scene at a size (counted), remembering the texture for this frame's preview.
		std::optional<ImTextureID> Render(Scene& scene, glm::uvec2 size);
		// Whether something covers the game view at a position (GameInputRouter::CoverageTest), from the windows of ImGui's
		// last frame.
		bool IsCoveredAt(glm::vec2 position) const;

		GameInputRouter& m_Input;
		Scope<SceneRenderer> m_Renderer;
		GameViewRenderFunction m_Render;
		std::function<void()> m_BesideAction;
		uint64_t m_RenderCount = 0;
		// The texture of the last render and the ImGui frame it was made in; the frame the view was last drawn visible in.
		std::optional<ImTextureID> m_Texture;
		int m_TextureFrame = -1;
		int m_VisibleFrame = -1;
		ImGuiID m_VisibleDockNode = 0;
		std::optional<float> m_ToolbarHeight;
		GameViewResolution m_Resolution = GameViewResolution::Free;
		glm::uvec2 m_RenderSize = glm::uvec2(1280, 720);
		// The panel's ImGui window once drawn (ImGui keeps windows for the context's lifetime).
		ImGuiWindow* m_Window = nullptr;
		// The OS windows have the game's locked cursor rather than ImGui's cursors (UpdateWindowCursors).
		bool m_WindowCursorsLocked = false;
	};
}
