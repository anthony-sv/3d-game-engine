#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	std::filesystem::path GetFixturePath(std::string_view name)
	{
		return FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / FileSystem::PathFromUtf8(name);
	}

	std::string ReadText(std::filesystem::path const& path)
	{
		Result<std::string> text = FileSystem::ReadTextFile(path);
		REQUIRE_MESSAGE(text.IsOk(), text.GetError());
		return text.TakeValue();
	}

	Json Parse(std::string_view text)
	{
		Result<Json> json = ParseJson(text);
		REQUIRE_MESSAGE(json.IsOk(), json.GetError());
		return json.TakeValue();
	}

	DeserializationContext MakeContext(UnknownFieldPolicy unknownFields, std::vector<std::string>* warnings = nullptr)
	{
		DeserializationContext context;
		context.UnknownFields = unknownFields;
		context.Warnings = warnings;
		return context;
	}

	// The issues' paths, comma-separated, so a failed check shows them all.
	std::string GetPaths(std::vector<VisualScriptIssue> const& issues)
	{
		std::string paths;
		for (VisualScriptIssue const& issue : issues)
		{
			paths += paths.empty() ? issue.Path : ", " + issue.Path;
		}
		return paths;
	}

	// What loads saves and loads again unchanged and without issues, as a document and as text.
	void CheckSavesAndLoadsAgain(VisualScriptDocument const& document)
	{
		Json const content = VisualScriptSerializer::SerializeDocument(document);
		std::vector<VisualScriptIssue> issues;
		Result<VisualScriptDocument> reloaded =
			VisualScriptSerializer::DeserializeDocument(content, MakeContext(UnknownFieldPolicy::Error), &issues);
		REQUIRE_MESSAGE(reloaded.IsOk(), reloaded.GetError());
		CHECK(GetPaths(issues).empty());
		CHECK(reloaded.GetValue() == document);

		issues.clear();
		Result<VisualScriptDocument> fromText =
			VisualScriptSerializer::DeserializeDocument(Parse(DumpJson(content)), MakeContext(UnknownFieldPolicy::Error), &issues);
		REQUIRE_MESSAGE(fromText.IsOk(), fromText.GetError());
		CHECK(GetPaths(issues).empty());
		CHECK(fromText.GetValue() == document);
	}

	struct LoadedDocument
	{
		VisualScriptDocument Document;
		std::vector<VisualScriptIssue> Issues;
	};

	// Loads the content form as files and strict use do (unknown keys are issues), checking that every issue is also a
	// warning of the context and that the result saves and loads again.
	LoadedDocument Load(Json const& content)
	{
		std::vector<std::string> warnings;
		LoadedDocument loaded;
		Result<VisualScriptDocument> document =
			VisualScriptSerializer::DeserializeDocument(content, MakeContext(UnknownFieldPolicy::Warn, &warnings), &loaded.Issues);
		REQUIRE_MESSAGE(document.IsOk(), document.GetError());
		REQUIRE(warnings.size() == loaded.Issues.size());
		for (size_t i = 0; i < warnings.size(); i++)
		{
			CHECK(warnings[i] == loaded.Issues[i].Path + ": " + loaded.Issues[i].Message);
		}
		loaded.Document = document.TakeValue();
		CheckSavesAndLoadsAgain(loaded.Document);
		return loaded;
	}

	// Strict use (automation input): the input is valid only when it loads without a single issue.
	bool IsStrictlyValid(Json const& content)
	{
		std::vector<VisualScriptIssue> issues;
		return VisualScriptSerializer::DeserializeDocument(content, MakeContext(UnknownFieldPolicy::Warn), &issues).IsOk() &&
		       issues.empty();
	}

	// The error of loading text as a .sgraph file written into the directory, or "" when it loads.
	std::string GetFileError(Testing::TemporaryDirectory const& directory, std::string_view text)
	{
		std::filesystem::path const path = directory.GetPath() / "Graph.sgraph";
		REQUIRE(FileSystem::WriteTextFile(path, text).IsOk());
		return VisualScriptSerializer::LoadFromFile(path, MakeContext(UnknownFieldPolicy::Warn)).GetError();
	}

	std::string MakeFileText(std::string_view header, std::string_view content)
	{
		return std::string("{\"Strada\": ").append(header).append(", \"VisualScript\": ").append(content).append("}");
	}

	constexpr std::string_view ValidHeader = R"({"Version": 1, "Type": "VisualScript"})";

	bool Contains(std::string_view text, std::string_view part)
	{
		return text.find(part) != std::string_view::npos;
	}

	// A raw value nested levels deep: the innermost value (a number unless given) inside levels - 1 arrays.
	Json MakeNestedValue(size_t levels, Json innermost = 0)
	{
		Json value = std::move(innermost);
		for (size_t level = 1; level < levels; level++)
		{
			value = Json::array({std::move(value)});
		}
		return value;
	}

	Json MakeVariable(uint32_t id, std::string name, std::string type = "int")
	{
		return Json::object({{"ID", id}, {"Name", std::move(name)}, {"Type", std::move(type)}});
	}

	Json MakeNode(uint32_t id, std::string type = "Flow.Branch")
	{
		return Json::object({{"ID", id}, {"Type", std::move(type)}});
	}

	Json MakeContent(Json variables, Json functions, Json graph)
	{
		return Json::object(
			{{"Description", ""}, {"Variables", std::move(variables)}, {"Functions", std::move(functions)}, {"Graph", std::move(graph)}});
	}

	Json MakeGraph(Json nodes, Json links = Json::array(), Json comments = Json::array())
	{
		return Json::object({{"Nodes", std::move(nodes)}, {"Links", std::move(links)}, {"Comments", std::move(comments)}});
	}

	// Counts every JSON value of a document: objects, arrays and what they hold.
	size_t CountValues(Json const& value)
	{
		size_t count = 1;
		if (value.is_structured())
		{
			for (Json const& child : value)
			{
				count += CountValues(child);
			}
		}
		return count;
	}

	// The kinds of JSON values (Json::value_t as numbers: nlohmann orders the number kinds as equal) and the depth they
	// nest to.
	void CollectKinds(Json const& value, std::set<uint8_t>& kinds, size_t& depth, size_t level = 1)
	{
		kinds.insert(static_cast<uint8_t>(value.type()));
		depth = std::max(depth, level);
		if (value.is_structured())
		{
			for (Json const& child : value)
			{
				CollectKinds(child, kinds, depth, level + 1);
			}
		}
	}
}

TEST_CASE("VisualScript: the example file loads without issues and saves byte for byte")
{
	std::string const text = ReadText(GetFixturePath("Example.sgraph"));
	std::vector<std::string> warnings;
	DeserializationContext const context = MakeContext(UnknownFieldPolicy::Warn, &warnings);
	std::vector<VisualScriptIssue> issues;
	Result<VisualScriptDocument> document = VisualScriptSerializer::Deserialize(Parse(text), context, &issues);
	REQUIRE_MESSAGE(document.IsOk(), document.GetError());
	CHECK(GetPaths(issues).empty());
	CHECK(warnings.empty());
	CHECK(DumpJson(VisualScriptSerializer::Serialize(document.GetValue())) == text);

	VisualScriptDocument const& example = document.GetValue();
	CHECK(example.Description == "Adds 5 points per frame while Space is held");
	REQUIRE(example.Variables.size() == 2);
	VisualScriptVariable const& speed = example.Variables[0];
	CHECK(speed.ID == 1);
	CHECK(speed.Name == "Speed");
	CHECK(speed.Type == "float");
	CHECK(speed.Value == Json(4.0));
	CHECK(speed.Public);
	CHECK(speed.Tooltip == "Meters per second");
	CHECK(speed.Range == std::optional<glm::vec2>(glm::vec2(0.0f, 20.0f)));
	CHECK(speed.FormerNames.empty());
	VisualScriptVariable const& score = example.Variables[1];
	CHECK(score.Value == Json(0));
	CHECK_FALSE(score.Public);
	CHECK_FALSE(score.Range);

	REQUIRE(example.Functions.size() == 1);
	VisualScriptFunction const& addScore = example.Functions[0];
	CHECK(addScore.ID == 3);
	CHECK(addScore.Name == "AddScore");
	CHECK_FALSE(addScore.Pure);
	CHECK(addScore.Inputs == std::vector<VisualScriptParameter>{{"Points", "int"}});
	CHECK(addScore.Outputs == std::vector<VisualScriptParameter>{{"Total", "int"}});
	CHECK(addScore.Locals.empty());
	CHECK(addScore.Graph.Nodes.size() == 5);
	CHECK(addScore.Graph.Links.size() == 6);
	CHECK(addScore.Graph.Comments.empty());

	REQUIRE(example.Graph.Nodes.size() == 4);
	VisualScriptNode const& isKeyDown = example.Graph.Nodes[1];
	CHECK(isKeyDown.Type == "Call:Strada.Input.IsKeyDown(Strada.KeyCode)");
	CHECK_FALSE(isKeyDown.Position);
	CHECK(isKeyDown.Settings == Json::object());
	CHECK(isKeyDown.Inputs == Json::object({{"Key", "Space"}}));
	VisualScriptNode const& call = example.Graph.Nodes[3];
	CHECK(call.Position == std::optional<glm::vec2>(glm::vec2(560.0f, 0.0f)));
	CHECK(call.Settings == Json::object({{"Function", 3}}));
	CHECK(call.Inputs == Json::object({{"Points", 5}}));
	REQUIRE(example.Graph.Links.size() == 3);
	CHECK(example.Graph.Links[1] == VisualScriptLink{{11, "Result"}, {12, "Condition"}});
	REQUIRE(example.Graph.Comments.size() == 1);
	VisualScriptComment const& comment = example.Graph.Comments[0];
	CHECK(comment.ID == 20);
	CHECK(comment.Text == "Scoring");
	CHECK(comment.Position == glm::vec2(-40.0f, -80.0f));
	CHECK(comment.Size == glm::vec2(880.0f, 320.0f));
	CHECK(comment.Color == glm::vec4(0.25f, 0.5f, 0.75f, 0.25f));

	// The file loads the same from disk, and the content form is the "VisualScript" object.
	Result<VisualScriptDocument> fromFile = VisualScriptSerializer::LoadFromFile(GetFixturePath("Example.sgraph"), context);
	REQUIRE_MESSAGE(fromFile.IsOk(), fromFile.GetError());
	CHECK(fromFile.GetValue() == example);
	CHECK(VisualScriptSerializer::Serialize(example)["VisualScript"] == VisualScriptSerializer::SerializeDocument(example));
}

