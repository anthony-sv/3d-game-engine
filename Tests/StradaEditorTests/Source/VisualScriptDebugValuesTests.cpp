#include "VisualScriptTestUtilities.h"

#include "Editor/VisualScripting/VisualScriptDebugValues.h"
#include "Editor/VisualScripting/VisualScriptTheme.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace Strada;
namespace Texts = VisualScriptDebugValueTexts;

namespace
{
	VisualScriptDebugValue MakeValue(std::string type, Json value)
	{
		VisualScriptDebugValue result;
		result.Type = std::move(type);
		result.Value = std::move(value);
		return result;
	}

	// The fixture library's types, enums and names for the formatter; entity 1234 is "Enemy 3" in the running scene.
	struct ValueFixture
	{
		Testing::FixtureVisualScriptSemantics Semantics;
		VisualScriptEnumLookup Enums;
		std::function<std::string(UUID)> DescribeEntity = [](UUID entity)
		{
			return entity == UUID(1234) ? std::string("Enemy 3") : "Missing (" + entity.ToString() + "\xE2\x80\xA6)";
		};
		VisualScriptAssetNameLookup AssetNames = [](AssetHandle handle)
		{
			return "Asset " + handle.ToString();
		};

		ValueFixture()
		{
			VisualScriptLibrary const* library = &Semantics.GetFixtureLibrary();
			Enums = [library](std::string_view typeId)
			{
				return library->FindEnum(typeId);
			};
		}

		VisualScriptValueCell Format(VisualScriptDebugValue const& value) const
		{
			return FormatVisualScriptDebugValue(value, &Semantics.GetFixtureLibrary(), Enums, DescribeEntity, AssetNames);
		}
	};
}

TEST_CASE("VisualScriptDebug: values with literals show their literals' texts at three digits, the tooltip at six")
{
	ValueFixture fixture;
	VisualScriptValueCell const pi = fixture.Format(MakeValue("float", 3.14159));
	CHECK(pi.Kind == VisualScriptValueCellKind::Text);
	CHECK(pi.Text == "3.14");
	CHECK(pi.Tooltip == "3.14159");
	CHECK_FALSE(pi.Dimmed);
	CHECK(pi.Note.empty());
	CHECK(fixture.Format(MakeValue("float", 12.3456)).Text == "12.3");
	CHECK(fixture.Format(MakeValue("int", 42)).Text == "42");
	CHECK(fixture.Format(MakeValue("long", "1234567890123")).Text == "1234567890123");
	CHECK(fixture.Format(MakeValue("bool", true)).Text == "true");
	CHECK(fixture.Format(MakeValue("Strada.Vector3", Json::array({1.0, 2.5, -3.0}))).Text == "(1, 2.5, -3)");
	CHECK(fixture.Format(MakeValue("Strada.ForceMode", "Impulse")).Text == "Impulse");
	CHECK(fixture.Format(MakeValue("Strada.Prefab", "77")).Text == "Asset 77");
	// A null reference.
	CHECK(fixture.Format(MakeValue("Strada.TransformComponent", nullptr)).Text == "None");
	// Without the library, literals read as themselves.
	VisualScriptValueCell const bare = FormatVisualScriptDebugValue(MakeValue("float", 2.0), nullptr, {}, {}, {});
	CHECK(bare.Text == "2");
}

TEST_CASE("VisualScriptDebug: colours are swatches without text, their components in the tooltip")
{
	ValueFixture fixture;
	VisualScriptValueCell const red = fixture.Format(MakeValue("Strada.Color", Json::array({1.0, 0.0, 0.0, 1.0})));
	CHECK(red.Kind == VisualScriptValueCellKind::Swatch);
	REQUIRE(red.Color.has_value());
	CHECK(*red.Color == VisualScriptRgba(255, 0, 0, 255));
	CHECK(red.Text.empty());
	CHECK(red.Tooltip == "(1, 0, 0, 1)");
}

