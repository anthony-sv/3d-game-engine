#include "Runtime/RuntimeLayer.h"

#include "Strada/Core/Application.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Window.h"
#include "Strada/Renderer/Renderer.h"
#include "Strada/Scene/SceneRendering.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace Strada
{
	namespace
	{
		SceneRunnerSettings MakeRunnerSettings(Project const& game)
		{
			SceneRunnerSettings settings;
			settings.Runtime = MakeSceneRuntimeSettings(game.GetSettings());
			settings.IsEditor = false;
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
		  m_Runner(MakeRunnerSettings(*m_Game)),
		  m_ViewportWidth(m_Specification.ViewportWidth),
		  m_ViewportHeight(m_Specification.ViewportHeight)
	{
	}

	void RuntimeLayer::OnAttach()
	{
		if (Application::Get().GetWindow() != nullptr && Renderer::IsInitialized())
		{
			m_Renderer = CreateScope<SceneRenderer>();
			m_Blitter = CreateScope<TextureBlitter>();
		}
		// The scripts see the view's size and the cursor from their first callback on.
		UpdateViewportSize();
		UpdateMousePosition();
		m_StartScene->OnViewportResize(m_ViewportWidth, m_ViewportHeight);
		m_Runner.Start(std::move(m_StartScene));
		ST_INFO("Started scene '{}' of {}", m_Runner.GetScene()->GetName(), m_Game->GetSettings().Name);
	}

	void RuntimeLayer::OnDetach()
	{
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
		m_Runner.Update(m_Specification.FixedTimestep > 0.0f ? Timestep(m_Specification.FixedTimestep) : timestep);
		m_Frames++;

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

		Render();
		if (!m_Specification.ScreenshotPath.empty() && Application::Get().GetFrameCount() == m_Specification.ScreenshotFrame)
		{
			Application::Get().RequestScreenshot(m_Specification.ScreenshotPath);
		}
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
		if (!RenderSceneFromPrimaryCamera(scene, *m_Renderer))
		{
			// The cleared window shows until a camera becomes primary.
			if (!m_MissingCameraReported)
			{
				ST_WARN("Scene '{}' has no primary camera: nothing is drawn", scene.GetName());
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
		// Checks and exceptions of the scripts' OnDestroy count too.
		m_Runner.Stop();
		TestRunReport const& report = m_Runner.GetTestReport();
		uint32_t const failures = report.GetFailureCount() + (report.Finished ? 0 : 1);
		if (failures == 0)
		{
			ST_INFO("Tests passed: {} in {}", CountOf(report.Results.size(), "check"), CountOf(m_Frames, "frame"));
		}
		else
		{
			std::string const unfinished = report.Finished ? std::string()
			                               : quit ? std::string("; the scripts quit before they finished testing")
			                                      : fmt::format("; the scripts did not finish testing in {}", CountOf(m_Frames, "frame"));
			ST_ERROR("Tests failed: {} of {} failed, {}{}", report.GetFailedCount(), CountOf(report.Results.size(), "check"),
			         CountOf(report.ScriptExceptions, "script exception"), unfinished);
		}
		Application::Get().SetExitCode(static_cast<int>(std::min<uint32_t>(failures, MaxTestExitCode)));
		Application::Get().Close();
	}
}
