#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/VisualScripting/VisualScriptSemantics.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/VisualScriptLibrary.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	VisualScriptVariable MakeVariable(uint32_t id, std::string name, std::string type)
	{
		VisualScriptVariable variable;
		variable.ID = id;
		variable.Name = std::move(name);
		variable.Type = std::move(type);
		return variable;
	}

	VisualScriptNode MakeNode(uint32_t id, std::string type, Json settings = Json::object(), Json inputs = Json::object())
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Position = glm::vec2(static_cast<float>(id) * 16.0f, 0.0f);
		node.Settings = std::move(settings);
		node.Inputs = std::move(inputs);
		return node;
	}

	VisualScriptLink MakeLink(uint32_t from, std::string fromPin, uint32_t to, std::string toPin)
	{
		return {{from, std::move(fromPin)}, {to, std::move(toPin)}};
	}

	constexpr std::string_view RaycastType = "Call:Strada.Physics.Raycast(Strada.Vector3,Strada.Vector3,float,out Strada.RaycastHit,uint)";

	// A node of every kind whose pins come from its settings or the document, with valid settings, in the event graph and
	// in a pure and a latent function; and nodes of template pins of every sort (wildcards, defaults, a Target, a tick).
	VisualScriptDocument MakeShapesDocument()
	{
		Json const parameters = Json::array(
			{Json::object({{"Name", "Amount"}, {"Type", "float"}}), Json::object({{"Name", "Other"}, {"Type", "Strada.Entity"}})});
		VisualScriptDocument document;
		document.Variables = {MakeVariable(1, "Speed", "float"), MakeVariable(2, "Target", "Strada.Vector3"),
		                      MakeVariable(3, "Mode", "Strada.ForceMode"), MakeVariable(4, "Other", "Strada.Entity")};

		VisualScriptFunction add;
		add.ID = 10;
		add.Name = "Add";
		add.Pure = true;
		add.Inputs = {{"A", "float"}, {"B", "float"}};
		add.Outputs = {{"Sum", "float"}};
		add.Graph.Nodes = {MakeNode(11, "Function.Entry"), MakeNode(12, "Function.Return"), MakeNode(13, "Operator.Add")};
		add.Graph.Links = {MakeLink(11, "A", 13, "A"), MakeLink(11, "B", 13, "B"), MakeLink(13, "Result", 12, "Sum")};

		VisualScriptFunction wait;
		wait.ID = 20;
		wait.Name = "Wait";
		wait.Inputs = {{"Seconds", "float"}};
		wait.Locals = {{21, "Count", "int", Json()}};
		wait.Graph.Nodes = {MakeNode(22, "Function.Entry"), MakeNode(23, "Flow.Delay"), MakeNode(24, "Variable.Get", {{"Variable", 21}}),
		                    MakeNode(25, "Variable.Set", {{"Variable", 21}}), MakeNode(26, "Function.Return")};
		wait.Graph.Links = {MakeLink(22, "Then", 23, "Execute"), MakeLink(22, "Seconds", 23, "Duration"),
		                    MakeLink(23, "Completed", 25, "Execute"), MakeLink(24, "Value", 25, "Value"),
		                    MakeLink(25, "Then", 26, "Execute")};
		document.Functions = {add, wait};

		document.Graph.Nodes = {
			MakeNode(30, "Event.Create"),
			MakeNode(31, "Variable.Get", {{"Variable", 1}}),
			MakeNode(32, "Variable.Set", {{"Variable", 2}}),
			MakeNode(33, "Function.Call", {{"Function", 10}}),
			MakeNode(34, "Function.Call", {{"Function", 20}}),
			MakeNode(35, "Flow.Sequence", {{"Count", 3}}),
			MakeNode(36, "Flow.Sequence"),
			MakeNode(37, "Flow.Switch", {{"Type", "int"}, {"Cases", Json::array({1, 2, 3})}}),
			MakeNode(38, "Flow.Switch", {{"Type", "string"}, {"Cases", Json::array({"a", "b c"})}}),
			MakeNode(39, "Flow.Switch", {{"Type", "Strada.ForceMode"}}),
			MakeNode(40, "Flow.Switch", {{"Type", "Strada.ForceMode"}, {"Cases", Json::array({"Impulse", 3})}}),
			MakeNode(41, "Event.Custom", {{"Name", "Hit"}, {"Parameters", parameters}}),
			MakeNode(42, "Event.Send", {{"Name", "Hit"}, {"Parameters", parameters}}),
			MakeNode(43, "Literal", {{"Type", "float"}, {"Value", 2.5}}),
			MakeNode(44, "Literal", {{"Type", "Strada.ForceMode"}, {"Value", "Impulse"}}),
			MakeNode(45, "Convert", {{"To", "string"}}),
			MakeNode(46, "Make", {{"Type", "Strada.Vector3"}}),
			MakeNode(47, "Break", {{"Type", "Strada.RaycastHit"}}),
			MakeNode(48, "Variable.Get", {{"Variable", 3}}),
			MakeNode(49, "Variable.Get", {{"Variable", 4}}),
			MakeNode(50, "Flow.Branch"),
			MakeNode(51, std::string(RaycastType)),
			MakeNode(52, "Set:Strada.TransformComponent.Translation"),
			MakeNode(53, "Reroute"),
			MakeNode(54, "Select"),
			MakeNode(55, "Flow.Tween"),
			MakeNode(56, "Event.KeyPressed", {{"Key", "Space"}}),
			MakeNode(57, "Self"),
		};
		document.Graph.Links = {
			MakeLink(30, "Then", 35, "Execute"),  MakeLink(35, "Then0", 37, "Execute"), MakeLink(35, "Then1", 32, "Execute"),
			MakeLink(35, "Then2", 34, "Execute"), MakeLink(31, "Value", 33, "A"),       MakeLink(41, "Amount", 33, "B"),
			MakeLink(33, "Sum", 45, "Value"),     MakeLink(46, "Value", 32, "Value"),   MakeLink(51, "Hit", 47, "Value")};
		return document;
	}

	// "<direction> <name>: <type>" for each pin, and " [<label>]" for a labelled one.
	std::vector<std::string> DescribePins(VisualScriptAnalyzedNode const& node)
	{
		std::vector<std::string> pins;
		for (VisualScriptPin const& pin : node.Pins)
		{
			std::string text = fmt::format("{} {}: {}", pin.Direction == VisualScriptPinDirection::In ? "In" : "Out", pin.Name, pin.Type);
			if (!pin.Label.empty())
			{
				text += fmt::format(" [{}]", pin.Label);
			}
			pins.push_back(std::move(text));
		}
		return pins;
	}

	VisualScriptAnalysis AnalyzeWithFixture(VisualScriptDocument const& document)
	{
		Testing::FixtureVisualScriptSemantics semantics;
		Result<VisualScriptAnalysis> analysis = semantics.Analyze(document);
		REQUIRE_MESSAGE(analysis.IsOk(), analysis.GetError());
		return analysis.TakeValue();
	}

	VisualScriptAnalyzedNode const& GetNode(VisualScriptAnalysis const& analysis, uint32_t id)
	{
		VisualScriptAnalyzedNode const* node = analysis.FindNode(id);
		REQUIRE_MESSAGE(node != nullptr, "no analyzed node " << id);
		return *node;
	}

	std::vector<std::string> GetCodes(VisualScriptAnalysis const& analysis)
	{
		std::vector<std::string> codes;
		for (VisualScriptDiagnostic const& diagnostic : analysis.Diagnostics)
		{
			codes.push_back(diagnostic.Code);
		}
		return codes;
	}

	// The fixture library as the scripting runtime's library gives it: the runtime's entries of the fixture's types and node
	// kinds, with every type an entry names, types by ID and nodes in library order, as the fixture keeps them.
	Json MakeExpectedFixture(VisualScriptLibrary const& library, VisualScriptLibrary const& fixture)
	{
		std::set<std::string> typeIDs;
		std::vector<std::string> named;
		auto const name = [&](std::string const& id)
		{
			if (library.FindType(id) != nullptr && typeIDs.insert(id).second)
			{
				named.push_back(id);
			}
		};
		for (VisualScriptTypeInfo const& type : fixture.GetTypes())
		{
			name(type.ID);
		}
		Json nodes = Json::array();
		for (VisualScriptNodeTemplate const& node : library.GetNodes())
		{
			if (fixture.FindNode(node.Type) == nullptr)
			{
				continue;
			}
			nodes.push_back(node.Source);
			for (VisualScriptPin const& pin : node.Pins)
			{
				name(pin.Type);
			}
			for (VisualScriptSetting const& setting : node.Settings)
			{
				name(setting.Type);
			}
		}
		while (!named.empty())
		{
			VisualScriptTypeInfo const& type = *library.FindType(named.back());
			named.pop_back();
			for (VisualScriptStructField const& field : type.Fields)
			{
				name(field.Type);
			}
			for (std::string const& target : type.ImplicitTo)
			{
				name(target);
			}
			name(type.Element);
			name(type.Base);
		}
		Json types = Json::array();
		for (VisualScriptTypeInfo const& type : library.GetTypes())
		{
			if (typeIDs.contains(type.ID))
			{
				types.push_back(type.Source);
			}
		}
		Json expected = Json::object();
		expected["Types"] = std::move(types);
		expected["Nodes"] = std::move(nodes);
		return expected;
	}

	// What differs between the fixture and the expected fixture, entry by entry.
	std::vector<std::string> DescribeDifferences(VisualScriptLibrary const& fixture, Json const& expected)
	{
		std::vector<std::string> differences;
		auto const compare = [&differences](std::string_view what, Json const& entries, char const* key, auto const& findInFixture)
		{
			for (Json const& entry : entries)
			{
				std::string const& id = entry[key].template get_ref<std::string const&>();
				Json const* fixtureEntry = findInFixture(id);
				if (fixtureEntry == nullptr)
				{
					differences.push_back(fmt::format("the {} {} is missing from the fixture", what, id));
				}
				else if (*fixtureEntry != entry)
				{
					differences.push_back(fmt::format("the {} {} differs", what, id));
				}
			}
		};
		compare("type", expected["Types"], "ID",
		        [&fixture](std::string const& id) -> Json const*
		        {
					VisualScriptTypeInfo const* type = fixture.FindType(id);
					return type != nullptr ? &type->Source : nullptr;
				});
		compare("node", expected["Nodes"], "Type",
		        [&fixture](std::string const& type) -> Json const*
		        {
					VisualScriptNodeTemplate const* node = fixture.FindNode(type);
					return node != nullptr ? &node->Source : nullptr;
				});
		auto const contains = [](Json const& entries, char const* key, std::string const& id)
		{
			return std::any_of(entries.begin(), entries.end(),
			                   [&](Json const& entry)
			                   {
								   return entry[key] == id;
							   });
		};
		for (VisualScriptTypeInfo const& type : fixture.GetTypes())
		{
			if (!contains(expected["Types"], "ID", type.ID))
			{
				differences.push_back(fmt::format("the type {} is not in the scripting runtime's library", type.ID));
			}
		}
		for (VisualScriptNodeTemplate const& node : fixture.GetNodes())
		{
			if (!contains(expected["Nodes"], "Type", node.Type))
			{
				differences.push_back(fmt::format("the node {} is not in the scripting runtime's library", node.Type));
			}
		}
		return differences;
	}
}

