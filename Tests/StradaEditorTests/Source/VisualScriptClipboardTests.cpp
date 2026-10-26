#include "Fuzzing.h"
#include "VisualScriptTestDocuments.h"

#include "Editor/VisualScripting/VisualScriptClipboard.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptLayout.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Script/VisualScriptLibrary.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	constexpr std::string_view Refusal = "the clipboard holds no visual script: ";
	AssetHandle const Scores(UUID(8146390251172604461ull));
	AssetHandle const Other(UUID(4242424242ull));

	VisualScriptDocument ParseDocument(std::string_view text)
	{
		Result<Json> json = ParseJson(text);
		REQUIRE_MESSAGE(json.IsOk(), json.GetError());
		DeserializationContext context;
		context.UnknownFields = UnknownFieldPolicy::Warn;
		std::vector<VisualScriptIssue> issues;
		Result<VisualScriptDocument> document = VisualScriptSerializer::DeserializeDocument(json.GetValue(), context, &issues);
		REQUIRE_MESSAGE(document.IsOk(), document.GetError());
		REQUIRE_MESSAGE(issues.empty(), (issues.empty() ? std::string() : issues.front().Path + ": " + issues.front().Message));
		return document.TakeValue();
	}

	VisualScriptDocument LoadTestDocument(std::string_view name)
	{
		Result<std::string> text = FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" /
		                                                    "Documents" / FileSystem::PathFromUtf8(name));
		REQUIRE_MESSAGE(text.IsOk(), text.GetError());
		return ParseDocument(text.GetValue());
	}

	// The editor's node library, with a node kind of its own whose settings name a variable and a function: the settings
	// a copy follows are those of these kinds, not only the built-in ones.
	Scope<VisualScriptLibrary> ParseLibrary(bool withWatch = false)
	{
		Result<std::string> text =
			FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Editor" / "Library.json");
		REQUIRE_MESSAGE(text.IsOk(), text.GetError());
		std::string json = text.TakeValue();
		if (withWatch)
		{
			Result<Json> library = ParseJson(json);
			REQUIRE(library.IsOk());
			Json watch = {{"Type", "Custom.Watch"},
			              {"Title", "Watch"},
			              {"Category", "Testing"},
			              {"Description", ""},
			              {"Keywords", Json::array()},
			              {"Kind", "Call"},
			              {"Pure", false},
			              {"Latent", false},
			              {"Pins", Json::array()},
			              {"Settings", Json::array({{{"Name", "Watched"}, {"Kind", "Variable"}, {"Required", true}},
			                                        {{"Name", "Callback"}, {"Kind", "Function"}, {"Required", false}}})}};
			library.GetValue()["Nodes"].push_back(std::move(watch));
			json = library.GetValue().dump();
		}
		Result<Scope<VisualScriptLibrary>> library = VisualScriptLibrary::Parse(json);
		REQUIRE_MESSAGE(library.IsOk(), library.GetError());
		return library.TakeValue();
	}

	// A script with a public variable that was renamed, an impure function with a local that calls a pure one, which reads
	// another variable, and an event graph with an unplaced node, an input event and a comment box.
	constexpr std::string_view ScoresText = R"json({
		"Description": "Scores",
		"Variables": [
			{ "ID": 1, "Name": "Score", "Type": "int", "Value": 3, "Public": true, "FormerNames": [ "Points" ] },
			{ "ID": 2, "Name": "Label", "Type": "string", "Value": "Total" }
		],
		"Functions": [
			{
				"ID": 10, "Name": "Add", "Inputs": [ { "Name": "Amount", "Type": "int" } ],
				"Locals": [ { "ID": 11, "Name": "Acc", "Type": "int", "Value": 0 } ],
				"Graph": {
					"Nodes": [
						{ "ID": 12, "Type": "Function.Entry", "Position": [ 0.0, 0.0 ] },
						{ "ID": 13, "Type": "Variable.Set", "Position": [ 320.0, 0.0 ], "Settings": { "Variable": 11 } },
						{ "ID": 14, "Type": "Function.Call", "Position": [ 640.0, 0.0 ], "Settings": { "Function": 20 } },
						{ "ID": 15, "Type": "Variable.Get", "Position": [ 320.0, 160.0 ], "Settings": { "Variable": 1 } }
					],
					"Links": [
						{ "From": [ 12, "Then" ], "To": [ 13, "Execute" ] },
						{ "From": [ 13, "Then" ], "To": [ 14, "Execute" ] },
						{ "From": [ 12, "Amount" ], "To": [ 13, "Value" ] }
					],
					"Comments": [ { "ID": 16, "Text": "Adds", "Position": [ -40.0, -80.0 ] } ]
				}
			},
			{
				"ID": 20, "Name": "Helper", "Pure": true, "Outputs": [ { "Name": "Result", "Type": "string" } ],
				"Graph": {
					"Nodes": [
						{ "ID": 21, "Type": "Function.Entry", "Position": [ 0.0, 0.0 ] },
						{ "ID": 22, "Type": "Function.Return", "Position": [ 640.0, 0.0 ] },
						{ "ID": 23, "Type": "Variable.Get", "Position": [ 320.0, 0.0 ], "Settings": { "Variable": 2 } }
					],
					"Links": [ { "From": [ 23, "Value" ], "To": [ 22, "Result" ] } ]
				}
			}
		],
		"Graph": {
			"Nodes": [
				{ "ID": 30, "Type": "Event.Create", "Position": [ 0.0, 0.0 ] },
				{ "ID": 31, "Type": "Function.Call", "Position": [ 320.0, 0.0 ], "Settings": { "Function": 10 } },
				{ "ID": 32, "Type": "Variable.Get", "Settings": { "Variable": 1 } },
				{ "ID": 33, "Type": "Event.KeyPressed", "Position": [ 0.0, 300.0 ], "Settings": { "Key": "Space" } },
				{ "ID": 34, "Type": "Call:Strada.Log.Info(string)", "Position": [ 320.0, 300.0 ], "Inputs": { "Message": "pressed" } }
			],
			"Links": [
				{ "From": [ 30, "Then" ], "To": [ 31, "Execute" ] },
				{ "From": [ 32, "Value" ], "To": [ 31, "Amount" ] },
				{ "From": [ 33, "Then" ], "To": [ 34, "Execute" ] }
			],
			"Comments": [ { "ID": 35, "Text": "Start", "Position": [ -40.0, -60.0 ], "Size": [ 800.0, 200.0 ] } ]
		}
	})json";

	VisualScriptClipboardContent ReadBack(std::string const& text)
	{
		Result<VisualScriptClipboardContent> content = ReadVisualScriptClipboard(text);
		REQUIRE_MESSAGE(content.IsOk(), content.GetError());
		return content.TakeValue();
	}

	VisualScriptClipboardContent Copy(VisualScriptDocument const& document, uint32_t function, std::vector<uint32_t> const& elements,
	                                  AssetHandle source = Scores, VisualScriptLibrary const* library = nullptr)
	{
		std::string const text = WriteVisualScriptClipboard(document, function, elements, source, library);
		CHECK(MayHoldVisualScriptClipboard(text));
		return ReadBack(text);
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

	VisualScriptVariable const& GetVariable(VisualScriptDocument const& document, std::string_view name)
	{
		auto const found = std::find_if(document.Variables.begin(), document.Variables.end(),
		                                [name](VisualScriptVariable const& variable)
		                                {
											return variable.Name == name;
										});
		REQUIRE(found != document.Variables.end());
		return *found;
	}

	VisualScriptFunction const& GetFunction(VisualScriptDocument const& document, std::string_view name)
	{
		auto const found = std::find_if(document.Functions.begin(), document.Functions.end(),
		                                [name](VisualScriptFunction const& function)
		                                {
											return function.Name == name;
										});
		REQUIRE(found != document.Functions.end());
		return *found;
	}

	VisualScriptFunction const& GetFunction(VisualScriptDocument const& document, uint32_t id)
	{
		auto const found = std::find_if(document.Functions.begin(), document.Functions.end(),
		                                [id](VisualScriptFunction const& function)
		                                {
											return function.ID == id;
										});
		REQUIRE(found != document.Functions.end());
		return *found;
	}

	int64_t GetSetting(VisualScriptNode const& node, char const* name)
	{
		REQUIRE(node.Settings.contains(name));
		REQUIRE(node.Settings[name].is_number_integer());
		return node.Settings[name].get<int64_t>();
	}

	// Every ID of the document is unique.
	void CheckUniqueIDs(VisualScriptDocument const& document)
	{
		std::set<uint32_t> ids;
		size_t count = 0;
		auto const takeGraph = [&](VisualScriptGraph const& graph)
		{
			for (VisualScriptNode const& node : graph.Nodes)
			{
				ids.insert(node.ID);
				count++;
			}
			for (VisualScriptComment const& comment : graph.Comments)
			{
				ids.insert(comment.ID);
				count++;
			}
		};
		for (VisualScriptVariable const& variable : document.Variables)
		{
			ids.insert(variable.ID);
			count++;
		}
		for (VisualScriptFunction const& function : document.Functions)
		{
			ids.insert(function.ID);
			count++;
			for (VisualScriptLocal const& local : function.Locals)
			{
				ids.insert(local.ID);
				count++;
			}
			takeGraph(function.Graph);
		}
		takeGraph(document.Graph);
		CHECK(ids.size() == count);
	}
}

