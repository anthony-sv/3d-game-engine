#include "VisualScriptTestDocuments.h"

#include "Editor/VisualScripting/VisualScriptEditing.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Script/VisualScriptLibrary.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	constexpr VisualScriptPinDirection In = VisualScriptPinDirection::In;
	constexpr VisualScriptPinDirection Out = VisualScriptPinDirection::Out;

	// Where the test pins sit on their nodes: inputs on the left edge, outputs on the right, one row each.
	constexpr float OutputX = 200.0f;
	constexpr float FirstRowY = 40.0f;
	constexpr float RowHeight = 24.0f;

	Scope<VisualScriptLibrary> ParseLibrary()
	{
		Result<std::string> text =
			FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Editor" / "Library.json");
		REQUIRE_MESSAGE(text.IsOk(), text.GetError());
		Result<Scope<VisualScriptLibrary>> library = VisualScriptLibrary::Parse(text.GetValue());
		REQUIRE_MESSAGE(library.IsOk(), library.GetError());
		return library.TakeValue();
	}

	struct TestPin
	{
		std::string Name;
		VisualScriptPinDirection Direction = In;
		std::string Type;
	};

	std::optional<uint32_t> ReadID(Json const& settings, char const* key)
	{
		auto const found = settings.find(key);
		if (found == settings.end() || !found->is_number_integer())
		{
			return std::nullopt;
		}
		return static_cast<uint32_t>(found->get<int64_t>());
	}

	VisualScriptFunction const* FindTestFunction(VisualScriptDocument const& document, uint32_t id)
	{
		auto const found = std::find_if(document.Functions.begin(), document.Functions.end(),
		                                [id](VisualScriptFunction const& function)
		                                {
											return function.ID == id;
										});
		return found != document.Functions.end() ? &*found : nullptr;
	}

	// The type of the variable, or of a local of the function, a Variable node's setting names.
	std::string FindVariableType(VisualScriptDocument const& document, VisualScriptFunction const* function, Json const& settings)
	{
		std::optional<uint32_t> const id = ReadID(settings, "Variable");
		for (VisualScriptVariable const& variable : document.Variables)
		{
			if (id == variable.ID)
			{
				return variable.Type;
			}
		}
		if (function != nullptr)
		{
			for (VisualScriptLocal const& local : function->Locals)
			{
				if (id == local.ID)
				{
					return local.Type;
				}
			}
		}
		return std::string(VisualScriptWildcardType);
	}

	std::vector<VisualScriptParameter> ReadParameters(Json const& settings)
	{
		std::vector<VisualScriptParameter> parameters;
		auto const found = settings.find("Parameters");
		if (found == settings.end() || !found->is_array())
		{
			return parameters;
		}
		for (Json const& entry : *found)
		{
			parameters.push_back({entry.value("Name", std::string()), entry.value("Type", std::string())});
		}
		return parameters;
	}

	// A node's pins as a small template analyzer gives them: the library template's, except for the nodes whose pins come from
	// their settings or the document (variables, functions, sequences, switches, custom events, Make and Break).
	std::vector<TestPin> GetBasePins(VisualScriptLibrary const& library, VisualScriptDocument const& document,
	                                 VisualScriptFunction const* owner, VisualScriptNode const& node)
	{
		std::string const exec(VisualScriptExecType);
		std::vector<TestPin> pins;
		if (node.Type == "Variable.Get" || node.Type == "Variable.Set")
		{
			std::string const type = FindVariableType(document, owner, node.Settings);
			if (node.Type == "Variable.Get")
			{
				return {{"Value", Out, type}};
			}
			return {{"Execute", In, exec}, {"Value", In, type}, {"Then", Out, exec}, {"Value", Out, type}};
		}
		if (node.Type == "Function.Entry" || node.Type == "Function.Return" || node.Type == "Function.Call")
		{
			VisualScriptFunction const* const function =
				node.Type == "Function.Call" ? FindTestFunction(document, ReadID(node.Settings, "Function").value_or(0)) : owner;
			if (function == nullptr)
			{
				return pins;
			}
			bool const hasInputs = node.Type != "Function.Entry";
			bool const hasOutputs = node.Type != "Function.Return";
			if (hasInputs && !function->Pure)
			{
				pins.push_back({"Execute", In, exec});
			}
			if (hasInputs)
			{
				for (VisualScriptParameter const& parameter : node.Type == "Function.Return" ? function->Outputs : function->Inputs)
				{
					pins.push_back({parameter.Name, In, parameter.Type});
				}
			}
			if (hasOutputs && !function->Pure)
			{
				pins.push_back({"Then", Out, exec});
			}
			if (hasOutputs)
			{
				for (VisualScriptParameter const& parameter : node.Type == "Function.Entry" ? function->Inputs : function->Outputs)
				{
					pins.push_back({parameter.Name, Out, parameter.Type});
				}
			}
			return pins;
		}
		if (node.Type == "Flow.Sequence")
		{
			pins.push_back({"Execute", In, exec});
			int64_t const count = node.Settings.value("Count", int64_t(2));
			for (int64_t index = 0; index < count; index++)
			{
				pins.push_back({"Then" + std::to_string(index), Out, exec});
			}
			return pins;
		}
		if (node.Type == "Flow.Switch")
		{
			pins.push_back({"Execute", In, exec});
			pins.push_back({"Selection", In, node.Settings.value("Type", std::string(VisualScriptWildcardType))});
			for (Json const& value : node.Settings.value("Cases", Json::array()))
			{
				pins.push_back({"Case:" + (value.is_string() ? value.get<std::string>() : value.dump()), Out, exec});
			}
			pins.push_back({"Default", Out, exec});
			return pins;
		}
		if (node.Type == "Event.Custom" || node.Type == "Event.Send")
		{
			bool const sends = node.Type == "Event.Send";
			if (sends)
			{
				pins.push_back({"Execute", In, exec});
				pins.push_back({"Target", In, "Strada.Entity"});
			}
			for (VisualScriptParameter const& parameter : ReadParameters(node.Settings))
			{
				pins.push_back({parameter.Name, sends ? In : Out, parameter.Type});
			}
			pins.push_back({"Then", Out, exec});
			return pins;
		}
		if (node.Type == "Make" || node.Type == "Break")
		{
			std::string const type = node.Settings.value("Type", std::string(VisualScriptWildcardType));
			VisualScriptTypeInfo const* const info = library.FindType(type);
			bool const make = node.Type == "Make";
			if (info != nullptr)
			{
				for (VisualScriptStructField const& field : info->Fields)
				{
					pins.push_back({field.Name, make ? In : Out, field.Type});
				}
			}
			pins.push_back({"Value", make ? Out : In, type});
			return pins;
		}
		if (VisualScriptNodeTemplate const* const kind = library.FindNode(node.Type))
		{
			for (VisualScriptPin const& pin : kind->Pins)
			{
				pins.push_back({pin.Name, pin.Direction, pin.Type});
			}
		}
		return pins;
	}

	// A split pin shows its fields in its place, a field split again its own.
	void AddShownPin(VisualScriptLibrary const& library, VisualScriptNode const& node, TestPin pin, std::vector<TestPin>& shown)
	{
		std::vector<std::string> const& split = pin.Direction == In ? node.Split.Inputs : node.Split.Outputs;
		VisualScriptTypeInfo const* const type = library.FindType(pin.Type);
		if (type == nullptr || std::find(split.begin(), split.end(), pin.Name) == split.end())
		{
			shown.push_back(std::move(pin));
			return;
		}
		for (VisualScriptStructField const& field : type->Fields)
		{
			AddShownPin(library, node, {pin.Name + "." + field.Name, pin.Direction, field.Type}, shown);
		}
	}

	std::vector<VisualScriptNodePin> DerivePins(VisualScriptLibrary const& library, VisualScriptDocument const& document, uint32_t id)
	{
		VisualScriptNode const* node = nullptr;
		VisualScriptFunction const* owner = nullptr;
		for (VisualScriptFunction const& function : document.Functions)
		{
			for (VisualScriptNode const& candidate : function.Graph.Nodes)
			{
				if (candidate.ID == id)
				{
					node = &candidate;
					owner = &function;
				}
			}
		}
		for (VisualScriptNode const& candidate : document.Graph.Nodes)
		{
			if (candidate.ID == id)
			{
				node = &candidate;
			}
		}
		std::vector<VisualScriptNodePin> pins;
		if (node == nullptr)
		{
			return pins;
		}
		std::vector<TestPin> shown;
		for (TestPin& pin : GetBasePins(library, document, owner, *node))
		{
			AddShownPin(library, *node, std::move(pin), shown);
		}
		float inputs = 0.0f;
		float outputs = 0.0f;
		for (TestPin& pin : shown)
		{
			float& row = pin.Direction == In ? inputs : outputs;
			glm::vec2 const anchor(pin.Direction == In ? 0.0f : OutputX, FirstRowY + row * RowHeight);
			row += 1.0f;
			pins.push_back({std::move(pin.Name), pin.Direction, std::move(pin.Type), anchor});
		}
		return pins;
	}

	// The pins of the document's nodes as it is when asked, as a canvas would give them.
	VisualScriptNodePins MakePins(VisualScriptLibrary const& library, VisualScriptDocument const& document)
	{
		return [&library, &document](uint32_t node)
		{
			return DerivePins(library, document, node);
		};
	}

	VisualScriptNode MakeNode(uint32_t id, std::string type, std::optional<glm::vec2> position = glm::vec2(0.0f),
	                          Json settings = Json::object(), Json inputs = Json::object())
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Position = position;
		node.Settings = std::move(settings);
		node.Inputs = std::move(inputs);
		return node;
	}

	VisualScriptLink MakeLink(uint32_t from, std::string fromPin, uint32_t to, std::string toPin)
	{
		return {{from, std::move(fromPin)}, {to, std::move(toPin)}};
	}

	VisualScriptVariable MakeVariable(uint32_t id, std::string name, std::string type, Json value = Json())
	{
		VisualScriptVariable variable;
		variable.ID = id;
		variable.Name = std::move(name);
		variable.Type = std::move(type);
		variable.Value = std::move(value);
		return variable;
	}

	VisualScriptFunction MakeFunction(uint32_t id, std::string name, std::vector<VisualScriptParameter> inputs = {},
	                                  std::vector<VisualScriptParameter> outputs = {})
	{
		VisualScriptFunction function;
		function.ID = id;
		function.Name = std::move(name);
		function.Inputs = std::move(inputs);
		function.Outputs = std::move(outputs);
		return function;
	}

	template<typename T>
	T RequireOk(Result<T> result)
	{
		REQUIRE_MESSAGE(result.IsOk(), result.GetError());
		return result.TakeValue();
	}

	void RequireOk(Result<void> const& result)
	{
		REQUIRE_MESSAGE(result.IsOk(), result.GetError());
	}

	// What every edit must leave: a document that saves and loads back as it is, addressing fields only of split pins.
	void CheckDocument(VisualScriptDocument const& document)
	{
		Testing::ExpectStrictRoundTrip(document);
		Testing::ExpectSplitConsistency(document);
	}

	// The operation fails with an error that says the message, and leaves the document as it was.
	template<typename TOperation>
	void CheckRefused(VisualScriptDocument& document, TOperation const& operation, std::string_view message)
	{
		VisualScriptDocument const before = document;
		auto const result = operation(document);
		REQUIRE_FALSE(result.IsOk());
		CHECK_MESSAGE(result.GetError().find(message) != std::string::npos, result.GetError());
		CHECK(document == before);
	}

	bool HasLink(VisualScriptGraph const& graph, VisualScriptLink const& link)
	{
		return std::find(graph.Links.begin(), graph.Links.end(), link) != graph.Links.end();
	}

	VisualScriptNode const& GetNode(VisualScriptGraph const& graph, uint32_t id)
	{
		auto const found = std::find_if(graph.Nodes.begin(), graph.Nodes.end(),
		                                [id](VisualScriptNode const& node)
		                                {
											return node.ID == id;
										});
		REQUIRE(found != graph.Nodes.end());
		return *found;
	}

	bool HasNode(VisualScriptGraph const& graph, uint32_t id)
	{
		return std::any_of(graph.Nodes.begin(), graph.Nodes.end(),
		                   [id](VisualScriptNode const& node)
		                   {
							   return node.ID == id;
						   });
	}

	// A value nested levels deep: [[...[1]...]].
	Json MakeNested(size_t levels)
	{
		Json value = 1;
		for (size_t level = 1; level < levels; level++)
		{
			value = Json::array({std::move(value)});
		}
		return value;
	}
}

TEST_CASE("VisualScriptEditing: names are made valid and free, from what a name cannot hold")
{
	auto const none = [](std::string_view)
	{
		return false;
	};
	CHECK(VisualScriptEditing::MakeUniqueVisualScriptName("Force.X", none) == "ForceX");
	CHECK(VisualScriptEditing::MakeUniqueVisualScriptName("  Max Speed  ", none) == "Max Speed");
	CHECK(VisualScriptEditing::MakeUniqueVisualScriptName("\t.\x01", none) == "Value");
	CHECK(VisualScriptEditing::MakeUniqueVisualScriptName("", {}) == "Value");
	CHECK(VisualScriptEditing::MakeUniqueVisualScriptName("A\xFF"
	                                                      "B\x7F",
	                                                      none) == "AB");
	// A replacement character that is in the text stays; a byte that is not UTF-8 goes.
	CHECK(VisualScriptEditing::MakeUniqueVisualScriptName("A\xEF\xBF\xBD", none) == "A\xEF\xBF\xBD");
	CHECK(VisualScriptEditing::MakeUniqueVisualScriptName(std::string(70, 'a'), none) == std::string(64, 'a'));
	// Cut at a character boundary: 22 three-byte characters are 66 bytes, 21 of them fit.
	std::string euros;
	for (int index = 0; index < 22; index++)
	{
		euros += "\xE2\x82\xAC";
	}
	std::string const cut = VisualScriptEditing::MakeUniqueVisualScriptName(euros, none);
	CHECK(cut.size() == 63);
	CHECK(VisualScriptSerializer::IsValidName(cut));
	// A cut that ends at a space loses it.
	CHECK(VisualScriptEditing::MakeUniqueVisualScriptName(std::string(63, 'a') + " b", none) == std::string(63, 'a'));

	std::vector<std::string> const taken = {"Score", "Score1", std::string(64, 'b'), "My Var"};
	auto const isTaken = [&taken](std::string_view name)
	{
		return std::find(taken.begin(), taken.end(), name) != taken.end();
	};
	CHECK(VisualScriptEditing::MakeUniqueVisualScriptName("Score", isTaken) == "Score2");
	CHECK(VisualScriptEditing::MakeUniqueVisualScriptName("Score.", isTaken) == "Score2");
	CHECK(VisualScriptEditing::MakeUniqueVisualScriptName("My Var", isTaken) == "My Var1");
	std::string const longName = VisualScriptEditing::MakeUniqueVisualScriptName(std::string(64, 'b'), isTaken);
	CHECK(longName == std::string(63, 'b') + "1");
	for (std::string const& name : {longName, cut, std::string("Score2")})
	{
		CHECK(VisualScriptSerializer::IsValidName(name));
	}
}