TEST_CASE("VisualScript: the shared fixture documents load strictly, round trip and keep the writer's layout")
{
	constexpr std::array<std::string_view, 6> Documents = {"Data.json",      "Events.json",    "Flow.json",
	                                                       "Functions.json", "Reflected.json", "Variables.json"};
	for (std::string_view const name : Documents)
	{
		CAPTURE(name);
		std::string const text = ReadText(GetFixturePath("Documents") / FileSystem::PathFromUtf8(name));
		LoadedDocument const loaded = Load(Parse(text));
		CHECK(GetPaths(loaded.Issues).empty());
		CHECK(DumpJson(VisualScriptSerializer::SerializeDocument(loaded.Document)) == text);
		CHECK_FALSE(loaded.Document.Graph.Comments.empty());
	}
}

TEST_CASE("VisualScript: the FeatureTest format sample is canonical and uses every feature of the format")
{
	std::filesystem::path const path = FileSystem::PathFromUtf8(STRADA_FEATURE_TEST_DIR) / "Assets" / "Scripts" / "FormatSample.sgraph";
	std::string const text = ReadText(path);
	Json const file = Parse(text);
	std::vector<VisualScriptIssue> issues;
	Result<VisualScriptDocument> loaded = VisualScriptSerializer::Deserialize(file, MakeContext(UnknownFieldPolicy::Warn), &issues);
	REQUIRE_MESSAGE(loaded.IsOk(), loaded.GetError());
	CHECK(GetPaths(issues).empty());
	CHECK(DumpJson(VisualScriptSerializer::Serialize(loaded.GetValue())) == text);
	// Small, so that the fuzz test damages its header often enough to see refusals.
	CHECK(CountValues(file) < 600);

	VisualScriptDocument const& sample = loaded.GetValue();
	CHECK_FALSE(sample.Description.empty());

	// A public variable of every exposable type (an enum among them), with tooltips, ranges and former names.
	constexpr std::array<std::string_view, 24> ExposableTypes = {"bool",
	                                                             "int",
	                                                             "uint",
	                                                             "long",
	                                                             "ulong",
	                                                             "float",
	                                                             "double",
	                                                             "string",
	                                                             "Strada.Vector2",
	                                                             "Strada.Vector3",
	                                                             "Strada.Vector4",
	                                                             "Strada.Quaternion",
	                                                             "Strada.Color",
	                                                             "Strada.Entity",
	                                                             "Strada.Prefab",
	                                                             "Strada.AssetHandle",
	                                                             "Strada.Mesh",
	                                                             "Strada.Material",
	                                                             "Strada.Texture",
	                                                             "Strada.AudioClip",
	                                                             "Strada.Font",
	                                                             "Strada.EnvironmentMap",
	                                                             "Strada.VisualScript",
	                                                             "Strada.ForceMode"};
	for (std::string_view const type : ExposableTypes)
	{
		CAPTURE(type);
		CHECK(std::any_of(sample.Variables.begin(), sample.Variables.end(),
		                  [type](VisualScriptVariable const& variable)
		                  {
							  return variable.Public && variable.Type == type && !variable.Value.is_null();
						  }));
	}
	auto const anyVariable = [&sample](auto const& predicate)
	{
		return std::any_of(sample.Variables.begin(), sample.Variables.end(), predicate);
	};
	CHECK(anyVariable(
		[](VisualScriptVariable const& variable)
		{
			return !variable.Tooltip.empty() && variable.Range && !variable.FormerNames.empty();
		}));
	CHECK(anyVariable(
		[](VisualScriptVariable const& variable)
		{
			return !variable.Public && variable.Value.is_null();
		}));

	// A pure and an impure function, each with parameters, locals and a graph of its own with links and comments.
	for (bool const pure : {true, false})
	{
		CAPTURE(pure);
		CHECK(std::any_of(sample.Functions.begin(), sample.Functions.end(),
		                  [pure](VisualScriptFunction const& function)
		                  {
							  return function.Pure == pure && !function.Inputs.empty() && !function.Outputs.empty() &&
			                         !function.Locals.empty() && !function.Graph.Nodes.empty() && !function.Graph.Links.empty() &&
			                         !function.Graph.Comments.empty();
						  }));
	}

	// Placed and unplaced nodes, links and comments in the event graph; settings and inputs of every JSON kind, nested.
	std::vector<VisualScriptGraph const*> graphs = {&sample.Graph};
	for (VisualScriptFunction const& function : sample.Functions)
	{
		graphs.push_back(&function.Graph);
	}
	bool placed = false;
	bool unplaced = false;
	std::set<uint8_t> kinds;
	size_t depth = 0;
	for (VisualScriptGraph const* graph : graphs)
	{
		for (VisualScriptNode const& node : graph->Nodes)
		{
			placed = placed || node.Position.has_value();
			unplaced = unplaced || !node.Position.has_value();
			for (Json const* members : {&node.Settings, &node.Inputs})
			{
				for (Json const& member : *members)
				{
					CollectKinds(member, kinds, depth);
				}
			}
		}
	}
	CHECK(placed);
	CHECK(unplaced);
	CHECK_FALSE(sample.Graph.Links.empty());
	CHECK_FALSE(sample.Graph.Comments.empty());
	for (Json::value_t const kind :
	     {Json::value_t::null, Json::value_t::boolean, Json::value_t::number_integer, Json::value_t::number_unsigned,
	      Json::value_t::number_float, Json::value_t::string, Json::value_t::array, Json::value_t::object})
	{
		CAPTURE(static_cast<int>(kind));
		CHECK(kinds.contains(static_cast<uint8_t>(kind)));
	}
	CHECK(depth >= 4);
}

TEST_CASE("VisualScript: the FeatureTest scene's graph loads without issues and keeps the writer's layout")
{
	std::filesystem::path const path = FileSystem::PathFromUtf8(STRADA_FEATURE_TEST_DIR) / "Assets" / "Scripts" / "Features.sgraph";
	std::string const text = ReadText(path);
	std::vector<VisualScriptIssue> issues;
	Result<VisualScriptDocument> loaded = VisualScriptSerializer::Deserialize(Parse(text), MakeContext(UnknownFieldPolicy::Warn), &issues);
	REQUIRE_MESSAGE(loaded.IsOk(), loaded.GetError());
	CHECK(GetPaths(issues).empty());
	CHECK(DumpJson(VisualScriptSerializer::Serialize(loaded.GetValue())) == text);
}