TEST_CASE("VisualScriptClipboard: a copy is versioned text that reads back with what the copied nodes use")
{
	VisualScriptDocument const scores = ParseDocument(ScoresText);
	std::vector<uint32_t> const elements = {31, 32, 30, 35, 99};
	std::string const text = WriteVisualScriptClipboard(scores, 0, elements, Scores, nullptr);
	CHECK(text.starts_with("{\n\t\"Strada\": {\n\t\t\"Version\": 1,\n\t\t\"Type\": \"VisualScriptClipboard\"\n\t},"));
	CHECK(text.ends_with("}\n"));
	CHECK(MayHoldVisualScriptClipboard(text));
	CHECK(MayHoldVisualScriptClipboard(" \r\n\t" + text));

	VisualScriptClipboardContent const content = ReadBack(text);
	CHECK(content.Source == Scores);
	CHECK(content.Function == 0);
	CHECK(content.Locals.empty());
	VisualScriptDocument const& copied = content.Document;
	CHECK(copied.Description.empty());
	// Score without its former name; Label because Helper, which Add calls, reads it.
	REQUIRE(copied.Variables.size() == 2);
	CHECK(copied.Variables[0].Name == "Score");
	CHECK(copied.Variables[0].ID == 1);
	CHECK(copied.Variables[0].Value == 3);
	CHECK(copied.Variables[0].Public);
	CHECK(copied.Variables[0].FormerNames.empty());
	CHECK(copied.Variables[1].Name == "Label");
	REQUIRE(copied.Functions.size() == 2);
	CHECK(copied.Functions[0] == scores.Functions[0]);
	CHECK(copied.Functions[1] == scores.Functions[1]);

	// Renumbered above the script's largest ID (35), in graph order, relative to the corner of the copied bounds (the
	// comment's (-40, -60)); the unplaced node where placing it puts it (below the placed nodes: (0, 540)).
	REQUIRE(copied.Graph.Nodes.size() == 3);
	CHECK(copied.Graph.Nodes[0].ID == 36);
	CHECK(copied.Graph.Nodes[0].Type == "Event.Create");
	CHECK(copied.Graph.Nodes[0].Position == glm::vec2(40.0f, 60.0f));
	CHECK(copied.Graph.Nodes[1].ID == 37);
	CHECK(copied.Graph.Nodes[1].Position == glm::vec2(360.0f, 60.0f));
	CHECK(GetSetting(copied.Graph.Nodes[1], "Function") == 10);
	CHECK(copied.Graph.Nodes[2].ID == 38);
	CHECK(copied.Graph.Nodes[2].Position == glm::vec2(40.0f, 600.0f));
	REQUIRE(copied.Graph.Comments.size() == 1);
	CHECK(copied.Graph.Comments[0].ID == 39);
	CHECK(copied.Graph.Comments[0].Position == glm::vec2(0.0f, 0.0f));
	CHECK(copied.Graph.Comments[0].Size == glm::vec2(800.0f, 200.0f));
	std::vector<VisualScriptLink> const links = {{{36, "Then"}, {37, "Execute"}}, {{38, "Value"}, {37, "Amount"}}};
	CHECK(copied.Graph.Links == links);
	Testing::ExpectStrictRoundTrip(copied);

	SUBCASE("a copy of nothing holds an empty document")
	{
		VisualScriptClipboardContent const empty = Copy(scores, 0, {});
		CHECK(empty.Document == VisualScriptDocument());
		VisualScriptClipboardContent const unknown = Copy(scores, 77, {12, 13});
		CHECK(unknown.Function == 77);
		CHECK(unknown.Document == VisualScriptDocument());
	}
	SUBCASE("without a source, the handle is 0")
	{
		CHECK(WriteVisualScriptClipboard(scores, 0, elements, AssetHandle(), nullptr).find("\"Graph\": \"0\"") != std::string::npos);
		CHECK_FALSE(Copy(scores, 0, elements, AssetHandle()).Source.IsValid());
	}
	SUBCASE("the settings a copy follows are those the library says name variables and functions")
	{
		Scope<VisualScriptLibrary> const library = ParseLibrary(true);
		VisualScriptDocument watched = scores;
		VisualScriptNode watch;
		watch.ID = 40;
		watch.Type = "Custom.Watch";
		watch.Position = glm::vec2(0.0f, 0.0f);
		watch.Settings = {{"Watched", 2}, {"Callback", 20}};
		watched.Graph.Nodes.push_back(watch);
		std::vector<uint32_t> const only = {40};
		VisualScriptClipboardContent const withLibrary = Copy(watched, 0, only, Scores, library.get());
		REQUIRE(withLibrary.Document.Variables.size() == 1);
		CHECK(withLibrary.Document.Variables[0].Name == "Label");
		REQUIRE(withLibrary.Document.Functions.size() == 1);
		CHECK(withLibrary.Document.Functions[0].Name == "Helper");
		VisualScriptClipboardContent const withoutLibrary = Copy(watched, 0, only);
		CHECK(withoutLibrary.Document.Variables.empty());
		CHECK(withoutLibrary.Document.Functions.empty());
	}
}

