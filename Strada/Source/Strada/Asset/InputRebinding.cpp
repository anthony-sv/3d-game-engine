#include "stpch.h"
#include "Strada/Asset/InputRebinding.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/InputActionsAsset.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/GameData.h"
#include "Strada/Core/Log.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		struct RebindingData
		{
			// The asset the table is for, and whether it was loaded at all.
			AssetHandle Actions;
			bool Loaded = false;
			// The game data folder the profile was read from (none: no game ran).
			std::optional<std::filesystem::path> Folder;
			InputBindingProfile Profile;
			uint64_t Version = 1;
		};

		RebindingData s_Data;

		// The asset's map, or the reason none can be read.
		Result<InputActionMap> FindMap(AssetHandle actions)
		{
			if (!actions.IsValid())
			{
				return Error{"the project names no input actions"};
			}
			AssetType const type = TypedDataAsset<InputActionMap>::GetStaticType();
			if (!AssetManager::IsInitialized() || type == AssetType::None || AssetManager::GetAssetType(actions) != type)
			{
				return MakeError("{} does not load as an InputActions asset",
				                 AssetManager::IsInitialized() ? AssetManager::GetReference(actions) : actions.ToString());
			}
			Ref<TypedDataAsset<InputActionMap>> const asset = AssetManager::GetAsset<TypedDataAsset<InputActionMap>>(actions);
			if (!asset)
			{
				return MakeError("{} does not load as an InputActions asset", AssetManager::GetReference(actions));
			}
			return asset->GetData();
		}

		std::string DescribeDropped(std::vector<std::string> const& dropped)
		{
			std::string text;
			for (std::string const& entry : dropped)
			{
				text += text.empty() ? entry : "; " + entry;
			}
			return text;
		}

		void LogDropped(std::vector<std::string> const& dropped, size_t total, std::string_view source)
		{
			ST_CORE_WARN("Input bindings: {} of {} changed bindings of {} no longer match the actions and are dropped: {}", dropped.size(),
			             total, source, DescribeDropped(dropped));
		}

		// The canonical text of controls: each control's name as FormatInputControl writes it, separated by spaces.
		std::string FormatControls(InputControlList const& controls)
		{
			std::string text;
			for (InputControl const& control : controls.Get())
			{
				text += text.empty() ? FormatInputControl(control) : " " + FormatInputControl(control);
			}
			return text;
		}

		// The action and binding addressed, in a map with the table applied; the refusal's message otherwise.
		struct Address
		{
			InputActionMap Map;
			InputActionMap Effective;
			size_t Action = 0;
			size_t Binding = 0;
		};

		Result<Address> FindBinding(AssetHandle actions, std::string_view action, int32_t binding)
		{
			Result<InputActionMap> map = FindMap(actions);
			if (!map)
			{
				return Error{map.GetError()};
			}
			InputRebinding::Load(actions);
			Address address;
			address.Map = map.TakeValue();
			address.Effective = InputRebinding::Apply(actions, address.Map);
			auto const found = std::ranges::find_if(address.Effective.Actions,
			                                        [action](InputAction const& candidate)
			                                        {
														return candidate.Name == action;
													});
			if (found == address.Effective.Actions.end())
			{
				return MakeError("the input actions have no action named '{}'", action);
			}
			address.Action = static_cast<size_t>(found - address.Effective.Actions.begin());
			size_t const count = found->Bindings.size();
			if (binding < 0 || static_cast<size_t>(binding) >= count)
			{
				return MakeError("'{}' has {} binding{}, so no binding {}", action, count, count == 1 ? "" : "s", binding);
			}
			address.Binding = static_cast<size_t>(binding);
			return address;
		}

		InputRebindResult Refuse(std::string_view action, int32_t binding, int32_t part, std::string message)
		{
			InputRebindResult result;
			result.Status = InputRebindStatus::Refused;
			result.Action = std::string(action);
			result.Binding = binding;
			result.Part = part;
			result.Message = std::move(message);
			return result;
		}

		// Writes the table as the profile of its asset; nothing to write without a game data folder or for an asset that is
		// no file.
		Result<void> Save()
		{
			std::optional<std::filesystem::path> const path = InputRebinding::GetProfilePath(s_Data.Actions);
			if (!path)
			{
				return {};
			}
			return FileSystem::WriteTextFile(*path, DumpJson(WriteInputBindingProfile(s_Data.Profile)));
		}

		// Saves after a change, logging a failure; the message a result carries for it (empty when saved).
		std::string SaveChange()
		{
			Result<void> const saved = Save();
			if (saved)
			{
				return {};
			}
			ST_CORE_ERROR("Input bindings: the changed bindings cannot be saved: {}", saved.GetError());
			return fmt::format("kept for this run only: {}", saved.GetError());
		}
	}

	void InputRebinding::Load(AssetHandle actions)
	{
		std::optional<std::filesystem::path> folder = GameData::GetDirectory();
		if (s_Data.Loaded && s_Data.Actions == actions && s_Data.Folder == folder)
		{
			return;
		}
		s_Data.Actions = actions;
		s_Data.Loaded = true;
		s_Data.Folder = std::move(folder);
		s_Data.Profile = {};
		s_Data.Version++;

		std::optional<std::filesystem::path> const path = GetProfilePath(actions);
		if (!path || !FileSystem::Exists(*path))
		{
			return;
		}
		std::string const source = FileSystem::PathToUtf8(*path);
		Result<std::string> const text = FileSystem::ReadTextFile(*path);
		Result<Json> const document = text ? ParseJson(text.GetValue()) : Result<Json>(Error{text.GetError()});
		std::vector<std::string> warnings;
		Result<InputBindingProfile> profile =
			document ? ReadInputBindingProfile(document.GetValue(), &warnings) : Result<InputBindingProfile>(Error{document.GetError()});
		if (!profile)
		{
			ST_CORE_WARN("Input bindings: {} is not read ({}); the bindings are the asset's until one changes, which writes the file again",
			             source, profile.GetError());
			return;
		}
		for (std::string const& warning : warnings)
		{
			ST_CORE_WARN("Input bindings: {}: {}", source, warning);
		}
		Result<InputActionMap> const map = FindMap(actions);
		if (!map)
		{
			s_Data.Profile = profile.TakeValue();
			return;
		}
		size_t const total = profile.GetValue().Bindings.size();
		InputBindingProfileFit fit = FitInputBindingProfile(profile.GetValue(), map.GetValue());
		if (!fit.Dropped.empty())
		{
			LogDropped(fit.Dropped, total, source);
		}
		s_Data.Profile = std::move(fit.Kept);
		if (!s_Data.Profile.Bindings.empty())
		{
			ST_CORE_INFO("Input bindings: {} changed binding{} from {}", s_Data.Profile.Bindings.size(),
			             s_Data.Profile.Bindings.size() == 1 ? "" : "s", source);
		}
	}

	void InputRebinding::Clear()
	{
		s_Data.Actions = {};
		s_Data.Loaded = false;
		s_Data.Folder.reset();
		s_Data.Profile = {};
		s_Data.Version++;
	}

	uint64_t InputRebinding::GetVersion()
	{
		return s_Data.Version;
	}

	AssetHandle InputRebinding::GetActions()
	{
		return s_Data.Loaded ? s_Data.Actions : AssetHandle();
	}

	InputBindingProfile const& InputRebinding::GetProfile()
	{
		return s_Data.Profile;
	}

	InputActionMap InputRebinding::Apply(AssetHandle actions, InputActionMap const& map)
	{
		if (!s_Data.Loaded || s_Data.Actions != actions || s_Data.Profile.Bindings.empty())
		{
			return map;
		}
		size_t const total = s_Data.Profile.Bindings.size();
		InputBindingProfileFit fit = FitInputBindingProfile(s_Data.Profile, map);
		if (!fit.Dropped.empty())
		{
			LogDropped(fit.Dropped, total, AssetManager::IsInitialized() ? AssetManager::GetReference(actions) : actions.ToString());
			s_Data.Profile = std::move(fit.Kept);
		}
		return ApplyInputBindingProfile(map, s_Data.Profile);
	}

	InputRebindResult InputRebinding::Rebind(AssetHandle actions, std::string_view action, int32_t binding, std::string_view controls,
	                                         int32_t part)
	{
		Result<Address> found = FindBinding(actions, action, binding);
		if (!found)
		{
			return Refuse(action, binding, part, found.GetError());
		}
		Address const address = found.TakeValue();
		InputAction const& effective = address.Effective.Actions[address.Action];
		std::string const& current = effective.Bindings[address.Binding].Controls;

		InputControlList replacement;
		if (part < 0)
		{
			Result<InputControlList> parsed = ParseInputControls(controls);
			if (!parsed)
			{
				return Refuse(action, binding, part, parsed.GetError());
			}
			replacement = parsed.GetValue();
		}
		else
		{
			Result<InputControlList> const parts = ParseInputControls(current);
			if (!parts || parts.GetValue().Count < 2)
			{
				return Refuse(
					action, binding, part,
					fmt::format("binding {} of '{}' reads '{}', which has no parts: rebind it whole (part -1)", binding, action, current));
			}
			if (part >= parts.GetValue().Count)
			{
				return Refuse(action, binding, part,
				              fmt::format("binding {} of '{}' has {} parts, so no part {}", binding, action, parts.GetValue().Count, part));
			}
			Result<InputControlList> const single = ParseInputControls(controls);
			if (!single || single.GetValue().Count != 1 || single.GetValue().Controls[0].Kind == InputControlKind::PadStick)
			{
				return Refuse(action, binding, part,
				              single ? fmt::format("a part takes one control that is not a stick, not '{}'", controls) : single.GetError());
			}
			replacement = parts.GetValue();
			replacement.Controls[static_cast<size_t>(part)] = single.GetValue().Controls[0];
		}
		if (Result<void> const fits = CheckInputBindingControls(effective.Type, replacement); !fits)
		{
			return Refuse(action, binding, part, fits.GetError());
		}

		std::string const text = FormatControls(replacement);
		std::string const& assetControls = address.Map.Actions[address.Action].Bindings[address.Binding].Controls;
		InputBindingProfile profile = s_Data.Profile;
		std::erase_if(profile.Bindings,
		              [&effective, &address](InputBindingProfileEntry const& entry)
		              {
						  return entry.Action == effective.Name && entry.Binding == address.Binding;
					  });
		Result<InputControlList> const asset = ParseInputControls(assetControls);
		if (!asset || asset.GetValue() != replacement)
		{
			profile.Bindings.push_back(
				InputBindingProfileEntry{effective.Name, static_cast<uint32_t>(address.Binding), text, assetControls});
		}

		InputRebindResult result;
		result.Status = InputRebindStatus::Done;
		result.Action = effective.Name;
		result.Binding = binding;
		result.Part = part;
		result.Controls = text;
		if (profile != s_Data.Profile)
		{
			s_Data.Profile = std::move(profile);
			s_Data.Version++;
			result.Message = SaveChange();
			ST_CORE_INFO("Input bindings: binding {} of '{}' reads '{}' (it read '{}')", binding, effective.Name, text, current);
		}
		InputActionMap const after = ApplyInputBindingProfile(address.Map, s_Data.Profile);
		result.Conflicts = FindInputBindingConflicts(after, address.Action, address.Binding);
		return result;
	}

	InputRebindResult InputRebinding::ResetBindings(AssetHandle actions)
	{
		if (Result<InputActionMap> const map = FindMap(actions); !map)
		{
			return Refuse({}, -1, -1, map.GetError());
		}
		Load(actions);
		InputRebindResult result;
		result.Status = InputRebindStatus::Done;
		if (!s_Data.Profile.Bindings.empty())
		{
			s_Data.Profile = {};
			s_Data.Version++;
			result.Message = SaveChange();
			ST_CORE_INFO("Input bindings: every binding of {} reads the asset's controls again", AssetManager::GetReference(actions));
		}
		return result;
	}

	Result<std::string> InputRebinding::GetBindingControls(AssetHandle actions, std::string_view action, int32_t binding)
	{
		Result<Address> found = FindBinding(actions, action, binding);
		if (!found)
		{
			return Error{found.GetError()};
		}
		return found.GetValue().Effective.Actions[found.GetValue().Action].Bindings[found.GetValue().Binding].Controls;
	}

	Result<InputCapturePlan> InputRebinding::PlanCapture(AssetHandle actions, std::string_view action, int32_t binding, int32_t part)
	{
		Result<Address> found = FindBinding(actions, action, binding);
		if (!found)
		{
			return Error{found.GetError()};
		}
		InputAction const& effective = found.GetValue().Effective.Actions[found.GetValue().Action];
		InputActionBinding const& current = effective.Bindings[found.GetValue().Binding];
		InputCapturePlan plan;
		plan.Gamepad = current.Gamepad;
		plan.Controls = current.Controls;
		if (part < 0)
		{
			plan.Target = effective.Type == InputActionType::Vector2 ? InputCaptureTarget::Stick : InputCaptureTarget::Control;
			return plan;
		}
		Result<InputControlList> const parts = ParseInputControls(current.Controls);
		if (!parts || parts.GetValue().Count < 2)
		{
			return MakeError("binding {} of '{}' reads '{}', which has no parts: rebind it whole (part -1)", binding, action,
			                 current.Controls);
		}
		if (part >= parts.GetValue().Count)
		{
			return MakeError("binding {} of '{}' has {} parts, so no part {}", binding, action, parts.GetValue().Count, part);
		}
		plan.Target = InputCaptureTarget::Part;
		return plan;
	}

	std::optional<std::filesystem::path> InputRebinding::GetProfilePath(AssetHandle actions)
	{
		std::optional<std::filesystem::path> const folder = GameData::GetDirectory();
		if (!folder || !AssetManager::IsInitialized())
		{
			return std::nullopt;
		}
		std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(actions);
		if (!metadata || !metadata->IsFileAsset())
		{
			return std::nullopt;
		}
		std::filesystem::path const file = FileSystem::PathFromUtf8(metadata->Path).filename();
		return *folder / "Input" / FileSystem::PathFromUtf8(FileSystem::PathToUtf8(file) + ".json");
	}
}
