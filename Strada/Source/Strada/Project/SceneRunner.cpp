#include "stpch.h"
#include "Strada/Project/SceneRunner.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Scene/SceneSerializer.h"

#include <algorithm>

namespace Strada
{
	uint32_t TestRunReport::GetFailedCount() const
	{
		return static_cast<uint32_t>(std::count_if(Results.begin(), Results.end(),
		                                           [](ScriptTestResult const& result)
		                                           {
													   return !result.Passed;
												   }));
	}

	SceneRunner::SceneRunner(SceneRunnerSettings settings)
		: m_Settings(std::move(settings))
	{
	}

	SceneRunner::~SceneRunner()
	{
		Stop();
	}

	void SceneRunner::Start(Ref<Scene> scene)
	{
		ST_CORE_ASSERT(scene != nullptr && !scene->IsRunning(), "SceneRunner::Start needs a scene that is not running");
		Stop();
		m_Scene = std::move(scene);
		m_SceneVersion++;
		ScriptEngine::SetHost(this);
		m_Scene->OnRuntimeStart(m_Settings.Runtime);
	}

	void SceneRunner::Stop()
	{
		if (m_Scene == nullptr)
		{
			return;
		}
		m_Scene->OnRuntimeStop();
		m_Scene.reset();
		m_SceneVersion++;
		m_RequestedScene = AssetHandle();
		if (ScriptEngine::GetHost() == this)
		{
			ScriptEngine::SetHost(nullptr);
		}
	}

	void SceneRunner::Update(Timestep timestep)
	{
		if (m_Scene == nullptr)
		{
			return;
		}
		m_Scene->OnUpdateRuntime(timestep);
		if (m_RequestedScene.IsValid())
		{
			LoadRequestedScene();
		}
	}

	void SceneRunner::RequestSceneLoad(AssetHandle scene)
	{
		m_RequestedScene = scene;
	}

	void SceneRunner::RequestQuit()
	{
		m_QuitRequested = true;
	}

	void SceneRunner::ReportTestResult(ScriptTestResult const& result)
	{
		m_TestReport.Results.push_back(result);
	}

	void SceneRunner::FinishTests()
	{
		m_TestReport.Finished = true;
	}

	void SceneRunner::OnScriptException()
	{
		m_TestReport.ScriptExceptions++;
	}

	void SceneRunner::LoadRequestedScene()
	{
		AssetHandle const handle = m_RequestedScene;
		m_RequestedScene = AssetHandle();
		std::filesystem::path const path = AssetManager::IsInitialized() ? AssetManager::GetAbsolutePath(handle) : std::filesystem::path();
		if (path.empty())
		{
			ST_CORE_ERROR("SceneManager.LoadScene: scene {} is not a scene file of the project", handle);
			return;
		}
		Result<Ref<Scene>> loaded =
			SceneSerializer::LoadFromFile(path, AssetManager::CreateDeserializationContext(UnknownFieldPolicy::Warn));
		if (!loaded)
		{
			ST_CORE_ERROR("SceneManager.LoadScene: {}", loaded.GetError());
			return;
		}

		// The new scene renders to the same view.
		uint32_t const width = m_Scene->GetViewportWidth();
		uint32_t const height = m_Scene->GetViewportHeight();
		// Only one scene runs scripts at a time: the current one stops first. Loads its scripts request while stopping are
		// dropped; those of the new scene's scripts (a loading scene moving on at once) are acted on after the next frame.
		m_Scene->OnRuntimeStop();
		m_RequestedScene = AssetHandle();
		m_Scene = loaded.TakeValue();
		m_SceneVersion++;
		m_Scene->OnViewportResize(width, height);
		ScriptEngine::SetHost(this);
		m_Scene->OnRuntimeStart(m_Settings.Runtime);
		ST_CORE_INFO("Loaded scene '{}' ({})", m_Scene->GetName(), FileSystem::PathToUtf8(path.filename()));
	}
}