TEST_CASE("VisualScriptClipboard: only text that starts like a clipboard may hold one")
{
	CHECK_FALSE(MayHoldVisualScriptClipboard(""));
	CHECK_FALSE(MayHoldVisualScriptClipboard("   "));
	CHECK_FALSE(MayHoldVisualScriptClipboard("VisualScriptClipboard"));
	CHECK_FALSE(MayHoldVisualScriptClipboard("[\"VisualScriptClipboard\"]"));
	CHECK_FALSE(MayHoldVisualScriptClipboard("{ \"Strada\": { \"Type\": \"Material\" } }"));
	CHECK_FALSE(MayHoldVisualScriptClipboard("{" + std::string(300, ' ') + "\"VisualScriptClipboard\"}"));
	CHECK(MayHoldVisualScriptClipboard("{\"VisualScriptClipboard\""));
}

TEST_CASE("VisualScriptClipboard: texts that are not a clipboard of version 1 are refused")
{
	VisualScriptDocument const scores = ParseDocument(ScoresText);
	std::vector<uint32_t> const elements = {31, 32};
	Result<Json> parsed = ParseJson(WriteVisualScriptClipboard(scores, 0, elements, Scores, nullptr));
	REQUIRE(parsed.IsOk());
	Json const valid = parsed.TakeValue();

	auto const refused = [](std::string const& text, std::string_view reason)
	{
		Result<VisualScriptClipboardContent> const content = ReadVisualScriptClipboard(text);
		REQUIRE(content.IsError());
		CHECK(content.GetError().starts_with(Refusal));
		CHECK_MESSAGE(content.GetError().find(reason) != std::string::npos, content.GetError());
	};
	auto const changed = [&valid](std::function<void(Json&)> const& change)
	{
		Json json = valid;
		change(json);
		return DumpJson(json);
	};

	std::string longText = DumpJson(valid);
	longText.resize(MaxVisualScriptClipboardBytes + 1, ' ');
	refused(longText, "longer than 16777216 bytes");
	refused("{ \"Strada\": ", "invalid JSON");
	refused("[1, 2]", "document must be a JSON object");
	refused("{}", "missing \"Strada\" file header");
	refused(changed(
				[](Json& json)
				{
					json["Strada"]["Type"] = "VisualScript";
				}),
	        "expected a VisualScriptClipboard file");
	refused(changed(
				[](Json& json)
				{
					json["Strada"]["Version"] = 2;
				}),
	        "newer than the supported version 1");
	refused(changed(
				[](Json& json)
				{
					json["Comment"] = "hi";
				}),
	        "the clipboard has the unknown key \"Comment\"");
	refused(changed(
				[](Json& json)
				{
					json["Strada"]["Comment"] = "hi";
				}),
	        "its header has the unknown key \"Comment\"");
	refused(changed(
				[](Json& json)
				{
					json.erase("VisualScriptClipboard");
				}),
	        "\"VisualScriptClipboard\" is missing");
	refused(changed(
				[](Json& json)
				{
					json["VisualScriptClipboard"]["Extra"] = 1;
				}),
	        "unknown key \"Extra\"");
	refused(changed(
				[](Json& json)
				{
					json["VisualScriptClipboard"].erase("Source");
				}),
	        "\"Source\" is missing");
	refused(changed(
				[](Json& json)
				{
					json["VisualScriptClipboard"]["Document"] = Json::array();
				}),
	        "\"Document\" is missing or not an object");
	refused(changed(
				[](Json& json)
				{
					json["VisualScriptClipboard"]["Source"]["Scene"] = "1";
				}),
	        "\"Source\" has the unknown key \"Scene\"");
	for (Json const& graph : {Json("abc"), Json("-1"), Json(""), Json(5), Json("18446744073709551616")})
	{
		CAPTURE(graph.dump());
		refused(changed(
					[&graph](Json& json)
					{
						json["VisualScriptClipboard"]["Source"]["Graph"] = graph;
					}),
		        "\"Source.Graph\" is not");
	}
	for (Json const& function : {Json(-1), Json(1.5), Json(2147483648ull), Json("3"), Json()})
	{
		CAPTURE(function.dump());
		refused(changed(
					[&function](Json& json)
					{
						json["VisualScriptClipboard"]["Source"]["Function"] = function;
					}),
		        "\"Source.Function\" is not 0 or an ID");
	}
	refused(changed(
				[](Json& json)
				{
					json["VisualScriptClipboard"]["Source"]["Locals"] = Json::object();
				}),
	        "\"Source.Locals\" is not an array");
	refused(changed(
				[](Json& json)
				{
					json["VisualScriptClipboard"]["Source"]["Locals"] = Json::array({1});
				}),
	        "is not an object");
	refused(changed(
				[](Json& json)
				{
					json["VisualScriptClipboard"]["Source"]["Locals"] = Json::array({{{"ID", 1}, {"Name", "Acc"}, {"Type", "int"}}});
				}),
	        "has the unknown key \"Type\"");
	refused(changed(
				[](Json& json)
				{
					json["VisualScriptClipboard"]["Source"]["Locals"] = Json::array({{{"ID", 77}, {"Name", "Acc"}}});
				}),
	        "names no variable of the document");
	refused(changed(
				[](Json& json)
				{
					json["VisualScriptClipboard"]["Source"]["Locals"] = Json::array({{{"ID", 1}, {"Name", "A.cc"}}});
				}),
	        "the local 1 has no name a local can have");
	refused(changed(
				[](Json& json)
				{
					json["VisualScriptClipboard"]["Source"]["Locals"] =
						Json::array({{{"ID", 1}, {"Name", "A"}}, {{"ID", 1}, {"Name", "B"}}});
				}),
	        "lists the variable 1 twice");
	// The document is read strictly: what the format's reader would repair is refused.
	refused(changed(
				[](Json& json)
				{
					json["VisualScriptClipboard"]["Document"]["Graph"]["Nodes"][0]["Colour"] = "red";
				}),
	        "Document.Graph.Nodes[0]");
	refused(changed(
				[](Json& json)
				{
					json["VisualScriptClipboard"]["Document"]["Graph"]["Nodes"][1]["ID"] = 1;
				}),
	        "Document.Graph.Nodes[1]");
	refused(changed(
				[](Json& json)
				{
					json["VisualScriptClipboard"]["Document"]["Graph"]["Links"].push_back(
						json["VisualScriptClipboard"]["Document"]["Graph"]["Links"][0]);
				}),
	        "Document.Graph.Links[1]");

	// Locals are optional; what is valid reads.
	Result<VisualScriptClipboardContent> const withoutLocals = ReadVisualScriptClipboard(changed(
		[](Json& json)
		{
			json["VisualScriptClipboard"]["Source"].erase("Locals");
		}));
	CHECK(withoutLocals.IsOk());
}

