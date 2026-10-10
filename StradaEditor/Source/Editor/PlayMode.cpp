#include "Editor/PlayMode.h"

#include "Strada/Core/Input.h"
#include "Strada/Core/Log.h"
#include "Strada/Project/ProjectSettings.h"

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

	Result<void> PlayMode::Start(EditorPlayState state, uint32_t viewportWidth, uint32_t viewportHeight, Ref<Scene> scene)
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
		m_Runner = CreateScope<SceneRunner>(settings);
		m_Runner->Start(running);
		m_RunnerSceneVersion = m_Runner->GetSceneVersion();
		m_Context.BeginPlay(std::move(running), state);
		ST_INFO("{} '{}'", state == EditorPlayState::Play ? "Playing" : "Simulating", m_Context.GetScene().GetName());
		return {};
	}

	void PlayMode::RequestStart(EditorPlayState state, uint32_t viewportWidth, uint32_t viewportHeight, Ref<Scene> scene,
	                            StartCallback callback)
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
			report(Start(state, viewportWidth, viewportHeight, std::move(scene)));
			return;
		}

		m_StartPending = true;
		ST_INFO("Playing once the scripts are built");
		m_Scripts.Build(
			[this, viewportWidth, viewportHeight, scene = std::move(scene),
		     callback = std::move(callback)](ScriptBuildReport const& build) mutable
			{
				m_StartPending = false;
				Result<void> started =
					build.Succeeded ? Start(EditorPlayState::Play, viewportWidth, viewportHeight, std::move(scene))
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
		m_Runner->Stop();
		m_LastTestReport = m_Runner->GetTestReport();
		m_Runner.reset();
		m_Context.EndPlay();
		// Keys held and a cursor the game locked must not carry over into editing.
		Input::ReleaseAll();
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

	void PlayMode::SetPaused(bool paused)
	{
		if (m_Runner != nullptr)
		{
			m_Runner->GetScene()->SetPaused(paused);
		}
	}

	bool PlayMode::IsPaused() const
	{
		return m_Runner != nullptr && m_Runner->GetScene()->IsPaused();
	}

	void PlayMode::Step(uint32_t frames)
	{
		if (m_Runner != nullptr)
		{
			m_Runner->GetScene()->Step(frames);
		}
	}

	void PlayMode::Update(Timestep timestep)
	{
		if (m_Runner == nullptr)
		{
			return;
		}
		m_Runner->Update(timestep);
		FollowRunner();
	}

	uint32_t PlayMode::Advance(uint32_t frames, Timestep timestep)
	{
		uint32_t run = 0;
		while (run < frames && m_Runner != nullptr)
		{
			Scene& scene = *m_Runner->GetScene();
			if (scene.IsPaused())
			{
				scene.Step(1);
			}
			m_Runner->Update(timestep);
			run++;
			FollowRunner();
			Input::EndFrame();
		}
		return run;
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
