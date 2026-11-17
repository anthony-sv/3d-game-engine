#include "stpch.h"
#include "Strada/Scene/Scene.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/InputActionsAsset.h"
#include "Strada/Core/Profiler.h"

#include <algorithm>

namespace Strada
{
	namespace
	{
		// The InputActions asset, or null when the handle names none that loads (quietly: the caller logs).
		Ref<TypedDataAsset<InputActionMap>> FindInputActions(AssetHandle handle)
		{
			AssetType const type = TypedDataAsset<InputActionMap>::GetStaticType();
			if (!handle.IsValid() || !AssetManager::IsInitialized() || type == AssetType::None ||
			    AssetManager::GetAssetType(handle) != type)
			{
				return nullptr;
			}
			return AssetManager::GetAsset<TypedDataAsset<InputActionMap>>(handle);
		}

		std::string DescribeAsset(AssetHandle handle)
		{
			return AssetManager::IsInitialized() ? AssetManager::GetReference(handle) : handle.ToString();
		}
	}

	void Scene::StartInputActions()
	{
		StopInputActions();
		if (m_RuntimeSettings.InputActions.IsValid())
		{
			RebuildInputActions(true);
		}
	}

	void Scene::StopInputActions()
	{
		m_InputActions.Clear();
		m_InputActionsEpoch = 0;
		m_InputActionsAssetVersion = 0;
		m_InputActionCurves.clear();
	}

	void Scene::UpdateInputActions()
	{
		if (!m_RuntimeSettings.InputActions.IsValid())
		{
			return;
		}
		ST_PROFILE_SCOPE("Scene.InputActions");
		if (DataAssetRegistry::GetEpoch() != m_InputActionsEpoch)
		{
			RebuildInputActions(false);
		}
		m_InputActions.Evaluate();
	}

	void Scene::RebuildInputActions(bool starting)
	{
		// Every data asset change moves the epoch, so an unchanged epoch means nothing the set was made from changed. Taken
		// after the assets below loaded (a load moves it too).
		struct EpochAtExit
		{
			uint64_t& Epoch;
			~EpochAtExit() { Epoch = DataAssetRegistry::GetEpoch(); }
		} const epochAtExit{m_InputActionsEpoch};
		AssetHandle const handle = m_RuntimeSettings.InputActions;
		Ref<TypedDataAsset<InputActionMap>> const asset = FindInputActions(handle);
		uint64_t const version = asset ? asset->GetVersion() : 0;
		bool const curvesChanged = std::ranges::any_of(m_InputActionCurves,
		                                               [](std::pair<AssetHandle, uint64_t> const& use)
		                                               {
														   return GetInputResponseCurveVersion(use.first) != use.second;
													   });
		if (!starting && version == m_InputActionsAssetVersion && !curvesChanged)
		{
			return;
		}
		m_InputActionsAssetVersion = version;
		m_InputActionCurves.clear();
		if (!asset)
		{
			ST_CORE_WARN("Input actions: {} does not load as an InputActions asset; {}", DescribeAsset(handle),
			             m_InputActions.GetActions() ? "the actions keep their bindings" : "every action reads zero");
			return;
		}
		std::vector<InputResponseCurveUse> curves;
		Result<Ref<InputActionSet const>> set = CompileInputActions(asset->GetData(), &curves);
		for (InputResponseCurveUse const& use : curves)
		{
			m_InputActionCurves.emplace_back(use.Curve, use.Version);
		}
		if (!set)
		{
			ST_CORE_WARN("Input actions: {}: {}; {}", DescribeAsset(handle), set.GetError(),
			             m_InputActions.GetActions() ? "the actions keep their bindings" : "every action reads zero");
			return;
		}
		if (starting)
		{
			ST_CORE_INFO("Input actions: {} actions from {}", set.GetValue()->GetActions().size(), DescribeAsset(handle));
		}
		m_InputActions.SetActions(std::move(set.GetValue()));
	}
}