TEST_CASE("VisualScript: files that are not visual scripts of this version are refused")
{
	Testing::TemporaryDirectory const directory;
	CHECK(GetFileError(directory, MakeFileText(ValidHeader, "{}")).empty());

	// What ParseJson refuses: syntax errors, values nested deeper than 256 levels, objects of more than 4096 members.
	CHECK(Contains(GetFileError(directory, MakeFileText(ValidHeader, "{\"Description\": ")), "invalid JSON"));
	std::string const nested255 = std::string(255, '[') + std::string(255, ']');
	CHECK(Contains(GetFileError(directory, MakeFileText(ValidHeader, "{\"Description\": " + nested255 + "}")), "nested deeper than 256"));
	std::string const nested254 = std::string(254, '[') + std::string(254, ']');
	CHECK(GetFileError(directory, MakeFileText(ValidHeader, "{\"Description\": " + nested254 + "}")).empty());
	auto const makeMembers = [](size_t count)
	{
		std::string members;
		for (size_t i = 0; i < count; i++)
		{
			members += fmt::format("{}\"M{}\": {}", i == 0 ? "" : ", ", i, i);
		}
		return "{\"Variables\": [{\"ID\": 1, \"Name\": \"V\", \"Type\": \"Map\", \"Value\": {" + members + "}}]}";
	};
	CHECK(Contains(GetFileError(directory, MakeFileText(ValidHeader, makeMembers(4097))), "more than 4096 members"));
	CHECK(GetFileError(directory, MakeFileText(ValidHeader, makeMembers(4096))).empty());

	// The root, the header and the content object.
	CHECK_FALSE(GetFileError(directory, "[]").empty());
	CHECK_FALSE(GetFileError(directory, "\"VisualScript\"").empty());
	CHECK_FALSE(GetFileError(directory, "{\"VisualScript\": {}}").empty());
	CHECK_FALSE(GetFileError(directory, "{\"Strada\": 1, \"VisualScript\": {}}").empty());
	for (std::string_view const version : {"0", "-1", "-9223372036854775808", "1.5", "1.0", "\"1\"", "true", "null", "[1]"})
	{
		CAPTURE(version);
		CHECK_FALSE(
			GetFileError(directory, MakeFileText(fmt::format(R"({{"Version": {}, "Type": "VisualScript"}})", version), "{}")).empty());
	}
	CHECK_FALSE(GetFileError(directory, MakeFileText(R"({"Type": "VisualScript"})", "{}")).empty());
	for (std::string_view const version : {"2", "2147483648", "4294967297", "18446744073709551615"})
	{
		CAPTURE(version);
		CHECK(Contains(GetFileError(directory, MakeFileText(fmt::format(R"({{"Version": {}, "Type": "VisualScript"}})", version), "{}")),
		               "newer"));
	}
	CHECK_FALSE(GetFileError(directory, MakeFileText(R"({"Version": 1, "Type": "Material"})", "{}")).empty());
	CHECK_FALSE(GetFileError(directory, MakeFileText(R"({"Version": 1, "Type": 9})", "{}")).empty());
	CHECK_FALSE(GetFileError(directory, MakeFileText(R"({"Version": 1})", "{}")).empty());
	CHECK_FALSE(GetFileError(directory, R"({"Strada": {"Version": 1, "Type": "VisualScript"}})").empty());
	for (std::string_view const content : {"[]", "\"\"", "null", "3"})
	{
		CAPTURE(content);
		CHECK_FALSE(GetFileError(directory, MakeFileText(ValidHeader, content)).empty());
	}

	// Errors name the file; the content form refuses anything but an object.
	CHECK(Contains(GetFileError(directory, "[]"), "Graph.sgraph"));
	CHECK(VisualScriptSerializer::LoadFromFile(directory.GetPath() / "Missing.sgraph", MakeContext(UnknownFieldPolicy::Warn)).IsError());
	for (Json const& content : {Json::array(), Json(), Json("Graph"), Json(1)})
	{
		CHECK(VisualScriptSerializer::DeserializeDocument(content, MakeContext(UnknownFieldPolicy::Warn)).IsError());
	}
}

TEST_CASE("VisualScript: unknown keys refuse the document under Error, are issues under Warn and are skipped under Ignore")
{
	Json const file = Parse(R"({
		"Strada": {"Version": 1, "Type": "VisualScript", "Editor": "2.0"},
		"VisualScript": {
			"Description": "",
			"Author": "someone",
			"Variables": [{"ID": 1, "Name": "V", "Type": "int", "Hidden": true}],
			"Functions": [
				{
					"ID": 2, "Name": "F", "Inline": true,
					"Inputs": [{"Name": "P", "Type": "int", "Default": 1}],
					"Locals": [{"ID": 3, "Name": "L", "Type": "int", "Public": true}],
					"Graph": {"Nodes": [], "Zoom": 1}
				}
			],
			"Graph": {
				"Nodes": [{"ID": 4, "Type": "Flow.Branch", "Comment": "c"}],
				"Links": [{"From": [4, "False"], "To": [4, "Execute"], "Color": 1}],
				"Comments": [{"ID": 5, "Collapsed": true}]
			}
		},
		"Thumbnail": "none"
	})");

	std::vector<std::string> warnings;
	std::vector<VisualScriptIssue> issues;
	Result<VisualScriptDocument> warned =
		VisualScriptSerializer::Deserialize(file, MakeContext(UnknownFieldPolicy::Warn, &warnings), &issues);
	REQUIRE(warned.IsOk());
	CHECK(GetPaths(issues) == "Thumbnail, Strada.Editor, Author, Variables[0].Hidden, Functions[0].Inline, Functions[0].Inputs[0].Default, "
	                          "Functions[0].Locals[0].Public, Functions[0].Graph.Zoom, Graph.Nodes[0].Comment, Graph.Links[0].Color, "
	                          "Graph.Comments[0].Collapsed");
	CHECK(warnings.size() == issues.size());
	CheckSavesAndLoadsAgain(warned.GetValue());

	warnings.clear();
	issues.clear();
	Result<VisualScriptDocument> ignored =
		VisualScriptSerializer::Deserialize(file, MakeContext(UnknownFieldPolicy::Ignore, &warnings), &issues);
	REQUIRE(ignored.IsOk());
	CHECK(issues.empty());
	CHECK(warnings.empty());
	CHECK(ignored.GetValue() == warned.GetValue());
	CHECK(ignored.GetValue().Functions.at(0).Locals.size() == 1);
	CHECK(ignored.GetValue().Graph.Links.size() == 1);

	// The error names the key's path; nothing is reported for a refused document.
	issues.clear();
	Result<VisualScriptDocument> refused =
		VisualScriptSerializer::Deserialize(file, MakeContext(UnknownFieldPolicy::Error, &warnings), &issues);
	CHECK(refused.GetError() == "Thumbnail: unknown key");
	CHECK(issues.empty());
	CHECK(warnings.empty());
	Json content = file["VisualScript"];
	CHECK(VisualScriptSerializer::DeserializeDocument(content, MakeContext(UnknownFieldPolicy::Error)).GetError() == "Author: unknown key");
	content.erase("Author");
	content["Variables"][0].erase("Hidden");
	content["Functions"][0].erase("Inline");
	CHECK(VisualScriptSerializer::DeserializeDocument(content, MakeContext(UnknownFieldPolicy::Error)).GetError() ==
	      "Functions[0].Inputs[0].Default: unknown key");
	content["Functions"].erase(0);
	CHECK(VisualScriptSerializer::DeserializeDocument(content, MakeContext(UnknownFieldPolicy::Error)).GetError() ==
	      "Graph.Nodes[0].Comment: unknown key");
}

TEST_CASE("VisualScript: text, flags, ranges and lists of the wrong kind are repaired")
{
	LoadedDocument const loaded = Load(Parse(R"({
		"Description": 5,
		"Variables": [
			{"ID": 1, "Name": "A", "Type": "int", "Public": "yes", "Tooltip": ["x"], "Range": [2.0, 1.0]},
			7,
			{"ID": 2, "Name": "B", "Type": "int", "Public": true, "Range": "wide"},
			{"ID": 3, "Name": "C", "Type": "int", "Range": [0.0]},
			{"ID": 4, "Name": "D", "Type": "int", "Range": [0.0, 1e39]},
			{"ID": 5, "Name": "E", "Type": "int", "Range": [-1.5, -1.5], "FormerNames": "Old"},
			{"ID": 6, "Name": "F", "Type": "int", "FormerNames": ["G", 3, null]}
		],
		"Functions": [
			{"ID": 10, "Name": "Pure", "Pure": 1, "Inputs": {}, "Outputs": "none", "Locals": 0, "Graph": []},
			"function"
		],
		"Graph": {
			"Nodes": {"ID": 20},
			"Links": null,
			"Comments": [{"ID": 21, "Text": false}, [22]]
		}
	})"));
	CHECK(GetPaths(loaded.Issues) ==
	      "Description, Variables[0].Public, Variables[0].Tooltip, Variables[0].Range, Variables[1], "
	      "Variables[2].Range, Variables[3].Range, Variables[4].Range, Variables[5].FormerNames, "
	      "Variables[6].FormerNames[1], Variables[6].FormerNames[2], Functions[0].Pure, Functions[0].Inputs, "
	      "Functions[0].Outputs, Functions[0].Locals, Functions[0].Graph, Functions[1], Graph.Nodes, Graph.Links, "
	      "Graph.Comments[0].Text, Graph.Comments[1]");

	VisualScriptDocument const& document = loaded.Document;
	CHECK(document.Description.empty());
	REQUIRE(document.Variables.size() == 6);
	CHECK_FALSE(document.Variables[0].Public);
	CHECK(document.Variables[0].Tooltip.empty());
	CHECK_FALSE(document.Variables[0].Range);
	CHECK(document.Variables[1].Public);
	CHECK_FALSE(document.Variables[1].Range);
	CHECK(document.Variables[4].Range == std::optional<glm::vec2>(glm::vec2(-1.5f, -1.5f)));
	CHECK(document.Variables[4].FormerNames.empty());
	CHECK(document.Variables[5].FormerNames == std::vector<std::string>{"G"});
	REQUIRE(document.Functions.size() == 1);
	CHECK_FALSE(document.Functions[0].Pure);
	CHECK(document.Functions[0].Inputs.empty());
	CHECK(document.Functions[0].Graph == VisualScriptGraph{});
	CHECK(document.Graph.Nodes.empty());
	REQUIRE(document.Graph.Comments.size() == 1);
	CHECK(document.Graph.Comments[0].ID == 21);
	CHECK(document.Graph.Comments[0].Text.empty());
}