TEST_CASE("VisualScriptEditor: the engine's semantics have no library and no analysis while the scripting runtime is not running")
{
	REQUIRE_FALSE(ScriptEngine::IsInitialized());
	EngineVisualScriptSemantics semantics;
	CHECK(semantics.GetLibrary() == nullptr);
	Result<VisualScriptAnalysis> const analysis = semantics.Analyze(MakeShapesDocument());
	REQUIRE(analysis.IsError());
	CHECK(analysis.GetError() == "the scripting runtime is not running");
}

TEST_CASE("VisualScriptEditor: the fixture library is the scripting runtime's own library, entry for entry")
{
	Testing::ScriptRuntimeScope runtime;
	EngineVisualScriptSemantics semantics;
	Ref<VisualScriptLibrary const> const library = semantics.GetLibrary();
	REQUIRE(library != nullptr);
	CHECK(semantics.GetLibrary() == library);
	Result<VisualScriptAnalysis> const analysis = semantics.Analyze(MakeShapesDocument());
	REQUIRE_MESSAGE(analysis.IsOk(), analysis.GetError());
	CHECK(analysis.GetValue().FindNode(50) != nullptr);

	// The fixture is the runtime's entries of its node kinds and types, as the editor's tests read them: when the scripting
	// API changes one of them, the expected fixture is written next to the test executable, to be copied over the fixture.
	Testing::FixtureVisualScriptSemantics fixture;
	Json const expected = MakeExpectedFixture(*library, fixture.GetFixtureLibrary());
	std::string const expectedText = DumpJson(expected);
	if (expectedText != Testing::GetFixtureLibraryText())
	{
		std::filesystem::path const written = FileSystem::GetExecutableDirectory() / "VisualScriptFixtures" / "Library.json";
		Result<void> const saved = FileSystem::WriteTextFile(written, expectedText);
		std::vector<std::string> const differences = DescribeDifferences(fixture.GetFixtureLibrary(), expected);
		std::string list;
		for (std::string const& difference : differences)
		{
			list += "\n  " + difference;
		}
		FAIL_CHECK(FileSystem::PathToUtf8(Testing::GetFixtureLibraryPath())
		           << " is not the scripting runtime's library:" << list << "\n"
		           << (saved ? "The expected fixture is " + FileSystem::PathToUtf8(written) + ": copy it over the fixture."
		                     : "The expected fixture could not be written: " + saved.GetError()));
	}
}

