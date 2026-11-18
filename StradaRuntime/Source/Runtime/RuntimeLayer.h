#pragma once

#include "Runtime/AudioCaptureClock.h"
#include "Runtime/ResolutionRequest.h"
#include "Runtime/ScreenshotRequest.h"
#include "Strada/Audio/WaveFileWriter.h"
#include "Strada/Core/Image.h"
#include "Strada/Core/Layer.h"
#include "Strada/Core/Result.h"
#include "Strada/Project/Project.h"
#include "Strada/Project/SceneRunner.h"
#include "Strada/Renderer/SceneRenderer.h"
#include "Strada/Renderer/TextureBlitter.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Strada
{
	struct RuntimeLayerSpecification
	{
		// A test run: ends when the scripts finish testing (Strada.Testing), quit or run out of frames, and exits with the
		// number of failures (at most MaxTestExitCode; a run that did not finish counts as one more).
		bool Test = false;
		uint64_t TestFrameLimit = 3600;
		// The fixed step whose physics state hash the test summary prints (SceneRuntimeSettings::PhysicsHashStep); 0 prints
		// the hash after the run's last step.
		uint64_t HashStep = 0;
		// Seconds each frame advances the game; 0 advances it by the real frame time.
		float FixedTimestep = 0.0f;
		// Saves the window's contents to this file in the first frame drawn at or after the given one (empty: no
		// screenshot); a run that saves none exits with an error (ScreenshotRequest).
		std::filesystem::path ScreenshotPath;
		uint64_t ScreenshotFrame = 0;
		// The drawing area's size in pixels the window was given (--resolution); a run whose window cannot have it exits
		// with an error (ResolutionRequest). None for a window of the game's own size.
		std::optional<glm::uvec2> Resolution;
		// Records the mix into this WAV file of 16-bit PCM (empty: none). The application mixes through manual output
		// (ApplicationSpecification::ManualAudio): after each frame's update the layer reads the frames of audio that frame's
		// time covers (AudioCaptureClock). A run whose capture cannot be made or written exits with an error.
		std::filesystem::path AudioCapturePath;
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
		// The game window shows the project's icon when it sets one.
		void ApplyWindowIcon();
		// The window's framebuffer size (kept while minimized), or the specification's size without a window.
		void UpdateViewportSize();
		// The cursor in the view's pixels: window events place it in screen coordinates, which differ from the framebuffer's
		// pixels on high-DPI displays (macOS, Wayland).
		void UpdateMousePosition();
		void Render();
		// Writes the captured window image to the screenshot's file and records how it went.
		void SaveScreenshot(Result<Image> image);
		// A run that wanted a screenshot and saved none says why and fails.
		void ReportMissingScreenshot();
		// A run whose drawing area is not the size it was asked for says so and fails.
		void ReportWrongResolution(std::string const& failure);
		// Opens the audio capture's file; false (logged, exit code 1) when it cannot record.
		bool StartAudioCapture();
		// Reads the frames of audio the frame's seconds cover into the capture's file.
		void CaptureAudio(float seconds);
		// Writes the capture's header and closes its file.
		void FinishAudioCapture();
		// A capture that could not be made or written stops the run with exit code 1.
		void FailAudioCapture(std::string const& failure);
		// Reports the test run, sets the exit code and closes the player.
		void FinishTestRun();
		// The end of the test summary: the running scene's physics state hash and the fixed step it was taken after.
		std::string DescribePhysicsState() const;

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
		ScreenshotRequest m_Screenshot;
		ResolutionRequest m_Resolution;
		bool m_MissingCameraReported = false;
		// The audio capture (--audio-capture): its file while it records, its clock and the frames each read fills.
		Scope<WaveFileWriter> m_AudioCapture;
		std::optional<AudioCaptureClock> m_AudioClock;
		std::vector<float> m_AudioSamples;
		bool m_AudioCaptureFailed = false;
	};
}