TEST_CASE("VisualScript: elements with a missing or invalid ID, name or type are dropped")
{
	LoadedDocument const loaded = Load(Parse(R"({
		"Variables": [
			{"Name": "NoID", "Type": "int"},
			{"ID": 0, "Name": "Zero", "Type": "int"},
			{"ID": 2147483648, "Name": "Large", "Type": "int"},
			{"ID": -3, "Name": "Negative", "Type": "int"},
			{"ID": 1.0, "Name": "Fraction", "Type": "int"},
			{"ID": "4", "Name": "Text", "Type": "int"},
			{"ID": 1, "Name": "First", "Type": "int"},
			{"ID": 1, "Name": "SameID", "Type": "int"},
			{"ID": 2, "Type": "int"},
			{"ID": 3, "Name": "First", "Type": "int"},
			{"ID": 4, "Name": "NoType"},
			{"ID": 5, "Name": "EmptyType", "Type": ""},
			{"ID": 6, "Name": "NumberType", "Type": 6},
			{"ID": 2147483647, "Name": "Last", "Type": "int"}
		],
		"Functions": [
			{"Name": "NoID"},
			{"ID": 7},
			{"ID": 2, "Name": "TakesAnIDNoKeptElementHas", "Inputs": [{"Type": "int"}, {"Name": "P", "Type": "int"}]}
		],
		"Graph": {
			"Nodes": [{"Type": "Flow.Branch"}, {"ID": 8}, {"ID": 9, "Type": ["Flow.Branch"]}, {"ID": 3, "Type": "Flow.Branch"}],
			"Comments": [{"Text": "no ID"}, {"ID": 4}]
		}
	})"));
	CHECK(GetPaths(loaded.Issues) ==
	      "Variables[0].ID, Variables[1].ID, Variables[2].ID, Variables[3].ID, Variables[4].ID, Variables[5].ID, "
	      "Variables[7].ID, Variables[8].Name, Variables[9].Name, Variables[10].Type, Variables[11].Type, "
	      "Variables[12].Type, Functions[0].ID, Functions[1].Name, Functions[2].Inputs[0].Name, Graph.Nodes[0].ID, "
	      "Graph.Nodes[1].Type, Graph.Nodes[2].Type, Graph.Comments[0].ID");

	// Dropped elements claim no ID: the function, the node and the comment keep IDs only dropped variables had.
	VisualScriptDocument const& document = loaded.Document;
	REQUIRE(document.Variables.size() == 2);
	CHECK(document.Variables[0].ID == 1);
	CHECK(document.Variables[0].Name == "First");
	CHECK(document.Variables[1].ID == VisualScriptSerializer::MaxID);
	REQUIRE(document.Functions.size() == 1);
	CHECK(document.Functions[0].ID == 2);
	CHECK(document.Functions[0].Inputs == std::vector<VisualScriptParameter>{{"P", "int"}});
	REQUIRE(document.Graph.Nodes.size() == 1);
	CHECK(document.Graph.Nodes[0].ID == 3);
	REQUIRE(document.Graph.Comments.size() == 1);
	CHECK(document.Graph.Comments[0].ID == 4);
}

TEST_CASE("VisualScript: of two elements with one ID the later in document order is dropped, with what it holds")
{
	// Order: variables, then each function with its locals, nodes and comments, then the event graph's nodes and comments.
	LoadedDocument const loaded = Load(Parse(R"({
		"Variables": [{"ID": 1, "Name": "V", "Type": "int"}],
		"Functions": [
			{
				"ID": 2, "Name": "F",
				"Locals": [{"ID": 1, "Name": "L1", "Type": "int"}, {"ID": 3, "Name": "L3", "Type": "int"}],
				"Graph": {"Nodes": [{"ID": 3, "Type": "N"}, {"ID": 4, "Type": "N"}], "Comments": [{"ID": 4}, {"ID": 5}]}
			},
			{"ID": 5, "Name": "G", "Locals": [{"ID": 6, "Name": "L6", "Type": "int"}]},
			{"ID": 7, "Name": "H", "Graph": {"Nodes": [{"ID": 2, "Type": "N"}, {"ID": 8, "Type": "N"}]}}
		],
		"Graph": {
			"Nodes": [{"ID": 6, "Type": "N"}, {"ID": 8, "Type": "N"}, {"ID": 9, "Type": "N"}],
			"Comments": [{"ID": 9}, {"ID": 10}]
		}
	})"));
	CHECK(GetPaths(loaded.Issues) == "Functions[0].Locals[0].ID, Functions[0].Graph.Nodes[0].ID, Functions[0].Graph.Comments[0].ID, "
	                                 "Functions[1].ID, Functions[2].Graph.Nodes[0].ID, Graph.Nodes[1].ID, Graph.Comments[0].ID");

	VisualScriptDocument const& document = loaded.Document;
	REQUIRE(document.Functions.size() == 2);
	VisualScriptFunction const& f = document.Functions[0];
	REQUIRE(f.Locals.size() == 1);
	CHECK(f.Locals[0].ID == 3);
	REQUIRE(f.Graph.Nodes.size() == 1);
	CHECK(f.Graph.Nodes[0].ID == 4);
	REQUIRE(f.Graph.Comments.size() == 1);
	CHECK(f.Graph.Comments[0].ID == 5);
	CHECK(document.Functions[1].Name == "H");
	// The dropped function G took its local with it, so the event graph keeps node 6.
	REQUIRE(document.Graph.Nodes.size() == 2);
	CHECK(document.Graph.Nodes[0].ID == 6);
	CHECK(document.Graph.Nodes[1].ID == 9);
	REQUIRE(document.Graph.Comments.size() == 1);
	CHECK(document.Graph.Comments[0].ID == 10);
}

TEST_CASE("VisualScript: names are checked for their shape and their uniqueness in each scope")
{
	Json variables = Json::array();
	uint32_t id = 1;
	for (std::string name : {std::string(64, 'a'),
	                         std::string("Gr\xC3\xB6\xC3\x9F"
	                                     "e der Welt"),
	                         std::string(65, 'b'), std::string(), std::string(" Lead"), std::string("Trail "), std::string("Split.Pin"),
	                         std::string("Tab\tName"), std::string("Delete\x7F"), std::string("\xC3")})
	{
		variables.push_back(MakeVariable(id++, std::move(name)));
	}
	LoadedDocument const shapes = Load(MakeContent(variables, Json::array(), MakeGraph(Json::array())));
	CHECK(GetPaths(shapes.Issues) == "Variables[2].Name, Variables[3].Name, Variables[4].Name, Variables[5].Name, Variables[6].Name, "
	                                 "Variables[7].Name, Variables[8].Name, Variables[9].Name");
	REQUIRE(shapes.Document.Variables.size() == 2);
	CHECK(shapes.Document.Variables[1].Name == "Gr\xC3\xB6\xC3\x9F"
	                                           "e der Welt");

	// Variables, functions and parameter lists are separate scopes; locals must also differ from every variable, and
	// former names from every current variable name.
	LoadedDocument const scopes = Load(Parse(R"({
		"Variables": [
			{"ID": 1, "Name": "Score", "Type": "int"},
			{"ID": 2, "Name": "Speed", "Type": "float", "FormerNames": ["Score", "Velocity", " Pace"]}
		],
		"Functions": [
			{
				"ID": 3, "Name": "Score",
				"Inputs": [{"Name": "P", "Type": "int"}, {"Name": "P", "Type": "float"}, {"Name": "Q"}, {"Name": "R", "Type": ""}, {"Name": "S.T", "Type": "int"}],
				"Outputs": [{"Name": "P", "Type": "int"}],
				"Locals": [
					{"ID": 4, "Name": "Score", "Type": "int"},
					{"ID": 5, "Name": "Sum", "Type": "int"},
					{"ID": 6, "Name": "Sum", "Type": "int"},
					{"ID": 7, "Name": "Velocity", "Type": "int"},
					{"ID": 8, "Name": "", "Type": "int"}
				]
			},
			{"ID": 9, "Name": "Score"},
			{"ID": 10, "Name": "Bad.Name"},
			{"ID": 11, "Name": "Other", "Locals": [{"ID": 12, "Name": "Sum", "Type": "int"}]}
		]
	})"));
	CHECK(GetPaths(scopes.Issues) ==
	      "Variables[1].FormerNames[0], Variables[1].FormerNames[2], Functions[0].Inputs[1].Name, "
	      "Functions[0].Inputs[2].Type, Functions[0].Inputs[3].Type, Functions[0].Inputs[4].Name, "
	      "Functions[0].Locals[0].Name, Functions[0].Locals[2].Name, Functions[0].Locals[4].Name, Functions[1].Name, "
	      "Functions[2].Name");
	VisualScriptDocument const& document = scopes.Document;
	CHECK(document.Variables[1].FormerNames == std::vector<std::string>{"Velocity"});
	REQUIRE(document.Functions.size() == 2);
	CHECK(document.Functions[0].Inputs == std::vector<VisualScriptParameter>{{"P", "int"}});
	CHECK(document.Functions[0].Outputs == std::vector<VisualScriptParameter>{{"P", "int"}});
	REQUIRE(document.Functions[0].Locals.size() == 2);
	CHECK(document.Functions[0].Locals[0].Name == "Sum");
	CHECK(document.Functions[0].Locals[1].Name == "Velocity");
	CHECK(document.Functions[1].Locals.at(0).Name == "Sum");
}