TEST_CASE("VisualScriptDebug: entities show their names and select them, missing ones say so")
{
	ValueFixture fixture;
	VisualScriptDebugValue enemy;
	enemy.Type = "Strada.Entity";
	enemy.Entity = UUID(1234);
	VisualScriptValueCell const cell = fixture.Format(enemy);
	CHECK(cell.Kind == VisualScriptValueCellKind::Entity);
	CHECK(cell.Entity == UUID(1234));
	CHECK(cell.Text == "Enemy 3");
	CHECK(cell.Tooltip == "Enemy 3 (1234)");

	VisualScriptDebugValue gone;
	gone.Type = "Strada.Entity";
	gone.Entity = UUID(5678);
	gone.EntityValid = false;
	VisualScriptValueCell const missing = fixture.Format(gone);
	CHECK(missing.Kind == VisualScriptValueCellKind::Entity);
	CHECK(missing.Text == "Missing (5678\xE2\x80\xA6)");

	VisualScriptDebugValue none;
	none.Type = "Strada.Entity";
	none.Entity = UUID::Invalid();
	VisualScriptValueCell const nothing = fixture.Format(none);
	CHECK(nothing.Kind == VisualScriptValueCellKind::Text);
	CHECK(nothing.Text == "None");

	// A component: the runtime's text, and its entity to select.
	VisualScriptDebugValue body;
	body.Type = "Strada.RigidBodyComponent";
	body.Text = "Rigid Body of Player (123)";
	body.Entity = UUID(123);
	VisualScriptValueCell const component = fixture.Format(body);
	CHECK(component.Kind == VisualScriptValueCellKind::Entity);
	CHECK(component.Entity == UUID(123));
	CHECK(component.Text == "Rigid Body of Player (123)");
}

TEST_CASE("VisualScriptDebug: arrays show their length and elements on one line, every element in the tooltip")
{
	ValueFixture fixture;
	VisualScriptDebugValue numbers = MakeValue("float[]", Json::array({{{"Value", 1}}, {{"Value", 2.5}}, {{"Value", 3}}}));
	numbers.Length = 3;
	VisualScriptValueCell const cell = fixture.Format(numbers);
	CHECK(cell.Text == "[3] 1, 2.5, 3");
	CHECK(cell.Tooltip == "[3]\n0: 1\n1: 2.5\n2: 3");

	// Cut by the runtime at 32 of 1,000 elements.
	Json elements = Json::array();
	for (int index = 0; index < 32; index++)
	{
		elements.push_back({{"Value", index}});
	}
	VisualScriptDebugValue many = MakeValue("int[]", elements);
	many.Length = 1000;
	many.Truncated = true;
	VisualScriptValueCell const cut = fixture.Format(many);
	CHECK(cut.Text.starts_with("[1000] 0, 1, 2"));
	CHECK(cut.Text.ends_with(", 31, \xE2\x80\xA6"));
	CHECK(cut.Tooltip.ends_with("\n31: 31\n\xE2\x80\xA6"
	                            "968 more"));

	// Entities by name; arrays nested deeper than the runtime sends show their length.
	VisualScriptDebugValue entities = MakeValue("Strada.Entity[]", Json::array({{{"Entity", "1234"}}, {{"Entity", "0"}}}));
	CHECK(fixture.Format(entities).Text == "[2] Enemy 3, None");
	VisualScriptDebugValue nested =
		MakeValue("int[][]", Json::array({{{"Value", Json::array({{{"Value", 7}}})}, {"Length", 1}}, {{"Length", 5}}}));
	CHECK(fixture.Format(nested).Text == "[2] [1] 7, [5]");
	VisualScriptDebugValue empty = MakeValue("float[]", Json::array());
	empty.Length = 0;
	CHECK(fixture.Format(empty).Text == "[0]");
}

