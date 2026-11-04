#include "Editor/PlayMode.h"

#include "Strada/Core/Input.h"
#include "Strada/Core/Log.h"
#include "Strada/Project/ProjectSettings.h"
#include "Strada/Script/VisualScriptEngine.h"

namespace Strada
{
	PlayMode::PlayMode(EditorContext& context, EditorScripts& scripts)
		: m_Context(context),
		  m_Scripts(scripts)
	{
	}

	PlayMode::~PlayMode()
	{
		Stop();
	}

	Result<void> PlayMode::Start(EditorPlayState state, uint32_t viewportWidth, uint32_t viewportHeight, Ref<Scene> scene,
	                             PlayStartOptions const& options)
	{
		if (IsPlaying())
		{
			return Error{"the editor is already playing"};
		}
		if (state == EditorPlayState::Edit)
		{
			return Error{"play mode starts Play or Simulate"};
		}

		SceneRunnerSettings settings;
		settings.IsEditor = true;
		if (Project const* const project = m_Context.GetProject())
		{
			settings.Runtime = MakeSceneRuntimeSettings(project->GetSettings());
		}
		if (state == EditorPlayState::Simulate)
		{
			settings.Runtime.RunScripts = false;
			settings.Runtime.PlayAudio = false;
		}

		Ref<Scene> running = scene != nullptr ? std::move(scene) : Scene::Copy(m_Context.GetScene());
		running->OnViewportResize(viewportWidth, viewportHeight);
		if (m_Observer != nullptr)
		{
			m_Observer->OnPlayStarting(state, options);
		}
		if (state == EditorPlayState::Play)
		{
			// The runtime problems of a play session count from its start; those its scripts' OnCreate raise are its own.
			VisualScriptEngine::ClearLiveLog();
		}
		m_Runner = CreateScope<SceneRunner>(settings);
		m_Runner->Start(running);
		if (options.Paused)
		{
			running->SetPaused(true);
		}
		m_RunnerSceneVersion = m_Runner->GetSceneVersion();
		m_Context.BeginPlay(std::move(running), state);
		ST_INFO("{} '{}'", state == EditorPlayState::Play ? "Playing" : "Simulating", m_Context.GetScene().GetName());
		// What the scripts' OnCreate did may hold the scene before its first frame (an execution stopped in Event.Create).
		NotifyFrame(Timestep(0.0f));
		return {};
	}

	void PlayMode::RequestStart(EditorPlayState state, uint32_t viewportWidth, uint32_t viewportHeight, Ref<Scene> scene,
	                            StartCallback callback, PlayStartOptions const& options)
	{
		auto const report = [&callback](Result<void> const& started)
		{
			if (callback)
			{
				callback(started);
			}
		};
		if (m_StartPending)
		{
			report(Error{"playing starts once the scripts are built"});
			return;
		}
		bool const buildFirst = state == EditorPlayState::Play && !IsPlaying() && m_Scripts.HasScriptProject() &&
		                        (m_Scripts.IsBuilding() || m_Scripts.IsOutOfDate());
		if (!buildFirst)
		{
			report(Start(state, viewportWidth, viewportHeight, std::move(scene), options));
			return;
		}

		m_StartPending = true;
		ST_INFO("Playing once the scripts are built");
		m_Scripts.Build(
			[this, viewportWidth, viewportHeight, options, scene = std::move(scene),
		     callback = std::move(callback)](ScriptBuildReport const& build) mutable
			{
				m_StartPending = false;
				Result<void> started =
					build.Succeeded ? Start(EditorPlayState::Play, viewportWidth, viewportHeight, std::move(scene), options)
									: Result<void>(Error{build.Error.empty() ? std::string("the scripts did not build: fix them to play")
			                                                                 : build.Error});
				if (callback)
				{
					callback(started);
				}
			});
	}

	void PlayMode::Stop()
	{
		if (m_Runner == nullptr)
		{
			return;
		}
		if (m_Observer != nullptr)
		{
			m_Observer->OnPlayStopping();
		}
		m_Runner->Stop();
		m_LastTestReport = m_Runner->GetTestReport();
		m_Runner.reset();
		m_Context.EndPlay();
		// Keys held and a cursor the game locked must not carry over into editing.
		Input::ReleaseKeysAndMouseButtons();
		Input::SetCursorMode(CursorMode::Normal);
		ST_INFO("Stopped playing");
	}

	TestRunReport const* PlayMode::GetTestReport() const
	{
		if (m_Runner != nullptr)
		{
			return &m_Runner->GetTestReport();
		}
		return m_LastTestReport ? &*m_LastTestReport : nullptr;
	}

	bool PlayMode::SetPaused(bool paused)
	{
		if (m_Runner == nullptr || (!paused && GetFrameStepRefusal()))
		{
			return false;
		}
		m_Runner->GetScene()->SetPaused(paused);
		return true;
	}

