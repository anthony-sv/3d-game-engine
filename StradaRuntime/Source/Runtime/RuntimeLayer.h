#pragma once

#include "Strada/Core/Layer.h"
#include "Strada/Project/Project.h"
#include "Strada/Project/SceneRunner.h"
#include "Strada/Renderer/SceneRenderer.h"
#include "Strada/Renderer/TextureBlitter.h"

#include <cstdint>
#include <filesystem>

namespace Strada
{
	struct RuntimeLayerSpecification
	{
		// A test run: ends when the scripts finish testing (Strada.Testing), quit or run out of frames, and exits with the
		// number of failures (at most MaxTestExitCode; a run that did not finish counts as one more).
		bool Test = false;
		uint64_t TestFrameLimit = 3600;
		// Seconds each frame advances the game; 0 advances it by the real frame time.
		float FixedTimestep = 0.0f;
		// Saves the window's contents to this file when the given frame is presented (empty: no screenshot).
		std::filesystem::path ScreenshotPath;
		uint64_t ScreenshotFrame = 0;
		// The scenes' view size when nothing is rendered (headless runs): the game's window size.
		uint32_t ViewportWidth = 1280;
		uint32_t ViewportHeight = 720;
	};

	// Runs a game in the player: a scene runner plays the start scene and the scenes its scripts load, the view of the
	// scene's primary camera fills the window, Application.Quit closes the player, and a test run ends with the scripts'
	// results as the exit code.
	class RuntimeLayer final : public Layer
	{
	public:
		static constexpr int MaxTestExitCode = 100;

		RuntimeLayer(RuntimeLayerSpecification specification, Ref<Project> game, Ref<Scene> startScene);

		void OnAttach() override;
		void OnDetach() override;
		void OnUpdate(Timestep timestep) override;

	private:
		// The window's framebuffer size (kept while minimized), or the specification's size without a window.
		void UpdateViewportSize();
		// The cursor in the view's pixels: window events place it in screen coordinates, which differ from the framebuffer's
		// pixels on high-DPI displays (macOS, Wayland).
		void UpdateMousePosition();
		void Render();
		// Reports the test run, sets the exit code and closes the player.
		void FinishTestRun();

		RuntimeLayerSpecification m_Specification;
		Ref<Project> m_Game;
		Ref<Scene> m_StartScene;
		SceneRunner m_Runner;
		// Without a window nothing is rendered.
		Scope<SceneRenderer> m_Renderer;
		Scope<TextureBlitter> m_Blitter;
		uint32_t m_ViewportWidth = 0;
		uint32_t m_ViewportHeight = 0;
		uint64_t m_Frames = 0;
		bool m_MissingCameraReported = false;
	};
}