TEST_CASE("VisualScript: type strings, pin names and member names are checked for their shape only")
{
	// Unknown node types, pins and settings are kept: their meaning is the scripting runtime's business.
	Json nodes = Json::array();
	nodes.push_back(MakeNode(1, "No.Such.Node"));
	nodes.push_back(MakeNode(2, std::string(1024, 'n')));
	nodes.push_back(MakeNode(3, std::string(1025, 'n')));
	nodes.push_back(MakeNode(4, "Line\nBreak"));
	nodes.push_back(MakeNode(5, ""));
	Json links = Json::array();
	std::string const longPin(128, 'p');
	links.push_back(Json::object({{"From", Json::array({1, longPin})}, {"To", Json::array({2, "In"})}}));
	links.push_back(Json::object({{"From", Json::array({1, longPin + "p"})}, {"To", Json::array({2, "In"})}}));
	links.push_back(Json::object({{"From", Json::array({1, "Out"})}, {"To", Json::array({2, "In\x01"})}}));
	Json variables = Json::array();
	variables.push_back(MakeVariable(10, "A", std::string(256, 't')));
	variables.push_back(MakeVariable(11, "B", std::string(257, 't')));
	variables.push_back(MakeVariable(12, "C", "Strada.Entity[]"));
	variables.push_back(MakeVariable(13, "D", "Bell\x07"));
	LoadedDocument const loaded = Load(MakeContent(variables, Json::array(), MakeGraph(nodes, links)));
	CHECK(GetPaths(loaded.Issues) == "Variables[1].Type, Variables[3].Type, Graph.Nodes[2].Type, Graph.Nodes[3].Type, Graph.Nodes[4].Type, "
	                                 "Graph.Links[1].From, Graph.Links[2].To");
	CHECK(loaded.Document.Variables.size() == 2);
	REQUIRE(loaded.Document.Graph.Nodes.size() == 2);
	CHECK(loaded.Document.Graph.Nodes[0].Type == "No.Such.Node");
	CHECK(loaded.Document.Graph.Links.size() == 1);
}

TEST_CASE("VisualScript: positions, sizes and colors are clamped or take their defaults")
{
	LoadedDocument const loaded = Load(Parse(R"({
		"Graph": {
			"Nodes": [
				{"ID": 1, "Type": "N", "Position": [1.0]},
				{"ID": 2, "Type": "N", "Position": "here"},
				{"ID": 3, "Type": "N", "Position": [1.0, "x"]},
				{"ID": 4, "Type": "N", "Position": null},
				{"ID": 5, "Type": "N", "Position": [2.0e7, -1.0e30]},
				{"ID": 6, "Type": "N", "Position": [10000000.0, -10000000.0]},
				{"ID": 7, "Type": "N", "Position": [0.25, -0.5]}
			],
			"Comments": [
				{"ID": 10, "Position": [1.0, 2.0, 3.0], "Size": [8.0, 2.0e6], "Color": [0.5, 0.5, 0.5]},
				{"ID": 11, "Position": [-3.0e7, 5.0], "Size": "big", "Color": [0.0, 1.0, 1.5, 0.0]},
				{"ID": 12, "Position": [10000000.0, -10000000.0], "Size": [16.0, 1000000.0], "Color": [0.0, 1.0, 0.0, 1.0]},
				{"ID": 13, "Color": [-0.25, 0.5, 0.5, 0.5]},
				{"ID": 14, "Size": [400.0]}
			]
		}
	})"));
	CHECK(GetPaths(loaded.Issues) ==
	      "Graph.Nodes[0].Position, Graph.Nodes[1].Position, Graph.Nodes[2].Position, Graph.Nodes[3].Position, "
	      "Graph.Nodes[4].Position, Graph.Comments[0].Position, Graph.Comments[0].Size, Graph.Comments[0].Color, "
	      "Graph.Comments[1].Position, Graph.Comments[1].Size, Graph.Comments[1].Color, Graph.Comments[3].Color, "
	      "Graph.Comments[4].Size");

	std::vector<VisualScriptNode> const& nodes = loaded.Document.Graph.Nodes;
	REQUIRE(nodes.size() == 7);
	for (size_t i = 0; i < 4; i++)
	{
		CHECK_FALSE(nodes[i].Position);
	}
	CHECK(nodes[4].Position == std::optional<glm::vec2>(glm::vec2(1.0e7f, -1.0e7f)));
	CHECK(nodes[5].Position == std::optional<glm::vec2>(glm::vec2(1.0e7f, -1.0e7f)));
	CHECK(nodes[6].Position == std::optional<glm::vec2>(glm::vec2(0.25f, -0.5f)));

	VisualScriptComment const defaults;
	std::vector<VisualScriptComment> const& comments = loaded.Document.Graph.Comments;
	REQUIRE(comments.size() == 5);
	CHECK(comments[0].Position == glm::vec2(0.0f));
	CHECK(comments[0].Size == glm::vec2(16.0f, 1.0e6f));
	CHECK(comments[0].Color == defaults.Color);
	CHECK(comments[1].Position == glm::vec2(-1.0e7f, 5.0f));
	CHECK(comments[1].Size == defaults.Size);
	CHECK(comments[1].Color == defaults.Color);
	CHECK(comments[2].Position == glm::vec2(1.0e7f, -1.0e7f));
	CHECK(comments[2].Size == glm::vec2(16.0f, 1.0e6f));
	CHECK(comments[2].Color == glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
	CHECK(comments[3].Color == defaults.Color);
	CHECK(comments[4].Size == defaults.Size);
}

TEST_CASE("VisualScript: settings, inputs and values keep only what is within the limits")
{
	std::string const longName(129, 'n');
	std::string const maxName(128, 'm');
	Json nodes = Json::array();
	nodes.push_back(Json::object({{"ID", 1}, {"Type", "N"}, {"Settings", Json::array({1})}, {"Inputs", 3}}));
	Json settings = Json::object();
	settings[""] = 1;
	settings["Good"] = 2;
	settings["Bad\x01"] = 3;
	settings[longName] = 4;
	settings[maxName] = 5;
	nodes.push_back(Json::object({{"ID", 2}, {"Type", "N"}, {"Settings", settings}}));
	Json inputs = Json::object();
	inputs["Deep"] = MakeNestedValue(17);
	inputs["Fine"] = MakeNestedValue(16);
	inputs["Long"] = std::string(65537, 's');
	inputs["Edge"] = std::string(65536, 's');
	inputs["Keys"] = Json::object({{std::string(65537, 'k'), 1}});
	// Empty arrays and objects are levels too.
	inputs["Empty"] = MakeNestedValue(17, Json::object());
	inputs["EmptyFine"] = MakeNestedValue(16, Json::array());
	nodes.push_back(Json::object({{"ID", 3}, {"Type", "N"}, {"Inputs", inputs}}));
	nodes.push_back(Json::object(
		{{"ID", 4}, {"Type", "N"}, {"Settings", Json::object({{"Null", nullptr}, {"List", Json::array()}, {"Map", Json::object()}})}}));

	Json variables = Json::array();
	variables.push_back(Json::object({{"ID", 10}, {"Name", "Deep"}, {"Type", "int"}, {"Value", MakeNestedValue(17)}}));
	variables.push_back(Json::object({{"ID", 11}, {"Name", "Long"}, {"Type", "string"}, {"Value", std::string(65537, 'v')}}));
	variables.push_back(Json::object({{"ID", 12}, {"Name", "None"}, {"Type", "int"}, {"Value", nullptr}}));
	variables.push_back(Json::object({{"ID", 13}, {"Name", "Fine"}, {"Type", "int"}, {"Value", MakeNestedValue(16)}}));
	Json functions = Json::array();
	functions.push_back(Json::object(
		{{"ID", 20},
	     {"Name", "F"},
	     {"Locals", Json::array({Json::object({{"ID", 21}, {"Name", "L"}, {"Type", "int"}, {"Value", MakeNestedValue(18)}})})}}));

	LoadedDocument const loaded = Load(MakeContent(variables, functions, MakeGraph(nodes)));
	CHECK(GetPaths(loaded.Issues) ==
	      "Variables[0].Value, Variables[1].Value, Functions[0].Locals[0].Value, Graph.Nodes[0].Settings, "
	      "Graph.Nodes[0].Inputs, Graph.Nodes[1].Settings, Graph.Nodes[1].Settings, Graph.Nodes[1].Settings, "
	      "Graph.Nodes[2].Inputs.Deep, Graph.Nodes[2].Inputs.Long, Graph.Nodes[2].Inputs.Keys, Graph.Nodes[2].Inputs.Empty");

	VisualScriptDocument const& document = loaded.Document;
	CHECK(document.Variables[0].Value.is_null());
	CHECK(document.Variables[1].Value.is_null());
	CHECK(document.Variables[2].Value.is_null());
	CHECK(document.Variables[3].Value == MakeNestedValue(16));
	CHECK(document.Functions[0].Locals[0].Value.is_null());
	std::vector<VisualScriptNode> const& kept = document.Graph.Nodes;
	CHECK(kept[0].Settings == Json::object());
	CHECK(kept[0].Inputs == Json::object());
	CHECK(kept[1].Settings == Json::object({{"Good", 2}, {maxName, 5}}));
	CHECK(kept[2].Inputs ==
	      Json::object(
			  {{"Fine", MakeNestedValue(16)}, {"Edge", std::string(65536, 's')}, {"EmptyFine", MakeNestedValue(16, Json::array())}}));
	CHECK(kept[3].Settings.size() == 3);
}

TEST_CASE("VisualScript: links need two endpoints in their own graph and are never repeated")
{
	LoadedDocument const loaded = Load(Parse(R"({
		"Functions": [{"ID": 1, "Name": "F", "Graph": {"Nodes": [{"ID": 2, "Type": "N"}]}}],
		"Graph": {
			"Nodes": [{"ID": 10, "Type": "A"}, {"ID": 11, "Type": "B"}, {"ID": 12}],
			"Links": [
				{"From": [10, "Out"], "To": [11, "In"]},
				{"From": [10, "Out"], "To": [11, "In"]},
				{"From": [10, "Out"]},
				{"To": [11, "In"]},
				{"From": [10], "To": [11, "In"]},
				{"From": [10, ""], "To": [11, "In"]},
				{"From": [10, 5], "To": [11, "In"]},
				{"From": ["10", "Out"], "To": [11, "In"]},
				{"From": [10, "Out", 1], "To": [11, "In"]},
				{"From": [10, "Out"], "To": [12, "In"]},
				{"From": [10, "Out"], "To": [2, "In"]},
				{"From": [11, "Out"], "To": [10, "In"]},
				{"From": [10, "Same"], "To": [10, "Same"]},
				{"From": [10, "Out"], "To": [11, "Other"]},
				[10, 11]
			]
		}
	})"));
	CHECK(GetPaths(loaded.Issues) ==
	      "Graph.Nodes[2].Type, Graph.Links[1], Graph.Links[2].To, Graph.Links[3].From, Graph.Links[4].From, "
	      "Graph.Links[5].From, Graph.Links[6].From, Graph.Links[7].From, Graph.Links[8].From, Graph.Links[9].To, "
	      "Graph.Links[10].To, Graph.Links[14]");
	std::vector<VisualScriptLink> const expected = {
		{{10, "Out"}, {11, "In"}}, {{11, "Out"}, {10, "In"}}, {{10, "Same"}, {10, "Same"}}, {{10, "Out"}, {11, "Other"}}};
	CHECK(loaded.Document.Graph.Links == expected);
}

TEST_CASE("VisualScript: former names are unique across the document and differ from every variable name")
{
	LoadedDocument const loaded = Load(Parse(R"({
		"Variables": [
			{"ID": 1, "Name": "Speed", "Type": "float", "FormerNames": ["Velocity", "Pace", 3, "Bad.Name", "Velocity", "Health", "Speed"]},
			{"ID": 2, "Name": "Health", "Type": "int", "FormerNames": ["Pace", "Life"]}
		]
	})"));
	CHECK(GetPaths(loaded.Issues) == "Variables[0].FormerNames[2], Variables[0].FormerNames[3], Variables[0].FormerNames[4], "
	                                 "Variables[0].FormerNames[5], Variables[0].FormerNames[6], Variables[1].FormerNames[0]");
	CHECK(loaded.Document.Variables[0].FormerNames == std::vector<std::string>{"Velocity", "Pace"});
	CHECK(loaded.Document.Variables[1].FormerNames == std::vector<std::string>{"Life"});
}

TEST_CASE("VisualScript: strict use accepts omitted optional keys and refuses every repair")
{
	// Only the required keys: everything else takes its default without an issue.
	Json const minimal = Parse(R"({
		"Variables": [{"ID": 1, "Name": "V", "Type": "int"}],
		"Functions": [{"ID": 2, "Name": "F", "Inputs": [{"Name": "P", "Type": "int"}], "Locals": [{"ID": 3, "Name": "L", "Type": "int"}]}],
		"Graph": {"Nodes": [{"ID": 4, "Type": "Flow.Branch"}], "Links": [{"From": [4, "True"], "To": [4, "Execute"]}], "Comments": [{"ID": 5}]}
	})");
	CHECK(IsStrictlyValid(Json::object()));
	CHECK(IsStrictlyValid(minimal));
	LoadedDocument const loaded = Load(minimal);
	VisualScriptDocument const& document = loaded.Document;
	CHECK(document.Description.empty());
	VisualScriptVariable const& variable = document.Variables.at(0);
	CHECK(variable.Value.is_null());
	CHECK_FALSE(variable.Public);
	CHECK(variable.Tooltip.empty());
	CHECK_FALSE(variable.Range);
	CHECK(variable.FormerNames.empty());
	VisualScriptFunction const& function = document.Functions.at(0);
	CHECK_FALSE(function.Pure);
	CHECK(function.Outputs.empty());
	CHECK(function.Locals.at(0).Value.is_null());
	CHECK(function.Graph == VisualScriptGraph{});
	VisualScriptNode const& node = document.Graph.Nodes.at(0);
	CHECK_FALSE(node.Position);
	CHECK(node.Settings == Json::object());
	CHECK(node.Inputs == Json::object());
	CHECK(document.Graph.Comments.at(0) ==
	      VisualScriptComment{5, "", glm::vec2(0.0f), glm::vec2(400.0f, 200.0f), glm::vec4(0.5f, 0.5f, 0.5f, 0.25f)});

	// Defaults written out, and "Value": null (no value), are no issues either.
	Json explicitDefaults = minimal;
	explicitDefaults["Description"] = "";
	explicitDefaults["Variables"][0].update(Parse(R"({"Value": null, "Public": false, "Tooltip": "", "FormerNames": []})"));
	explicitDefaults["Graph"]["Nodes"][0].update(Parse(R"({"Settings": {}, "Inputs": {}})"));
	explicitDefaults["Graph"]["Comments"][0].update(
		Parse(R"({"Text": "", "Position": [0, 0], "Size": [400, 200], "Color": [0.5, 0.5, 0.5, 0.25]})"));
	CHECK(IsStrictlyValid(explicitDefaults));
	CHECK(Load(explicitDefaults).Document == document);

	// One repair anywhere makes the input invalid.
	auto const damaged = [&minimal](auto const& damage)
	{
		Json content = minimal;
		damage(content);
		return content;
	};
	CHECK_FALSE(IsStrictlyValid(damaged(
		[](Json& content)
		{
			content["Variables"][0]["Public"] = "true";
		})));
	CHECK_FALSE(IsStrictlyValid(damaged(
		[](Json& content)
		{
			content["Graph"]["Nodes"][0]["Position"] = Json::array({1});
		})));
	CHECK_FALSE(IsStrictlyValid(damaged(
		[](Json& content)
		{
			content["Graph"]["Links"][0]["To"][0] = 9;
		})));
	CHECK_FALSE(IsStrictlyValid(damaged(
		[](Json& content)
		{
			content["Functions"][0]["Locals"][0]["Name"] = "V";
		})));
	CHECK_FALSE(IsStrictlyValid(damaged(
		[](Json& content)
		{
			content["Graph"]["Comments"][0]["Color"] = Json::array({2, 0, 0, 1});
		})));
	CHECK_FALSE(IsStrictlyValid(damaged(
		[](Json& content)
		{
			content["Layout"] = "auto";
		})));
}