TEST_CASE("VisualScriptEditor: the fixture analysis gives the pins, titles and kinds the scripting runtime's analysis gives")
{
	Testing::ScriptRuntimeScope runtime;
	EngineVisualScriptSemantics semantics;
	VisualScriptDocument const document = MakeShapesDocument();
	Result<VisualScriptAnalysis> const real = semantics.Analyze(document);
	REQUIRE_MESSAGE(real.IsOk(), real.GetError());
	VisualScriptAnalysis const fixture = AnalyzeWithFixture(document);
	CHECK(fixture.Diagnostics.empty());

	REQUIRE(fixture.Nodes.size() == real.GetValue().Nodes.size());
	for (size_t index = 0; index < fixture.Nodes.size(); index++)
	{
		VisualScriptAnalyzedNode const& expected = real.GetValue().Nodes[index];
		VisualScriptAnalyzedNode const& node = fixture.Nodes[index];
		CAPTURE(node.ID);
		CAPTURE(node.Type);
		REQUIRE(node.ID == expected.ID);
		CHECK(node.Function == expected.Function);
		CHECK(node.Type == expected.Type);
		CHECK(node.Resolved == expected.Resolved);
		CHECK(node.Title == expected.Title);
		CHECK(node.Subtitle == expected.Subtitle);
		CHECK(node.Kind == expected.Kind);
		CHECK(node.Pure == expected.Pure);
		CHECK(node.Latent == expected.Latent);
		REQUIRE(node.Pins.size() == expected.Pins.size());
		for (size_t pinIndex = 0; pinIndex < node.Pins.size(); pinIndex++)
		{
			VisualScriptPin const& pin = node.Pins[pinIndex];
			VisualScriptPin const& expectedPin = expected.Pins[pinIndex];
			CAPTURE(pin.Name);
			CHECK(pin.Name == expectedPin.Name);
			CHECK(pin.Direction == expectedPin.Direction);
			CHECK(pin.Label == expectedPin.Label);
			CHECK(pin.DefaultsToSelf == expectedPin.DefaultsToSelf);
			CHECK(pin.Role == expectedPin.Role);
			CHECK(pin.Constraint == expectedPin.Constraint);
			CHECK(pin.Group == expectedPin.Group);
			CHECK(pin.ElementOf == expectedPin.ElementOf);
			CHECK(pin.CarriesExec == expectedPin.CarriesExec);
			// The fixture resolves no wildcard, which links resolve in the runtime's analysis.
			if (pin.Type != VisualScriptWildcardType)
			{
				CHECK(pin.Type == expectedPin.Type);
				CHECK(pin.Default == expectedPin.Default);
			}
		}
	}
}