TEST_CASE("VisualScriptClipboard: a paste lands at the point, takes new IDs and binds to the script copied from")
{
	VisualScriptDocument const scores = ParseDocument(ScoresText);
	VisualScriptClipboardContent const content = Copy(scores, 0, {31, 32, 35});

	VisualScriptDocument document = scores;
	Result<VisualScriptPasteResult> pasted = PasteVisualScriptClipboard(document, Scores, 0, content, glm::vec2(1000.0f, 2000.0f), nullptr);
	REQUIRE_MESSAGE(pasted.IsOk(), pasted.GetError());
	CHECK(pasted.GetValue().Notes.empty());
	CHECK(pasted.GetValue().NodeCount == 2);
	std::vector<uint32_t> const ids = {36, 37, 38};
	CHECK(pasted.GetValue().Pasted == ids);
	// The comment at (-40, -60) was the corner: it lands at the point, the nodes as they stood to it.
	CHECK(document.Graph.Comments.back().Position == glm::vec2(1000.0f, 2000.0f));
	CHECK(GetNode(document.Graph, 36).Position == glm::vec2(1360.0f, 2060.0f));
	CHECK(GetNode(document.Graph, 37).Position == glm::vec2(1040.0f, 2600.0f));
	CHECK(GetSetting(GetNode(document.Graph, 36), "Function") == 10);
	CHECK(GetSetting(GetNode(document.Graph, 37), "Variable") == 1);
	CHECK(document.Graph.Links.back() == VisualScriptLink{{37, "Value"}, {36, "Amount"}});
	CHECK(document.Variables == scores.Variables);
	CHECK(document.Functions == scores.Functions);
	Testing::ExpectStrictRoundTrip(document);
	CheckUniqueIDs(document);

	SUBCASE("a variable renamed since binds by name and type to another")
	{
		VisualScriptDocument renamed = scores;
		renamed.Variables[0].Name = "Total";
		VisualScriptVariable score;
		score.ID = 50;
		score.Name = "Score";
		score.Type = "int";
		renamed.Variables.push_back(score);
		REQUIRE(PasteVisualScriptClipboard(renamed, Scores, 0, content, glm::vec2(0.0f), nullptr).IsOk());
		CHECK(GetSetting(GetNode(renamed.Graph, 52), "Variable") == 50);
		Testing::ExpectStrictRoundTrip(renamed);
	}
	SUBCASE("the same variable binds by name and type from another script")
	{
		VisualScriptDocument other = scores;
		REQUIRE(PasteVisualScriptClipboard(other, Other, 0, content, glm::vec2(0.0f), nullptr).IsOk());
		CHECK(other.Variables == scores.Variables);
		CHECK(other.Functions == scores.Functions);
	}
	SUBCASE("bad arguments change nothing")
	{
		VisualScriptDocument unchanged = scores;
		Result<VisualScriptPasteResult> const missing = PasteVisualScriptClipboard(unchanged, Scores, 7, content, glm::vec2(0.0f), nullptr);
		CHECK(missing.GetError() == "the document has no function 7");
		Result<VisualScriptPasteResult> const notFinite =
			PasteVisualScriptClipboard(unchanged, Scores, 0, content, glm::vec2(std::numeric_limits<float>::infinity(), 0.0f), nullptr);
		CHECK(notFinite.GetError() == "a paste's position must be finite");
		CHECK(unchanged == scores);
	}
}

