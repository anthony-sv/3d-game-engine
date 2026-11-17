#include "stpch.h"
#include "Strada/Asset/InputBindingProfile.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace Strada
{
	namespace
	{
		// The controls of a binding's text with the gamepad they read; nothing when the text does not parse.
		std::optional<InputControlList> Parse(std::string_view text)
		{
			Result<InputControlList> parsed = ParseInputControls(text);
			return parsed ? std::optional(parsed.GetValue()) : std::nullopt;
		}

		bool IsGamepadControl(InputControl control)
		{
			return control.Kind == InputControlKind::PadButton || control.Kind == InputControlKind::PadAxis ||
			       control.Kind == InputControlKind::PadStick;
		}

		// Whether a stick (its index) and an axis are the stick and one of its own axes.
		bool IsAxisOfStick(InputControl stick, InputControl axis)
		{
			if (stick.Kind != InputControlKind::PadStick || axis.Kind != InputControlKind::PadAxis)
			{
				return false;
			}
			GamepadAxis const value = static_cast<GamepadAxis>(axis.Code);
			return stick.Code == 0 ? value == GamepadAxis::LeftX || value == GamepadAxis::LeftY
			                       : value == GamepadAxis::RightX || value == GamepadAxis::RightY;
		}

		bool Meet(InputControl first, int32_t firstGamepad, InputControl second, int32_t secondGamepad)
		{
			if (IsGamepadControl(first) != IsGamepadControl(second))
			{
				return false;
			}
			if (IsGamepadControl(first) && firstGamepad != AnyGamepad && secondGamepad != AnyGamepad && firstGamepad != secondGamepad)
			{
				return false;
			}
			return first == second || IsAxisOfStick(first, second) || IsAxisOfStick(second, first);
		}

		bool BindingsMeet(InputActionBinding const& first, InputActionBinding const& second)
		{
			std::optional<InputControlList> const firstControls = Parse(first.Controls);
			std::optional<InputControlList> const secondControls = Parse(second.Controls);
			if (!firstControls || !secondControls)
			{
				return false;
			}
			for (InputControl const& one : firstControls->Get())
			{
				for (InputControl const& other : secondControls->Get())
				{
					if (Meet(one, first.Gamepad, other, second.Gamepad))
					{
						return true;
					}
				}
			}
			return false;
		}

		InputAction const* FindAction(InputActionMap const& map, std::string_view name)
		{
			auto const found = std::ranges::find_if(map.Actions,
			                                        [name](InputAction const& action)
			                                        {
														return action.Name == name;
													});
			return found != map.Actions.end() ? &*found : nullptr;
		}

		// Why an entry does not apply to the map; empty when it does.
		std::string CheckEntry(InputBindingProfileEntry const& entry, InputActionMap const& map)
		{
			InputAction const* const action = FindAction(map, entry.Action);
			if (action == nullptr)
			{
				return "the map has no action of that name";
			}
			if (entry.Binding >= action->Bindings.size())
			{
				return fmt::format("the action has {} binding{}", action->Bindings.size(), action->Bindings.size() == 1 ? "" : "s");
			}
			std::optional<InputControlList> const asset = Parse(action->Bindings[entry.Binding].Controls);
			std::optional<InputControlList> const replaced = Parse(entry.Replaced);
			if (!asset || !replaced || *asset != *replaced)
			{
				return fmt::format("the asset's binding reads '{}', not '{}'", action->Bindings[entry.Binding].Controls, entry.Replaced);
			}
			Result<InputControlList> const controls = ParseInputControls(entry.Controls);
			if (!controls)
			{
				return controls.GetError();
			}
			if (Result<void> const fits = CheckInputBindingControls(action->Type, controls.GetValue()); !fits)
			{
				return fits.GetError();
			}
			if (controls.GetValue() == *asset)
			{
				return "it reads what the asset's binding reads";
			}
			return {};
		}
	}

	Result<InputBindingProfile> ReadInputBindingProfile(Json const& document, std::vector<std::string>* warnings)
	{
		DeserializationContext context;
		context.UnknownFields = UnknownFieldPolicy::Warn;
		context.Warnings = warnings;
		return ReadStructDocument<InputBindingProfile>(document, InputBindingProfileFormat, nullptr, context);
	}

	Json WriteInputBindingProfile(InputBindingProfile const& profile)
	{
		return WriteStructDocument(profile, InputBindingProfileFormat);
	}

	InputBindingProfileFit FitInputBindingProfile(InputBindingProfile const& profile, InputActionMap const& map)
	{
		InputBindingProfileFit fit;
		for (InputBindingProfileEntry const& entry : profile.Bindings)
		{
			std::string problem = CheckEntry(entry, map);
			bool const repeated =
				problem.empty() && std::ranges::any_of(fit.Kept.Bindings,
			                                           [&entry](InputBindingProfileEntry const& kept)
			                                           {
														   return kept.Action == entry.Action && kept.Binding == entry.Binding;
													   });
			if (repeated)
			{
				problem = "an earlier entry names the same binding";
			}
			if (problem.empty())
			{
				fit.Kept.Bindings.push_back(entry);
			}
			else
			{
				fit.Dropped.push_back(fmt::format("{} binding {}: {}", entry.Action, entry.Binding, problem));
			}
		}
		return fit;
	}

	InputActionMap ApplyInputBindingProfile(InputActionMap map, InputBindingProfile const& profile)
	{
		InputBindingProfileFit const fit = FitInputBindingProfile(profile, map);
		for (InputBindingProfileEntry const& entry : fit.Kept.Bindings)
		{
			auto const action = std::ranges::find_if(map.Actions,
			                                         [&entry](InputAction const& candidate)
			                                         {
														 return candidate.Name == entry.Action;
													 });
			action->Bindings[entry.Binding].Controls = entry.Controls;
		}
		return map;
	}

	std::vector<std::string> FindInputBindingConflicts(InputActionMap const& map, size_t action, size_t binding)
	{
		std::vector<std::string> conflicts;
		if (action >= map.Actions.size() || binding >= map.Actions[action].Bindings.size())
		{
			return conflicts;
		}
		InputActionBinding const& rebound = map.Actions[action].Bindings[binding];
		for (size_t index = 0; index < map.Actions.size(); index++)
		{
			if (index == action)
			{
				continue;
			}
			bool const meets = std::ranges::any_of(map.Actions[index].Bindings,
			                                       [&rebound](InputActionBinding const& other)
			                                       {
													   return BindingsMeet(rebound, other);
												   });
			if (meets)
			{
				conflicts.push_back(map.Actions[index].Name);
			}
		}
		return conflicts;
	}
}