TEST_CASE("VisualScriptEditor: the fixture analysis shapes nodes from their templates, settings and the document as the runtime does")
{
	VisualScriptAnalysis const analysis = AnalyzeWithFixture(MakeShapesDocument());
	CHECK(analysis.Diagnostics.empty());
	CHECK(analysis.Source.contains("Nodes"));

	// The functions' graphs in document order, then the event graph.
	std::vector<uint32_t> ids;
	for (VisualScriptAnalyzedNode const& node : analysis.Nodes)
	{
		ids.push_back(node.ID);
	}
	REQUIRE(ids.size() == 36);
	CHECK(std::vector<uint32_t>(ids.begin(), ids.begin() + 9) == std::vector<uint32_t>{11, 12, 13, 22, 23, 24, 25, 26, 30});
	CHECK(GetNode(analysis, 11).Function == 10);
	CHECK(GetNode(analysis, 24).Function == 20);
	CHECK(GetNode(analysis, 30).Function == 0);

	VisualScriptAnalyzedNode const& branch = GetNode(analysis, 50);
	CHECK(branch.Resolved == VisualScriptResolution::Exact);
	CHECK(branch.Kind == VisualScriptNodeKind::Flow);
	CHECK(branch.Title == "Branch");
	CHECK(DescribePins(branch) == std::vector<std::string>{"In Execute: exec", "In Condition: bool", "Out True: exec", "Out False: exec"});

	// Variables and locals type their nodes and title them.
	CHECK(GetNode(analysis, 31).Title == "Get Speed");
	CHECK(GetNode(analysis, 31).Pure);
	CHECK(DescribePins(GetNode(analysis, 31)) == std::vector<std::string>{"Out Value: float"});
	CHECK(GetNode(analysis, 32).Title == "Set Target");
	CHECK(DescribePins(GetNode(analysis, 32)) ==
	      std::vector<std::string>{"In Execute: exec", "In Value: Strada.Vector3", "Out Then: exec", "Out Value: Strada.Vector3"});
	CHECK(GetNode(analysis, 24).Title == "Get Count");
	CHECK(DescribePins(GetNode(analysis, 24)) == std::vector<std::string>{"Out Value: int"});
	CHECK(DescribePins(GetNode(analysis, 48)) == std::vector<std::string>{"Out Value: Strada.ForceMode"});
	CHECK(GetNode(analysis, 49).Title == "Get Other");
	CHECK(DescribePins(GetNode(analysis, 49)) == std::vector<std::string>{"Out Value: Strada.Entity"});

	// Function signatures: a pure function has no flow pins, and a function that waits makes its calls latent.
	CHECK(DescribePins(GetNode(analysis, 11)) == std::vector<std::string>{"Out A: float", "Out B: float"});
	CHECK(DescribePins(GetNode(analysis, 12)) == std::vector<std::string>{"In Sum: float"});
	CHECK(DescribePins(GetNode(analysis, 22)) == std::vector<std::string>{"Out Then: exec", "Out Seconds: float"});
	CHECK(DescribePins(GetNode(analysis, 26)) == std::vector<std::string>{"In Execute: exec"});
	VisualScriptAnalyzedNode const& add = GetNode(analysis, 33);
	CHECK(add.Title == "Add");
	CHECK(add.Kind == VisualScriptNodeKind::Function);
	CHECK(add.Pure);
	CHECK_FALSE(add.Latent);
	CHECK(DescribePins(add) == std::vector<std::string>{"In A: float", "In B: float", "Out Sum: float"});
	VisualScriptAnalyzedNode const& wait = GetNode(analysis, 34);
	CHECK(wait.Title == "Wait");
	CHECK_FALSE(wait.Pure);
	CHECK(wait.Latent);
	CHECK(DescribePins(wait) == std::vector<std::string>{"In Execute: exec", "In Seconds: float", "Out Then: exec"});

	// Sequences and switches.
	CHECK(DescribePins(GetNode(analysis, 35)) ==
	      std::vector<std::string>{"In Execute: exec", "Out Then0: exec", "Out Then1: exec", "Out Then2: exec"});
	CHECK(DescribePins(GetNode(analysis, 36)) == std::vector<std::string>{"In Execute: exec", "Out Then0: exec", "Out Then1: exec"});
	CHECK(DescribePins(GetNode(analysis, 37)) == std::vector<std::string>{"In Execute: exec", "In Selection: int", "Out Case:1: exec [1]",
	                                                                      "Out Case:2: exec [2]", "Out Case:3: exec [3]",
	                                                                      "Out Default: exec"});
	CHECK(DescribePins(GetNode(analysis, 38)) == std::vector<std::string>{"In Execute: exec", "In Selection: string",
	                                                                      "Out Case:a: exec [a]", "Out Case:b c: exec [b c]",
	                                                                      "Out Default: exec"});
	CHECK(DescribePins(GetNode(analysis, 39)) ==
	      std::vector<std::string>{"In Execute: exec", "In Selection: Strada.ForceMode", "Out Case:Force: exec [Force]",
	                               "Out Case:Impulse: exec [Impulse]", "Out Case:Acceleration: exec [Acceleration]",
	                               "Out Case:VelocityChange: exec [VelocityChange]", "Out Default: exec"});
	CHECK(DescribePins(GetNode(analysis, 40)) ==
	      std::vector<std::string>{"In Execute: exec", "In Selection: Strada.ForceMode", "Out Case:Impulse: exec [Impulse]",
	                               "Out Case:VelocityChange: exec [VelocityChange]", "Out Default: exec"});

	// Custom events and their senders.
	CHECK(GetNode(analysis, 41).Title == "Hit");
	CHECK(DescribePins(GetNode(analysis, 41)) ==
	      std::vector<std::string>{"Out Then: exec", "Out Amount: float", "Out Other: Strada.Entity"});
	VisualScriptAnalyzedNode const& send = GetNode(analysis, 42);
	CHECK(send.Title == "Send Hit");
	CHECK(DescribePins(send) == std::vector<std::string>{"In Execute: exec", "In Target: Strada.Entity", "In Amount: float",
	                                                     "In Other: Strada.Entity", "Out Then: exec"});
	CHECK(send.Pins[1].DefaultsToSelf);

	// Literal, Convert, Make and Break by their types.
	CHECK(GetNode(analysis, 43).Subtitle == "Float");
	CHECK(DescribePins(GetNode(analysis, 43)) == std::vector<std::string>{"Out Value: float"});
	CHECK(GetNode(analysis, 44).Subtitle == "Force Mode");
	CHECK(GetNode(analysis, 45).Title == "To String");
	CHECK(DescribePins(GetNode(analysis, 45)) == std::vector<std::string>{"In Value: *", "Out Result: string"});
	CHECK(GetNode(analysis, 46).Title == "Make Vector3");
	CHECK(DescribePins(GetNode(analysis, 46)) ==
	      std::vector<std::string>{"In X: float", "In Y: float", "In Z: float", "Out Value: Strada.Vector3"});
	CHECK(GetNode(analysis, 47).Title == "Break Raycast Hit");
	CHECK(DescribePins(GetNode(analysis, 47)) == std::vector<std::string>{"In Value: Strada.RaycastHit", "Out Entity: Strada.Entity",
	                                                                      "Out Point: Strada.Vector3", "Out Normal: Strada.Vector3",
	                                                                      "Out Distance: float"});

	// Template pins as the library has them: a reroute's wildcards carry the flow too, a tween ticks.
	VisualScriptAnalyzedNode const& reroute = GetNode(analysis, 53);
	REQUIRE(reroute.Pins.size() == 2);
	CHECK(reroute.Pins[0].CarriesExec);
	CHECK(reroute.Pins[0].Group == "T");
	VisualScriptAnalyzedNode const& tween = GetNode(analysis, 55);
	CHECK(tween.Latent);
	CHECK(std::any_of(tween.Pins.begin(), tween.Pins.end(),
	                  [](VisualScriptPin const& pin)
	                  {
						  return pin.Role == VisualScriptPinRole::Tick;
					  }));
	CHECK(GetNode(analysis, 51).Subtitle == "Physics");
	CHECK(GetNode(analysis, 51).Pins[4].Default == Json(4294967295u));
}

