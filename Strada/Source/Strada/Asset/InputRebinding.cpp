#include "stpch.h"
#include "Strada/Asset/InputRebinding.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/InputActionsAsset.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/GameData.h"
#include "Strada/Core/Log.h"

#include <algorithm>
#include <optional>
#include <string>
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
			// The asset's map with the table applied, as GetBindingControls last made it, and what it was made from (the
			// registry's epoch moves with every edit of the asset, the version with every change of the table): a settings
			// screen asks for bindings every frame.
			std::optional<InputActionMap> Effective;
			AssetHandle EffectiveActions;
			uint64_t EffectiveEpoch = 0;
			uint64_t EffectiveVersion = 0;
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

		// The dropped entries a log line names; the rest are counted, so that a profile of thousands of stale entries makes
		// one line of bounded length.
		constexpr size_t MaxDescribedDropped = 8;

		std::string DescribeDropped(std::vector<std::string> const& dropped)
		{
			std::string text;
			for (size_t index = 0; index < dropped.size() && index < MaxDescribedDropped; index++)
			{
				text += index == 0 ? dropped[index] : "; " + dropped[index];
			}
			if (dropped.size() > MaxDescribedDropped)
			{
				text += fmt::format("; and {} more", dropped.size() - MaxDescribedDropped);
			}
			return text;
		}

		// Why a part is no part of any binding: -1 is the whole binding, 0 to 3 one control of a composite (whether the
		// binding has that part is looked at once it is found).
		std::optional<std::string> CheckPart(int32_t part)
		{
			if (part >= -1)
			{
				return std::nullopt;
			}
			return fmt::format("part {}: -1 is the whole binding, 0 to {} a part of a composite", part, MaxInputControlsPerBinding - 1);
		}

		void LogDropped(std::vector<std::string> const& dropped, size_t total, std::string_view source)
		{
			ST_CORE_WARN("Input bindings: {} of {} changed bindings of {} no longer match the actions and are dropped: {}", dropped.size(),
			             total, source, DescribeDropped(dropped));
		}

		// The action and binding addressed, in a map with the table applied; the refusal's message otherwise.
		struct Address
		{
			InputActionMap Map;
			InputActionMap Effective;
			size_t Action = 0;
			size_t Binding = 0;
		};

		// The indices of the action and binding in a map; the refusal's message otherwise.
		Result<std::pair<size_t, size_t>> Locate(InputActionMap const& map, std::string_view action, int32_t binding)
		{
			auto const found = std::ranges::find_if(map.Actions,
			                                        [action](InputAction const& candidate)
			                                        {
														return candidate.Name == action;
													});
			if (found == map.Actions.end())
			{
				return MakeError("the input actions have no action named '{}'", action);
			}
			size_t const count = found->Bindings.size();
			if (binding < 0 || static_cast<size_t>(binding) >= count)
			{
				return MakeError("'{}' has {} binding{}, so no binding {}", action, count, count == 1 ? "" : "s", binding);
			}
			return std::pair(static_cast<size_t>(found - map.Actions.begin()), static_cast<size_t>(binding));
		}

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
			Result<std::pair<size_t, size_t>> const located = Locate(address.Effective, action, binding);
			if (!located)
			{
				return Error{located.GetError()};
			}
			address.Action = located.GetValue().first;
			address.Binding = located.GetValue().second;
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
		s_Data.Effective.reset();
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
		if (std::optional<std::string> problem = CheckPart(part))
		{
			return Refuse(action, binding, part, std::move(*problem));
		}
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

		InputRebindResult result;
		result.Status = InputRebindStatus::Done;
		result.Action = effective.Name;
		result.Binding = binding;
		result.Part = part;
		result.Controls = FormatInputControls(replacement);
		// Controls the binding reads already change nothing: no write, no version, no log (a settings screen may apply the
		// player's bindings again every frame).
		if (Result<InputControlList> const now = ParseInputControls(current); now && now.GetValue() == replacement)
		{
			result.Conflicts = FindInputBindingConflicts(address.Effective, address.Action, address.Binding);
			return result;
		}

		// The binding's entry is replaced where it stands, so the profile keeps the order bindings were first changed in.
		std::string const& assetControls = address.Map.Actions[address.Action].Bindings[address.Binding].Controls;
		Result<InputControlList> const asset = ParseInputControls(assetControls);
		auto const entry = std::ranges::find_if(s_Data.Profile.Bindings,
		                                        [&effective, &address](InputBindingProfileEntry const& candidate)
		                                        {
													return candidate.Action == effective.Name && candidate.Binding == address.Binding;
												});
		if (asset && asset.GetValue() == replacement)
		{
			if (entry != s_Data.Profile.Bindings.end())
			{
				s_Data.Profile.Bindings.erase(entry);
			}
		}
		else
		{
			InputBindingProfileEntry changed{effective.Name, static_cast<uint32_t>(address.Binding), result.Controls,
			                                 asset ? FormatInputControls(asset.GetValue()) : assetControls};
			if (entry != s_Data.Profile.Bindings.end())
			{
				*entry = std::move(changed);
			}
			else
			{
				s_Data.Profile.Bindings.push_back(std::move(changed));
			}
		}
		s_Data.Version++;
		result.Message = SaveChange();
		ST_CORE_INFO("Input bindings: binding {} of '{}' reads '{}' (it read '{}')", binding, effective.Name, result.Controls, current);
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
		if (!actions.IsValid())
		{
			return Error{"the project names no input actions"};
		}
		Load(actions);
		bool const current = s_Data.Effective && s_Data.EffectiveActions == actions &&
		                     s_Data.EffectiveEpoch == DataAssetRegistry::GetEpoch() && s_Data.EffectiveVersion == s_Data.Version;
		if (!current)
		{
			Result<InputActionMap> const map = FindMap(actions);
			if (!map)
			{
				s_Data.Effective.reset();
				return Error{map.GetError()};
			}
			s_Data.Effective = Apply(actions, map.GetValue());
			s_Data.EffectiveActions = actions;
			// Taken after the asset loaded (a load moves the epoch too).
			s_Data.EffectiveEpoch = DataAssetRegistry::GetEpoch();
			s_Data.EffectiveVersion = s_Data.Version;
		}
		Result<std::pair<size_t, size_t>> const located = Locate(*s_Data.Effective, action, binding);
		if (!located)
		{
			return Error{located.GetError()};
		}
		return s_Data.Effective->Actions[located.GetValue().first].Bindings[located.GetValue().second].Controls;
	}

	Result<InputCapturePlan> InputRebinding::PlanCapture(AssetHandle actions, std::string_view action, int32_t binding, int32_t part)
	{
		if (std::optional<std::string> problem = CheckPart(part))
		{
			return Error{std::move(*problem)};
		}
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