TEST_CASE("VisualScriptEditing: each kind of name is free or taken in its own scope")
{
	VisualScriptDocument document;
	VisualScriptVariable speed = MakeVariable(1, "Speed", "float");
	speed.FormerNames = {"Velocity"};
	document.Variables.push_back(speed);
	VisualScriptFunction addScore = MakeFunction(2, "AddScore", {{"Points", "int"}});
	addScore.Locals.push_back({3, "Total", "int", Json()});
	addScore.Graph.Nodes.push_back(MakeNode(4, "Event.Custom", glm::vec2(0.0f), {{"Name", "Inner"}}));
	document.Functions.push_back(addScore);
	document.Functions.push_back(MakeFunction(5, "Other"));
	document.Graph.Nodes.push_back(MakeNode(6, "Event.Custom", glm::vec2(0.0f), {{"Name", "Hit"}}));
	document.Graph.Nodes.push_back(MakeNode(7, "Event.Send", glm::vec2(0.0f), {{"Name", "Sent"}}));

	// A new variable avoids every variable name, every former name and every local.
	CHECK(VisualScriptEditing::IsVariableNameTaken(document, "Speed"));
	CHECK(VisualScriptEditing::IsVariableNameTaken(document, "Velocity"));
	CHECK(VisualScriptEditing::IsVariableNameTaken(document, "Total"));
	CHECK_FALSE(VisualScriptEditing::IsVariableNameTaken(document, "Health"));
	// A new local avoids its function's locals and every variable name, not former names nor other functions' locals.
	CHECK(VisualScriptEditing::IsLocalNameTaken(document, 2, "Total"));
	CHECK(VisualScriptEditing::IsLocalNameTaken(document, 2, "Speed"));
	CHECK_FALSE(VisualScriptEditing::IsLocalNameTaken(document, 2, "Velocity"));
	CHECK_FALSE(VisualScriptEditing::IsLocalNameTaken(document, 5, "Total"));
	// A new parameter avoids its list and the reserved pin names.
	std::vector<VisualScriptParameter> const parameters = {{"Points", "int"}};
	CHECK(VisualScriptEditing::IsParameterNameTaken(parameters, "Points"));
	for (std::string_view const reserved : {"Execute", "Then", "Target", "Self", "Default"})
	{
		CHECK(VisualScriptEditing::IsReservedPinName(reserved));
		CHECK(VisualScriptEditing::IsParameterNameTaken({}, reserved));
	}
	CHECK_FALSE(VisualScriptEditing::IsReservedPinName("execute"));
	CHECK_FALSE(VisualScriptEditing::IsParameterNameTaken(parameters, "Bonus"));
	// Functions avoid function names; custom events the names of the event graph's custom events.
	CHECK(VisualScriptEditing::IsFunctionNameTaken(document, "AddScore"));
	CHECK_FALSE(VisualScriptEditing::IsFunctionNameTaken(document, "Speed"));
	CHECK(VisualScriptEditing::IsCustomEventNameTaken(document, "Hit"));
	CHECK_FALSE(VisualScriptEditing::IsCustomEventNameTaken(document, "Inner"));
	CHECK_FALSE(VisualScriptEditing::IsCustomEventNameTaken(document, "Sent"));
}

TEST_CASE("VisualScriptEditing: nodes are added with the next ID, and what a document cannot hold is refused")
{
	VisualScriptDocument document;
	document.Variables.push_back(MakeVariable(5, "Speed", "float"));
	VisualScriptFunction function = MakeFunction(9, "Apply");
	function.Graph.Nodes.push_back(MakeNode(12, "Function.Entry"));
	document.Functions.push_back(function);
	document.Graph.Nodes.push_back(MakeNode(3, "Event.Update"));
	VisualScriptComment comment;
	comment.ID = 20;
	document.Graph.Comments.push_back(comment);

	CHECK(RequireOk(VisualScriptEditing::AddNode(document, 0, "Flow.Branch", Json(), glm::vec2(10.0f, 20.0f))) == 21);
	VisualScriptNode const& branch = GetNode(document.Graph, 21);
	CHECK(branch.Type == "Flow.Branch");
	CHECK(branch.Position == glm::vec2(10.0f, 20.0f));
	CHECK(branch.Settings == Json::object());
	CHECK(RequireOk(VisualScriptEditing::AddNode(document, 9, "Literal", {{"Type", "int"}, {"Value", 3}}, glm::vec2(1.0e8f, -3.0e7f))) ==
	      22);
	VisualScriptNode const& literal = GetNode(document.Functions[0].Graph, 22);
	CHECK(literal.Settings == Json({{"Type", "int"}, {"Value", 3}}));
	CHECK(literal.Position == glm::vec2(1.0e7f, -1.0e7f));
	CheckDocument(document);

	auto const add = [](uint32_t owner, std::string type, Json settings, glm::vec2 position)
	{
		return [owner, type, settings, position](VisualScriptDocument& edited)
		{
			return VisualScriptEditing::AddNode(edited, owner, type, settings, position);
		};
	};
	CheckRefused(document, add(77, "Flow.Branch", Json::object(), glm::vec2(0.0f)), "the document has no function 77");
	CheckRefused(document, add(0, "", Json::object(), glm::vec2(0.0f)), "expected a node type");
	CheckRefused(document, add(0, "Flow.\nBranch", Json::object(), glm::vec2(0.0f)), "expected a node type");
	CheckRefused(document, add(0, "Flow.Branch", Json::array(), glm::vec2(0.0f)), "an object");
	CheckRefused(document, add(0, "Literal", {{"Bad\nName", 1}}, glm::vec2(0.0f)), "expected a setting name");
	CheckRefused(document, add(0, "Literal", {{"Value", MakeNested(17)}}, glm::vec2(0.0f)), "the setting Value must nest");
	CheckRefused(document, add(0, "Literal", {{"Value", std::numeric_limits<double>::quiet_NaN()}}, glm::vec2(0.0f)), "finite numbers");
	CheckRefused(document, add(0, "Literal", {{"Value", "\xFF"}}, glm::vec2(0.0f)), "UTF-8");
	CheckRefused(document, add(0, "Flow.Branch", Json::object(), glm::vec2(std::numeric_limits<float>::infinity(), 0.0f)), "finite");

	VisualScriptDocument full = document;
	full.Variables[0].ID = VisualScriptSerializer::MaxID;
	CheckRefused(full, add(0, "Flow.Branch", Json::object(), glm::vec2(0.0f)), "no free ID");
	VisualScriptDocument crowded;
	for (uint32_t id = 1; id <= VisualScriptSerializer::MaxNodesPerGraph; id++)
	{
		crowded.Graph.Nodes.push_back(MakeNode(id, "Reroute"));
	}
	CheckRefused(crowded, add(0, "Flow.Branch", Json::object(), glm::vec2(0.0f)), "at most 10000 nodes");
}

TEST_CASE("VisualScriptEditing: links join pins the nodes have, each exec output and data input keeping one")
{
	Scope<VisualScriptLibrary> const library = ParseLibrary();
	VisualScriptDocument document;
	VisualScriptFunction function = MakeFunction(50, "Apply");
	function.Graph.Nodes.push_back(MakeNode(51, "Function.Entry"));
	document.Functions.push_back(function);
	VisualScriptGraph& graph = document.Graph;
	graph.Nodes.push_back(MakeNode(1, "Event.Update"));
	graph.Nodes.push_back(MakeNode(2, "Flow.Branch", glm::vec2(300.0f, 0.0f)));
	graph.Nodes.push_back(MakeNode(3, "Call:Strada.Log.Info(string)", glm::vec2(600.0f, 0.0f)));
	graph.Nodes.push_back(MakeNode(4, "Call:Strada.Log.Info(string)", glm::vec2(600.0f, 200.0f)));
	graph.Nodes.push_back(MakeNode(5, "Call:Strada.Input.IsKeyDown(Strada.KeyCode)", glm::vec2(0.0f, 200.0f)));
	graph.Nodes.push_back(MakeNode(6, "Call:Strada.Input.IsKeyDown(Strada.KeyCode)", glm::vec2(0.0f, 400.0f)));
	graph.Nodes.push_back(MakeNode(7, "Set:Strada.TransformComponent.Translation", glm::vec2(900.0f, 0.0f)));
	graph.Nodes.push_back(MakeNode(8, "Get:Strada.TransformComponent.Translation", glm::vec2(600.0f, 400.0f)));
	graph.Nodes.push_back(MakeNode(9, "Flow.Branch", glm::vec2(300.0f, 400.0f)));
	graph.Links = {MakeLink(1, "Then", 2, "Execute"), MakeLink(5, "Result", 2, "Condition"), MakeLink(2, "True", 3, "Execute"),
	               MakeLink(8, "Value", 7, "Value")};
	VisualScriptNodePins const pins = MakePins(*library, document);

	SUBCASE("connecting")
	{
		// An exec input takes any number of links, a data output too.
		CHECK(RequireOk(VisualScriptEditing::Connect(document, 0, MakeLink(2, "False", 4, "Execute"), pins)) == 0);
		CHECK(RequireOk(VisualScriptEditing::Connect(document, 0, MakeLink(9, "True", 4, "Execute"), pins)) == 0);
		CHECK(RequireOk(VisualScriptEditing::Connect(document, 0, MakeLink(5, "Result", 9, "Condition"), pins)) == 0);
		CHECK(HasLink(graph, MakeLink(2, "False", 4, "Execute")));
		CHECK(HasLink(graph, MakeLink(5, "Result", 2, "Condition")));
		// A data input keeps one link, an exec output too.
		CHECK(RequireOk(VisualScriptEditing::Connect(document, 0, MakeLink(6, "Result", 2, "Condition"), pins)) == 1);
		CHECK_FALSE(HasLink(graph, MakeLink(5, "Result", 2, "Condition")));
		CHECK(HasLink(graph, MakeLink(6, "Result", 2, "Condition")));
		CHECK(RequireOk(VisualScriptEditing::Connect(document, 0, MakeLink(2, "True", 4, "Execute"), pins)) == 1);
		CHECK_FALSE(HasLink(graph, MakeLink(2, "True", 3, "Execute")));
		CHECK(graph.Links.back() == MakeLink(2, "True", 4, "Execute"));
		CheckDocument(document);

		// A link the graph has changes nothing.
		VisualScriptDocument const before = document;
		CHECK(RequireOk(VisualScriptEditing::Connect(document, 0, MakeLink(1, "Then", 2, "Execute"), pins)) == 0);
		CHECK(document == before);

		auto const connect = [&pins](uint32_t owner, VisualScriptLink link)
		{
			return [&pins, owner, link](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::Connect(edited, owner, link, pins);
			};
		};
		CheckRefused(document, connect(0, MakeLink(2, "True", 2, "Execute")), "a link cannot connect node 2 to itself");
		CheckRefused(document, connect(0, MakeLink(2, "Maybe", 3, "Execute")), "node 2 has no output Maybe");
		CheckRefused(document, connect(0, MakeLink(3, "Execute", 4, "Execute")), "node 3 has no output Execute");
		CheckRefused(document, connect(0, MakeLink(2, "True", 4, "Then")), "node 4 has no input Then");
		CheckRefused(document, connect(0, MakeLink(8, "Value.X", 4, "Message")), "node 8 has no output Value.X");
		CheckRefused(document, connect(0, MakeLink(51, "Then", 3, "Execute")), "the graph has no node 51");
		CheckRefused(document, connect(77, MakeLink(1, "Then", 3, "Execute")), "the document has no function 77");
	}

	SUBCASE("disconnecting and moving links")
	{
		RequireOk(VisualScriptEditing::Disconnect(document, 0, std::vector<VisualScriptLink>{MakeLink(1, "Then", 2, "Execute")}));
		CHECK_FALSE(HasLink(graph, MakeLink(1, "Then", 2, "Execute")));
		CheckRefused(
			document,
			[](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::Disconnect(edited, 0, std::vector<VisualScriptLink>{MakeLink(1, "Then", 2, "Execute")});
			},
			"the graph has no link from Then of node 1 to Execute of node 2");

		std::vector<VisualScriptLink> const remove = {MakeLink(5, "Result", 2, "Condition")};
		std::vector<VisualScriptLink> const add = {MakeLink(5, "Result", 9, "Condition"), MakeLink(2, "True", 4, "Execute")};
		CHECK(RequireOk(VisualScriptEditing::Relink(document, 0, remove, add, pins)) == 1);
		CHECK_FALSE(HasLink(graph, remove[0]));
		CHECK(HasLink(graph, add[0]));
		CHECK(HasLink(graph, add[1]));
		CHECK_FALSE(HasLink(graph, MakeLink(2, "True", 3, "Execute")));
		CheckDocument(document);
		// Nothing changes when one link cannot be added.
		CheckRefused(
			document,
			[&pins](VisualScriptDocument& edited)
			{
				std::vector<VisualScriptLink> const moved = {MakeLink(5, "Result", 9, "Condition")};
				std::vector<VisualScriptLink> const into = {MakeLink(5, "Result", 3, "Nope")};
				return VisualScriptEditing::Relink(edited, 0, moved, into, pins);
			},
			"node 3 has no input Nope");
	}

	SUBCASE("reroutes and splices go into links")
	{
		uint32_t const reroute =
			RequireOk(VisualScriptEditing::InsertReroute(document, 0, MakeLink(1, "Then", 2, "Execute"), glm::vec2(150.0f, 30.0f)));
		CHECK(reroute == 52);
		CHECK(GetNode(graph, reroute).Type == "Reroute");
		CHECK(GetNode(graph, reroute).Position == glm::vec2(150.0f, 30.0f));
		REQUIRE(graph.Links.size() == 5);
		CHECK(graph.Links[0] == MakeLink(1, "Then", reroute, "In"));
		CHECK(graph.Links[1] == MakeLink(reroute, "Out", 2, "Execute"));
		CHECK_FALSE(HasLink(graph, MakeLink(1, "Then", 2, "Execute")));
		CheckDocument(document);
		CheckRefused(
			document,
			[](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::InsertReroute(edited, 0, MakeLink(1, "Then", 2, "Execute"), glm::vec2(0.0f));
			},
			"the graph has no link");

		// A node without links goes into a link of exec pins, another into one of data pins.
		uint32_t const log =
			RequireOk(VisualScriptEditing::AddNode(document, 0, "Call:Strada.Log.Info(string)", Json(), glm::vec2(450.0f, 100.0f)));
		RequireOk(VisualScriptEditing::SpliceNode(document, 0, log, MakeLink(2, "True", 3, "Execute"), "Execute", "Then", pins));
		CHECK(HasLink(graph, MakeLink(2, "True", log, "Execute")));
		CHECK(HasLink(graph, MakeLink(log, "Then", 3, "Execute")));
		CHECK_FALSE(HasLink(graph, MakeLink(2, "True", 3, "Execute")));
		uint32_t const passer = RequireOk(VisualScriptEditing::AddNode(document, 0, "Reroute", Json(), glm::vec2(750.0f, 300.0f)));
		RequireOk(VisualScriptEditing::SpliceNode(document, 0, passer, MakeLink(8, "Value", 7, "Value"), "In", "Out", pins));
		CHECK(HasLink(graph, MakeLink(8, "Value", passer, "In")));
		CHECK(HasLink(graph, MakeLink(passer, "Out", 7, "Value")));
		CheckDocument(document);

		auto const splice = [&pins](uint32_t node, VisualScriptLink link, std::string input, std::string output)
		{
			return [&pins, node, link, input, output](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::SpliceNode(edited, 0, node, link, input, output, pins);
			};
		};
		CheckRefused(document, splice(2, MakeLink(5, "Result", 2, "Condition"), "Execute", "True"), "node 2 is an end of the link");
		CheckRefused(document, splice(4, MakeLink(9, "True", 3, "Execute"), "Execute", "Then"), "the graph has no link");
		CheckRefused(document, splice(4, MakeLink(log, "Then", 3, "Execute"), "Execute", "Nope"), "node 4 has no output Nope");
		CheckRefused(document, splice(99, MakeLink(log, "Then", 3, "Execute"), "Execute", "Then"), "the graph has no node 99");
	}
}