TEST_CASE(
	"VisualScriptEditor: the fixture analysis keeps what it can of nodes whose settings shape nothing, whose links then name their pins")
{
	VisualScriptDocument document;
	document.Variables = {MakeVariable(1, "Speed", "float")};
	VisualScriptFunction function;
	function.ID = 2;
	function.Name = "Report";
	function.Locals = {{3, "Count", "int", Json()}};
	document.Functions = {function};
	document.Graph.Nodes = {
		MakeNode(10, "Variable.Get", {{"Variable", 99}}),
		MakeNode(11, "Variable.Get", {{"Variable", 1.0}}),
		// A local of another function's graph.
		MakeNode(12, "Variable.Get", {{"Variable", 3}}),
		MakeNode(13, "Function.Call", {{"Function", 99}}),
		MakeNode(14, "Flow.Sequence", {{"Count", 100}}),
		// A type that has no literals (an array of structs).
		MakeNode(15, "Literal", {{"Type", "Strada.RaycastHit[]"}}),
		MakeNode(16, "Flow.Switch", {{"Type", "float"}}),
		MakeNode(17, "Event.Custom", {{"Name", "Hit"}, {"Parameters", Json::array({Json::object({{"Name", "Then"}, {"Type", "float"}})})}}),
		MakeNode(18, "Function.Entry"),
		MakeNode(19, "Make", {{"Type", "Strada.RaycastHit"}}),
		MakeNode(20, "FormatString", {{"Format", "{Name} scored"}}),
		MakeNode(21, "Flow.Switch", {{"Type", "int"}, {"Cases", Json::array({1, 1, "two"})}}),
		MakeNode(22, "Flow.Branch"),
	};
	document.Graph.Links = {MakeLink(13, "Result", 22, "Condition"), MakeLink(14, "Then5", 13, "Execute"),
	                        MakeLink(21, "Case:4", 22, "Execute"), MakeLink(22, "True", 20, "Name")};
	VisualScriptAnalysis const analysis = AnalyzeWithFixture(document);

	CHECK(GetNode(analysis, 10).Title == "Get Variable");
	CHECK(DescribePins(GetNode(analysis, 10)) == std::vector<std::string>{"Out Value: *"});
	// IDs are integers, as the runtime reads them.
	CHECK(GetNode(analysis, 11).Title == "Get Variable");
	CHECK(GetNode(analysis, 12).Title == "Get Variable");
	// A call of no function has the pins its links name.
	CHECK(GetNode(analysis, 13).Title == "Call Function");
	CHECK(DescribePins(GetNode(analysis, 13)) == std::vector<std::string>{"Out Result: *", "In Execute: *"});
	CHECK(GetNode(analysis, 13).Pins[0].Constraint == VisualScriptPinConstraint::None);
	CHECK(DescribePins(GetNode(analysis, 14)) ==
	      std::vector<std::string>{"In Execute: exec", "Out Then0: exec", "Out Then1: exec", "Out Then5: *"});
	CHECK(DescribePins(GetNode(analysis, 15)) == std::vector<std::string>{"Out Value: *"});
	CHECK(GetNode(analysis, 15).Subtitle.empty());
	CHECK(DescribePins(GetNode(analysis, 16)) == std::vector<std::string>{"In Execute: exec", "In Selection: *", "Out Default: exec"});
	CHECK(GetNode(analysis, 17).Title == "Hit");
	CHECK(DescribePins(GetNode(analysis, 17)) == std::vector<std::string>{"Out Then: exec"});
	CHECK(GetNode(analysis, 18).Pins.empty());
	CHECK(DescribePins(GetNode(analysis, 19)) == std::vector<std::string>{"Out Value: *"});
	// FormatString's placeholders are left to the runtime: its inputs are unknown here.
	CHECK(DescribePins(GetNode(analysis, 20)) == std::vector<std::string>{"Out Result: string"});
	CHECK(DescribePins(GetNode(analysis, 21)) ==
	      std::vector<std::string>{"In Execute: exec", "In Selection: int", "Out Case:1: exec [1]", "Out Default: exec", "Out Case:4: *"});

	REQUIRE(GetCodes(analysis) == std::vector<std::string>{"UnknownPin"});
	VisualScriptDiagnostic const& unknownPin = analysis.Diagnostics[0];
	CHECK(unknownPin.Severity == VisualScriptSeverity::Error);
	CHECK(unknownPin.Node == 20u);
	CHECK(unknownPin.Pin == "Name");
	CHECK(unknownPin.Direction == VisualScriptPinDirection::In);
	CHECK(unknownPin.Link == MakeLink(22, "True", 20, "Name"));
	CHECK(unknownPin.Message == "Format String has no input pin \"Name\"");
}