TEST_CASE("VisualScriptClipboard: pasted into another script, what the nodes use is added under names of its own")
{
	VisualScriptDocument const scores = ParseDocument(ScoresText);
	VisualScriptClipboardContent const content = Copy(scores, 0, {31, 32});

	SUBCASE("an empty script gets the variables and the functions, transitively, with their locals and graphs")
	{
		VisualScriptDocument document;
		Result<VisualScriptPasteResult> pasted = PasteVisualScriptClipboard(document, Other, 0, content, glm::vec2(0.0f), nullptr);
		REQUIRE_MESSAGE(pasted.IsOk(), pasted.GetError());
		std::vector<std::string> const notes = {"added function Add", "added variable Score (int)", "added function Helper",
		                                        "added variable Label (string)"};
		CHECK(pasted.GetValue().Notes == notes);
		VisualScriptVariable const score = GetVariable(document, "Score");
		CHECK(score.Value == 3);
		CHECK(score.Public);
		CHECK(score.FormerNames.empty());
		VisualScriptFunction const add = GetFunction(document, "Add");
		VisualScriptFunction const helper = GetFunction(document, "Helper");
		CHECK(add.Inputs == scores.Functions[0].Inputs);
		CHECK(helper.Pure);
		REQUIRE(add.Locals.size() == 1);
		CHECK(add.Locals[0].Name == "Acc");
		CHECK(add.Locals[0].Value == 0);
		// The copied graphs name the copies: Add's Set its own local, its call Helper, its Get the new Score.
		REQUIRE(add.Graph.Nodes.size() == 4);
		CHECK(GetSetting(add.Graph.Nodes[1], "Variable") == add.Locals[0].ID);
		CHECK(GetSetting(add.Graph.Nodes[2], "Function") == helper.ID);
		CHECK(GetSetting(add.Graph.Nodes[3], "Variable") == score.ID);
		CHECK(add.Graph.Nodes[2].Position == glm::vec2(640.0f, 0.0f));
		REQUIRE(add.Graph.Links.size() == 3);
		CHECK(add.Graph.Links[2] == VisualScriptLink{{add.Graph.Nodes[0].ID, "Amount"}, {add.Graph.Nodes[1].ID, "Value"}});
		REQUIRE(add.Graph.Comments.size() == 1);
		CHECK(add.Graph.Comments[0].Text == "Adds");
		CHECK(GetSetting(helper.Graph.Nodes[2], "Variable") == GetVariable(document, "Label").ID);
		CHECK(GetSetting(GetNode(document.Graph, pasted.GetValue().Pasted[0]), "Function") == add.ID);
		Testing::ExpectStrictRoundTrip(document);
		CheckUniqueIDs(document);
	}
	SUBCASE("a name another type has, or another function's, is made unique")
	{
		VisualScriptDocument document;
		VisualScriptVariable score;
		score.ID = 1;
		score.Name = "Score";
		score.Type = "float";
		document.Variables.push_back(score);
		VisualScriptVariable former;
		former.ID = 2;
		former.Name = "Old";
		former.Type = "bool";
		former.FormerNames = {"Label"};
		document.Variables.push_back(former);
		VisualScriptFunction add;
		add.ID = 3;
		add.Name = "Add";
		add.Pure = true;
		document.Functions.push_back(add);
		VisualScriptFunction helper;
		helper.ID = 4;
		helper.Name = "Helper";
		helper.Pure = true;
		helper.Outputs = {{"Result", "string"}};
		helper.Locals = {{5, "Acc", "int", nullptr}};
		document.Functions.push_back(helper);

		Result<VisualScriptPasteResult> pasted = PasteVisualScriptClipboard(document, Other, 0, content, glm::vec2(0.0f), nullptr);
		REQUIRE_MESSAGE(pasted.IsOk(), pasted.GetError());
		// Add has another signature, so the copy is Add1, whose call binds to Helper, which has the signature; a local named
		// Acc in another function leaves Add1's its name; Score is a float, so the variable is Score1.
		std::vector<std::string> const notes = {"added function Add1", "added variable Score1 (int)"};
		CHECK(pasted.GetValue().Notes == notes);
		VisualScriptFunction const added = GetFunction(document, "Add1");
		CHECK(added.Locals[0].Name == "Acc");
		CHECK(GetSetting(added.Graph.Nodes[2], "Function") == 4);
		CHECK(GetVariable(document, "Score1").Type == "int");
		CHECK(document.Variables.size() == 3);
		Testing::ExpectStrictRoundTrip(document);
		CheckUniqueIDs(document);
	}
	SUBCASE("a local of a new function avoids the script's variable names")
	{
		VisualScriptDocument document;
		VisualScriptVariable acc;
		acc.ID = 1;
		acc.Name = "Acc";
		acc.Type = "int";
		document.Variables.push_back(acc);
		REQUIRE(PasteVisualScriptClipboard(document, Other, 0, content, glm::vec2(0.0f), nullptr).IsOk());
		CHECK(GetFunction(document, "Add").Locals[0].Name == "Acc1");
		Testing::ExpectStrictRoundTrip(document);
	}
}

