#include "Fuzzing.h"

#include "Strada/Asset/InputActionMap.h"
#include "Strada/Asset/InputActionsAsset.h"
#include "Strada/Asset/InputBindingProfile.h"
#include "Strada/Core/InputActions.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <string>
#include <utility>
#include <vector>

using namespace Strada;

// A player's binding profile (Docs/Architecture.md §7.4, "Rebinding"; §7.3): the document, which entries apply to a map, and
// the conflicts a binding has. Reading and writing it in the game data folder is SceneInputRebindingTests.cpp's.
namespace
{
	InputActionBinding MakeBinding(std::string controls, int32_t gamepad = AnyGamepad)
	{
		InputActionBinding binding;
		binding.Controls = std::move(controls);
		binding.Gamepad = gamepad;
		return binding;
	}

	InputAction MakeAction(std::string name, InputActionType type, std::vector<InputActionBinding> bindings)
	{
		InputAction action;
		action.Name = std::move(name);
		action.Type = type;
		action.Bindings = std::move(bindings);
		return action;
	}

	// Jump (Space; any pad's A), Steer (A and D; pad 0's left stick X), Move (WASD; any pad's left stick), Fire (pad 1's A).
	InputActionMap MakeMap()
	{
		InputActionMap map;
		map.Actions.push_back(MakeAction("Jump", InputActionType::Button, {MakeBinding("Key/Space"), MakeBinding("Gamepad/A")}));
		map.Actions.push_back(MakeAction("Steer", InputActionType::Axis, {MakeBinding("Key/A Key/D"), MakeBinding("Gamepad/LeftX", 0)}));
		map.Actions.push_back(
			MakeAction("Move", InputActionType::Vector2, {MakeBinding("Key/W Key/S Key/A Key/D"), MakeBinding("Gamepad/LeftStick")}));
		map.Actions.push_back(MakeAction("Fire", InputActionType::Button, {MakeBinding("Gamepad/A", 1)}));
		return map;
	}

	InputBindingProfileEntry MakeEntry(std::string action, uint32_t binding, std::string controls, std::string replaced)
	{
		return InputBindingProfileEntry{std::move(action), binding, std::move(controls), std::move(replaced)};
	}
}

TEST_CASE("InputBindingProfile: documents round trip every field, warn about unknown ones and refuse another type, a newer "
          "version and values out of range")
{
	InputBindingProfile profile;
	profile.Bindings.push_back(MakeEntry("Jump", 0, "Key/J", "Key/Space"));
	profile.Bindings.push_back(MakeEntry("Steer", 1, "Gamepad/RightX", "Gamepad/LeftX"));
	Json const document = WriteInputBindingProfile(profile);
	CHECK(document["Strada"]["Type"] == "InputBindingProfile");
	CHECK(document["Strada"]["Version"] == 1);
	CHECK(document["InputBindingProfile"]["Bindings"][1]["Replaced"] == "Gamepad/LeftX");
	Result<InputBindingProfile> const read = ReadInputBindingProfile(document);
	REQUIRE(read.IsOk());
	CHECK(read.GetValue() == profile);

	Json extended = document;
	extended["InputBindingProfile"]["Later"] = 1;
	std::vector<std::string> warnings;
	REQUIRE(ReadInputBindingProfile(extended, &warnings).IsOk());
	CHECK(warnings.size() == 1);

	Json other = document;
	other["Strada"]["Type"] = "InputActions";
	CHECK_FALSE(ReadInputBindingProfile(other).IsOk());
	Json newer = document;
	newer["Strada"]["Version"] = 2;
	CHECK_FALSE(ReadInputBindingProfile(newer).IsOk());
	Json outOfRange = document;
	outOfRange["InputBindingProfile"]["Bindings"][0]["Binding"] = MaxInputBindingsPerAction;
	CHECK_FALSE(ReadInputBindingProfile(outOfRange).IsOk());
	Json wrongKind = document;
	wrongKind["InputBindingProfile"]["Bindings"][0]["Controls"] = 3;
	CHECK_FALSE(ReadInputBindingProfile(wrongKind).IsOk());

	// An empty profile is a document too: what a reset writes.
	Result<InputBindingProfile> const empty = ReadInputBindingProfile(WriteInputBindingProfile({}));
	REQUIRE(empty.IsOk());
	CHECK(empty.GetValue().Bindings.empty());
}