TEST_CASE(
	"VisualScriptEditor: the fixture analysis splits struct pins into their fields, nested ones too, and leaves pins that cannot split")
{
	VisualScriptDocument document;
	VisualScriptNode set = MakeNode(1, "Set:Strada.TransformComponent.Translation");
	set.Split.Inputs = {"Value"};
	VisualScriptNode raycast = MakeNode(2, std::string(RaycastType));
	raycast.Split.Inputs = {"LayerMask", "Nothing"};
	raycast.Split.Outputs = {"Hit", "Hit.Point", "Result"};
	VisualScriptNode add = MakeNode(3, "Operator.Add");
	add.Split.Inputs = {"A"};
	VisualScriptNode breakHit = MakeNode(4, "Break", {{"Type", "Strada.RaycastHit"}});
	breakHit.Split.Outputs = {"Point"};
	VisualScriptNode make = MakeNode(5, "Make", {{"Type", "Strada.Vector3"}});
	make.Split.Inputs = {"X"};
	document.Graph.Nodes = {set, raycast, add, breakHit, make};
	document.Graph.Links = {MakeLink(2, "Hit.Point.Y", 1, "Value.Y"), MakeLink(2, "Hit.Distance", 1, "Value.X"),
	                        MakeLink(4, "Point.Z", 1, "Value.Z"), MakeLink(2, "Hit", 4, "Value"), MakeLink(5, "Value", 1, "Value")};
	VisualScriptAnalysis const analysis = AnalyzeWithFixture(document);

	CHECK(DescribePins(GetNode(analysis, 1)) == std::vector<std::string>{"In Execute: exec", "In Target: Strada.Entity",
	                                                                     "In Value.X: float", "In Value.Y: float", "In Value.Z: float",
	                                                                     "Out Then: exec"});
	CHECK(DescribePins(GetNode(analysis, 2)) ==
	      std::vector<std::string>{"In Execute: exec", "In Origin: Strada.Vector3", "In Direction: Strada.Vector3", "In MaxDistance: float",
	                               "In LayerMask: uint", "Out Then: exec", "Out Hit.Entity: Strada.Entity", "Out Hit.Point.X: float",
	                               "Out Hit.Point.Y: float", "Out Hit.Point.Z: float", "Out Hit.Normal: Strada.Vector3",
	                               "Out Hit.Distance: float", "Out Result: bool"});
	// A wildcard does not split until it is resolved, nor does a field of a type that does not split.
	CHECK(DescribePins(GetNode(analysis, 3)) == std::vector<std::string>{"In A: *", "In B: *", "Out Result: *"});
	CHECK(DescribePins(GetNode(analysis, 4)) == std::vector<std::string>{"In Value: Strada.RaycastHit", "Out Entity: Strada.Entity",
	                                                                     "Out Point.X: float", "Out Point.Y: float", "Out Point.Z: float",
	                                                                     "Out Normal: Strada.Vector3", "Out Distance: float"});
	CHECK(DescribePins(GetNode(analysis, 5)) ==
	      std::vector<std::string>{"In X: float", "In Y: float", "In Z: float", "Out Value: Strada.Vector3"});

	// Links reach the field pins; a split pin itself is no pin any more, output or input.
	REQUIRE(GetCodes(analysis) == std::vector<std::string>{"UnknownPin", "UnknownPin"});
	CHECK(analysis.Diagnostics[0].Node == 2u);
	CHECK(analysis.Diagnostics[0].Pin == "Hit");
	CHECK(analysis.Diagnostics[0].Direction == VisualScriptPinDirection::Out);
	CHECK(analysis.Diagnostics[1].Node == 1u);
	CHECK(analysis.Diagnostics[1].Pin == "Value");
	CHECK(analysis.Diagnostics[1].Direction == VisualScriptPinDirection::In);
}