TEST_CASE("VisualScriptEditing: deleting keeps the flow through nodes with one exec input and one exec output, and through reroutes")
{
	Scope<VisualScriptLibrary> const library = ParseLibrary();
	VisualScriptDocument document;
	document.Variables.push_back(MakeVariable(100, "Speed", "float"));
	VisualScriptGraph& graph = document.Graph;
	graph.Nodes.push_back(MakeNode(1, "Event.Update"));
	graph.Nodes.push_back(MakeNode(2, "Variable.Set", glm::vec2(300.0f, 0.0f), {{"Variable", 100}}));
	graph.Nodes.push_back(MakeNode(3, "Call:Strada.Log.Info(string)", glm::vec2(600.0f, 0.0f)));
	graph.Nodes.push_back(MakeNode(4, "Flow.Delay", glm::vec2(900.0f, 0.0f)));
	graph.Nodes.push_back(MakeNode(5, "Call:Strada.Log.Info(string)", glm::vec2(1200.0f, 0.0f)));
	graph.Nodes.push_back(MakeNode(6, "Get:Strada.Time.DeltaTime", glm::vec2(0.0f, 200.0f)));
	graph.Nodes.push_back(MakeNode(7, "Event.Create", glm::vec2(0.0f, -200.0f)));
	graph.Links = {MakeLink(1, "Then", 2, "Execute"),      MakeLink(2, "Then", 3, "Execute"), MakeLink(3, "Then", 4, "Execute"),
	               MakeLink(4, "Completed", 5, "Execute"), MakeLink(6, "Value", 2, "Value"),  MakeLink(7, "Then", 2, "Execute")};
	VisualScriptNodePins const pins = MakePins(*library, document);

	SUBCASE("a Set in a chain, each link into it going on to what followed it")
	{
		std::vector<uint32_t> const removed = {2};
		VisualScriptRemoval const removal = RequireOk(VisualScriptEditing::RemoveElements(document, 0, removed, &pins, true));
		CHECK(removal.Nodes == 1);
		CHECK(removal.Comments == 0);
		CHECK(removal.Notes.empty());
		CHECK_FALSE(HasNode(graph, 2));
		CHECK(graph.Links == std::vector<VisualScriptLink>{MakeLink(1, "Then", 3, "Execute"), MakeLink(3, "Then", 4, "Execute"),
		                                                   MakeLink(4, "Completed", 5, "Execute"), MakeLink(7, "Then", 3, "Execute")});
		CheckDocument(document);
	}

	SUBCASE("consecutive nodes bridge end to end, without pins only reroutes, and without bridging nothing")
	{
		std::vector<uint32_t> const removed = {2, 3, 4};
		VisualScriptDocument unbridged = document;
		VisualScriptDocument withoutPins = document;
		CHECK(RequireOk(VisualScriptEditing::RemoveElements(document, 0, removed, &pins, true)).Nodes == 3);
		CHECK(graph.Links == std::vector<VisualScriptLink>{MakeLink(1, "Then", 5, "Execute"), MakeLink(7, "Then", 5, "Execute")});
		CheckDocument(document);

		RequireOk(VisualScriptEditing::RemoveElements(unbridged, 0, removed, &pins, false));
		CHECK(unbridged.Graph.Links.empty());
		RequireOk(VisualScriptEditing::RemoveElements(withoutPins, 0, removed, nullptr, true));
		CHECK(withoutPins.Graph.Links.empty());
	}

	SUBCASE("a Branch and a Tween are not bridged")
	{
		graph.Nodes.push_back(MakeNode(8, "Flow.Branch", glm::vec2(0.0f, 400.0f)));
		graph.Nodes.push_back(MakeNode(9, "Flow.Tween", glm::vec2(300.0f, 400.0f)));
		graph.Links = {MakeLink(1, "Then", 8, "Execute"), MakeLink(8, "True", 3, "Execute"), MakeLink(7, "Then", 9, "Execute"),
		               MakeLink(5, "Then", 9, "Stop"), MakeLink(9, "Completed", 4, "Execute")};
		std::vector<uint32_t> const removed = {8, 9};
		RequireOk(VisualScriptEditing::RemoveElements(document, 0, removed, &pins, true));
		CHECK(graph.Links.empty());
		CheckDocument(document);
	}

	SUBCASE("data and exec reroutes, alone, in runs and in cycles")
	{
		graph.Nodes.push_back(MakeNode(10, "Reroute", glm::vec2(150.0f, 200.0f)));
		graph.Nodes.push_back(MakeNode(11, "Reroute", glm::vec2(750.0f, 100.0f)));
		graph.Nodes.push_back(MakeNode(12, "Reroute", glm::vec2(750.0f, 300.0f)));
		graph.Nodes.push_back(MakeNode(13, "Reroute", glm::vec2(750.0f, 400.0f)));
		graph.Links = {MakeLink(1, "Then", 2, "Execute"),  MakeLink(6, "Value", 10, "In"), MakeLink(10, "Out", 2, "Value"),
		               MakeLink(10, "Out", 4, "Duration"), MakeLink(2, "Then", 11, "In"),  MakeLink(11, "Out", 3, "Execute"),
		               MakeLink(3, "Then", 4, "Execute"),  MakeLink(7, "Then", 12, "In"),  MakeLink(12, "Out", 13, "In"),
		               MakeLink(13, "Out", 12, "In")};
		std::vector<uint32_t> const reroutes = {10, 11, 12, 13};
		VisualScriptDocument withRun = document;
		RequireOk(VisualScriptEditing::RemoveElements(document, 0, reroutes, nullptr, true));
		CHECK(graph.Links == std::vector<VisualScriptLink>{MakeLink(1, "Then", 2, "Execute"), MakeLink(6, "Value", 2, "Value"),
		                                                   MakeLink(6, "Value", 4, "Duration"), MakeLink(2, "Then", 3, "Execute"),
		                                                   MakeLink(3, "Then", 4, "Execute")});
		CheckDocument(document);

		// A Set and the exec reroute after it bridge as one run.
		std::vector<uint32_t> const run = {2, 11};
		VisualScriptNodePins const runPins = MakePins(*library, withRun);
		RequireOk(VisualScriptEditing::RemoveElements(withRun, 0, run, &runPins, true));
		CHECK(HasLink(withRun.Graph, MakeLink(1, "Then", 3, "Execute")));
		CHECK(HasLink(withRun.Graph, MakeLink(6, "Value", 10, "In")));
		CHECK_FALSE(HasLink(withRun.Graph, MakeLink(10, "Out", 2, "Value")));
		CheckDocument(withRun);
	}

	SUBCASE("comments go without their contents, a function's entry stays, and unknown elements are refused")
	{
		VisualScriptComment box;
		box.ID = 20;
		box.Position = glm::vec2(-50.0f, -50.0f);
		box.Size = glm::vec2(800.0f, 400.0f);
		graph.Comments.push_back(box);
		VisualScriptFunction function = MakeFunction(30, "AddScore");
		function.Graph.Nodes = {MakeNode(31, "Function.Entry"), MakeNode(32, "Call:Strada.Log.Info(string)", glm::vec2(300.0f, 0.0f)),
		                        MakeNode(33, "Function.Return", glm::vec2(600.0f, 0.0f)),
		                        MakeNode(34, "Function.Entry", glm::vec2(0.0f, 300.0f))};
		function.Graph.Links = {MakeLink(31, "Then", 32, "Execute"), MakeLink(32, "Then", 33, "Execute")};
		document.Functions.push_back(function);

		std::vector<uint32_t> const comment = {20};
		VisualScriptRemoval const removal = RequireOk(VisualScriptEditing::RemoveElements(document, 0, comment, &pins, true));
		CHECK(removal.Comments == 1);
		CHECK(removal.Nodes == 0);
		CHECK(graph.Comments.empty());
		CHECK(graph.Nodes.size() == 7);

		// The first entry stays with a note, said once; a second entry and the return go.
		std::vector<uint32_t> const inFunction = {31, 32, 33, 34, 31};
		VisualScriptRemoval const functionRemoval = RequireOk(VisualScriptEditing::RemoveElements(document, 30, inFunction, &pins, true));
		CHECK(functionRemoval.Nodes == 3);
		CHECK(functionRemoval.Notes ==
		      std::vector<std::string>{"The entry of AddScore stays: to remove the function, delete it in My Script"});
		REQUIRE(document.Functions[0].Graph.Nodes.size() == 1);
		CHECK(document.Functions[0].Graph.Nodes[0].ID == 31);
		CHECK(document.Functions[0].Graph.Links.empty());
		CheckDocument(document);

		CheckRefused(
			document,
			[&pins](VisualScriptDocument& edited)
			{
				std::vector<uint32_t> const elements = {1, 31};
				return VisualScriptEditing::RemoveElements(edited, 0, elements, &pins, true);
			},
			"the graph has no node or comment 31");
	}
}

TEST_CASE("VisualScriptEditing: moving and placing keep nodes and comments within the format's coordinates")
{
	VisualScriptDocument document;
	document.Graph.Nodes = {MakeNode(1, "Event.Update", glm::vec2(10.0f, 20.0f)), MakeNode(2, "Flow.Branch", std::nullopt)};
	VisualScriptComment comment;
	comment.ID = 3;
	comment.Position = glm::vec2(9.9e6f, 0.0f);
	document.Graph.Comments.push_back(comment);

	std::vector<uint32_t> const moved = {1, 3, 1};
	RequireOk(VisualScriptEditing::MoveElements(document, 0, moved, glm::vec2(200000.0f, -16.0f)));
	CHECK(document.Graph.Nodes[0].Position == glm::vec2(200010.0f, 4.0f));
	CHECK(document.Graph.Comments[0].Position == glm::vec2(1.0e7f, -16.0f));
	std::vector<std::pair<uint32_t, glm::vec2>> const positions = {{2, glm::vec2(64.0f, 32.0f)}, {3, glm::vec2(-2.0e7f, 5.0f)}};
	RequireOk(VisualScriptEditing::SetPositions(document, 0, positions));
	CHECK(document.Graph.Nodes[1].Position == glm::vec2(64.0f, 32.0f));
	CHECK(document.Graph.Comments[0].Position == glm::vec2(-1.0e7f, 5.0f));
	CheckDocument(document);

	document.Graph.Nodes[1].Position.reset();
	CheckRefused(
		document,
		[](VisualScriptDocument& edited)
		{
			std::vector<uint32_t> const elements = {1, 2};
			return VisualScriptEditing::MoveElements(edited, 0, elements, glm::vec2(1.0f));
		},
		"node 2 has no position yet");
	CheckRefused(
		document,
		[](VisualScriptDocument& edited)
		{
			std::vector<uint32_t> const elements = {1};
			return VisualScriptEditing::MoveElements(edited, 0, elements, glm::vec2(std::numeric_limits<float>::quiet_NaN(), 0.0f));
		},
		"an offset must be finite");
	CheckRefused(
		document,
		[](VisualScriptDocument& edited)
		{
			std::vector<std::pair<uint32_t, glm::vec2>> const placed = {{1, glm::vec2(1.0f)}, {9, glm::vec2(1.0f)}};
			return VisualScriptEditing::SetPositions(edited, 0, placed);
		},
		"the graph has no node or comment 9");
}