	bool PlayMode::IsPaused() const
	{
		return m_Runner != nullptr && m_Runner->GetScene()->IsPaused();
	}

	bool PlayMode::Resume()
	{
		if (m_Runner == nullptr)
		{
			return false;
		}
		// The stopped executions continue first; one that stopped again keeps the scene paused.
		if (m_Observer != nullptr && m_Observer->OnPlayResuming())
		{
			return false;
		}
		// Continuing may have ended play (a script quit) or replaced the scene (a scene load).
		return SetPaused(false);
	}

	void PlayMode::Step(uint32_t frames)
	{
		RefreshObserver();
		if (m_Runner != nullptr && !GetFrameStepRefusal())
		{
			m_Runner->GetScene()->Step(frames);
		}
	}

	void PlayMode::CancelSteps()
	{
		if (m_Runner != nullptr)
		{
			m_Runner->GetScene()->CancelSteps();
		}
	}

	void PlayMode::RefreshObserver()
	{
		if (m_Observer != nullptr && m_Runner != nullptr)
		{
			m_Observer->Refresh();
		}
	}

	std::optional<std::string> PlayMode::GetFrameStepRefusal() const
	{
		return m_Observer != nullptr ? m_Observer->GetFrameStepRefusal() : std::nullopt;
	}

	PlayDebugSummary PlayMode::GetDebugSummary() const
	{
		return m_Observer != nullptr ? m_Observer->GetDebugSummary() : PlayDebugSummary{};
	}

	VisualScriptStop const* PlayMode::FindDebugStop(uint64_t stop) const
	{
		return m_Observer != nullptr ? m_Observer->FindStop(stop) : nullptr;
	}

	void PlayMode::FollowScripts()
	{
		if (m_Runner == nullptr)
		{
			return;
		}
		m_Runner->ApplyRequests();
		FollowRunner();
	}

	void PlayMode::Update(Timestep timestep)
	{
		if (m_Runner == nullptr || GetFrameStepRefusal())
		{
			return;
		}
		// The scene that runs the frame (a scene load replaces it after the frame) says whether one ran: a paused scene
		// without queued steps runs none.
		Ref<Scene> const scene = m_Runner->GetScene();
		uint64_t const frame = scene->GetRuntimeFrame();
		bool const paused = scene->IsPaused();
		scene->SetUIPointerEnabled(m_UIPointerEnabled);
		m_Runner->Update(timestep);
		FollowRunner();
		KeepPaused(*scene, paused);
		if (scene->GetRuntimeFrame() != frame)
		{
			NotifyFrame(timestep);
		}
	}

	uint32_t PlayMode::Advance(uint32_t frames, Timestep timestep)
	{
		RefreshObserver();
		uint32_t run = 0;
		while (run < frames && m_Runner != nullptr && !GetFrameStepRefusal())
		{
			Ref<Scene> const scene = m_Runner->GetScene();
			if (scene->IsPaused())
			{
				scene->Step(1);
			}
			uint64_t const frame = scene->GetRuntimeFrame();
			bool const paused = scene->IsPaused();
			scene->SetUIPointerEnabled(m_UIPointerEnabled);
			m_Runner->Update(timestep);
			run++;
			FollowRunner();
			KeepPaused(*scene, paused);
			bool const held = scene->GetRuntimeFrame() != frame && NotifyFrame(timestep);
			Input::EndFrame();
			if (held)
			{
				break;
			}
		}
		return run;
	}

	bool PlayMode::NotifyFrame(Timestep timestep)
	{
		if (m_Observer == nullptr || m_Runner == nullptr || !m_Observer->OnPlayFrame(timestep))
		{
			return false;
		}
		// An observer may stop playing from OnPlayFrame.
		if (m_Runner != nullptr)
		{
			m_Runner->GetScene()->SetPaused(true);
		}
		return true;
	}

	void PlayMode::KeepPaused(Scene const& ran, bool paused)
	{
		// A scene starts running when it loads (Scene::OnRuntimeStart), and the steps queued for the scene it replaced went
		// with that scene: an agent stepping a paused game would see the game run on otherwise.
		if (!paused || m_Runner == nullptr || m_Runner->GetScene().get() == &ran)
		{
			return;
		}
		Ref<Scene> const loaded = m_Runner->GetScene();
		loaded->SetPaused(true);
		loaded->Step(ran.GetQueuedSteps());
	}

	void PlayMode::FollowRunner()
	{
		if (m_Runner->IsQuitRequested())
		{
			Stop();
			return;
		}
		if (m_Runner->GetSceneVersion() != m_RunnerSceneVersion)
		{
			m_RunnerSceneVersion = m_Runner->GetSceneVersion();
			m_Context.SetRunningScene(m_Runner->GetScene());
		}
	}
}