TEST_CASE("VisualScriptClipboard: a copy from a function flattens the locals it uses, and pastes bind them by name")
{
	VisualScriptDocument const functions = LoadTestDocument("Functions.json");
	// SumTo (4) calls itself (34) and reads its accumulator Sum (local 5, node 38).
	VisualScriptClipboardContent const content = Copy(functions, 4, {34, 38});
	CHECK(content.Function == 4);
	REQUIRE(content.Locals.size() == 1);
	CHECK(content.Locals[0].Name == "Sum");
	// The local is in the copied function and flattened beside it, under a name of its own and a new ID above the rest.
	REQUIRE(content.Document.Functions.size() == 1);
	CHECK(content.Document.Functions[0].Locals[0].Name == "Sum");
	VisualScriptVariable const flattened = GetVariable(content.Document, "Sum1");
	CHECK(flattened.ID == content.Locals[0].ID);
	CHECK(flattened.Type == "int");
	CHECK(flattened.Value == 0);
	for (VisualScriptNode const& node : content.Document.Graph.Nodes)
	{
		CHECK(node.ID < flattened.ID);
	}
	CHECK(GetSetting(content.Document.Graph.Nodes[1], "Variable") == flattened.ID);
	CHECK(GetSetting(content.Document.Graph.Nodes[0], "Function") == 4);

	SUBCASE("into the same function: the call and the local are its own")
	{
		VisualScriptDocument document = functions;
		Result<VisualScriptPasteResult> pasted = PasteVisualScriptClipboard(document, Scores, 4, content, glm::vec2(0.0f), nullptr);
		REQUIRE_MESSAGE(pasted.IsOk(), pasted.GetError());
		CHECK(pasted.GetValue().Notes.empty());
		VisualScriptGraph const graph = GetFunction(document, 4u).Graph;
		CHECK(GetSetting(GetNode(graph, pasted.GetValue().Pasted[0]), "Function") == 4);
		CHECK(GetSetting(GetNode(graph, pasted.GetValue().Pasted[1]), "Variable") == 5);
		CHECK(document.Variables == functions.Variables);
		CHECK(document.Functions.size() == functions.Functions.size());
		Testing::ExpectStrictRoundTrip(document);
	}
	SUBCASE("into another function: a local of its own")
	{
		VisualScriptDocument document = functions;
		Result<VisualScriptPasteResult> pasted = PasteVisualScriptClipboard(document, Scores, 6, content, glm::vec2(0.0f), nullptr);
		REQUIRE_MESSAGE(pasted.IsOk(), pasted.GetError());
		std::vector<std::string> const notes = {"added local Sum (int) to WaitThenLog"};
		CHECK(pasted.GetValue().Notes == notes);
		VisualScriptFunction const wait = GetFunction(document, 6u);
		REQUIRE(wait.Locals.size() == 1);
		CHECK(GetSetting(GetNode(wait.Graph, pasted.GetValue().Pasted[1]), "Variable") == wait.Locals[0].ID);
		Testing::ExpectStrictRoundTrip(document);
	}
	SUBCASE("into the event graph: a variable, named apart from every local")
	{
		VisualScriptDocument document = functions;
		Result<VisualScriptPasteResult> pasted = PasteVisualScriptClipboard(document, Scores, 0, content, glm::vec2(0.0f), nullptr);
		REQUIRE_MESSAGE(pasted.IsOk(), pasted.GetError());
		std::vector<std::string> const notes = {"added variable Sum1 (int)"};
		CHECK(pasted.GetValue().Notes == notes);
		CHECK(GetSetting(GetNode(document.Graph, pasted.GetValue().Pasted[1]), "Variable") == GetVariable(document, "Sum1").ID);
		Testing::ExpectStrictRoundTrip(document);
	}
	SUBCASE("into another script: the function and its local come too")
	{
		VisualScriptDocument document;
		VisualScriptFunction work;
		work.ID = 1;
		work.Name = "Work";
		VisualScriptNode entry;
		entry.ID = 2;
		entry.Type = "Function.Entry";
		work.Graph.Nodes.push_back(entry);
		document.Functions.push_back(work);
		Result<VisualScriptPasteResult> pasted = PasteVisualScriptClipboard(document, Other, 1, content, glm::vec2(0.0f), nullptr);
		REQUIRE_MESSAGE(pasted.IsOk(), pasted.GetError());
		std::vector<std::string> const notes = {"added function SumTo", "added local Sum (int) to Work"};
		CHECK(pasted.GetValue().Notes == notes);
		VisualScriptFunction const sumTo = GetFunction(document, "SumTo");
		// Its recursive call calls the copy itself.
		bool callsItself = false;
		for (VisualScriptNode const& node : sumTo.Graph.Nodes)
		{
			callsItself = callsItself || (node.Type == "Function.Call" && GetSetting(node, "Function") == sumTo.ID);
		}
		CHECK(callsItself);
		Testing::ExpectStrictRoundTrip(document);
		CheckUniqueIDs(document);
	}
}