TEST_CASE("VisualScriptEditing: inputs and settings are stored and removed, settings that change pins only by their own operations")
{
	VisualScriptDocument document;
	document.Graph.Nodes = {MakeNode(1, "Set:Strada.TransformComponent.Translation"),
	                        MakeNode(2, "Literal", glm::vec2(0.0f), {{"Type", "int"}}),
	                        MakeNode(3, "Flow.Sequence"),
	                        MakeNode(4, "Flow.Switch", glm::vec2(0.0f), {{"Type", "int"}}),
	                        MakeNode(5, "Event.Custom", glm::vec2(0.0f), {{"Name", "Hit"}}),
	                        MakeNode(6, "Event.Send", glm::vec2(0.0f), {{"Name", "Hit"}})};

	RequireOk(VisualScriptEditing::SetInput(document, 1, "Value", Json::array({1, 2.5, -3})));
	CHECK(document.Graph.Nodes[0].Inputs == Json({{"Value", {1, 2.5, -3}}}));
	RequireOk(VisualScriptEditing::SetInput(document, 1, "Value", Json()));
	CHECK(document.Graph.Nodes[0].Inputs == Json::object());
	RequireOk(VisualScriptEditing::SetInput(document, 1, "Missing", Json()));
	RequireOk(VisualScriptEditing::SetSetting(document, 2, "Value", 3));
	RequireOk(VisualScriptEditing::SetSetting(document, 6, "Name", "Damaged"));
	CHECK(document.Graph.Nodes[1].Settings == Json({{"Type", "int"}, {"Value", 3}}));
	RequireOk(VisualScriptEditing::SetSetting(document, 2, "Value", Json()));
	CHECK(document.Graph.Nodes[1].Settings == Json({{"Type", "int"}}));
	CheckDocument(document);

	auto const setInput = [](uint32_t node, std::string pin, Json literal)
	{
		return [node, pin, literal](VisualScriptDocument& edited)
		{
			return VisualScriptEditing::SetInput(edited, node, pin, literal);
		};
	};
	CheckRefused(document, setInput(1, "Value.X", 1.0), "Value.X is a field of an input node 1 does not split");
	CheckRefused(document, setInput(1, "", 1.0), "expected a pin name");
	CheckRefused(document, setInput(1, "Value", MakeNested(17)), "a literal must nest");
	CheckRefused(document, setInput(9, "Value", 1.0), "the document has no node 9");
	document.Graph.Nodes[0].Split.Inputs = {"Value"};
	RequireOk(VisualScriptEditing::SetInput(document, 1, "Value.X", 2.0));
	CheckRefused(document, setInput(1, "Value", Json::array({1, 2, 3})), "node 1 splits its input Value");
	for (int index = static_cast<int>(document.Graph.Nodes[0].Inputs.size()); index < 256; index++)
	{
		document.Graph.Nodes[0].Inputs["Pin" + std::to_string(index)] = index;
	}
	CheckRefused(document, setInput(1, "Another", 1), "at most 256 literals");
	RequireOk(VisualScriptEditing::SetInput(document, 1, "Value.X", 3.0));

	auto const setSetting = [](uint32_t node, std::string name, Json value)
	{
		return [node, name, value](VisualScriptDocument& edited)
		{
			return VisualScriptEditing::SetSetting(edited, node, name, value);
		};
	};
	CheckRefused(document, setSetting(3, "Count", 3), "Count of node 3 (Flow.Sequence) changes its pins");
	CheckRefused(document, setSetting(4, "Cases", Json::array({1})), "Cases of node 4 (Flow.Switch)");
	CheckRefused(document, setSetting(5, "Parameters", Json::array()), "Parameters of node 5 (Event.Custom)");
	CheckRefused(document, setSetting(6, "Parameters", Json::array()), "Parameters of node 6 (Event.Send)");
	CheckRefused(document, setSetting(5, "Name", "Damaged"), "Name of node 5 (Event.Custom)");
	CheckRefused(document, setSetting(2, "Value", std::numeric_limits<double>::infinity()), "the setting Value must nest");
}

TEST_CASE("VisualScriptEditing: a Sequence's outputs are counted, inserted and removed with their links")
{
	Scope<VisualScriptLibrary> const library = ParseLibrary();
	VisualScriptDocument document;
	VisualScriptGraph& graph = document.Graph;
	graph.Nodes = {MakeNode(1, "Flow.Sequence", glm::vec2(0.0f), {{"Count", 3}}), MakeNode(2, "Call:Strada.Log.Info(string)"),
	               MakeNode(3, "Call:Strada.Log.Info(string)"), MakeNode(4, "Call:Strada.Log.Info(string)"),
	               MakeNode(5, "Call:Strada.Log.Info(string)")};
	// Then5 is no output of a Sequence of 3: its link is an orphan, which stays as it is.
	graph.Links = {MakeLink(1, "Then0", 2, "Execute"), MakeLink(1, "Then1", 3, "Execute"), MakeLink(1, "Then2", 4, "Execute"),
	               MakeLink(1, "Then5", 5, "Execute")};

	SUBCASE("lowering the count removes the links of the outputs that go")
	{
		CHECK(RequireOk(VisualScriptEditing::SetSequenceOutputs(document, 1, 2, std::nullopt, std::nullopt)) == 1);
		CHECK(graph.Nodes[0].Settings == Json({{"Count", 2}}));
		CHECK(graph.Links == std::vector<VisualScriptLink>{MakeLink(1, "Then0", 2, "Execute"), MakeLink(1, "Then1", 3, "Execute"),
		                                                   MakeLink(1, "Then5", 5, "Execute")});
		CHECK(RequireOk(VisualScriptEditing::SetSequenceOutputs(document, 1, 4, std::nullopt, std::nullopt)) == 0);
		CheckDocument(document);
	}

	SUBCASE("inserting outputs moves the later links up")
	{
		CHECK(RequireOk(VisualScriptEditing::SetSequenceOutputs(document, 1, 5, 1, std::nullopt)) == 0);
		CHECK(graph.Nodes[0].Settings == Json({{"Count", 5}}));
		CHECK(graph.Links == std::vector<VisualScriptLink>{MakeLink(1, "Then0", 2, "Execute"), MakeLink(1, "Then3", 3, "Execute"),
		                                                   MakeLink(1, "Then4", 4, "Execute"), MakeLink(1, "Then5", 5, "Execute")});
		CheckDocument(document);
	}

	SUBCASE("removing outputs removes their links and moves the later ones down")
	{
		CHECK(RequireOk(VisualScriptEditing::SetSequenceOutputs(document, 1, 2, std::nullopt, 0)) == 1);
		CHECK(graph.Links == std::vector<VisualScriptLink>{MakeLink(1, "Then0", 3, "Execute"), MakeLink(1, "Then1", 4, "Execute"),
		                                                   MakeLink(1, "Then5", 5, "Execute")});
		CheckDocument(document);
	}

	SUBCASE("a sequence without Count has two outputs, and one with a Count that is no count takes a new count only")
	{
		graph.Nodes[0].Settings = Json::object();
		CHECK(RequireOk(VisualScriptEditing::SetSequenceOutputs(document, 1, 3, 2, std::nullopt)) == 0);
		CHECK(HasLink(graph, MakeLink(1, "Then2", 4, "Execute")));
		graph.Nodes[0].Settings = {{"Count", "three"}};
		CheckRefused(
			document,
			[](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::SetSequenceOutputs(edited, 1, 4, 0, std::nullopt);
			},
			"node 1 has no Count of 2 to 64");
		CHECK(RequireOk(VisualScriptEditing::SetSequenceOutputs(document, 1, 2, std::nullopt, std::nullopt)) == 2);
		CHECK(graph.Links == std::vector<VisualScriptLink>{MakeLink(1, "Then0", 2, "Execute"), MakeLink(1, "Then1", 3, "Execute")});
	}

	SUBCASE("what a Sequence cannot have is refused")
	{
		auto const setOutputs = [](uint32_t node, int count, std::optional<int> insertAt, std::optional<int> removeAt)
		{
			return [node, count, insertAt, removeAt](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::SetSequenceOutputs(edited, node, count, insertAt, removeAt);
			};
		};
		CheckRefused(document, setOutputs(1, 1, std::nullopt, std::nullopt), "a Sequence has 2 to 64 outputs");
		CheckRefused(document, setOutputs(1, 65, std::nullopt, std::nullopt), "a Sequence has 2 to 64 outputs");
		CheckRefused(document, setOutputs(1, 4, 1, 1), "not both at once");
		CheckRefused(document, setOutputs(1, 3, 1, std::nullopt), "inserting outputs at 1 takes a count above 3");
		CheckRefused(document, setOutputs(1, 4, 4, std::nullopt), "an index from 0 to 3");
		CheckRefused(document, setOutputs(1, 4, std::nullopt, 0), "removing outputs at 0 takes a count below 3");
		CheckRefused(document, setOutputs(1, 2, std::nullopt, 3), "an index from 0 to 2");
		CheckRefused(document, setOutputs(2, 3, std::nullopt, std::nullopt),
		             "node 2 is no Flow.Sequence: its type is Call:Strada.Log.Info(string)");
	}
}

TEST_CASE("VisualScriptEditing: a Switch's cases keep, rename and remove their links")
{
	VisualScriptDocument document;
	VisualScriptGraph& graph = document.Graph;
	graph.Nodes = {MakeNode(1, "Flow.Switch", glm::vec2(0.0f), {{"Type", "int"}, {"Cases", {1, 2, 3}}}),
	               MakeNode(2, "Flow.Switch", glm::vec2(0.0f), {{"Type", "string"}, {"Cases", {"open", "a.b"}}}),
	               MakeNode(3, "Flow.Switch", glm::vec2(0.0f), {{"Type", "Strada.KeyCode"}}),
	               MakeNode(4, "Call:Strada.Log.Info(string)"),
	               MakeNode(5, "Call:Strada.Log.Info(string)"),
	               MakeNode(6, "Call:Strada.Log.Info(string)"),
	               MakeNode(7, "Call:Strada.Log.Info(string)")};
	graph.Links = {MakeLink(1, "Case:1", 4, "Execute"),     MakeLink(1, "Case:2", 5, "Execute"),    MakeLink(1, "Case:3", 6, "Execute"),
	               MakeLink(1, "Default", 7, "Execute"),    MakeLink(2, "Case:open", 4, "Execute"), MakeLink(2, "Case:a.b", 5, "Execute"),
	               MakeLink(3, "Case:Space", 6, "Execute"), MakeLink(3, "Case:W", 7, "Execute")};

	SUBCASE("a list of the same length renames the cases that changed, in order")
	{
		CHECK(RequireOk(VisualScriptEditing::SetSwitchCases(document, 1, Json::array({1, 5, 3}))) == 0);
		CHECK(HasLink(graph, MakeLink(1, "Case:5", 5, "Execute")));
		CHECK_FALSE(HasLink(graph, MakeLink(1, "Case:2", 5, "Execute")));
		CHECK(graph.Nodes[0].Settings["Cases"] == Json::array({1, 5, 3}));
		std::vector<VisualScriptLink> const links = graph.Links;
		CHECK(RequireOk(VisualScriptEditing::SetSwitchCases(document, 1, Json::array({3, 1, 5}))) == 0);
		CHECK(graph.Links == links);
		CheckDocument(document);
	}

	SUBCASE("cases that go take their links, without Cases a Switch on int has none, and one on an enum has every enumerator")
	{
		CHECK(RequireOk(VisualScriptEditing::SetSwitchCases(document, 1, Json::array({1, 3}))) == 1);
		CHECK_FALSE(HasLink(graph, MakeLink(1, "Case:2", 5, "Execute")));
		CHECK(RequireOk(VisualScriptEditing::SetSwitchCases(document, 1, Json())) == 2);
		CHECK_FALSE(graph.Nodes[0].Settings.contains("Cases"));
		CHECK(HasLink(graph, MakeLink(1, "Default", 7, "Execute")));
		// A string case is a pin of its own: "Case:a.b" is no field of another pin.
		CHECK(RequireOk(VisualScriptEditing::SetSwitchCases(document, 2, Json::array({"a.b"}))) == 1);
		CHECK(HasLink(graph, MakeLink(2, "Case:a.b", 5, "Execute")));
		CHECK(RequireOk(VisualScriptEditing::SetSwitchCases(document, 3, Json::array({"Space"}))) == 1);
		CHECK(HasLink(graph, MakeLink(3, "Case:Space", 6, "Execute")));
		CHECK(RequireOk(VisualScriptEditing::SetSwitchCases(document, 3, Json())) == 0);
		CHECK(HasLink(graph, MakeLink(3, "Case:Space", 6, "Execute")));
		CheckDocument(document);
	}

	SUBCASE("cases a Switch cannot have are refused")
	{
		auto const setCases = [](uint32_t node, Json cases)
		{
			return [node, cases](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::SetSwitchCases(edited, node, cases);
			};
		};
		CheckRefused(document, setCases(1, "1"), "the cases of a Switch are an array");
		CheckRefused(document, setCases(1, Json::array({1, 1})), "the case 1 is listed twice");
		CheckRefused(document, setCases(1, Json::array({"1"})), "\"1\" is no case of this Switch: its cases are integers");
		CheckRefused(document, setCases(1, Json::array({1.5})), "1.5 is no case of this Switch");
		CheckRefused(document, setCases(2, Json::array({""})), "its cases are non-empty strings");
		CheckRefused(document, setCases(2, Json::array({std::string(124, 'x')})), "would name a pin of more than 128 bytes");
		CheckRefused(document, setCases(4, Json::array()), "node 4 is no Flow.Switch");
	}
}

