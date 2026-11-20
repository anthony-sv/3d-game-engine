#include "Runtime/RuntimeLayer.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/TextureAsset.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Core/Application.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Image.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Profiler.h"
#include "Strada/Core/Window.h"
#include "Strada/Project/PlayerWindow.h"
#include "Strada/RHI/Swapchain.h"
#include "Strada/Renderer/Renderer.h"
#include "Strada/Scene/SceneRendering.h"
#include "Strada/Script/PlayerSettings.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Strada
{
	namespace
	{
		// Gives the window the state the player's settings and the command line make of it. `applied` is the state the
		// window has from the player (it opened in it, and each request since changed it), and becomes the new one. A size is
		// the size a window opened at it has (Window::GetCreatedSize, scaled by the display where the system scales a new
		// window), and is set only when it differs from the size last given, so a request of vsync or of the mode alone
		// leaves the window's size as it is (also one the player dragged); a size asked for while fullscreen is the size the
		// window returns to.
		void ApplyPlayerWindow(PlayerWindowState const& state, PlayerWindowState& applied)
		{
			Application& application = Application::Get();
			Window* const window = application.GetWindow();
			if (window == nullptr)
			{
				return;
			}
			bool const resize = state.Size && state.Size != applied.Size;
			auto const setSize = [window, &applied](glm::uvec2 size)
			{
				glm::uvec2 const created = window->GetCreatedSize(size);
				window->SetSize(created.x, created.y);
				applied.Size = size;
			};
			if (state.Fullscreen)
			{
				// Sized while windowed, so that the window returns to that size. A fullscreen window keeps the size it returns
				// to, and the size last given stays the one it had, so that leaving fullscreen applies the new one.
				if (!window->IsFullscreen())
				{
					if (resize)
					{
						setSize(*state.Size);
					}
					window->SetFullscreen(true);
				}
			}
			else
			{
				// Back in a window first: leaving fullscreen restores the size the window had, which the settings then change.
				window->SetFullscreen(false);
				if (resize)
				{
					setSize(*state.Size);
				}
			}
			applied.Fullscreen = state.Fullscreen;
			if (window->IsVSync() != state.VSync)
			{
				application.SetVSync(state.VSync);
			}
			applied.VSync = state.VSync;
		}

		// Whether the frames are presented waiting for the display, as the swapchain now holds it (the window's flag without
		// one).
		bool IsPresentedWithVSync()
		{
			Application& application = Application::Get();
			if (Swapchain const* const swapchain = application.GetSwapchain())
			{
				return swapchain->IsVSync();
			}
			return application.GetWindow() != nullptr && application.GetWindow()->IsVSync();
		}

		SceneRunnerSettings MakeRunnerSettings(Project const& game, RuntimeLayerSpecification const& specification)
		{
			SceneRunnerSettings settings;
			settings.Runtime = MakeSceneRuntimeSettings(game.GetSettings());
			settings.Runtime.PhysicsHashStep = specification.Test ? specification.HashStep : 0;
			settings.IsEditor = false;
			settings.PlayerDefaults = MakePlayerSettingsDefaults(game.GetSettings().Window);
			// A run without a window keeps the window settings a game changes without applying them.
			if (Application::Get().GetWindow() != nullptr)
			{
				PlayerWindowOverrides overrides;
				overrides.KeepSize = specification.Resolution.has_value();
				overrides.NoVSync = specification.NoVSync;
				settings.ApplyWindow = [overrides, applied = specification.OpenedWindow](PlayerWindowSettings const& window) mutable
				{
					PlayerWindowState const state = ResolvePlayerWindow(window, overrides);
					ApplyPlayerWindow(state, applied);
					ST_INFO("The window takes the player's settings: {}, vsync {} (the swapchain's: {})",
					        state.Fullscreen ? std::string("fullscreen")
					        : state.Size     ? fmt::format("windowed at {}x{}", state.Size->x, state.Size->y)
					                         : std::string("windowed at the size the command line names"),
					        state.VSync ? "on" : "off", IsPresentedWithVSync() ? "on" : "off");
				};
			}
			return settings;
		}

		// "1 frame", "2 frames".
		std::string CountOf(uint64_t count, std::string_view noun)
		{
			return fmt::format("{} {}{}", count, noun, count == 1 ? "" : "s");
		}
	}

	RuntimeLayer::RuntimeLayer(RuntimeLayerSpecification specification, Ref<Project> game, Ref<Scene> startScene)
		: Layer("RuntimeLayer"),
		  m_Specification(std::move(specification)),
		  m_Game(std::move(game)),
		  m_StartScene(std::move(startScene)),
		  m_Runner(MakeRunnerSettings(*m_Game, m_Specification)),
		  m_ViewportWidth(m_Specification.ViewportWidth),
		  m_ViewportHeight(m_Specification.ViewportHeight),
		  m_Screenshot(m_Specification.ScreenshotPath, m_Specification.ScreenshotFrame),
		  m_Resolution(m_Specification.Resolution)
	{
	}

	void RuntimeLayer::OnAttach()
	{
		ApplyWindowIcon();
		if (Application::Get().GetWindow() != nullptr && Renderer::IsInitialized())
		{
			m_Renderer = CreateScope<SceneRenderer>("Game");
			m_Blitter = CreateScope<TextureBlitter>();
		}
		// The scripts see the view's size and the cursor from their first callback on.
		UpdateViewportSize();
		UpdateMousePosition();
		m_StartScene->OnViewportResize(m_ViewportWidth, m_ViewportHeight);
		if (!m_Specification.AudioCapturePath.empty() && !StartAudioCapture())
		{
			return;
		}
		m_Runner.Start(std::move(m_StartScene));
		ST_INFO("Started scene '{}' of {}", m_Runner.GetScene()->GetName(), m_Game->GetSettings().Name);
	}

	void RuntimeLayer::ApplyWindowIcon()
	{
		Window* const window = Application::Get().GetWindow();
		AssetHandle const icon = m_Game->GetSettings().Window.Icon;
		if (window == nullptr || !icon.IsValid())
		{
			return;
		}
		Ref<TextureAsset> const texture = AssetManager::GetAsset<TextureAsset>(icon);
		Result<Image> image = texture ? texture->Decode() : Result<Image>(Error{"it is not a loadable texture"});
		if (!image)
		{
			ST_WARN("The game's icon is not used: {}", image.GetError());
			return;
		}
		// Windows take RGBA: gray and RGB images are expanded, opaque where they have no alpha.
		Image const& decoded = image.GetValue();
		Image rgba(decoded.GetWidth(), decoded.GetHeight(), 4);
		uint32_t const channels = decoded.GetChannels();
		for (uint32_t y = 0; y < decoded.GetHeight(); y++)
		{
			for (uint32_t x = 0; x < decoded.GetWidth(); x++)
			{
				uint8_t const* source = decoded.GetPixel(x, y);
				uint8_t* target = rgba.GetPixel(x, y);
				bool const gray = channels < 3;
				target[0] = source[0];
				target[1] = gray ? source[0] : source[1];
				target[2] = gray ? source[0] : source[2];
				target[3] = channels == 2 ? source[1] : (channels == 4 ? source[3] : 255);
			}
		}
		window->SetIcon(rgba.GetWidth(), rgba.GetHeight(), rgba.GetPixels().data());
	}

	void RuntimeLayer::OnDetach()
	{
		// A run too short to judge its drawing area is judged by its last frame.
		if (std::string const unjudged = m_Resolution.DescribeUnjudged(); !unjudged.empty())
		{
			ReportWrongResolution(unjudged);
		}
		// The application has served or cancelled every capture before its layers detach.
		ReportMissingScreenshot();
		FinishAudioCapture();
		ReportProfiledQuality();
		if (m_Specification.Test && m_Runner.IsRunning())
		{
			// Closed before the run ended (--frames): it did not finish.
			FinishTestRun();
		}
		// The scripts get OnDestroy while every system is still up.
		m_Runner.Stop();
		m_Blitter.reset();
		m_Renderer.reset();
	}

	void RuntimeLayer::OnUpdate(Timestep timestep)
	{
		if (!m_Runner.IsRunning())
		{
			return;
		}
		UpdateViewportSize();
		UpdateMousePosition();
		m_Runner.GetScene()->OnViewportResize(m_ViewportWidth, m_ViewportHeight);
		Timestep const step = m_Specification.FixedTimestep > 0.0f ? Timestep(m_Specification.FixedTimestep) : timestep;
		m_Runner.Update(step);
		m_Frames++;
		// The frame's sound, the last frame of a test run's included. A capture that failed has closed the player: a test run
		// it cut short is reported as unfinished.
		CaptureAudio(step.GetSeconds());
		if (m_AudioCaptureFailed)
		{
			return;
		}

		if (m_Specification.Test)
		{
			if (m_Runner.GetTestReport().Finished || m_Runner.IsQuitRequested() || m_Frames >= m_Specification.TestFrameLimit)
			{
				FinishTestRun();
				return;
			}
		}
		else if (m_Runner.IsQuitRequested())
		{
			// Application.Quit; the scene stops as the layer detaches.
			Application::Get().Close();
			return;
		}

		Application& application = Application::Get();
		if (nvrhi::IFramebuffer* const backBuffer = application.GetBackBuffer(); backBuffer != nullptr)
		{
			nvrhi::FramebufferInfoEx const& target = backBuffer->getFramebufferInfo();
			if (std::string const failure = m_Resolution.Judge(application.GetFrameCount(), glm::uvec2(target.width, target.height));
			    !failure.empty())
			{
				// Nothing more is drawn, captured or profiled at a size nobody asked for.
				ReportWrongResolution(failure);
				application.Close();
				return;
			}
		}

		Render();
		// A frame with a back buffer is presented, and captured, at its end.
		if (m_Screenshot.TakeRequest(application.GetFrameCount(), application.GetBackBuffer() != nullptr))
		{
			// The layer outlives the request: the application serves or cancels captures before detaching its layers.
			application.RequestScreenshotImage(
				[this](Result<Image> image)
				{
					SaveScreenshot(std::move(image));
				});
		}
	}

	void RuntimeLayer::SaveScreenshot(Result<Image> image)
	{
		if (!image)
		{
			m_Screenshot.SetFailed(fmt::format("the window could not be captured: {}", image.GetError()));
			return;
		}
		if (Result<void> written = image.GetValue().WritePNG(m_Screenshot.GetPath()); !written)
		{
			m_Screenshot.SetFailed(written.GetError());
			return;
		}
		m_Screenshot.SetSaved();
		ST_INFO("Saved screenshot to '{}'", FileSystem::PathToUtf8(m_Screenshot.GetPath()));
	}

	void RuntimeLayer::ReportMissingScreenshot()
	{
		std::string const failure = m_Screenshot.DescribeFailure();
		if (failure.empty())
		{
			return;
		}
		ST_ERROR("No screenshot was saved to '{}': {}", FileSystem::PathToUtf8(m_Screenshot.GetPath()), failure);
		// The run did not do what it was asked; a failure code it already has stays.
		if (Application::Get().GetExitCode() == 0)
		{
			Application::Get().SetExitCode(1);
		}
	}

	void RuntimeLayer::ReportWrongResolution(std::string const& failure)
	{
		ST_ERROR("The run stops: {}", failure);
		if (Application::Get().GetExitCode() == 0)
		{
			Application::Get().SetExitCode(1);
		}
	}

	bool RuntimeLayer::StartAudioCapture()
	{
		if (!AudioEngine::IsInitialized() || AudioEngine::GetOutput() != AudioOutput::Manual)
		{
			FailAudioCapture("the audio engine did not start");
			return false;
		}
		Result<Scope<WaveFileWriter>> writer =
			WaveFileWriter::Create(m_Specification.AudioCapturePath, AudioEngine::GetSampleRate(), AudioEngine::GetChannelCount());
		if (!writer)
		{
			FailAudioCapture(writer.GetError());
			return false;
		}
		m_AudioCapture = writer.TakeValue();
		m_AudioClock.emplace(AudioEngine::GetSampleRate());
		ST_INFO("Recording the mix into '{}' ({} Hz, {} channels, 16-bit)", FileSystem::PathToUtf8(m_Specification.AudioCapturePath),
		        AudioEngine::GetSampleRate(), AudioEngine::GetChannelCount());
		return true;
	}

	void RuntimeLayer::CaptureAudio(float seconds)
	{
		if (!m_AudioCapture)
		{
			return;
		}
		uint64_t const frames = m_AudioClock->Advance(static_cast<double>(seconds));
		if (frames == 0)
		{
			return;
		}
		ST_PROFILE_SCOPE("Audio.Capture");
		// Streamed sounds read ahead first, so that what the capture holds never depends on when their thread ran.
		AudioEngine::WaitForStreams();
		m_AudioSamples.resize(static_cast<size_t>(frames) * AudioEngine::GetChannelCount());
		uint64_t const read = AudioEngine::ReadFrames(m_AudioSamples);
		if (read != frames)
		{
			FailAudioCapture(fmt::format("the mix gave {} of {} frames", read, frames));
			return;
		}
		if (Result<void> written = m_AudioCapture->Write(m_AudioSamples); !written)
		{
			FailAudioCapture(written.GetError());
		}
	}

	void RuntimeLayer::FinishAudioCapture()
	{
		if (!m_AudioCapture)
		{
			return;
		}
		uint64_t const frames = m_AudioCapture->GetFrameCount();
		Result<void> const finished = m_AudioCapture->Finish();
		m_AudioCapture.reset();
		if (!finished)
		{
			FailAudioCapture(finished.GetError());
			return;
		}
		ST_INFO("Saved the audio capture of {} frames ({:.3f} s) to '{}'", frames,
		        static_cast<double>(frames) / static_cast<double>(AudioEngine::GetSampleRate()),
		        FileSystem::PathToUtf8(m_Specification.AudioCapturePath));
	}

	void RuntimeLayer::FailAudioCapture(std::string const& failure)
	{
		ST_ERROR("The run stops: the audio capture '{}' failed: {}", FileSystem::PathToUtf8(m_Specification.AudioCapturePath), failure);
		m_AudioCapture.reset();
		m_AudioCaptureFailed = true;
		if (Application::Get().GetExitCode() == 0)
		{
			Application::Get().SetExitCode(1);
		}
		Application::Get().Close();
	}

	void RuntimeLayer::UpdateViewportSize()
	{
		Window const* window = Application::Get().GetWindow();
		if (window != nullptr && window->GetWidth() > 0 && window->GetHeight() > 0)
		{
			m_ViewportWidth = window->GetWidth();
			m_ViewportHeight = window->GetHeight();
		}
	}

	void RuntimeLayer::UpdateMousePosition()
	{
		Window const* window = Application::Get().GetWindow();
		if (window == nullptr)
		{
			return;
		}
		glm::uvec2 const size = window->GetWindowSize();
		if (size.x == 0 || size.y == 0 || window->GetWidth() == 0 || window->GetHeight() == 0)
		{
			return;
		}
		glm::vec2 const pixelsPerUnit(static_cast<float>(window->GetWidth()) / static_cast<float>(size.x),
		                              static_cast<float>(window->GetHeight()) / static_cast<float>(size.y));
		Input::SetMousePosition(window->GetCursorPosition() * pixelsPerUnit);
	}

	void RuntimeLayer::Render()
	{
		nvrhi::IFramebuffer* backBuffer = Application::Get().GetBackBuffer();
		if (backBuffer == nullptr || m_Renderer == nullptr)
		{
			return;
		}
		nvrhi::FramebufferInfoEx const& target = backBuffer->getFramebufferInfo();
		m_Renderer->SetViewportSize(target.width, target.height);
		Scene& scene = *m_Runner.GetScene();
		// The lines scripts draw are as wide on every display.
		SceneRenderOptions options;
		if (Window const* window = Application::Get().GetWindow())
		{
			options.DisplayScale = window->GetContentScale().x;
		}
		// The player's graphics quality, over the scene's settings (Docs/Architecture.md §6.2, "Quality presets").
		options.Quality = PlayerSettingsStore::Get().Quality;
		m_LastQuality = options.Quality;
		m_RenderedQualities = static_cast<uint8_t>(m_RenderedQualities | (1u << static_cast<uint32_t>(options.Quality)));
		if (!RenderSceneFromPrimaryCamera(scene, *m_Renderer, options))
		{
			// The cleared window shows until a camera becomes primary or the scene gets screen-space content.
			if (!m_MissingCameraReported)
			{
				ST_WARN("Scene '{}' has no primary camera and no screen-space UI: nothing is drawn", scene.GetName());
				m_MissingCameraReported = true;
			}
			return;
		}
		m_MissingCameraReported = false;
		m_Blitter->Blit(m_Renderer->GetFinalImage(), backBuffer);
	}

	void RuntimeLayer::FinishTestRun()
	{
		bool const quit = m_Runner.IsQuitRequested();
		// Taken before the scene stops, whose OnDestroy scripts run no steps.
		std::string const physics = DescribePhysicsState();
		// Checks and exceptions of the scripts' OnDestroy count too.
		m_Runner.Stop();
		TestRunReport const& report = m_Runner.GetTestReport();
		uint32_t const failures = report.GetFailureCount() + (report.Finished ? 0 : 1);
		if (failures == 0)
		{
			ST_INFO("Tests passed: {} in {}; {}", CountOf(report.Results.size(), "check"), CountOf(m_Frames, "frame"), physics);
		}
		else
		{
			std::string const unfinished = report.Finished ? std::string()
			                               : quit ? std::string("; the scripts quit before they finished testing")
			                                      : fmt::format("; the scripts did not finish testing in {}", CountOf(m_Frames, "frame"));
			ST_ERROR("Tests failed: {} of {} failed, {}{}; {}", report.GetFailedCount(), CountOf(report.Results.size(), "check"),
			         CountOf(report.ScriptExceptions, "script exception"), unfinished, physics);
		}
		Application::Get().SetExitCode(static_cast<int>(std::min<uint32_t>(failures, MaxTestExitCode)));
		Application::Get().Close();
	}

	void RuntimeLayer::ReportProfiledQuality() const
	{
		if (!Profiler::GetStatus().Recording || m_RenderedQualities == 0)
		{
			return;
		}
		std::string others;
		for (auto const& [preset, name] : EnumTraits<QualityPreset>::Values)
		{
			if (preset != m_LastQuality && (m_RenderedQualities & (1u << static_cast<uint32_t>(preset))) != 0)
			{
				others += fmt::format("{}{}", others.empty() ? "" : ", ", name);
			}
		}
		ST_INFO("Profile quality: {}{}", EnumToString(m_LastQuality),
		        others.empty() ? std::string() : fmt::format(" (the run also rendered at {})", others));
	}

	std::string RuntimeLayer::DescribePhysicsState() const
	{
		Scene const& scene = *m_Runner.GetScene();
		if (m_Specification.HashStep == 0)
		{
			return fmt::format("physics state {:016x} at step {}", scene.ComputePhysicsStateHash(), scene.GetFixedStepCount());
		}
		if (std::optional<uint64_t> const hash = scene.GetRecordedPhysicsHash())
		{
			return fmt::format("physics state {:016x} at step {}", *hash, m_Specification.HashStep);
		}
		return fmt::format("no physics state at step {}: the run ended at step {}", m_Specification.HashStep, scene.GetFixedStepCount());
	}
}