TEST_CASE("VisualScriptEditor: the fixture analysis infers the pins of unknown nodes and reports unknown types and pins")
{
	VisualScriptDocument document;
	VisualScriptFunction function;
	function.ID = 5;
	function.Name = "Helper";
	function.Graph.Nodes = {MakeNode(6, "Call:Strada.Gone.Away(int)")};
	document.Functions = {function};
	document.Graph.Nodes = {MakeNode(1, "Flow.Teleport", Json::object(), {{"Distance", 5}}), MakeNode(2, "Flow.Branch"),
	                        MakeNode(3, "Get:Strada.TransformComponent.Size")};
	document.Graph.Links = {MakeLink(2, "True", 1, "Execute"), MakeLink(1, "Done", 2, "Execute"), MakeLink(2, "Maybe", 1, "Execute"),
	                        MakeLink(1, "Done", 2, "True")};
	VisualScriptAnalysis const analysis = AnalyzeWithFixture(document);

	VisualScriptAnalyzedNode const& teleport = GetNode(analysis, 1);
	CHECK(teleport.Kind == VisualScriptNodeKind::Unknown);
	CHECK(teleport.Resolved == VisualScriptResolution::Unknown);
	CHECK(teleport.Title == "Teleport");
	CHECK_FALSE(teleport.Pure);
	// In the order the links, then the stored inputs, name them.
	CHECK(DescribePins(teleport) == std::vector<std::string>{"In Execute: *", "Out Done: *", "In Distance: *"});
	CHECK(teleport.Pins[0].Constraint == VisualScriptPinConstraint::None);
	CHECK(GetNode(analysis, 3).Title == "Size");
	CHECK(GetNode(analysis, 6).Title == "Away");
	CHECK(GetNode(analysis, 6).Function == 5);

	REQUIRE(GetCodes(analysis) ==
	        std::vector<std::string>{"UnknownNodeType", "UnknownNodeType", "UnknownNodeType", "UnknownPin", "UnknownPin"});
	CHECK(analysis.Diagnostics[0].Node == 6u);
	CHECK(analysis.Diagnostics[0].Function == 5u);
	CHECK(analysis.Diagnostics[0].Message == "unknown node type \"Call:Strada.Gone.Away(int)\"");
	CHECK(analysis.Diagnostics[1].Node == 1u);
	CHECK(analysis.Diagnostics[1].Function == 0u);
	VisualScriptDiagnostic const& noOutput = analysis.Diagnostics[3];
	CHECK(noOutput.Node == 2u);
	CHECK(noOutput.Pin == "Maybe");
	CHECK(noOutput.Direction == VisualScriptPinDirection::Out);
	CHECK(noOutput.Link == MakeLink(2, "Maybe", 1, "Execute"));
	CHECK(noOutput.Message == "Branch has no output pin \"Maybe\"");
	CHECK(analysis.Diagnostics[4].Message == "Branch has no input pin \"True\" (True is an output)");
}