TEST_CASE("VisualScriptEditing: an event's parameters keep, rename and remove their pins, and a custom event's senders follow its name")
{
	Scope<VisualScriptLibrary> const library = ParseLibrary();
	VisualScriptDocument document;
	document.Variables.push_back(MakeVariable(40, "Amount", "float"));
	Json const parameters = Json::array({{{"Name", "Amount"}, {"Type", "float"}},
	                                     {{"Name", "Source"}, {"Type", "Strada.Entity"}},
	                                     {{"Name", "Where"}, {"Type", "Strada.Vector3"}}});
	VisualScriptGraph& graph = document.Graph;
	VisualScriptNode custom = MakeNode(1, "Event.Custom", glm::vec2(0.0f), {{"Name", "Hit"}, {"Parameters", parameters}});
	custom.Split.Outputs = {"Where"};
	VisualScriptNode send = MakeNode(5, "Event.Send", glm::vec2(0.0f), {{"Name", "Hit"}, {"Parameters", parameters}}, {{"Amount", 2.5}});
	send.Split.Inputs = {"Where"};
	send.Inputs["Where.X"] = 1;
	graph.Nodes = {custom,
	               MakeNode(2, "Variable.Set", glm::vec2(0.0f), {{"Variable", 40}}),
	               MakeNode(3, "Set:Strada.TransformComponent.Translation"),
	               MakeNode(4, "Flow.Delay"),
	               send,
	               MakeNode(6, "Get:Strada.Time.DeltaTime"),
	               MakeNode(7, "Event.Custom", glm::vec2(0.0f), {{"Name", "Other"}})};
	graph.Links = {MakeLink(1, "Amount", 2, "Value"), MakeLink(1, "Source", 3, "Target"), MakeLink(1, "Where.X", 4, "Duration"),
	               MakeLink(6, "Value", 5, "Where.Y")};
	VisualScriptFunction function = MakeFunction(10, "Notify");
	function.Graph.Nodes = {MakeNode(11, "Function.Entry"), MakeNode(12, "Event.Send", glm::vec2(0.0f), {{"Name", "Hit"}})};
	document.Functions.push_back(function);

	SUBCASE("renamed parameters keep their links, literals and split pins")
	{
		std::vector<VisualScriptParameter> renamed = {{"Damage", "float"}, {"Source", "Strada.Entity"}, {"Spot", "Strada.Vector3"}};
		CHECK(RequireOk(VisualScriptEditing::SetEventParameters(document, 1, renamed)) == 0);
		REQUIRE(graph.Links.size() == 4);
		CHECK(graph.Links[0] == MakeLink(1, "Damage", 2, "Value"));
		CHECK(graph.Links[2] == MakeLink(1, "Spot.X", 4, "Duration"));
		CHECK(graph.Nodes[0].Split.Outputs == std::vector<std::string>{"Spot"});
		CHECK(graph.Nodes[0].Settings["Parameters"] == Json::array({{{"Name", "Damage"}, {"Type", "float"}},
		                                                            {{"Name", "Source"}, {"Type", "Strada.Entity"}},
		                                                            {{"Name", "Spot"}, {"Type", "Strada.Vector3"}}}));
		CHECK(RequireOk(VisualScriptEditing::SetEventParameters(document, 5, renamed)) == 0);
		CHECK(graph.Nodes[4].Inputs == Json({{"Damage", 2.5}, {"Spot.X", 1}}));
		CHECK(graph.Nodes[4].Split.Inputs == std::vector<std::string>{"Spot"});
		CHECK(graph.Links[3] == MakeLink(6, "Value", 5, "Spot.Y"));
		CheckDocument(document);
	}

	SUBCASE("parameters that go take their links, literals and split pins")
	{
		std::vector<VisualScriptParameter> const fewer = {{"Source", "Strada.Entity"}};
		CHECK(RequireOk(VisualScriptEditing::SetEventParameters(document, 1, fewer)) == 2);
		CHECK(graph.Links == std::vector<VisualScriptLink>{MakeLink(1, "Source", 3, "Target"), MakeLink(6, "Value", 5, "Where.Y")});
		CHECK(graph.Nodes[0].Split.Outputs.empty());
		CHECK(RequireOk(VisualScriptEditing::SetEventParameters(document, 5, fewer)) == 1);
		CHECK(graph.Nodes[4].Inputs == Json::object());
		CHECK(graph.Nodes[4].Split.Inputs.empty());
		CheckDocument(document);
	}

	SUBCASE("renaming a custom event renames the senders of the document")
	{
		CHECK(RequireOk(VisualScriptEditing::RenameCustomEvent(document, 1, "Damaged")) == 2);
		CHECK(graph.Nodes[0].Settings["Name"] == "Damaged");
		CHECK(graph.Nodes[4].Settings["Name"] == "Damaged");
		CHECK(document.Functions[0].Graph.Nodes[1].Settings["Name"] == "Damaged");
		CHECK(RequireOk(VisualScriptEditing::RenameCustomEvent(document, 1, "Damaged")) == 0);
		CheckDocument(document);
	}

	SUBCASE("parameters and names an event cannot have are refused")
	{
		auto const setParameters = [](uint32_t node, std::vector<VisualScriptParameter> list)
		{
			return [node, list](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::SetEventParameters(edited, node, list);
			};
		};
		CheckRefused(document, setParameters(1, {{"Target", "float"}}), "Target is a name of pins every node may have");
		CheckRefused(document, setParameters(1, {{"A", "float"}, {"A", "int"}}), "two parameters named A");
		CheckRefused(document, setParameters(1, {{"A", "exec"}}), "a parameter cannot have the type exec");
		CheckRefused(document, setParameters(1, {{"A.B", "float"}}), "expected a name of 1 to 64 bytes");
		CheckRefused(document, setParameters(2, {}), "node 2 is no Event.Custom or Event.Send: its type is Variable.Set");
		std::vector<VisualScriptParameter> many;
		for (int index = 0; index < 65; index++)
		{
			many.push_back({"P" + std::to_string(index), "int"});
		}
		CheckRefused(document, setParameters(1, many), "at most 64 parameters");

		auto const rename = [](uint32_t node, std::string name)
		{
			return [node, name](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::RenameCustomEvent(edited, node, name);
			};
		};
		CheckRefused(document, rename(1, "Other"), "a custom event is named Other already");
		CheckRefused(document, rename(1, " Hit"), "expected a name");
		CheckRefused(document, rename(5, "Damaged"), "node 5 is no Event.Custom: its type is Event.Send");
	}
}

TEST_CASE("VisualScriptEditing: node comments, collapsed and enabled states are set on nodes of any graph")
{
	VisualScriptDocument document;
	document.Graph.Nodes = {MakeNode(1, "Flow.Delay"), MakeNode(2, "Call:Strada.Log.Info(string)")};
	VisualScriptFunction function = MakeFunction(3, "Apply");
	function.Graph.Nodes = {MakeNode(4, "Function.Entry"), MakeNode(5, "Flow.Delay")};
	document.Functions.push_back(function);

	std::vector<uint32_t> const nodes = {1, 5, 1};
	RequireOk(VisualScriptEditing::SetNodeComment(document, nodes, "Waits\nfor the door"));
	RequireOk(VisualScriptEditing::SetNodeCollapsed(document, nodes, true));
	RequireOk(VisualScriptEditing::SetNodeEnabled(document, nodes, VisualScriptNodeEnabled::DevelopmentOnly));
	for (VisualScriptNode const* const node : {&document.Graph.Nodes[0], &document.Functions[0].Graph.Nodes[1]})
	{
		CHECK(node->Comment == "Waits\nfor the door");
		CHECK(node->Collapsed);
		CHECK(node->Enabled == VisualScriptNodeEnabled::DevelopmentOnly);
	}
	CHECK(document.Graph.Nodes[1].Comment.empty());
	CheckDocument(document);
	RequireOk(VisualScriptEditing::SetNodeComment(document, nodes, ""));
	RequireOk(VisualScriptEditing::SetNodeEnabled(document, nodes, VisualScriptNodeEnabled::Disabled));
	CHECK(document.Graph.Nodes[0].Comment.empty());
	CHECK(document.Graph.Nodes[0].Enabled == VisualScriptNodeEnabled::Disabled);
	CheckDocument(document);

	std::vector<uint32_t> const missing = {1, 9};
	CheckRefused(
		document,
		[&missing](VisualScriptDocument& edited)
		{
			return VisualScriptEditing::SetNodeCollapsed(edited, missing, false);
		},
		"the document has no node 9");
	CheckRefused(
		document,
		[&nodes](VisualScriptDocument& edited)
		{
			return VisualScriptEditing::SetNodeComment(edited, nodes, "\xC3");
		},
		"a node comment must be UTF-8");
	CheckRefused(
		document,
		[&nodes](VisualScriptDocument& edited)
		{
			return VisualScriptEditing::SetNodeEnabled(edited, nodes, static_cast<VisualScriptNodeEnabled>(7));
		},
		"7 is no enabled state");
}

TEST_CASE("VisualScriptEditing: struct pins split into their fields and recombine, their literals with them")
{
	Scope<VisualScriptLibrary> const library = ParseLibrary();
	VisualScriptDocument document;
	document.Variables = {MakeVariable(60, "Pose", "Strada.Matrix4"), MakeVariable(61, "Aim", "Strada.Ray"),
	                      MakeVariable(62, "Tint", "Strada.Vector4")};
	Json matrix = Json::array();
	for (int index = 1; index <= 16; index++)
	{
		matrix.push_back(index);
	}
	VisualScriptGraph& graph = document.Graph;
	graph.Nodes = {MakeNode(40, "Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,Strada.ForceMode)", glm::vec2(400.0f, 0.0f),
	                        Json::object(), {{"Force", {1, 2.5, -3}}, {"Mode", "Impulse"}}),
	               MakeNode(41, "Get:Strada.TransformComponent.Translation", glm::vec2(0.0f, 200.0f)),
	               MakeNode(42, "Call:Strada.Physics.Raycast(Strada.Vector3,Strada.Vector3,float,out Strada.RaycastHit,uint)",
	                        glm::vec2(0.0f, 400.0f)),
	               MakeNode(43, "Variable.Set", glm::vec2(0.0f), {{"Variable", 60}}, {{"Value", matrix}}),
	               MakeNode(44, "Variable.Set", glm::vec2(0.0f), {{"Variable", 61}}, {{"Value", {1, 2, 3, 4, 5, 6}}}),
	               MakeNode(45, "Variable.Set", glm::vec2(0.0f), {{"Variable", 62}}, {{"Value", {0.5, 0.25, 1, 0}}}),
	               MakeNode(46, "Get:Strada.Time.DeltaTime"),
	               MakeNode(47, "Reroute"),
	               MakeNode(48, "Flow.Delay")};
	graph.Links = {MakeLink(41, "Value", 40, "Force")};
	VisualScriptNodePins const pins = MakePins(*library, document);
	VisualScriptNode const& force = graph.Nodes[0];

	SUBCASE("an input splits with its literal and its links go; its fields' literals come back as one when it recombines")
	{
		CHECK(RequireOk(VisualScriptEditing::SplitPin(document, 40, In, "Force", pins, *library)) == 1);
		CHECK(force.Split.Inputs == std::vector<std::string>{"Force"});
		CHECK(force.Inputs == Json({{"Mode", "Impulse"}, {"Force.X", 1}, {"Force.Y", 2.5}, {"Force.Z", -3}}));
		CHECK(graph.Links.empty());
		CheckDocument(document);
		// A row that still shows the split pin itself (a stale one) does not make it linkable or promotable.
		VisualScriptNodePins const stale = [](uint32_t)
		{
			return std::vector<VisualScriptNodePin>{{"Force", In, "Strada.Vector3", glm::vec2(0.0f)},
			                                        {"Value", Out, "Strada.Vector3", glm::vec2(0.0f)}};
		};
		CheckRefused(
			document,
			[&stale](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::Connect(edited, 0, MakeLink(41, "Value", 40, "Force"), stale);
			},
			"node 40 has no input Force");
		CheckRefused(
			document,
			[&stale](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::PromoteToVariable(edited, 0, {40, "Force"}, In, stale);
			},
			"node 40 has no input Force");
		CHECK(RequireOk(VisualScriptEditing::RecombinePin(document, 40, In, "Force", pins, *library)) == 0);
		CHECK(force.Split.Inputs.empty());
		CHECK(force.Inputs == Json({{"Mode", "Impulse"}, {"Force", {1, 2.5, -3}}}));
		CheckDocument(document);

		// A linked field keeps the fields' literals from becoming the pin's.
		RequireOk(VisualScriptEditing::SplitPin(document, 40, In, "Force", pins, *library));
		RequireOk(VisualScriptEditing::Connect(document, 0, MakeLink(46, "Value", 40, "Force.Y"), pins));
		CheckDocument(document);
		CHECK(RequireOk(VisualScriptEditing::RecombinePin(document, 40, In, "Force", pins, *library)) == 1);
		CHECK(force.Inputs == Json({{"Mode", "Impulse"}}));
		CHECK(graph.Links.empty());
		CheckDocument(document);
	}

	SUBCASE("outputs split, a field splits again, and recombining takes every field below")
	{
		CHECK(RequireOk(VisualScriptEditing::SplitPin(document, 42, Out, "Hit", pins, *library)) == 0);
		CHECK(RequireOk(VisualScriptEditing::SplitPin(document, 42, Out, "Hit.Point", pins, *library)) == 0);
		CHECK(graph.Nodes[2].Split.Outputs == std::vector<std::string>{"Hit", "Hit.Point"});
		RequireOk(VisualScriptEditing::Connect(document, 0, MakeLink(42, "Hit.Point.Y", 48, "Duration"), pins));
		CheckDocument(document);
		CHECK(RequireOk(VisualScriptEditing::RecombinePin(document, 42, Out, "Hit", pins, *library)) == 1);
		CHECK(graph.Nodes[2].Split.Outputs.empty());
		CHECK(graph.Links == std::vector<VisualScriptLink>{MakeLink(41, "Value", 40, "Force")});
		CheckDocument(document);
	}

	SUBCASE("a matrix splits into columns that split again, and comes back whole; structs without literals keep none")
	{
		RequireOk(VisualScriptEditing::SplitPin(document, 43, In, "Value", pins, *library));
		CHECK(graph.Nodes[3].Inputs == Json({{"Value.Column0", {1, 2, 3, 4}},
		                                     {"Value.Column1", {5, 6, 7, 8}},
		                                     {"Value.Column2", {9, 10, 11, 12}},
		                                     {"Value.Column3", {13, 14, 15, 16}}}));
		RequireOk(VisualScriptEditing::SplitPin(document, 43, In, "Value.Column1", pins, *library));
		CHECK(graph.Nodes[3].Inputs["Value.Column1.W"] == 8);
		CHECK(graph.Nodes[3].Split.Inputs == std::vector<std::string>{"Value", "Value.Column1"});
		CheckDocument(document);
		RequireOk(VisualScriptEditing::RecombinePin(document, 43, In, "Value", pins, *library));
		CHECK(graph.Nodes[3].Inputs == Json({{"Value", matrix}}));
		CHECK(graph.Nodes[3].Split.Inputs.empty());

		// A ray has no literal: what it had goes, and its fields' literals do not make one.
		RequireOk(VisualScriptEditing::SplitPin(document, 44, In, "Value", pins, *library));
		CHECK(graph.Nodes[4].Inputs == Json::object());
		RequireOk(VisualScriptEditing::SetInput(document, 44, "Value.Origin", Json::array({1, 2, 3})));
		RequireOk(VisualScriptEditing::SetInput(document, 44, "Value.Direction", Json::array({0, 0, 1})));
		RequireOk(VisualScriptEditing::RecombinePin(document, 44, In, "Value", pins, *library));
		CHECK(graph.Nodes[4].Inputs == Json::object());

		// Vector4 and Quaternion have the same fields, and literals of the same form.
		RequireOk(VisualScriptEditing::SplitPin(document, 45, In, "Value", pins, *library));
		CHECK(graph.Nodes[5].Inputs == Json({{"Value.X", 0.5}, {"Value.Y", 0.25}, {"Value.Z", 1}, {"Value.W", 0}}));
		RequireOk(VisualScriptEditing::RecombinePin(document, 45, In, "Value", pins, *library));
		CHECK(graph.Nodes[5].Inputs == Json({{"Value", {0.5, 0.25, 1, 0}}}));
		CheckDocument(document);
	}

	SUBCASE("pins that cannot split or recombine are refused")
	{
		auto const split = [&pins, &library](uint32_t node, VisualScriptPinDirection direction, std::string path)
		{
			return [&pins, &library, node, direction, path](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::SplitPin(edited, node, direction, path, pins, *library);
			};
		};
		CheckRefused(document, split(42, Out, "Result"), "Result (bool) cannot be split");
		CheckRefused(document, split(42, Out, "Then"), "Then (exec) cannot be split");
		CheckRefused(document, split(40, In, "Mode"), "Mode (Strada.ForceMode) cannot be split");
		CheckRefused(document, split(40, In, "Target"), "Target (Strada.Entity) cannot be split");
		CheckRefused(document, split(47, Out, "Out"), "the type of Out is not known yet");
		CheckRefused(document, split(40, In, "Nope"), "node 40 has no input Nope");
		CheckRefused(document, split(42, Out, "Hit.Point"), "node 42 has no output Hit.Point");
		CheckRefused(document, split(99, Out, "Hit"), "the document has no node 99");
		RequireOk(VisualScriptEditing::SplitPin(document, 42, Out, "Hit", pins, *library));
		CheckRefused(document, split(42, Out, "Hit"), "node 42 splits its output Hit already");
		CheckRefused(document, split(42, Out, "Hit.Entity"), "Hit.Entity (Strada.Entity) cannot be split");
		CheckRefused(
			document,
			[&pins, &library](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::RecombinePin(edited, 40, In, "Mode", pins, *library);
			},
			"node 40 does not split its input Mode");
	}
}