TEST_CASE("VisualScript: elements beyond the limits of a document are dropped, the later ones first")
{
	static_assert(VisualScriptSerializer::MaxVariables == 1024 && VisualScriptSerializer::MaxFunctions == 1024);
	static_assert(VisualScriptSerializer::MaxParameters == 64 && VisualScriptSerializer::MaxLocals == 256);
	static_assert(VisualScriptSerializer::MaxFormerNames == 16);

	auto const makeVariables = [](size_t count)
	{
		Json variables = Json::array();
		for (size_t i = 0; i < count; i++)
		{
			variables.push_back(MakeVariable(static_cast<uint32_t>(i + 1), fmt::format("V{}", i)));
		}
		return variables;
	};
	CHECK(IsStrictlyValid(MakeContent(makeVariables(1024), Json::array(), MakeGraph(Json::array()))));
	LoadedDocument const variables = Load(MakeContent(makeVariables(1025), Json::array(), MakeGraph(Json::array())));
	CHECK(GetPaths(variables.Issues) == "Variables[1024]");
	CHECK(variables.Document.Variables.back().Name == "V1023");

	auto const makeFunctions = [](size_t count)
	{
		Json functions = Json::array();
		for (size_t i = 0; i < count; i++)
		{
			functions.push_back(Json::object({{"ID", i + 1}, {"Name", fmt::format("F{}", i)}}));
		}
		return functions;
	};
	CHECK(IsStrictlyValid(MakeContent(Json::array(), makeFunctions(1024), MakeGraph(Json::array()))));
	LoadedDocument const functions = Load(MakeContent(Json::array(), makeFunctions(1025), MakeGraph(Json::array())));
	CHECK(GetPaths(functions.Issues) == "Functions[1024]");
	CHECK(functions.Document.Functions.size() == 1024);

	auto const makeFunction = [](size_t parameters, size_t locals)
	{
		Json inputs = Json::array();
		Json outputs = Json::array();
		for (size_t i = 0; i < parameters; i++)
		{
			inputs.push_back(Json::object({{"Name", fmt::format("In{}", i)}, {"Type", "int"}}));
			outputs.push_back(Json::object({{"Name", fmt::format("Out{}", i)}, {"Type", "int"}}));
		}
		Json localList = Json::array();
		for (size_t i = 0; i < locals; i++)
		{
			localList.push_back(MakeVariable(static_cast<uint32_t>(i + 2), fmt::format("L{}", i)));
		}
		return Json::array({Json::object({{"ID", 1}, {"Name", "F"}, {"Inputs", inputs}, {"Outputs", outputs}, {"Locals", localList}})});
	};
	CHECK(IsStrictlyValid(MakeContent(Json::array(), makeFunction(64, 256), MakeGraph(Json::array()))));
	LoadedDocument const lists = Load(MakeContent(Json::array(), makeFunction(65, 257), MakeGraph(Json::array())));
	CHECK(GetPaths(lists.Issues) == "Functions[0].Inputs[64], Functions[0].Outputs[64], Functions[0].Locals[256]");
	VisualScriptFunction const& function = lists.Document.Functions.at(0);
	CHECK(function.Inputs.size() == 64);
	CHECK(function.Outputs.size() == 64);
	CHECK(function.Locals.size() == 256);
	CHECK(function.Locals.back().Name == "L255");

	Json formerNames = Json::array();
	for (size_t i = 0; i < 17; i++)
	{
		formerNames.push_back(fmt::format("Old{}", i));
	}
	Json variable = MakeVariable(1, "V");
	variable["FormerNames"] = formerNames;
	LoadedDocument const former = Load(MakeContent(Json::array({variable}), Json::array(), MakeGraph(Json::array())));
	CHECK(GetPaths(former.Issues) == "Variables[0].FormerNames[16]");
	CHECK(former.Document.Variables.at(0).FormerNames.size() == 16);
	CHECK(former.Document.Variables.at(0).FormerNames.back() == "Old15");
}