TEST_CASE("VisualScriptClipboard: events the graph has, events in functions and a second entry are left out")
{
	VisualScriptDocument const scores = ParseDocument(ScoresText);
	VisualScriptClipboardContent const events = Copy(scores, 0, {30, 33, 34});

	SUBCASE("the event graph has them")
	{
		VisualScriptDocument document = scores;
		Result<VisualScriptPasteResult> pasted = PasteVisualScriptClipboard(document, Scores, 0, events, glm::vec2(0.0f), nullptr);
		REQUIRE(pasted.IsOk());
		std::vector<std::string> const notes = {"left out 2 events the event graph has already"};
		CHECK(pasted.GetValue().Notes == notes);
		CHECK(pasted.GetValue().NodeCount == 1);
		CHECK(GetNode(document.Graph, pasted.GetValue().Pasted[0]).Type == "Call:Strada.Log.Info(string)");
		// The link from the key event went with it.
		CHECK(document.Graph.Links == scores.Graph.Links);
		Testing::ExpectStrictRoundTrip(document);
	}
	SUBCASE("another key is another event")
	{
		VisualScriptDocument document = scores;
		document.Graph.Nodes[3].Settings["Key"] = "Enter";
		Result<VisualScriptPasteResult> pasted = PasteVisualScriptClipboard(document, Scores, 0, events, glm::vec2(0.0f), nullptr);
		REQUIRE(pasted.IsOk());
		std::vector<std::string> const notes = {"left out 1 event the event graph has already"};
		CHECK(pasted.GetValue().Notes == notes);
		CHECK(pasted.GetValue().NodeCount == 2);
		std::vector<uint32_t> const pastedIDs = pasted.GetValue().Pasted;
		CHECK(document.Graph.Links.back() == VisualScriptLink{{pastedIDs[0], "Then"}, {pastedIDs[1], "Execute"}});
	}
	SUBCASE("events belong in the event graph")
	{
		VisualScriptDocument document = scores;
		Result<VisualScriptPasteResult> pasted = PasteVisualScriptClipboard(document, Scores, 10, events, glm::vec2(0.0f), nullptr);
		REQUIRE(pasted.IsOk());
		std::vector<std::string> const notes = {"left out 2 events: events belong in the event graph"};
		CHECK(pasted.GetValue().Notes == notes);
		CHECK(pasted.GetValue().NodeCount == 1);
	}
	SUBCASE("a function has one entry")
	{
		VisualScriptClipboardContent const entry = Copy(scores, 10, {12, 13});
		VisualScriptDocument document = scores;
		Result<VisualScriptPasteResult> pasted = PasteVisualScriptClipboard(document, Scores, 10, entry, glm::vec2(0.0f), nullptr);
		REQUIRE(pasted.IsOk());
		std::vector<std::string> const notes = {"left out 1 function entry: the function has one already"};
		CHECK(pasted.GetValue().Notes == notes);
		CHECK(GetFunction(document, 10u).Graph.Links == scores.Functions[0].Graph.Links);
		// Into a function without one, it stays.
		VisualScriptDocument empty;
		VisualScriptFunction work;
		work.ID = 1;
		work.Name = "Work";
		empty.Functions.push_back(work);
		Result<VisualScriptPasteResult> kept = PasteVisualScriptClipboard(empty, Other, 1, entry, glm::vec2(0.0f), nullptr);
		REQUIRE(kept.IsOk());
		CHECK(kept.GetValue().NodeCount == 2);
		CHECK(empty.Functions[0].Graph.Links.size() == 2);
	}
	SUBCASE("a clipboard with an event twice keeps the first")
	{
		VisualScriptDocument twice;
		for (uint32_t id = 1; id <= 2; id++)
		{
			VisualScriptNode custom;
			custom.ID = id;
			custom.Type = "Event.Custom";
			custom.Position = glm::vec2(0.0f, 200.0f * static_cast<float>(id));
			custom.Settings = {{"Name", "Hit"}};
			twice.Graph.Nodes.push_back(custom);
		}
		VisualScriptClipboardContent const copied = Copy(twice, 0, {1, 2});
		VisualScriptDocument document;
		Result<VisualScriptPasteResult> pasted = PasteVisualScriptClipboard(document, Other, 0, copied, glm::vec2(0.0f), nullptr);
		REQUIRE(pasted.IsOk());
		CHECK(pasted.GetValue().NodeCount == 1);
		CHECK(document.Graph.Nodes[0].Position == glm::vec2(0.0f, 0.0f));
	}
}

TEST_CASE("VisualScriptClipboard: a paste that would exceed a limit of the format changes nothing")
{
	VisualScriptDocument const scores = ParseDocument(ScoresText);
	VisualScriptClipboardContent const content = Copy(scores, 0, {31, 32});

	SUBCASE("nodes of a graph")
	{
		VisualScriptDocument document;
		for (uint32_t id = 1; id < VisualScriptSerializer::MaxNodesPerGraph; id++)
		{
			VisualScriptNode node;
			node.ID = id;
			node.Type = "Self";
			document.Graph.Nodes.push_back(node);
		}
		VisualScriptDocument const before = document;
		Result<VisualScriptPasteResult> const pasted = PasteVisualScriptClipboard(document, Other, 0, content, glm::vec2(0.0f), nullptr);
		CHECK(pasted.GetError() == "a graph holds at most 10000 nodes: the paste would make 10001");
		CHECK(document == before);
	}
	SUBCASE("variables of a script")
	{
		VisualScriptDocument document;
		for (uint32_t id = 1; id <= VisualScriptSerializer::MaxVariables; id++)
		{
			VisualScriptVariable variable;
			variable.ID = id;
			variable.Name = "V" + std::to_string(id);
			variable.Type = "int";
			document.Variables.push_back(variable);
		}
		VisualScriptDocument const before = document;
		Result<VisualScriptPasteResult> const pasted = PasteVisualScriptClipboard(document, Other, 0, content, glm::vec2(0.0f), nullptr);
		CHECK(pasted.GetError() == "a script holds at most 1024 variables");
		CHECK(document == before);
	}
	SUBCASE("IDs")
	{
		VisualScriptDocument document;
		VisualScriptNode last;
		last.ID = VisualScriptSerializer::MaxID;
		last.Type = "Self";
		document.Graph.Nodes.push_back(last);
		VisualScriptDocument const before = document;
		Result<VisualScriptPasteResult> const pasted = PasteVisualScriptClipboard(document, Other, 0, content, glm::vec2(0.0f), nullptr);
		CHECK(pasted.GetError() == "the document has no free ID left: IDs end at 2147483647");
		CHECK(document == before);

		// A copy from it renumbers below the last ID, so the clipboard still reads.
		std::vector<uint32_t> const only = {VisualScriptSerializer::MaxID};
		VisualScriptClipboardContent const copied = Copy(document, 0, only);
		REQUIRE(copied.Document.Graph.Nodes.size() == 1);
		CHECK(copied.Document.Graph.Nodes[0].ID == 1);
	}
}