TEST_CASE("VisualScriptEditor: the fixture semantics count their calls, stop and start, read their library again and add diagnostics")
{
	Testing::FixtureVisualScriptSemantics semantics;
	CHECK(semantics.GetLibraryCallCount() == 0);
	CHECK(semantics.GetAnalyzeCallCount() == 0);
	Ref<VisualScriptLibrary const> const library = semantics.GetLibrary();
	REQUIRE(library != nullptr);
	CHECK(&semantics.GetFixtureLibrary() == library.get());
	CHECK(semantics.GetLibrary() == library);
	CHECK(semantics.GetLibraryCallCount() == 2);
	CHECK(library->FindNode("Flow.Branch") != nullptr);

	VisualScriptDocument document;
	document.Graph.Nodes = {MakeNode(1, "Flow.Branch")};
	REQUIRE(semantics.Analyze(document).IsOk());
	CHECK(semantics.GetAnalyzeCallCount() == 1);

	// Stopped, the semantics have no library and analyze nothing; started again, the library is the same object.
	semantics.SetAvailable(false);
	CHECK_FALSE(semantics.IsAvailable());
	CHECK(semantics.GetLibrary() == nullptr);
	Result<VisualScriptAnalysis> const stopped = semantics.Analyze(document);
	REQUIRE(stopped.IsError());
	CHECK(stopped.GetError() == "the scripting runtime is not running");
	CHECK(semantics.GetLibraryCallCount() == 3);
	CHECK(semantics.GetAnalyzeCallCount() == 2);
	semantics.SetAvailable(true);
	CHECK(semantics.GetLibrary() == library);

	// Read again, the library is another object with the same entries.
	semantics.ReloadLibrary();
	Ref<VisualScriptLibrary const> const reloaded = semantics.GetLibrary();
	REQUIRE(reloaded != nullptr);
	CHECK(reloaded != library);
	CHECK(reloaded->GetNodes().size() == library->GetNodes().size());
	CHECK(reloaded->GetTypes().size() == library->GetTypes().size());

	// Diagnostics a test injects follow the analysis' own.
	VisualScriptDiagnostic warning;
	warning.Severity = VisualScriptSeverity::Warning;
	warning.Code = "NodeUpdated";
	warning.Node = 1;
	warning.Pin = "Condition";
	warning.Direction = VisualScriptPinDirection::In;
	warning.Link = MakeLink(1, "True", 1, "Execute");
	warning.Variable = 7;
	warning.Message = "node updated";
	warning.Suggestions = {"Flow.Branch"};
	semantics.InjectDiagnostic(warning);
	document.Graph.Nodes.push_back(MakeNode(2, "Flow.Unknown"));
	Result<VisualScriptAnalysis> const injected = semantics.Analyze(document);
	REQUIRE(injected.IsOk());
	REQUIRE(GetCodes(injected.GetValue()) == std::vector<std::string>{"UnknownNodeType", "NodeUpdated"});
	VisualScriptDiagnostic const& read = injected.GetValue().Diagnostics[1];
	CHECK(read.Severity == warning.Severity);
	CHECK(read.Node == warning.Node);
	CHECK(read.Pin == warning.Pin);
	CHECK(read.Direction == warning.Direction);
	CHECK(read.Link == warning.Link);
	CHECK(read.Variable == warning.Variable);
	CHECK(read.Message == warning.Message);
	CHECK(read.Suggestions == warning.Suggestions);
	semantics.ClearInjectedDiagnostics();
	Result<VisualScriptAnalysis> const cleared = semantics.Analyze(document);
	REQUIRE(cleared.IsOk());
	CHECK(GetCodes(cleared.GetValue()) == std::vector<std::string>{"UnknownNodeType"});
}

TEST_CASE("VisualScriptEditor: test projects keep their graph's document as its file, and the fixed-width measurer counts bytes")
{
	Testing::AssetManagerScope assets;
	Testing::TemporaryDirectory temporary;
	VisualScriptDocument document = MakeShapesDocument();
	document.Graph.Nodes[0].Position.reset();
	Testing::GraphProject const project = Testing::MakeGraphProject(temporary, document);
	REQUIRE(project.Project != nullptr);
	CHECK(AssetManager::GetAssetDirectory() == project.Project->GetAssetDirectory());
	CHECK(AssetManager::GetAssetType(project.Graph) == AssetType::VisualScript);
	CHECK(AssetManager::FindByPath("Scripts/Graph.sgraph") == project.Graph);
	CHECK(project.File == project.Project->GetAssetDirectory() / "Scripts" / "Graph.sgraph");
	Ref<VisualScriptAsset> const asset = AssetManager::GetAsset<VisualScriptAsset>(project.Graph);
	REQUIRE(asset != nullptr);
	// Stored as given: the unplaced node stays unplaced.
	CHECK(asset->GetDocument() == document);
	CHECK_FALSE(asset->GetDocument().Graph.Nodes[0].Position.has_value());

	auto const measure = Testing::FixedWidthMeasurer(0.5f);
	CHECK(measure("Branch", 15.0f) == 45.0f);
	CHECK(measure("", 13.0f) == 0.0f);
	// Bytes, not characters: U+00E9 is two.
	CHECK(measure("\xC3\xA9", 10.0f) == 10.0f);
}