TEST_CASE("VisualScript: nodes, links and comments beyond the limits of a graph are dropped, the later ones first")
{
	static_assert(VisualScriptSerializer::MaxNodesPerGraph == 10000 && VisualScriptSerializer::MaxLinksPerGraph == 40000);
	static_assert(VisualScriptSerializer::MaxCommentsPerGraph == 2000);

	// One more than each limit in the event graph. The function's graph, read first, does not count towards them: the
	// limits hold per graph. (Large documents load slowly in Debug builds, so this one loads once, without the round trip
	// the smaller documents of these tests go through.)
	Json nodes = Json::array();
	for (uint32_t id = 1; id <= 10001; id++)
	{
		nodes.push_back(MakeNode(id));
	}
	Json links = Json::array();
	for (size_t i = 0; i <= 40000; i++)
	{
		links.push_back(Json::object({{"From", Json::array({1, fmt::format("Out{}", i)})}, {"To", Json::array({2, "In"})}}));
	}
	Json comments = Json::array();
	for (uint32_t id = 20001; id <= 22001; id++)
	{
		comments.push_back(Json::object({{"ID", id}}));
	}
	Json function = Parse(R"({
		"ID": 30000, "Name": "F",
		"Graph": {"Nodes": [{"ID": 30001, "Type": "N"}], "Links": [{"From": [30001, "Then"], "To": [30001, "Execute"]}], "Comments": [{"ID": 30002}]}
	})");
	Json const content =
		MakeContent(Json::array(), Json::array({std::move(function)}), MakeGraph(std::move(nodes), std::move(links), std::move(comments)));

	std::vector<VisualScriptIssue> issues;
	Result<VisualScriptDocument> loaded =
		VisualScriptSerializer::DeserializeDocument(content, MakeContext(UnknownFieldPolicy::Warn), &issues);
	REQUIRE(loaded.IsOk());
	CHECK(GetPaths(issues) == "Graph.Nodes[10000], Graph.Links[40000], Graph.Comments[2000]");
	VisualScriptGraph const& graph = loaded.GetValue().Graph;
	CHECK(graph.Nodes.size() == 10000);
	CHECK(graph.Nodes.back().ID == 10000);
	CHECK(graph.Links.size() == 40000);
	CHECK(graph.Links.back().From.Pin == "Out39999");
	CHECK(graph.Comments.size() == 2000);
	CHECK(graph.Comments.back().ID == 22000);
	VisualScriptGraph const& functionGraph = loaded.GetValue().Functions.at(0).Graph;
	CHECK(functionGraph.Nodes.size() == 1);
	CHECK(functionGraph.Links.size() == 1);
	CHECK(functionGraph.Comments.size() == 1);
}

TEST_CASE("VisualScript: text, settings and values at their limits are kept, one more is repaired")
{
	static_assert(VisualScriptSerializer::MaxTextBytes == 65536 && VisualScriptSerializer::MaxSettingsMembers == 256);
	static_assert(VisualScriptSerializer::MaxValueDepth == 16 && VisualScriptSerializer::MaxNameBytes == 64);

	std::string const maxText(65536, 't');
	Json variables = Json::array({MakeVariable(1, "A"), MakeVariable(2, "B")});
	variables[0]["Tooltip"] = maxText;
	variables[1]["Tooltip"] = maxText + "t";
	Json comments = Json::array({Json::object({{"ID", 3}, {"Text", maxText}}), Json::object({{"ID", 4}, {"Text", maxText + "t"}})});
	Json settings = Json::object();
	Json inputs = Json::object();
	for (size_t i = 0; i < 257; i++)
	{
		settings[fmt::format("S{}", i)] = i;
		if (i < 256)
		{
			inputs[fmt::format("I{}", i)] = i;
		}
	}
	Json nodes = Json::array({Json::object({{"ID", 5}, {"Type", "N"}, {"Settings", settings}, {"Inputs", inputs}})});
	Json content = MakeContent(variables, Json::array(), MakeGraph(nodes, Json::array(), comments));
	content["Description"] = maxText + "t";

	LoadedDocument const loaded = Load(content);
	CHECK(GetPaths(loaded.Issues) == "Description, Variables[1].Tooltip, Graph.Nodes[0].Settings.S256, Graph.Comments[1].Text");
	VisualScriptDocument const& document = loaded.Document;
	CHECK(document.Description.empty());
	CHECK(document.Variables[0].Tooltip == maxText);
	CHECK(document.Variables[1].Tooltip.empty());
	CHECK(document.Graph.Comments[0].Text == maxText);
	CHECK(document.Graph.Comments[1].Text.empty());
	CHECK(document.Graph.Nodes[0].Settings.size() == 256);
	CHECK_FALSE(document.Graph.Nodes[0].Settings.contains("S256"));
	CHECK(document.Graph.Nodes[0].Inputs.size() == 256);

	content["Description"] = maxText;
	content["Variables"][1].erase("Tooltip");
	content["Graph"]["Comments"].erase(1);
	content["Graph"]["Nodes"][0]["Settings"].erase("S256");
	CHECK(IsStrictlyValid(content));
}

TEST_CASE("VisualScript: the writer omits defaults, keeps raw values as stored and widens floats")
{
	VisualScriptDocument document;
	VisualScriptVariable plain;
	plain.ID = 1;
	plain.Name = "Plain";
	plain.Type = "int";
	VisualScriptVariable full;
	full.ID = 2;
	full.Name = "Full";
	full.Type = "float";
	full.Value = 0.5;
	full.Public = true;
	full.Tooltip = "Tip";
	full.Range = glm::vec2(0.2f, 1.0f);
	full.FormerNames = {"Old"};
	document.Variables = {plain, full};
	VisualScriptFunction function;
	function.ID = 3;
	function.Name = "F";
	function.Locals = {VisualScriptLocal{4, "L", "int", Json()}, VisualScriptLocal{5, "M", "Strada.Vector3", Json::array({1, 2, 3})}};
	document.Functions = {function};
	VisualScriptNode unplaced;
	unplaced.ID = 6;
	unplaced.Type = "Flow.Branch";
	VisualScriptNode placed;
	placed.ID = 7;
	placed.Type = "Literal";
	placed.Position = glm::vec2(0.2f, -16.0f);
	placed.Settings = Json::object({{"Type", "int"}, {"Value", 3}});
	placed.Inputs = Json::object({{"Z", 1}, {"A", Json::object({{"Y", true}, {"B", nullptr}})}});
	document.Graph.Nodes = {unplaced, placed};
	document.Graph.Links = {VisualScriptLink{{7, "Value"}, {6, "Condition"}}};
	document.Graph.Comments = {VisualScriptComment{8, "", glm::vec2(0.0f), glm::vec2(400.0f, 200.0f), glm::vec4(0.5f, 0.5f, 0.5f, 0.25f)}};

	std::string const expected = DumpJson(Parse(R"({
		"Description": "",
		"Variables": [
			{"ID": 1, "Name": "Plain", "Type": "int"},
			{"ID": 2, "Name": "Full", "Type": "float", "Value": 0.5, "Public": true, "Tooltip": "Tip", "Range": [0.20000000298023224, 1.0], "FormerNames": ["Old"]}
		],
		"Functions": [
			{
				"ID": 3, "Name": "F", "Pure": false, "Inputs": [], "Outputs": [],
				"Locals": [{"ID": 4, "Name": "L", "Type": "int"}, {"ID": 5, "Name": "M", "Type": "Strada.Vector3", "Value": [1, 2, 3]}],
				"Graph": {"Nodes": [], "Links": [], "Comments": []}
			}
		],
		"Graph": {
			"Nodes": [
				{"ID": 6, "Type": "Flow.Branch"},
				{"ID": 7, "Type": "Literal", "Position": [0.20000000298023224, -16.0], "Settings": {"Type": "int", "Value": 3}, "Inputs": {"Z": 1, "A": {"Y": true, "B": null}}}
			],
			"Links": [{"From": [7, "Value"], "To": [6, "Condition"]}],
			"Comments": [{"ID": 8, "Text": "", "Position": [0.0, 0.0], "Size": [400.0, 200.0], "Color": [0.5, 0.5, 0.5, 0.25]}]
		}
	})"));
	Json const content = VisualScriptSerializer::SerializeDocument(document);
	CHECK(DumpJson(content) == expected);
	CHECK(content["Variables"][0]["ID"].is_number_unsigned());
	CHECK(content["Graph"]["Links"][0]["From"][0].is_number_unsigned());
	CheckSavesAndLoadsAgain(document);

	// The empty document still has every content key; the file form adds the header.
	CHECK(DumpJson(VisualScriptSerializer::SerializeDocument({})) ==
	      DumpJson(Parse(R"({"Description": "", "Variables": [], "Functions": [], "Graph": {"Nodes": [], "Links": [], "Comments": []}})")));
	Json const file = VisualScriptSerializer::Serialize(document);
	CHECK(file["Strada"] == Parse(R"({"Version": 1, "Type": "VisualScript"})"));
	CHECK(file.size() == 2);
	CHECK(VisualScriptSerializer::FormatVersion == 1);
}