TEST_CASE("VisualScriptClipboard: duplicate pastes a copy 32 right and down, bound to what the originals use")
{
	VisualScriptDocument const scores = ParseDocument(ScoresText);
	for (AssetHandle const graph : {Scores, AssetHandle()})
	{
		CAPTURE(graph.ToString());
		VisualScriptDocument document = scores;
		std::vector<uint32_t> const elements = {31, 32};
		Result<VisualScriptPasteResult> duplicated = DuplicateVisualScriptElements(document, graph, 0, elements, nullptr);
		REQUIRE_MESSAGE(duplicated.IsOk(), duplicated.GetError());
		CHECK(duplicated.GetValue().Notes.empty());
		std::vector<uint32_t> const ids = {36, 37};
		CHECK(duplicated.GetValue().Pasted == ids);
		CHECK(GetNode(document.Graph, 36).Position == glm::vec2(352.0f, 32.0f));
		// The unplaced original stands where the canvas shows it.
		CHECK(GetNode(document.Graph, 37).Position == glm::vec2(32.0f, 572.0f));
		CHECK(GetSetting(GetNode(document.Graph, 36), "Function") == 10);
		CHECK(GetSetting(GetNode(document.Graph, 37), "Variable") == 1);
		CHECK(document.Graph.Links.back() == VisualScriptLink{{37, "Value"}, {36, "Amount"}});
		CHECK(document.Variables == scores.Variables);
		CHECK(document.Functions == scores.Functions);
		Testing::ExpectStrictRoundTrip(document);
	}

	VisualScriptDocument document = scores;
	std::vector<uint32_t> const local = {13, 15, 16};
	Result<VisualScriptPasteResult> inFunction = DuplicateVisualScriptElements(document, Scores, 10, local, nullptr);
	REQUIRE_MESSAGE(inFunction.IsOk(), inFunction.GetError());
	CHECK(inFunction.GetValue().Notes.empty());
	CHECK(GetSetting(GetNode(GetFunction(document, 10u).Graph, inFunction.GetValue().Pasted[0]), "Variable") == 11);
	CHECK(GetFunction(document, 10u).Locals.size() == 1);
	Testing::ExpectStrictRoundTrip(document);
	CHECK(DuplicateVisualScriptElements(document, Scores, 77, local, nullptr).GetError() == "the document has no function 77");
}

TEST_CASE("VisualScriptClipboard: damaged clipboard texts are refused or paste into a document that saves")
{
	Json const file = Testing::LoadFeatureTestDocument("Assets/Scripts/FormatSample.sgraph");
	DeserializationContext context;
	context.UnknownFields = UnknownFieldPolicy::Warn;
	std::vector<VisualScriptIssue> issues;
	Result<VisualScriptDocument> loaded = VisualScriptSerializer::Deserialize(file, context, &issues);
	REQUIRE(loaded.IsOk());
	REQUIRE(issues.empty());
	VisualScriptDocument const sample = loaded.TakeValue();
	REQUIRE_FALSE(sample.Functions.empty());
	Scope<VisualScriptLibrary> const library = ParseLibrary();

	// A copy of all of the event graph.
	std::vector<uint32_t> elements;
	for (VisualScriptNode const& node : sample.Graph.Nodes)
	{
		elements.push_back(node.ID);
	}
	for (VisualScriptComment const& comment : sample.Graph.Comments)
	{
		elements.push_back(comment.ID);
	}
	Result<Json> seed = ParseJson(WriteVisualScriptClipboard(sample, 0, elements, Scores, library.get()));
	REQUIRE(seed.IsOk());

	Testing::DocumentMutator mutator(10200);
	int pasted = 0;
	int refused = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(200); round < rounds; round++)
	{
		CAPTURE(round);
		std::string const text = DumpJson(mutator.Damage(seed.GetValue(), round));
		Result<VisualScriptClipboardContent> content = ReadVisualScriptClipboard(text);
		if (!content)
		{
			CHECK(content.GetError().starts_with(Refusal));
			refused++;
			continue;
		}
		// Into the script copied from and into another, the event graph and a function, with and without the library.
		VisualScriptDocument document = sample;
		AssetHandle const target = round % 2 == 0 ? Scores : Other;
		uint32_t const function = round % 3 == 0 ? sample.Functions.front().ID : 0;
		glm::vec2 const at(static_cast<float>(round % 7) * 160.0f, -static_cast<float>(round % 5) * 96.0f);
		Result<VisualScriptPasteResult> result =
			PasteVisualScriptClipboard(document, target, function, content.GetValue(), at, round % 4 < 2 ? library.get() : nullptr);
		if (!result)
		{
			CHECK(document == sample);
			refused++;
			continue;
		}
		Testing::ExpectStrictRoundTrip(document);
		pasted++;
	}
	CHECK(pasted > 0);
	CHECK(refused > 0);
}