TEST_CASE("VisualScriptEditing: comment boxes frame elements, and keep their geometry and colour within the format's ranges")
{
	VisualScriptDocument document;
	VisualScriptGraph& graph = document.Graph;
	graph.Nodes = {MakeNode(1, "Event.Update"), MakeNode(2, "Flow.Branch", glm::vec2(300.0f, 50.0f)),
	               MakeNode(3, "Flow.Branch", std::nullopt)};
	VisualScriptComment existing;
	existing.ID = 20;
	existing.Position = glm::vec2(-100.0f, 400.0f);
	existing.Size = glm::vec2(100.0f, 50.0f);
	graph.Comments.push_back(existing);
	VisualScriptNodeExtents const extents = [](uint32_t node)
	{
		return node == 1 ? glm::vec2(200.0f, 100.0f) : glm::vec2(150.0f, 80.0f);
	};

	std::vector<uint32_t> const framed = {1, 2, 20};
	uint32_t const group = RequireOk(
		VisualScriptEditing::AddComment(document, 0, "Group", framed, extents, glm::vec2(0.0f), glm::vec4(2.0f, -1.0f, 0.5f, 0.25f)));
	CHECK(group == 21);
	REQUIRE(graph.Comments.size() == 2);
	CHECK(graph.Comments[1].Text == "Group");
	CHECK(graph.Comments[1].Position == glm::vec2(-132.0f, -72.0f));
	CHECK(graph.Comments[1].Size == glm::vec2(614.0f, 554.0f));
	CHECK(graph.Comments[1].Color == glm::vec4(1.0f, 0.0f, 0.5f, 0.25f));
	uint32_t const standalone =
		RequireOk(VisualScriptEditing::AddComment(document, 0, "Free", {}, {}, glm::vec2(10.0f, 20.0f), glm::vec4(0.25f)));
	CHECK(standalone == 22);
	REQUIRE(graph.Comments.size() == 3);
	CHECK(graph.Comments[2].Position == glm::vec2(10.0f, 20.0f));
	CHECK(graph.Comments[2].Size == glm::vec2(400.0f, 200.0f));

	RequireOk(VisualScriptEditing::SetCommentText(document, group, "Scoring\nAdds points"));
	RequireOk(VisualScriptEditing::SetCommentGeometry(document, group, glm::vec2(5.0e7f, 1.0f), glm::vec2(5.0f, 2.0e6f)));
	RequireOk(VisualScriptEditing::SetCommentColor(document, standalone, glm::vec4(-1.0f, 2.0f, 0.5f, 1.0f)));
	RequireOk(VisualScriptEditing::SetDescription(document, "Moves the player"));
	VisualScriptComment const& box = graph.Comments[1];
	CHECK(box.Text == "Scoring\nAdds points");
	CHECK(box.Position == glm::vec2(1.0e7f, 1.0f));
	CHECK(box.Size == glm::vec2(16.0f, 1.0e6f));
	CHECK(graph.Comments[2].Color == glm::vec4(0.0f, 1.0f, 0.5f, 1.0f));
	CHECK(document.Description == "Moves the player");
	CheckDocument(document);

	auto const add = [&extents](std::vector<uint32_t> elements, bool measured, glm::vec2 position, std::string text, glm::vec4 color)
	{
		return [&extents, elements, measured, position, text, color](VisualScriptDocument& edited)
		{
			return VisualScriptEditing::AddComment(edited, 0, text, elements, measured ? extents : VisualScriptNodeExtents(), position,
			                                       color);
		};
	};
	glm::vec4 const grey(0.5f);
	float const nan = std::numeric_limits<float>::quiet_NaN();
	CheckRefused(document, add({3}, true, glm::vec2(0.0f), "Box", grey), "node 3 has no position yet");
	CheckRefused(document, add({1}, false, glm::vec2(0.0f), "Box", grey), "the size of node 1 is not known");
	CheckRefused(document, add({99}, true, glm::vec2(0.0f), "Box", grey), "the graph has no node or comment 99");
	CheckRefused(document, add({}, true, glm::vec2(nan, 0.0f), "Box", grey), "a comment's position must be finite");
	CheckRefused(document, add({}, true, glm::vec2(0.0f), "\xFF", grey), "a comment's text must be UTF-8");
	CheckRefused(document, add({}, true, glm::vec2(0.0f), "Box", glm::vec4(nan)), "a comment's colour must be finite");
	CheckRefused(
		document,
		[](VisualScriptDocument& edited)
		{
			return VisualScriptEditing::SetCommentText(edited, 99, "Text");
		},
		"the document has no comment 99");
	CheckRefused(
		document,
		[nan](VisualScriptDocument& edited)
		{
			return VisualScriptEditing::SetCommentGeometry(edited, 21, glm::vec2(0.0f), glm::vec2(nan));
		},
		"must be finite");
	CheckRefused(
		document,
		[](VisualScriptDocument& edited)
		{
			return VisualScriptEditing::SetDescription(edited, std::string(VisualScriptSerializer::MaxTextBytes + 1, 'a'));
		},
		"a description must be UTF-8 of at most 65536 bytes");

	VisualScriptDocument crowded;
	for (uint32_t id = 1; id <= VisualScriptSerializer::MaxCommentsPerGraph; id++)
	{
		VisualScriptComment comment;
		comment.ID = id;
		crowded.Graph.Comments.push_back(comment);
	}
	CheckRefused(crowded, add({}, true, glm::vec2(0.0f), "Box", grey), "at most 2000 comments");
}

TEST_CASE("VisualScriptEditing: variables are added, changed, removed, reordered and duplicated within their name scope")
{
	VisualScriptDocument document;
	VisualScriptVariable speed = MakeVariable(1, "Speed", "float", 4.5);
	speed.FormerNames = {"Velocity"};
	document.Variables = {speed, MakeVariable(2, "Score", "int", 3)};
	VisualScriptFunction function = MakeFunction(10, "AddScore");
	function.Locals.push_back({11, "Total", "int", Json()});
	function.Graph.Nodes = {MakeNode(12, "Function.Entry"), MakeNode(13, "Variable.Get", glm::vec2(0.0f), {{"Variable", 1}}),
	                        MakeNode(14, "Variable.Get", glm::vec2(0.0f), {{"Variable", 11}})};
	document.Functions.push_back(function);
	VisualScriptGraph& graph = document.Graph;
	graph.Nodes = {MakeNode(20, "Variable.Get", glm::vec2(0.0f), {{"Variable", 1}}),
	               MakeNode(21, "Variable.Set", glm::vec2(0.0f), {{"Variable", 1}}),
	               MakeNode(22, "Variable.Get", glm::vec2(0.0f), {{"Variable", 2}}), MakeNode(23, "Call:Strada.Log.Info(string)")};
	graph.Links = {MakeLink(20, "Value", 21, "Value"), MakeLink(21, "Then", 23, "Execute")};

	SUBCASE("adding")
	{
		CHECK(RequireOk(VisualScriptEditing::AddVariable(document, "Health", "float")) == 24);
		CHECK(document.Variables.back() == MakeVariable(24, "Health", "float"));
		CheckDocument(document);
		auto const add = [](std::string name, std::string type)
		{
			return [name, type](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::AddVariable(edited, name, type);
			};
		};
		CheckRefused(document, add("Speed", "float"), "a variable is named Speed already");
		CheckRefused(document, add("Velocity", "float"), "Velocity is a former name of the variable Speed");
		CheckRefused(document, add("Total", "float"), "a local of AddScore is named Total");
		CheckRefused(document, add("Bad.Name", "float"), "expected a name of 1 to 64 bytes");
		CheckRefused(document, add("Armor", "exec"), "a variable cannot have the type exec");
		CheckRefused(document, add("Armor", "*"), "a variable cannot have the type *");
		CheckRefused(document, add("Armor", ""), "expected a type of 1 to 256 bytes");
	}

	SUBCASE("renaming and retyping")
	{
		VisualScriptVariable renamed = document.Variables[0];
		renamed.Name = "MaxSpeed";
		CHECK_FALSE(RequireOk(VisualScriptEditing::SetVariable(document, 1, renamed)));
		CHECK(document.Variables[0].Name == "MaxSpeed");
		CHECK(document.Variables[0].FormerNames == std::vector<std::string>{"Velocity"});
		CheckDocument(document);

		// Back to a former name once it is forgotten.
		renamed.Name = "Velocity";
		CheckRefused(
			document,
			[&renamed](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::SetVariable(edited, 1, renamed);
			},
			"Velocity is one of the variable's own former names");
		renamed.FormerNames.clear();
		CHECK_FALSE(RequireOk(VisualScriptEditing::SetVariable(document, 1, renamed)));
		CheckDocument(document);

		// A value that no longer reads as the type goes; one that does stays.
		VisualScriptVariable retyped = document.Variables[0];
		retyped.Type = "int";
		CHECK(RequireOk(VisualScriptEditing::SetVariable(document, 1, retyped)));
		CHECK(document.Variables[0].Value.is_null());
		VisualScriptVariable widened = document.Variables[1];
		widened.Type = "float";
		CHECK_FALSE(RequireOk(VisualScriptEditing::SetVariable(document, 2, widened)));
		CHECK(document.Variables[1].Value == 3);
		VisualScriptVariable exposed = document.Variables[1];
		exposed.Public = true;
		exposed.Tooltip = "Points so far";
		exposed.Range = glm::vec2(0.0f, 10.0f);
		RequireOk(VisualScriptEditing::SetVariable(document, 2, exposed));
		CHECK(document.Variables[1].Public);
		CheckDocument(document);
	}

	SUBCASE("what a variable cannot be is refused")
	{
		auto const set = [](uint32_t id, VisualScriptVariable variable)
		{
			return [id, variable](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::SetVariable(edited, id, variable);
			};
		};
		VisualScriptVariable variable = document.Variables[1];
		variable.Name = "Speed";
		CheckRefused(document, set(2, variable), "a variable is named Speed already");
		variable.Name = "Total";
		CheckRefused(document, set(2, variable), "a local of AddScore is named Total");
		variable = document.Variables[1];
		variable.Type = "Strada.Matrix4";
		variable.Public = true;
		CheckRefused(document, set(2, variable), "Strada.Matrix4 cannot be public: components cannot store it");
		variable = document.Variables[1];
		variable.Range = glm::vec2(5.0f, 1.0f);
		CheckRefused(document, set(2, variable), "a range is two finite numbers, the minimum first");
		variable.Range = glm::vec2(std::numeric_limits<float>::quiet_NaN(), 1.0f);
		CheckRefused(document, set(2, variable), "a range is two finite numbers");
		variable = document.Variables[1];
		variable.FormerNames = {"Old", "Old"};
		CheckRefused(document, set(2, variable), "Old is listed twice among the former names");
		variable.FormerNames = {"Speed"};
		CheckRefused(document, set(2, variable), "the former name Speed is the name of a variable");
		variable.FormerNames = {"Velocity"};
		CheckRefused(document, set(2, variable), "Velocity is a former name of the variable Speed already");
		variable.FormerNames = {"a.b"};
		CheckRefused(document, set(2, variable), "expected a name");
		variable.FormerNames.clear();
		for (int index = 0; index < 17; index++)
		{
			variable.FormerNames.push_back("Old" + std::to_string(index));
		}
		CheckRefused(document, set(2, variable), "at most 16 former names");
		variable = document.Variables[1];
		variable.Tooltip = "\xC3";
		CheckRefused(document, set(2, variable), "a tooltip must be UTF-8");
		variable = document.Variables[1];
		variable.Value = std::numeric_limits<double>::quiet_NaN();
		CheckRefused(document, set(2, variable), "a variable's value must nest");
		CheckRefused(document, set(99, variable), "the document has no variable 99");
	}

	SUBCASE("removing takes the nodes that name a variable from every graph, when asked")
	{
		CHECK(RequireOk(VisualScriptEditing::RemoveVariable(document, 1, true)) == 3);
		CHECK(document.Variables.size() == 1);
		CHECK(graph.Nodes.size() == 2);
		CHECK(graph.Links.empty());
		CHECK(document.Functions[0].Graph.Nodes.size() == 2);
		CHECK(RequireOk(VisualScriptEditing::RemoveVariable(document, 2, false)) == 0);
		CHECK(HasNode(graph, 22));
		CheckRefused(
			document,
			[](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::RemoveVariable(edited, 2, true);
			},
			"the document has no variable 2");
	}

	SUBCASE("reordering and duplicating")
	{
		std::vector<uint32_t> const order = {2, 1};
		RequireOk(VisualScriptEditing::ReorderVariables(document, order));
		CHECK(document.Variables[0].ID == 2);
		CHECK(document.Variables[1].ID == 1);
		auto const reorder = [](std::vector<uint32_t> ids)
		{
			return [ids](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::ReorderVariables(edited, ids);
			};
		};
		CheckRefused(document, reorder({1}), "the order lists 1 variables of 2");
		CheckRefused(document, reorder({1, 1}), "the order lists variable 1 twice");
		CheckRefused(document, reorder({1, 99}), "the document has no variable 99");

		CHECK(RequireOk(VisualScriptEditing::DuplicateVariable(document, 1)) == 24);
		REQUIRE(document.Variables.size() == 3);
		VisualScriptVariable const& copy = document.Variables[2];
		CHECK(copy.ID == 24);
		CHECK(copy.Name == "Speed1");
		CHECK(copy.FormerNames.empty());
		CHECK(copy.Value == 4.5);
		CHECK(document.Variables[1].Name == "Speed");
		CheckDocument(document);
	}
}

