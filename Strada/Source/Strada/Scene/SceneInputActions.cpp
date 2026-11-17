#include "stpch.h"
#include "Strada/Scene/Scene.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/InputActionsAsset.h"
#include "Strada/Asset/InputRebinding.h"
#include "Strada/Core/Profiler.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/VisualScriptEngine.h"

#include <algorithm>
#include <vector>

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

		InputRebindResult RefuseOutsideRuntime(std::string_view action, int32_t binding, int32_t part)
		{
			InputRebindResult result;
			result.Status = InputRebindStatus::Refused;
			result.Action = std::string(action);
			result.Binding = binding;
			result.Part = part;
			result.Message = "the scene does not run";
			return result;
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
		m_InputRebindingVersion = 0;
		m_InputCapture.Stop();
		m_InputRebind = {};
		m_InputRebindAnswer = {};
	}

	void Scene::UpdateInputActions(float frameSeconds)
	{
		if (!m_RuntimeSettings.InputActions.IsValid())
		{
			return;
		}
		ST_PROFILE_SCOPE("Scene.InputActions");
		UpdateInputRebind(frameSeconds);
		if (DataAssetRegistry::GetEpoch() != m_InputActionsEpoch || InputRebinding::GetVersion() != m_InputRebindingVersion)
		{
			RebuildInputActions(false);
		}
		m_InputActions.Evaluate();
		DispatchInputActionEvents();
	}

	void Scene::DispatchInputActionEvents()
	{
		if (!m_RunsScripts || m_InputActions.GetEdges().empty())
		{
			return;
		}
		// Copies: handlers may start and stop instances, and the set stays alive for the names whatever they do.
		Ref<InputActionSet const> const actions = m_InputActions.GetActions();
		std::vector<InputActionEdge> const edges(m_InputActions.GetEdges().begin(), m_InputActions.GetEdges().end());
		std::vector<UUID> const scripts = GetScriptInstanceIDs();
		std::vector<UUID> const visualScripts = GetVisualScriptInstanceIDs();
		for (InputActionEdge const& edge : edges)
		{
			ScriptEngine::InputActionEventScope const scope(actions->GetActions()[edge.Action].Name);
			for (UUID const id : scripts)
			{
				if (edge.Pressed)
				{
					ScriptEngine::InvokeOnActionPressed(id);
				}
				else
				{
					ScriptEngine::InvokeOnActionReleased(id);
				}
			}
			for (UUID const id : visualScripts)
			{
				if (edge.Pressed)
				{
					VisualScriptEngine::InvokeOnActionPressed(id);
				}
				else
				{
					VisualScriptEngine::InvokeOnActionReleased(id);
				}
			}
		}
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
		if (starting)
		{
			// The player's changed bindings come with the actions: read from the game's profile when the process holds none
			// for this asset and game data folder yet, kept otherwise (a scene loaded after another).
			InputRebinding::Load(handle);
		}
		uint64_t const rebindingVersion = InputRebinding::GetVersion();
		if (!starting && version == m_InputActionsAssetVersion && !curvesChanged && rebindingVersion == m_InputRebindingVersion)
		{
			return;
		}
		m_InputActionsAssetVersion = version;
		m_InputRebindingVersion = rebindingVersion;
		m_InputActionCurves.clear();
		if (!asset)
		{
			ST_CORE_WARN("Input actions: {} does not load as an InputActions asset; {}", DescribeAsset(handle),
			             m_InputActions.GetActions() ? "the actions keep their bindings" : "every action reads zero");
			return;
		}
		std::vector<InputResponseCurveUse> curves;
		Result<Ref<InputActionSet const>> set = CompileInputActions(InputRebinding::Apply(handle, asset->GetData()), &curves);
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

	void Scene::UpdateInputRebind(float frameSeconds)
	{
		if (!m_InputCapture.IsListening())
		{
			return;
		}
		switch (m_InputCapture.Update(frameSeconds, !m_UIPointerOver))
		{
			case InputCaptureOutcome::Waiting:
				return;
			case InputCaptureOutcome::Escaped:
				m_InputRebind.Status = InputRebindStatus::Cancelled;
				m_InputRebind.Message = "Escape cancelled the capture";
				return;
			case InputCaptureOutcome::TimedOut:
				m_InputRebind.Status = InputRebindStatus::Cancelled;
				m_InputRebind.Message = fmt::format("no control came within {} seconds", InputControlCapture::Timeout);
				return;
			case InputCaptureOutcome::Captured:
			{
				m_InputRebind = InputRebinding::Rebind(m_RuntimeSettings.InputActions, m_InputRebind.Action, m_InputRebind.Binding,
				                                       FormatInputControl(m_InputCapture.GetControl()), m_InputRebind.Part);
				// The press that bound the control is no press of the action, whether or not a new set comes: binding the
				// control the binding already read compiles nothing again.
				Ref<InputActionSet const> const& actions = m_InputActions.GetActions();
				if (std::optional<uint32_t> const rebound = actions ? actions->Find(m_InputRebind.Action) : std::nullopt)
				{
					m_InputActions.Settle(*rebound);
				}
				return;
			}
		}
	}

	InputRebindResult Scene::StartInputRebind(std::string_view action, int32_t binding, int32_t part)
	{
		CancelInputRebind();
		if (!m_IsRunning)
		{
			m_InputRebindAnswer = RefuseOutsideRuntime(action, binding, part);
			return m_InputRebindAnswer;
		}
		InputRebindResult result;
		result.Action = std::string(action);
		result.Binding = binding;
		result.Part = part;
		Result<InputCapturePlan> const plan = InputRebinding::PlanCapture(m_RuntimeSettings.InputActions, action, binding, part);
		if (plan)
		{
			result.Status = InputRebindStatus::Listening;
			result.Controls = plan.GetValue().Controls;
			m_InputCapture.Start(plan.GetValue().Target, plan.GetValue().Gamepad);
		}
		else
		{
			result.Status = InputRebindStatus::Refused;
			result.Message = plan.GetError();
		}
		m_InputRebind = result;
		m_InputRebindAnswer = result;
		return result;
	}

	void Scene::CancelInputRebind()
	{
		if (!m_InputCapture.IsListening())
		{
			return;
		}
		m_InputCapture.Stop();
		m_InputRebind.Status = InputRebindStatus::Cancelled;
		m_InputRebind.Message = "the capture was cancelled";
	}

	InputRebindResult Scene::RebindInput(std::string_view action, int32_t binding, std::string_view controls, int32_t part)
	{
		if (!m_IsRunning)
		{
			m_InputRebindAnswer = RefuseOutsideRuntime(action, binding, part);
			return m_InputRebindAnswer;
		}
		m_InputRebindAnswer = InputRebinding::Rebind(m_RuntimeSettings.InputActions, action, binding, controls, part);
		return m_InputRebindAnswer;
	}

	InputRebindResult Scene::ResetInputBindings()
	{
		if (!m_IsRunning)
		{
			m_InputRebindAnswer = RefuseOutsideRuntime({}, -1, -1);
			return m_InputRebindAnswer;
		}
		m_InputRebindAnswer = InputRebinding::ResetBindings(m_RuntimeSettings.InputActions);
		return m_InputRebindAnswer;
	}

	Result<std::string> Scene::GetInputBindingControls(std::string_view action, int32_t binding) const
	{
		if (!m_IsRunning)
		{
			return Error{"the scene does not run"};
		}
		return InputRebinding::GetBindingControls(m_RuntimeSettings.InputActions, action, binding);
	}
}