TEST_CASE("VisualScriptDebug: strings are quoted on one line, newlines shown as \\n, cut ones with their length")
{
	ValueFixture fixture;
	VisualScriptValueCell const cell = fixture.Format(MakeValue("string", "Hello\nWorld"));
	CHECK(cell.Text == "\"Hello\\nWorld\"");
	CHECK(cell.Escapes == std::vector<uint32_t>{6});
	CHECK(cell.Tooltip == "Hello\nWorld");

	VisualScriptDebugValue cut = MakeValue("string", "abc");
	cut.Length = 300;
	cut.Truncated = true;
	VisualScriptValueCell const shortened = fixture.Format(cut);
	CHECK(shortened.Text == "\"abc\xE2\x80\xA6\"");
	CHECK(shortened.Tooltip == "abc\xE2\x80\xA6 (300 characters)");

	VisualScriptDebugValue lines = MakeValue("string[]", Json::array({{{"Value", "a\nb"}}}));
	CHECK(fixture.Format(lines).Text == "[1] \"a\\nb\"");
}

TEST_CASE("VisualScriptDebug: values the runtime sends as text show it, for any type")
{
	ValueFixture fixture;
	VisualScriptDebugValue nan;
	nan.Type = "float";
	nan.Text = "NaN";
	CHECK(fixture.Format(nan).Text == "NaN");
	VisualScriptDebugValue vector;
	vector.Type = "Strada.Vector3";
	vector.Text = "(Infinity, 0, 1)";
	CHECK(fixture.Format(vector).Text == "(Infinity, 0, 1)");
	VisualScriptDebugValue object;
	object.Type = "System.Object";
	object.Text = "Strada.RaycastHit";
	VisualScriptValueCell const cell = fixture.Format(object);
	CHECK(cell.Kind == VisualScriptValueCellKind::Text);
	CHECK(cell.Text == "Strada.RaycastHit");
}

TEST_CASE("VisualScriptDebug: values not computed show a dash; earlier and unknown ones are dimmed and say why")
{
	ValueFixture fixture;
	VisualScriptDebugValue value = MakeValue("int", 5);
	value.Currency = VisualScriptValueCurrency::NotComputed;
	VisualScriptValueCell const none = fixture.Format(value);
	CHECK(none.Kind == VisualScriptValueCellKind::None);
	CHECK(none.Text == Texts::NotComputed);
	CHECK(none.Tooltip == Texts::NotComputedTip);

	value.Currency = VisualScriptValueCurrency::Earlier;
	VisualScriptValueCell const earlier = fixture.Format(value);
	CHECK(earlier.Dimmed);
	CHECK(earlier.Text == "5");
	CHECK(earlier.Tooltip == "5\n" + std::string(Texts::EarlierTip));

	value.Currency = VisualScriptValueCurrency::Unknown;
	VisualScriptValueCell const unknown = fixture.Format(value);
	CHECK(unknown.Dimmed);
	CHECK(unknown.Tooltip == "5\n" + std::string(Texts::UnknownTip));

	value.Currency = VisualScriptValueCurrency::Current;
	CHECK_FALSE(fixture.Format(value).Dimmed);
}

TEST_CASE("VisualScriptDebug: values changed elsewhere and edited inputs carry their notes")
{
	ValueFixture fixture;
	VisualScriptDebugValue changed = MakeValue("float", 2.0);
	changed.Changed = true;
	CHECK(fixture.Format(changed).Note == Texts::ChangedElsewhere);

	VisualScriptDebugValue edited = MakeValue("int", 50);
	edited.Edited = true;
	edited.Before = {{"Value", 10}};
	CHECK(fixture.Format(edited).Note == "edited: 10 \xE2\x86\x92 50");

	VisualScriptDebugValue recoloured = MakeValue("Strada.Color", Json::array({0.0, 0.0, 1.0, 1.0}));
	recoloured.Edited = true;
	recoloured.Before = {{"Value", Json::array({1.0, 0.0, 0.0, 1.0})}};
	CHECK(fixture.Format(recoloured).Note == "edited: (1, 0, 0, 1) \xE2\x86\x92 (0, 0, 1, 1)");
}
