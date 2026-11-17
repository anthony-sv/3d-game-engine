#include "stpch.h"
#include "Strada/Asset/InputActionsAsset.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/CurveAsset.h"

#include <algorithm>

namespace Strada
{
	namespace
	{
		// The curve asset, or null when the handle names none that loads (quietly: the caller logs).
		Ref<TypedDataAsset<Curve>> FindCurve(AssetHandle handle)
		{
			AssetType const type = TypedDataAsset<Curve>::GetStaticType();
			if (!handle.IsValid() || !AssetManager::IsInitialized() || type == AssetType::None ||
			    AssetManager::GetAssetType(handle) != type)
			{
				return nullptr;
			}
			return AssetManager::GetAsset<TypedDataAsset<Curve>>(handle);
		}

		// The runtime's specifications of a map, with each binding's controls parsed; the responses are left empty.
		Result<std::vector<InputActionSpecification>> MakeSpecifications(InputActionMap const& map)
		{
			std::vector<InputActionSpecification> actions;
			actions.reserve(map.Actions.size());
			for (InputAction const& action : map.Actions)
			{
				InputActionSpecification& specification = actions.emplace_back();
				specification.Name = action.Name;
				specification.Type = action.Type;
				specification.PressThreshold = action.PressThreshold;
				specification.ReleaseThreshold = action.ReleaseThreshold;
				specification.Bindings.reserve(action.Bindings.size());
				for (size_t index = 0; index < action.Bindings.size(); index++)
				{
					InputActionBinding const& binding = action.Bindings[index];
					Result<InputControlList> controls = ParseInputControls(binding.Controls);
					if (!controls)
					{
						return MakeError("action '{}', binding {}: {}", action.Name, index, controls.GetError());
					}
					InputBindingSpecification& bound = specification.Bindings.emplace_back();
					bound.Controls = controls.GetValue();
					bound.Gamepad = binding.Gamepad;
					bound.DeadZone = binding.DeadZone;
					bound.Invert = binding.Invert;
					bound.InvertY = binding.InvertY;
					bound.Scale = binding.Scale;
				}
			}
			return actions;
		}
	}

	Result<void> ValidateInputActionMap(InputActionMap const& map)
	{
		// One set of rules: the runtime's, which a map must pass to be evaluated.
		Result<std::vector<InputActionSpecification>> actions = MakeSpecifications(map);
		if (!actions)
		{
			return Error{actions.GetError()};
		}
		if (Result<Ref<InputActionSet const>> const set = InputActionSet::Create(std::move(actions.GetValue())); !set)
		{
			return Error{set.GetError()};
		}
		return {};
	}

	Result<Ref<InputActionSet const>> CompileInputActions(InputActionMap const& map, std::vector<InputResponseCurveUse>* curves)
	{
		Result<std::vector<InputActionSpecification>> actions = MakeSpecifications(map);
		if (!actions)
		{
			return Error{actions.GetError()};
		}
		std::vector<InputResponseCurveUse> used;
		std::vector<InputActionSpecification>& specifications = actions.GetValue();
		for (size_t action = 0; action < map.Actions.size(); action++)
		{
			for (size_t binding = 0; binding < map.Actions[action].Bindings.size(); binding++)
			{
				AssetHandle const handle = map.Actions[action].Bindings[binding].ResponseCurve;
				if (!handle.IsValid())
				{
					continue;
				}
				Ref<TypedDataAsset<Curve>> const asset = FindCurve(handle);
				bool const known = std::ranges::any_of(used,
				                                       [handle](InputResponseCurveUse const& use)
				                                       {
														   return use.Curve == handle;
													   });
				if (!known)
				{
					used.push_back({handle, asset ? asset->GetVersion() : 0});
					if (!asset)
					{
						ST_CORE_WARN("Input actions: response curve {} of action '{}' does not load; the binding reads as a straight line",
						             AssetManager::IsInitialized() ? AssetManager::GetReference(handle) : handle.ToString(),
						             map.Actions[action].Name);
					}
				}
				if (asset)
				{
					// A copy: the set never reads the asset again, so an edit reaches it only through a new set.
					specifications[action].Bindings[binding].Response = [curve = asset->GetData()](float magnitude)
					{
						return curve.Evaluate(static_cast<double>(magnitude));
					};
				}
			}
		}
		Result<Ref<InputActionSet const>> set = InputActionSet::Create(std::move(specifications));
		if (set && curves != nullptr)
		{
			std::ranges::sort(used,
			                  [](InputResponseCurveUse const& left, InputResponseCurveUse const& right)
			                  {
								  return left.Curve.GetUUID().GetValue() < right.Curve.GetUUID().GetValue();
							  });
			*curves = std::move(used);
		}
		return set;
	}

	uint64_t GetInputResponseCurveVersion(AssetHandle curve)
	{
		Ref<TypedDataAsset<Curve>> const asset = FindCurve(curve);
		return asset ? asset->GetVersion() : 0;
	}
}