TEST_CASE("VisualScriptEditing: locals are added, changed and removed within their function")
{
	VisualScriptDocument document;
	VisualScriptVariable speed = MakeVariable(1, "Speed", "float");
	speed.FormerNames = {"Velocity"};
	document.Variables.push_back(speed);
	VisualScriptFunction function = MakeFunction(10, "AddScore");
	function.Locals.push_back({11, "Total", "int", 3});
	function.Graph.Nodes = {MakeNode(12, "Function.Entry"), MakeNode(13, "Variable.Get", glm::vec2(0.0f), {{"Variable", 11}}),
	                        MakeNode(14, "Variable.Get", glm::vec2(0.0f), {{"Variable", 1}})};
	document.Functions = {function, MakeFunction(20, "Other")};

	CHECK(RequireOk(VisualScriptEditing::AddLocal(document, 10, "Count", "int")) == 21);
	CHECK(RequireOk(VisualScriptEditing::AddLocal(document, 10, "Velocity", "float")) == 22);
	CHECK(RequireOk(VisualScriptEditing::AddLocal(document, 20, "Total", "int")) == 23);
	CheckDocument(document);
	auto const add = [](uint32_t owner, std::string name)
	{
		return [owner, name](VisualScriptDocument& edited)
		{
			return VisualScriptEditing::AddLocal(edited, owner, name, "int");
		};
	};
	CheckRefused(document, add(10, "Total"), "a local of AddScore is named Total already");
	CheckRefused(document, add(10, "Speed"), "a variable is named Speed");
	CheckRefused(document, add(0, "Sum"), "the document has no function 0");

	VisualScriptLocal local = document.Functions[0].Locals[0];
	local.Name = "Sum";
	local.Type = "float";
	CHECK_FALSE(RequireOk(VisualScriptEditing::SetLocal(document, 10, 11, local)));
	CHECK(document.Functions[0].Locals[0].Value == 3);
	local.Type = "string";
	CHECK(RequireOk(VisualScriptEditing::SetLocal(document, 10, 11, local)));
	CHECK(document.Functions[0].Locals[0].Value.is_null());
	CHECK(document.Functions[0].Locals[0].Name == "Sum");
	CheckDocument(document);
	auto const set = [](uint32_t id, std::string name)
	{
		return [id, name](VisualScriptDocument& edited)
		{
			VisualScriptLocal changed{id, name, "int", Json()};
			return VisualScriptEditing::SetLocal(edited, 10, id, changed);
		};
	};
	CheckRefused(document, set(11, "Count"), "a local of AddScore is named Count already");
	CheckRefused(document, set(11, "Speed"), "a variable is named Speed");
	CheckRefused(document, set(99, "Sum"), "AddScore has no local 99");

	CHECK(RequireOk(VisualScriptEditing::RemoveLocal(document, 10, 11, true)) == 1);
	CHECK_FALSE(HasNode(document.Functions[0].Graph, 13));
	CHECK(HasNode(document.Functions[0].Graph, 14));
	CHECK(RequireOk(VisualScriptEditing::RemoveLocal(document, 20, 23, false)) == 0);
	CheckDocument(document);
	CheckRefused(
		document,
		[](VisualScriptDocument& edited)
		{
			return VisualScriptEditing::RemoveLocal(edited, 10, 11, true);
		},
		"AddScore has no local 11");
}

TEST_CASE("VisualScriptEditing: functions are added with an entry linked to a return, and renamed")
{
	VisualScriptDocument document;
	document.Variables.push_back(MakeVariable(5, "Score", "int"));

	CHECK(RequireOk(VisualScriptEditing::AddFunction(document, "AddScore")) == 6);
	VisualScriptFunction const& added = document.Functions[0];
	CHECK(added.Name == "AddScore");
	CHECK_FALSE(added.Pure);
	CHECK(added.Graph.Nodes ==
	      std::vector<VisualScriptNode>{MakeNode(7, "Function.Entry"), MakeNode(8, "Function.Return", glm::vec2(480.0f, 0.0f))});
	CHECK(added.Graph.Links == std::vector<VisualScriptLink>{MakeLink(7, "Then", 8, "Execute")});
	CHECK(RequireOk(VisualScriptEditing::AddFunction(document, "Other")) == 9);
	RequireOk(VisualScriptEditing::RenameFunction(document, 6, "Score"));
	RequireOk(VisualScriptEditing::RenameFunction(document, 6, "Score"));
	CHECK(document.Functions[0].Name == "Score");
	CheckDocument(document);
	CheckRefused(
		document,
		[](VisualScriptDocument& edited)
		{
			return VisualScriptEditing::AddFunction(edited, "Other");
		},
		"a function is named Other already");
	CheckRefused(
		document,
		[](VisualScriptDocument& edited)
		{
			return VisualScriptEditing::AddFunction(edited, "Add.Score");
		},
		"expected a name");
	CheckRefused(
		document,
		[](VisualScriptDocument& edited)
		{
			return VisualScriptEditing::RenameFunction(edited, 9, "Score");
		},
		"a function is named Score already");
}

TEST_CASE("VisualScriptEditing: a function's signature and purity rewrite its pins, and functions are removed and duplicated")
{
	VisualScriptDocument document;
	document.Variables.push_back(MakeVariable(5, "Score", "int"));

	// A function with parameters, used by its own graph and by the event graph.
	VisualScriptFunction function = MakeFunction(6, "AddScore", {{"Points", "int"}, {"Bonus", "int"}}, {{"Total", "int"}});
	function.Locals.push_back({17, "Sum", "int", Json()});
	function.Graph.Nodes = {MakeNode(7, "Function.Entry"), MakeNode(8, "Function.Return", glm::vec2(600.0f, 0.0f)),
	                        MakeNode(12, "Operator.Add", glm::vec2(300.0f, 100.0f)),
	                        MakeNode(15, "Function.Call", glm::vec2(300.0f, 300.0f), {{"Function", 6}}, {{"Points", 2}}),
	                        MakeNode(18, "Variable.Get", glm::vec2(0.0f, 300.0f), {{"Variable", 17}})};
	function.Graph.Links = {MakeLink(7, "Then", 8, "Execute"), MakeLink(7, "Points", 12, "A"), MakeLink(7, "Bonus", 12, "B"),
	                        MakeLink(12, "Result", 8, "Total")};
	VisualScriptComment comment;
	comment.ID = 19;
	function.Graph.Comments.push_back(comment);
	document.Functions.push_back(function);
	VisualScriptGraph& graph = document.Graph;
	graph.Nodes = {MakeNode(13, "Function.Call", glm::vec2(300.0f, 0.0f), {{"Function", 6}}, {{"Points", 5}, {"Bonus", 1}}),
	               MakeNode(14, "Call:Strada.Log.Info(string)", glm::vec2(600.0f, 0.0f)), MakeNode(16, "Event.Update")};
	graph.Links = {MakeLink(16, "Then", 13, "Execute"), MakeLink(13, "Then", 14, "Execute"), MakeLink(13, "Total", 14, "Message")};
	VisualScriptGraph& own = document.Functions[0].Graph;

	SUBCASE("a renamed input renames the pins of its entry and calls; a removed one removes them; reordering changes nothing")
	{
		CHECK(RequireOk(VisualScriptEditing::SetFunctionSignature(document, 6, {{"Score", "int"}, {"Bonus", "int"}}, {{"Total", "int"}})) ==
		      0);
		CHECK(HasLink(own, MakeLink(7, "Score", 12, "A")));
		CHECK(graph.Nodes[0].Inputs == Json({{"Score", 5}, {"Bonus", 1}}));
		CHECK(own.Nodes[3].Inputs == Json({{"Score", 2}}));
		CheckDocument(document);

		CHECK(RequireOk(VisualScriptEditing::SetFunctionSignature(document, 6, {{"Score", "int"}}, {{"Total", "int"}})) == 1);
		CHECK_FALSE(HasLink(own, MakeLink(7, "Bonus", 12, "B")));
		CHECK(graph.Nodes[0].Inputs == Json({{"Score", 5}}));
		CHECK(document.Functions[0].Inputs == std::vector<VisualScriptParameter>{{"Score", "int"}});

		std::vector<VisualScriptLink> const links = own.Links;
		CHECK(RequireOk(
				  VisualScriptEditing::SetFunctionSignature(document, 6, {{"Score", "int"}, {"Extra", "float"}}, {{"Total", "int"}})) == 0);
		CHECK(RequireOk(
				  VisualScriptEditing::SetFunctionSignature(document, 6, {{"Extra", "float"}, {"Score", "int"}}, {{"Total", "int"}})) == 0);
		CHECK(own.Links == links);
		CheckDocument(document);
	}

	SUBCASE("a renamed output renames the pins of the returns and calls")
	{
		CHECK(RequireOk(VisualScriptEditing::SetFunctionSignature(document, 6, {{"Points", "int"}, {"Bonus", "int"}}, {{"Sum", "int"}})) ==
		      0);
		CHECK(HasLink(own, MakeLink(12, "Result", 8, "Sum")));
		CHECK(HasLink(graph, MakeLink(13, "Sum", 14, "Message")));
		CheckDocument(document);
		CHECK(RequireOk(VisualScriptEditing::SetFunctionSignature(document, 6, {{"Points", "int"}, {"Bonus", "int"}}, {})) == 2);
		CheckDocument(document);
	}

	SUBCASE("signatures that cannot be are refused")
	{
		auto const sign = [](uint32_t id, std::vector<VisualScriptParameter> inputs, std::vector<VisualScriptParameter> outputs)
		{
			return [id, inputs, outputs](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::SetFunctionSignature(edited, id, inputs, outputs);
			};
		};
		CheckRefused(document, sign(6, {{"Then", "int"}}, {}), "Then is a name of pins every node may have");
		CheckRefused(document, sign(6, {{"A", "int"}, {"A", "int"}}, {}), "a function's inputs have two parameters named A");
		CheckRefused(document, sign(6, {}, {{"X", "*"}}), "a parameter cannot have the type *");
		CheckRefused(document, sign(77, {}, {}), "the document has no function 77");
	}

	SUBCASE("a pure function loses the flow links of its entry, returns and calls")
	{
		CHECK(RequireOk(VisualScriptEditing::SetFunctionPure(document, 6, true)) == 3);
		CHECK(document.Functions[0].Pure);
		CHECK(graph.Links == std::vector<VisualScriptLink>{MakeLink(13, "Total", 14, "Message")});
		CHECK_FALSE(HasLink(own, MakeLink(7, "Then", 8, "Execute")));
		CHECK(RequireOk(VisualScriptEditing::SetFunctionPure(document, 6, false)) == 0);
		CHECK_FALSE(document.Functions[0].Pure);
		CheckDocument(document);
	}

	SUBCASE("removing a function takes its calls when asked")
	{
		VisualScriptDocument keepingCalls = document;
		CHECK(RequireOk(VisualScriptEditing::RemoveFunction(document, 6, true)) == 1);
		CHECK(document.Functions.empty());
		CHECK_FALSE(HasNode(graph, 13));
		CHECK(graph.Links.empty());
		CheckDocument(document);
		CHECK(RequireOk(VisualScriptEditing::RemoveFunction(keepingCalls, 6, false)) == 0);
		CHECK(HasNode(keepingCalls.Graph, 13));
		CheckRefused(
			document,
			[](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::RemoveFunction(edited, 6, true);
			},
			"the document has no function 6");
	}

	SUBCASE("a copy takes new IDs, names its own locals and calls itself")
	{
		VisualScriptGraph const original = own;
		CHECK(RequireOk(VisualScriptEditing::DuplicateFunction(document, 6)) == 20);
		REQUIRE(document.Functions.size() == 2);
		VisualScriptFunction const& copy = document.Functions[1];
		REQUIRE(copy.Locals.size() == 1);
		REQUIRE(copy.Graph.Nodes.size() == 5);
		REQUIRE(copy.Graph.Comments.size() == 1);
		CHECK(copy.Name == "AddScore1");
		CHECK(copy.Locals[0].ID == 21);
		std::vector<uint32_t> ids;
		for (VisualScriptNode const& node : copy.Graph.Nodes)
		{
			ids.push_back(node.ID);
		}
		CHECK(ids == std::vector<uint32_t>{22, 23, 24, 25, 26});
		CHECK(copy.Graph.Nodes[3].Settings == Json({{"Function", 20}}));
		CHECK(copy.Graph.Nodes[4].Settings == Json({{"Variable", 21}}));
		CHECK(copy.Graph.Links == std::vector<VisualScriptLink>{MakeLink(22, "Then", 23, "Execute"), MakeLink(22, "Points", 24, "A"),
		                                                        MakeLink(22, "Bonus", 24, "B"), MakeLink(24, "Result", 23, "Total")});
		CHECK(copy.Graph.Comments[0].ID == 27);
		CHECK(document.Functions[0].Graph == original);
		CheckDocument(document);
	}
}

