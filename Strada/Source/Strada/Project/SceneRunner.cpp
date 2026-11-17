#include "stpch.h"
#include "Strada/Project/SceneRunner.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/MixerAsset.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Scene/SceneSerializer.h"

#include <algorithm>
#include <string>

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

	Result<Ref<Scene>> LoadSceneAsset(AssetHandle scene)
	{
		if (!AssetManager::IsInitialized() || AssetManager::GetAssetType(scene) != AssetType::Scene)
		{
			return MakeError("asset {} is not a scene file of the project", scene);
		}
		if (AssetManager::IsMissing(scene))
		{
			return MakeError("the scene file of asset {} is missing", scene);
		}
		return SceneSerializer::LoadFromFile(AssetManager::GetAbsolutePath(scene),
		                                     AssetManager::CreateDeserializationContext(UnknownFieldPolicy::Warn));
	}

	namespace
	{
		// The Mixer asset, or null when the handle names none that loads (quietly: the caller logs).
		Ref<TypedDataAsset<Mixer>> FindMixer(AssetHandle handle)
		{
			AssetType const type = TypedDataAsset<Mixer>::GetStaticType();
			if (!handle.IsValid() || !AssetManager::IsInitialized() || type == AssetType::None ||
			    AssetManager::GetAssetType(handle) != type)
			{
				return nullptr;
			}
			return AssetManager::GetAsset<TypedDataAsset<Mixer>>(handle);
		}

		std::string DescribeAsset(AssetHandle handle)
		{
			return AssetManager::IsInitialized() ? AssetManager::GetReference(handle) : handle.ToString();
		}

		std::string ListBusNames(Mixer const& mixer)
		{
			std::string names;
			for (MixerBus const& bus : mixer.Buses)
			{
				names += names.empty() ? bus.Name : ", " + bus.Name;
			}
			return names;
		}
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
		if (m_Settings.Runtime.PlayAudio && AudioEngine::IsInitialized())
		{
			AudioEngine::ResetGameValues();
			SyncMixer(true);
		}
		m_Scene->OnRuntimeStart(m_Settings.Runtime);
	}

	void SceneRunner::SyncMixer(bool starting)
	{
		// Every data asset change moves the epoch, so an unchanged epoch means the mixer did not change. Taken after the asset
		// loaded (a load moves it too).
		struct EpochAtExit
		{
			uint64_t& Epoch;
			~EpochAtExit() { Epoch = DataAssetRegistry::GetEpoch(); }
		} const epochAtExit{m_MixerEpoch};
		AssetHandle const handle = m_Settings.Runtime.Mixer;
		Ref<TypedDataAsset<Mixer>> const asset = FindMixer(handle);
		uint64_t const version = asset ? asset->GetVersion() : 0;
		if (!starting && version == m_MixerVersion)
		{
			return;
		}
		m_MixerVersion = version;
		if (asset)
		{
			AudioEngine::SetMixer(asset->GetData());
			if (starting)
			{
				Mixer const& mixer = AudioEngine::GetMixer();
				ST_CORE_INFO("Mixer: {} buses from {} ({})", mixer.Buses.size(), DescribeAsset(handle), ListBusNames(mixer));
			}
			return;
		}
		if (starting || !handle.IsValid())
		{
			AudioEngine::SetMixer(Mixer{});
		}
		if (handle.IsValid())
		{
			// At the start the game mixes through Master alone; later the mixer it had stays.
			ST_CORE_WARN("Mixer: {} does not load as a Mixer asset; {}", DescribeAsset(handle),
			             starting ? "sounds play on Master alone" : "the mix keeps its buses");
		}
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
		if (m_Settings.Runtime.PlayAudio && AudioEngine::IsInitialized())
		{
			AudioEngine::ResetGameValues();
			AudioEngine::ClearEffectState();
		}
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
		if (m_Settings.Runtime.PlayAudio && AudioEngine::IsInitialized() && DataAssetRegistry::GetEpoch() != m_MixerEpoch)
		{
			SyncMixer(false);
		}
		m_Scene->OnUpdateRuntime(timestep);
		ApplyRequests();
	}

	void SceneRunner::ApplyRequests()
	{
		if (m_Scene != nullptr && m_RequestedScene.IsValid())
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
		Result<Ref<Scene>> loaded = LoadSceneAsset(handle);
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
		ST_CORE_INFO("Loaded scene '{}' ({})", m_Scene->GetName(),
		             FileSystem::PathToUtf8(AssetManager::GetAbsolutePath(handle).filename()));
	}
}
