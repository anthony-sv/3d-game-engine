#include "Runtime/RuntimeLayer.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/TextureAsset.h"
#include "Strada/Core/Application.h"
#include "Strada/Core/Image.h"
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
		ApplyWindowIcon();
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
		// The lines scripts draw are as wide on every display.
		SceneRenderOptions options;
		if (Window const* window = Application::Get().GetWindow())
		{
			options.DisplayScale = window->GetContentScale().x;
		}
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