TEST_CASE("VisualScriptEditing: pins are promoted to variables and to function inputs, named after the pin")
{
	Scope<VisualScriptLibrary> const library = ParseLibrary();
	VisualScriptDocument document;
	VisualScriptVariable taken = MakeVariable(1, "ForceX", "float");
	taken.FormerNames = {"Mode"};
	document.Variables.push_back(taken);
	VisualScriptGraph& graph = document.Graph;
	graph.Nodes = {MakeNode(70, "Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,Strada.ForceMode)", glm::vec2(400.0f, 0.0f),
	                        Json::object(), {{"Force", {1, 0, 0}}, {"Mode", "Impulse"}}),
	               MakeNode(71, "Call:Strada.Physics.Raycast(Strada.Vector3,Strada.Vector3,float,out Strada.RaycastHit,uint)",
	                        glm::vec2(0.0f, 200.0f)),
	               MakeNode(72, "Call:Strada.Log.Info(string)", glm::vec2(400.0f, 200.0f)),
	               MakeNode(73, "Get:Strada.Time.DeltaTime", glm::vec2(0.0f, 400.0f)),
	               MakeNode(74, "Reroute", glm::vec2(0.0f, 600.0f)),
	               MakeNode(75, "Flow.Branch", std::nullopt),
	               MakeNode(83, "Function.Call", glm::vec2(0.0f, 800.0f), {{"Function", 80}})};
	graph.Links = {MakeLink(71, "Then", 72, "Execute")};
	VisualScriptFunction apply = MakeFunction(80, "Apply");
	apply.Graph.Nodes = {MakeNode(81, "Function.Entry"),
	                     MakeNode(82, "Call:Strada.Log.Info(string)", glm::vec2(300.0f, 0.0f), Json::object(), {{"Message", "hi"}}),
	                     MakeNode(85, "Set:Strada.TransformComponent.Translation", glm::vec2(300.0f, 200.0f))};
	VisualScriptFunction other = MakeFunction(90, "Other");
	other.Graph.Nodes = {MakeNode(91, "Function.Entry"), MakeNode(84, "Function.Call", glm::vec2(300.0f, 0.0f), {{"Function", 80}})};
	document.Functions = {apply, other};
	VisualScriptNodePins const pins = MakePins(*library, document);

	SUBCASE("an input gets a Get, its literal becoming the variable's value")
	{
		CHECK(RequireOk(VisualScriptEditing::PromoteToVariable(document, 0, {70, "Force"}, In, pins)) == 92);
		CHECK(document.Variables.back() == MakeVariable(92, "Force", "Strada.Vector3", Json::array({1, 0, 0})));
		VisualScriptNode const& get = GetNode(graph, 93);
		CHECK(get.Type == "Variable.Get");
		CHECK(get.Settings == Json({{"Variable", 92}}));
		CHECK(get.Position == glm::vec2(160.0f, 72.0f));
		CHECK(HasLink(graph, MakeLink(93, "Value", 70, "Force")));
		CHECK(graph.Nodes[0].Inputs == Json({{"Mode", "Impulse"}}));
		// A former name is taken: the next free one.
		CHECK(RequireOk(VisualScriptEditing::PromoteToVariable(document, 0, {70, "Mode"}, In, pins)) == 94);
		CHECK(document.Variables.back().Name == "Mode1");
		CHECK(document.Variables.back().Value == "Impulse");
		CheckDocument(document);
	}

	SUBCASE("a field of a split input is named without its dot")
	{
		RequireOk(VisualScriptEditing::SplitPin(document, 70, In, "Force", pins, *library));
		CHECK(RequireOk(VisualScriptEditing::PromoteToVariable(document, 0, {70, "Force.X"}, In, pins)) == 92);
		CHECK(document.Variables.back() == MakeVariable(92, "ForceX1", "float", 1));
		CHECK(HasLink(graph, MakeLink(93, "Value", 70, "Force.X")));
		CheckDocument(document);
	}

	SUBCASE("an output gets a Set after the node's first exec output, or a Set it only feeds")
	{
		CHECK(RequireOk(VisualScriptEditing::PromoteToVariable(document, 0, {71, "Result"}, Out, pins)) == 92);
		CHECK(document.Variables.back() == MakeVariable(92, "Result", "bool"));
		VisualScriptNode const& set = GetNode(graph, 93);
		CHECK(set.Type == "Variable.Set");
		CHECK(set.Position == glm::vec2(280.0f, 272.0f));
		CHECK(graph.Links == std::vector<VisualScriptLink>{MakeLink(71, "Then", 93, "Execute"), MakeLink(71, "Result", 93, "Value"),
		                                                   MakeLink(93, "Then", 72, "Execute")});
		CHECK(RequireOk(VisualScriptEditing::PromoteToVariable(document, 0, {73, "Value"}, Out, pins)) == 94);
		CHECK(document.Variables.back().Name == "Value");
		REQUIRE(graph.Links.size() == 4);
		CHECK(graph.Links.back() == MakeLink(73, "Value", 95, "Value"));
		CheckDocument(document);
	}

	SUBCASE("pins that hold no values, or whose type is not known, are not promoted")
	{
		auto const promote = [&pins](uint32_t node, std::string pin, VisualScriptPinDirection direction)
		{
			return [&pins, node, pin, direction](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::PromoteToVariable(edited, 0, {node, pin}, direction, pins);
			};
		};
		CheckRefused(document, promote(70, "Execute", In), "Execute is a flow pin: variables hold values");
		CheckRefused(document, promote(74, "Out", Out), "the type of Out is not known yet");
		CheckRefused(document, promote(70, "Nope", In), "node 70 has no input Nope");
		CheckRefused(document, promote(75, "Condition", In), "node 75 has no position yet");
		CheckRefused(document, promote(99, "Value", Out), "the graph has no node 99");
	}

	SUBCASE("an input of a function's node becomes the function's, its literal going to the calls")
	{
		RequireOk(VisualScriptEditing::PromoteToFunctionInput(document, 80, {82, "Message"}, pins));
		VisualScriptFunction const& function = document.Functions[0];
		CHECK(function.Inputs == std::vector<VisualScriptParameter>{{"Message", "string"}});
		CHECK(HasLink(function.Graph, MakeLink(81, "Message", 82, "Message")));
		CHECK(function.Graph.Nodes[1].Inputs == Json::object());
		CHECK(GetNode(graph, 83).Inputs == Json({{"Message", "hi"}}));
		CHECK(document.Functions[1].Graph.Nodes[1].Inputs == Json({{"Message", "hi"}}));
		// A reserved pin name is no parameter's.
		RequireOk(VisualScriptEditing::PromoteToFunctionInput(document, 80, {85, "Target"}, pins));
		REQUIRE(function.Inputs.size() == 2);
		CHECK(function.Inputs.back() == VisualScriptParameter{"Target1", "Strada.Entity"});
		CHECK(HasLink(function.Graph, MakeLink(81, "Target1", 85, "Target")));
		CHECK(GetNode(graph, 83).Inputs == Json({{"Message", "hi"}}));
		CheckDocument(document);

		auto const promote = [&pins](uint32_t owner, uint32_t node, std::string pin)
		{
			return [&pins, owner, node, pin](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::PromoteToFunctionInput(edited, owner, {node, pin}, pins);
			};
		};
		CheckRefused(document, promote(0, 72, "Message"), "only an input of a node in a function's graph");
		CheckRefused(document, promote(80, 82, "Message"), "Message is linked");
		CheckRefused(document, promote(80, 82, "Execute"), "Execute is a flow pin: function inputs are values");
		CheckRefused(document, promote(77, 82, "Message"), "the document has no function 77");
	}
}

TEST_CASE("VisualScriptEditing: elements align, distribute and straighten to whole canvas units")
{
	Scope<VisualScriptLibrary> const library = ParseLibrary();
	VisualScriptDocument document;
	VisualScriptGraph& graph = document.Graph;
	graph.Nodes = {MakeNode(1, "Flow.Branch", glm::vec2(10.0f, 0.0f)), MakeNode(2, "Flow.Branch", glm::vec2(100.0f, 50.0f)),
	               MakeNode(3, "Flow.Branch", glm::vec2(40.0f, 200.0f)), MakeNode(5, "Flow.Branch", std::nullopt)};
	VisualScriptComment comment;
	comment.ID = 4;
	comment.Position = glm::vec2(0.0f, 300.0f);
	comment.Size = glm::vec2(300.0f, 100.0f);
	graph.Comments.push_back(comment);
	std::unordered_map<uint32_t, glm::vec2> const sizes = {
		{1, glm::vec2(100.0f, 40.0f)}, {2, glm::vec2(51.0f, 60.0f)}, {3, glm::vec2(200.0f, 20.0f)}};
	VisualScriptNodeExtents const extents = [&sizes](uint32_t node)
	{
		return sizes.at(node);
	};
	std::vector<uint32_t> const all = {1, 2, 3, 4};
	auto const positions = [&graph]()
	{
		return std::vector<glm::vec2>{*graph.Nodes[0].Position, *graph.Nodes[1].Position, *graph.Nodes[2].Position,
		                              graph.Comments[0].Position};
	};

	SUBCASE("aligning")
	{
		struct Expected
		{
			VisualScriptAlignment Alignment;
			std::vector<glm::vec2> Positions;
		};
		std::vector<Expected> const expected = {
			{VisualScriptAlignment::Left, {{0.0f, 0.0f}, {0.0f, 50.0f}, {0.0f, 200.0f}, {0.0f, 300.0f}}},
			{VisualScriptAlignment::Right, {{200.0f, 0.0f}, {249.0f, 50.0f}, {100.0f, 200.0f}, {0.0f, 300.0f}}},
			{VisualScriptAlignment::Top, {{10.0f, 0.0f}, {100.0f, 0.0f}, {40.0f, 0.0f}, {0.0f, 0.0f}}},
			{VisualScriptAlignment::Bottom, {{10.0f, 360.0f}, {100.0f, 340.0f}, {40.0f, 380.0f}, {0.0f, 300.0f}}},
			{VisualScriptAlignment::Centers, {{100.0f, 0.0f}, {125.0f, 50.0f}, {50.0f, 200.0f}, {0.0f, 300.0f}}},
			{VisualScriptAlignment::Middles, {{10.0f, 180.0f}, {100.0f, 170.0f}, {40.0f, 190.0f}, {0.0f, 150.0f}}}};
		VisualScriptDocument const original = document;
		for (Expected const& entry : expected)
		{
			CAPTURE(static_cast<int>(entry.Alignment));
			document = original;
			RequireOk(VisualScriptEditing::AlignElements(document, 0, all, entry.Alignment, extents));
			CHECK(positions() == entry.Positions);
			CheckDocument(document);
		}
	}

	SUBCASE("distributing")
	{
		graph.Nodes[0].Position = glm::vec2(0.0f, 0.0f);
		graph.Nodes[1].Position = glm::vec2(500.0f, 50.0f);
		graph.Nodes[2].Position = glm::vec2(150.0f, 200.0f);
		std::vector<uint32_t> const nodes = {1, 2, 3};
		RequireOk(VisualScriptEditing::DistributeElements(document, 0, nodes, VisualScriptDistribution::Horizontal, extents));
		CHECK(*graph.Nodes[2].Position == glm::vec2(200.0f, 200.0f));
		CHECK(*graph.Nodes[0].Position == glm::vec2(0.0f, 0.0f));
		CHECK(*graph.Nodes[1].Position == glm::vec2(500.0f, 50.0f));
		RequireOk(VisualScriptEditing::DistributeElements(document, 0, nodes, VisualScriptDistribution::Vertical, extents));
		CHECK(*graph.Nodes[1].Position == glm::vec2(500.0f, 90.0f));
		CheckDocument(document);
	}

	SUBCASE("what cannot be aligned or distributed is refused")
	{
		auto const align = [&extents](std::vector<uint32_t> elements, VisualScriptAlignment alignment, bool measured)
		{
			return [&extents, elements, alignment, measured](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::AlignElements(edited, 0, elements, alignment, measured ? extents : VisualScriptNodeExtents());
			};
		};
		CheckRefused(document, align({1, 1}, VisualScriptAlignment::Left, true), "aligning takes two elements or more");
		CheckRefused(document, align({1, 5}, VisualScriptAlignment::Left, true), "node 5 has no position yet");
		CheckRefused(document, align({1, 2}, VisualScriptAlignment::Left, false), "the size of node 1 is not known");
		CheckRefused(document, align({1, 99}, VisualScriptAlignment::Left, true), "the graph has no node or comment 99");
		CheckRefused(document, align({1, 2}, static_cast<VisualScriptAlignment>(9), true), "9 is no alignment");
		CheckRefused(
			document,
			[&extents](VisualScriptDocument& edited)
			{
				std::vector<uint32_t> const elements = {1, 2};
				return VisualScriptEditing::DistributeElements(edited, 0, elements, VisualScriptDistribution::Horizontal, extents);
			},
			"distributing takes three elements or more");
		CheckRefused(
			document,
			[&extents, &all](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::DistributeElements(edited, 0, all, static_cast<VisualScriptDistribution>(9), extents);
			},
			"9 is no distribution");
	}

	SUBCASE("straightening follows the exec links, each input level with its output")
	{
		VisualScriptDocument flow;
		flow.Graph.Nodes = {MakeNode(1, "Event.Update"),
		                    MakeNode(2, "Flow.Branch", glm::vec2(300.0f, 37.0f)),
		                    MakeNode(3, "Call:Strada.Log.Info(string)", glm::vec2(600.0f, -20.0f)),
		                    MakeNode(4, "Call:Strada.Log.Info(string)", glm::vec2(900.0f, 100.0f)),
		                    MakeNode(5, "Call:Strada.Log.Info(string)", glm::vec2(0.0f, 10.0f)),
		                    MakeNode(6, "Call:Strada.Log.Info(string)", glm::vec2(300.0f, 50.0f)),
		                    MakeNode(7, "Flow.Branch", std::nullopt)};
		flow.Graph.Links = {MakeLink(3, "Then", 4, "Execute"), MakeLink(2, "False", 3, "Execute"), MakeLink(1, "Then", 2, "Execute"),
		                    MakeLink(5, "Then", 6, "Execute"), MakeLink(6, "Then", 5, "Execute")};
		VisualScriptNodePins const pins = MakePins(*library, flow);
		std::vector<uint32_t> const chain = {4, 3, 2, 1};
		CHECK(RequireOk(VisualScriptEditing::StraightenExec(flow, 0, chain, pins)) == 3);
		CHECK(flow.Graph.Nodes[1].Position == glm::vec2(300.0f, 0.0f));
		CHECK(flow.Graph.Nodes[2].Position == glm::vec2(600.0f, 24.0f));
		CHECK(flow.Graph.Nodes[3].Position == glm::vec2(900.0f, 24.0f));
		CHECK(RequireOk(VisualScriptEditing::StraightenExec(flow, 0, chain, pins)) == 0);
		// In a cycle the first node in the graph's order stays.
		std::vector<uint32_t> const cycle = {6, 5};
		CHECK(RequireOk(VisualScriptEditing::StraightenExec(flow, 0, cycle, pins)) == 1);
		CHECK(flow.Graph.Nodes[4].Position == glm::vec2(0.0f, 10.0f));
		CHECK(flow.Graph.Nodes[5].Position == glm::vec2(300.0f, 10.0f));
		CheckDocument(flow);

		auto const straighten = [&pins](std::vector<uint32_t> nodes)
		{
			return [&pins, nodes](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::StraightenExec(edited, 0, nodes, pins);
			};
		};
		CheckRefused(flow, straighten({1, 1}), "straightening takes two nodes or more");
		CheckRefused(flow, straighten({1, 7}), "node 7 has no position yet");
		CheckRefused(flow, straighten({1, 9}), "the graph has no node 9");
	}
}