TEST_CASE("VisualScript: unplaced nodes, locals and former names survive saving and loading")
{
	VisualScriptDocument document = MakeVisualScriptTemplate(VisualScriptTemplate::Events);
	VisualScriptVariable speed;
	speed.ID = 3;
	speed.Name = "Speed";
	speed.Type = "float";
	speed.Value = 4.0;
	speed.Public = true;
	speed.FormerNames = {"Velocity", "Pace"};
	document.Variables.push_back(speed);
	VisualScriptFunction accumulate;
	accumulate.ID = 4;
	accumulate.Name = "Accumulate";
	accumulate.Inputs = {{"Amount", "int"}};
	accumulate.Outputs = {{"Total", "int"}};
	accumulate.Locals = {VisualScriptLocal{5, "Sum", "int", 0}, VisualScriptLocal{6, "Step", "int", Json()}};
	VisualScriptNode entry;
	entry.ID = 7;
	entry.Type = "Function.Entry";
	accumulate.Graph.Nodes.push_back(entry);
	document.Functions.push_back(accumulate);
	VisualScriptNode unplaced;
	unplaced.ID = 8;
	unplaced.Type = "Flow.Branch";
	document.Graph.Nodes.push_back(unplaced);

	Testing::TemporaryDirectory directory;
	std::filesystem::path const path = directory.GetPath() / "Scripts" / "Accumulate.sgraph";
	REQUIRE(VisualScriptSerializer::SaveToFile(document, path).IsOk());
	CHECK(ReadText(path) == DumpJson(VisualScriptSerializer::Serialize(document)));
	std::vector<VisualScriptIssue> issues;
	Result<VisualScriptDocument> loaded = VisualScriptSerializer::LoadFromFile(path, MakeContext(UnknownFieldPolicy::Error), &issues);
	REQUIRE_MESSAGE(loaded.IsOk(), loaded.GetError());
	CHECK(issues.empty());
	CHECK(loaded.GetValue() == document);
	CHECK_FALSE(loaded.GetValue().Graph.Nodes.back().Position);
	CHECK_FALSE(loaded.GetValue().Functions.at(0).Graph.Nodes.at(0).Position);
	CHECK(loaded.GetValue().Functions.at(0).Locals == accumulate.Locals);
	CHECK(loaded.GetValue().Variables.at(0).FormerNames == speed.FormerNames);
}

TEST_CASE("VisualScript: templates start new visual scripts")
{
	VisualScriptDocument const events = MakeVisualScriptTemplate(VisualScriptTemplate::Events);
	REQUIRE(events.Graph.Nodes.size() == 2);
	CHECK(events.Graph.Nodes[0].ID == 1);
	CHECK(events.Graph.Nodes[0].Type == "Event.Create");
	CHECK(events.Graph.Nodes[0].Position == std::optional<glm::vec2>(glm::vec2(0.0f, 0.0f)));
	CHECK(events.Graph.Nodes[1].ID == 2);
	CHECK(events.Graph.Nodes[1].Type == "Event.Update");
	CHECK(events.Graph.Nodes[1].Position == std::optional<glm::vec2>(glm::vec2(0.0f, 160.0f)));
	VisualScriptDocument withoutNodes = events;
	withoutNodes.Graph.Nodes.clear();
	CHECK(withoutNodes == VisualScriptDocument{});
	CHECK(IsStrictlyValid(VisualScriptSerializer::SerializeDocument(events)));
	CheckSavesAndLoadsAgain(events);

	CHECK(MakeVisualScriptTemplate(VisualScriptTemplate::Empty) == VisualScriptDocument{});
	CheckSavesAndLoadsAgain(MakeVisualScriptTemplate(VisualScriptTemplate::Empty));
}

TEST_CASE("VisualScript: the asset holds a document and counts its changes")
{
	CHECK(VisualScriptAsset::GetStaticType() == AssetType::VisualScript);
	CHECK(static_cast<int>(AssetType::VisualScript) == 9);
	CHECK(std::string_view(AssetTypeToString(AssetType::VisualScript)) == "VisualScript");
	CHECK(AssetTypeFromString("VisualScript") == AssetType::VisualScript);

	VisualScriptAsset asset(MakeVisualScriptTemplate(VisualScriptTemplate::Events));
	CHECK(asset.GetAssetType() == AssetType::VisualScript);
	CHECK(asset.GetVersion() == 0);
	CHECK(asset.GetDocument() == MakeVisualScriptTemplate(VisualScriptTemplate::Events));
	VisualScriptDocument changed = asset.GetDocument();
	changed.Description = "Spins";
	asset.SetDocument(changed);
	CHECK(asset.GetVersion() == 1);
	CHECK(asset.GetDocument() == changed);
	asset.SetDocument(VisualScriptDocument{});
	CHECK(asset.GetVersion() == 2);
	CHECK(VisualScriptAsset().GetDocument() == VisualScriptDocument{});
}

TEST_CASE("VisualScript: the asset manager loads .sgraph files and logs their repairs with the file")
{
	Testing::AssetManagerScope scope;
	Testing::TemporaryDirectory directory;
	std::filesystem::path const assets = directory.GetPath() / "Assets";
	REQUIRE(VisualScriptSerializer::SaveToFile(MakeVisualScriptTemplate(VisualScriptTemplate::Events), assets / "Scripts" / "Spin.sgraph")
	            .IsOk());
	REQUIRE(FileSystem::WriteTextFile(assets / "Scripts" / "Damaged.sgraph",
	                                  MakeFileText(ValidHeader, R"({"Variables": [{"ID": 1, "Name": "Speed"}], "Layout": "auto"})"))
	            .IsOk());
	REQUIRE(FileSystem::WriteTextFile(assets / "Scripts" / "Newer.sgraph", MakeFileText(R"({"Version": 2, "Type": "VisualScript"})", "{}"))
	            .IsOk());
	REQUIRE(AssetManager::OpenAssetDirectory(assets).IsOk());

	AssetHandle const spin = AssetManager::FindByPath("Scripts/Spin.sgraph");
	CHECK(AssetManager::GetAssetType(spin) == AssetType::VisualScript);
	CHECK(AssetManager::GetAssets(AssetType::VisualScript).size() == 3);
	Ref<VisualScriptAsset> const asset = AssetManager::GetAsset<VisualScriptAsset>(spin);
	REQUIRE(asset);
	CHECK(asset->Handle == spin);
	CHECK(asset->GetDocument() == MakeVisualScriptTemplate(VisualScriptTemplate::Events));

	uint64_t const logStart = Log::GetNextEntryIndex();
	Ref<VisualScriptAsset> const damaged = AssetManager::GetAsset<VisualScriptAsset>(AssetManager::FindByPath("Scripts/Damaged.sgraph"));
	REQUIRE(damaged);
	CHECK(damaged->GetDocument().Variables.empty());
	std::vector<LogEntry> const entries = Log::GetEntries(logStart);
	auto const logged = [&entries](std::string_view text)
	{
		return std::any_of(entries.begin(), entries.end(),
		                   [text](LogEntry const& entry)
		                   {
							   return entry.Severity == LogLevel::Warn && Contains(entry.Message, text);
						   });
	};
	CHECK(logged("'Scripts/Damaged.sgraph': Variables[0].Type: "));
	CHECK(logged("'Scripts/Damaged.sgraph': Layout: "));

	AssetHandle const newer = AssetManager::FindByPath("Scripts/Newer.sgraph");
	CHECK(AssetManager::LoadAsset(newer).IsError());
	CHECK(AssetManager::HasLoadFailure(newer));
	CHECK_FALSE(AssetManager::GetAsset<VisualScriptAsset>(newer));
}