TEST_CASE("InputBindingProfile: entries apply only to the bindings they were made for, and the others are dropped with why")
{
	InputActionMap const map = MakeMap();
	InputBindingProfile profile;
	profile.Bindings.push_back(MakeEntry("Jump", 0, "Key/J", "key/space"));
	profile.Bindings.push_back(MakeEntry("Jump", 0, "Key/K", "Key/Space"));
	profile.Bindings.push_back(MakeEntry("Steer", 0, "Key/Q Key/E", "Key/A Key/D"));
	profile.Bindings.push_back(MakeEntry("Climb", 0, "Key/C", "Key/Space"));
	profile.Bindings.push_back(MakeEntry("Fire", 3, "Key/F", "Gamepad/A"));
	profile.Bindings.push_back(MakeEntry("Steer", 1, "Gamepad/RightX", "Gamepad/LeftY"));
	profile.Bindings.push_back(MakeEntry("Move", 1, "Key/W", "Gamepad/LeftStick"));
	profile.Bindings.push_back(MakeEntry("Move", 0, "Key/Nothing Key/S Key/A Key/D", "Key/W Key/S Key/A Key/D"));
	profile.Bindings.push_back(MakeEntry("Fire", 0, "Gamepad/A", "Gamepad/A"));
	profile.Bindings.push_back(MakeEntry("jump", 1, "Gamepad/B", "Gamepad/A"));

	InputBindingProfileFit const fit = FitInputBindingProfile(profile, map);
	REQUIRE(fit.Kept.Bindings.size() == 2);
	CHECK(fit.Kept.Bindings[0] == profile.Bindings[0]);
	CHECK(fit.Kept.Bindings[1] == profile.Bindings[2]);
	REQUIRE(fit.Dropped.size() == 8);
	CHECK(fit.Dropped[0] == "Jump binding 0: an earlier entry names the same binding");
	CHECK(fit.Dropped[1] == "Climb binding 0: the map has no action of that name");
	CHECK(fit.Dropped[2] == "Fire binding 3: the action has 1 binding");
	CHECK(fit.Dropped[3] == "Steer binding 1: the asset's binding reads 'Gamepad/LeftX', not 'Gamepad/LeftY'");
	CHECK(fit.Dropped[4].starts_with("Move binding 1: a Vector2 action takes a stick"));
	CHECK(fit.Dropped[5].starts_with("Move binding 0: 'Key/Nothing' names no control"));
	CHECK(fit.Dropped[6] == "Fire binding 0: it reads what the asset's binding reads");
	CHECK(fit.Dropped[7] == "jump binding 1: the map has no action of that name");

	InputActionMap const applied = ApplyInputBindingProfile(map, profile);
	CHECK(applied.Actions[0].Bindings[0].Controls == "Key/J");
	CHECK(applied.Actions[0].Bindings[1].Controls == "Gamepad/A");
	CHECK(applied.Actions[1].Bindings[0].Controls == "Key/Q Key/E");
	CHECK(applied.Actions[1].Bindings[1].Controls == "Gamepad/LeftX");
	CHECK(applied.Actions[2] == map.Actions[2]);
	CHECK(applied.Actions[3] == map.Actions[3]);
	CHECK(ValidateInputActionMap(applied).IsOk());
}

TEST_CASE("InputBindingProfile: a binding conflicts with the other actions that read one of its controls on a pad both read, in "
          "the map's order")
{
	InputActionMap map = MakeMap();
	// A and D are Steer's and Move's.
	CHECK(FindInputBindingConflicts(map, 1, 0) == std::vector<std::string>{"Move"});
	// Move's keys meet Steer's A and D, and its stick meets Steer's left stick X on pad 0 (Move reads any pad).
	CHECK(FindInputBindingConflicts(map, 2, 0) == std::vector<std::string>{"Steer"});
	CHECK(FindInputBindingConflicts(map, 2, 1) == std::vector<std::string>{"Steer"});
	// Jump's A of any pad meets Fire's A of pad 1; Fire's meets Jump's.
	CHECK(FindInputBindingConflicts(map, 0, 1) == std::vector<std::string>{"Fire"});
	CHECK(FindInputBindingConflicts(map, 3, 0) == std::vector<std::string>{"Jump"});
	CHECK(FindInputBindingConflicts(map, 0, 0).empty());

	// On two distinct pads, the same button does not meet.
	map.Actions[0].Bindings[1].Gamepad = 0;
	CHECK(FindInputBindingConflicts(map, 3, 0).empty());
	// Every other action that meets is listed once, in the map's order, whichever binding of it meets.
	map.Actions[3].Bindings.push_back(MakeBinding("Key/Space"));
	map.Actions[1].Bindings.push_back(MakeBinding("Key/Space"));
	map.Actions[1].Bindings.push_back(MakeBinding("Key/Space Key/W"));
	CHECK(FindInputBindingConflicts(map, 0, 0) == std::vector<std::string>{"Steer", "Fire"});
	// A binding the map lacks has none.
	CHECK(FindInputBindingConflicts(map, 0, 5).empty());
	CHECK(FindInputBindingConflicts(map, 9, 0).empty());
}

TEST_CASE("InputBindingProfile: damaged profiles are refused or read, what is read writes and reads back alike, and it fits any "
          "map to a valid one")
{
	InputBindingProfile profile;
	profile.Bindings.push_back(MakeEntry("Jump", 0, "Key/J", "Key/Space"));
	profile.Bindings.push_back(MakeEntry("Steer", 0, "Key/Q Key/E", "Key/A Key/D"));
	profile.Bindings.push_back(MakeEntry("Move", 1, "Gamepad/RightStick", "Gamepad/LeftStick"));
	profile.Bindings.push_back(MakeEntry("Fire", 0, "Mouse/Left", "Gamepad/A"));
	Json const original = WriteInputBindingProfile(profile);
	InputActionMap const map = MakeMap();
	REQUIRE(FitInputBindingProfile(profile, map).Dropped.empty());

	Testing::DocumentMutator mutator(6100);
	int read = 0;
	int refused = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(400); round < rounds; round++)
	{
		CAPTURE(round);
		Json const damaged = mutator.Damage(original, round);
		std::vector<std::string> warnings;
		Result<InputBindingProfile> const loaded = ReadInputBindingProfile(damaged, &warnings);
		if (!loaded)
		{
			refused++;
			continue;
		}
		read++;
		Result<InputBindingProfile> const again = ReadInputBindingProfile(WriteInputBindingProfile(loaded.GetValue()));
		REQUIRE(again.IsOk());
		CHECK(again.GetValue() == loaded.GetValue());

		InputBindingProfileFit const fit = FitInputBindingProfile(loaded.GetValue(), map);
		CHECK(fit.Kept.Bindings.size() + fit.Dropped.size() == loaded.GetValue().Bindings.size());
		InputActionMap const applied = ApplyInputBindingProfile(map, loaded.GetValue());
		CHECK(ValidateInputActionMap(applied).IsOk());
		CHECK(FitInputBindingProfile(fit.Kept, map).Dropped.empty());
	}
	CHECK(read > 0);
	CHECK(refused > 0);
}
