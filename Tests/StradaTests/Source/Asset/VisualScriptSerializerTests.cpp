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
#include <optional>
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
	constexpr std::string_view Version2Header = R"({"Version": 2, "Type": "VisualScript"})";
	constexpr std::string_view Version3Header = R"({"Version": 3, "Type": "VisualScript"})";

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

	// The text writers write what nlohmann writes of the Json writers' documents: the file form as DumpJson writes it, the
	// content form compact.
	void CheckTextWriters(VisualScriptDocument const& document)
	{
		CHECK(VisualScriptSerializer::WriteText(document) == DumpJson(VisualScriptSerializer::Serialize(document)));
		CHECK(VisualScriptSerializer::WriteContentText(document) ==
		      VisualScriptSerializer::SerializeDocument(document).dump(-1, ' ', false, Json::error_handler_t::replace));
	}

	// A graph of nodeCount nodes with every key the writer writes: positions on and off the grid and unplaced nodes, settings
	// and inputs of every kind of value, split pins, comments, collapsed and disabled nodes, 1.5 links per node and a comment
	// box per 100 nodes.
	VisualScriptGraph MakeWriterGraph(uint32_t firstID, uint32_t nodeCount)
	{
		VisualScriptGraph graph;
		for (uint32_t index = 0; index < nodeCount; index++)
		{
			VisualScriptNode node;
			node.ID = firstID + index;
			node.Type = index % 3 == 0 ? "Operator.Add" : "Call:Strada.Log.Info(string)";
			if (index % 17 != 0)
			{
				node.Position = glm::vec2(static_cast<float>(index % 50) * 320.0f + static_cast<float>(index % 7) * 0.1f,
				                          static_cast<float>(index / 50) * -160.5f);
			}
			if (index % 4 == 0)
			{
				node.Settings = Json::object({{"Function", index}, {"Offset", Json::array({0.25 * index, -1.0e-7, 1.0e16})}});
			}
			node.Inputs = Json::object({{"B", 1.5 + index}, {"Message", "Line \"" + std::to_string(index) + "\"\n\tnext \xC3\xA9"}});
			if (index % 9 == 0)
			{
				node.Inputs["Flags"] =
					Json::array({true, nullptr, std::numeric_limits<int64_t>::min(), std::numeric_limits<uint64_t>::max()});
				node.Split.Inputs = {"Hit", "Hit.Point"};
			}
			if (index % 11 == 0)
			{
				node.Split.Outputs = {"Result"};
				node.Comment = "Adds \xE2\x86\x92 logs";
				node.Collapsed = true;
				node.Enabled = index % 22 == 0 ? VisualScriptNodeEnabled::Disabled : VisualScriptNodeEnabled::DevelopmentOnly;
			}
			graph.Nodes.push_back(std::move(node));
		}
		for (uint32_t index = 0; index + 1 < nodeCount; index++)
		{
			graph.Links.push_back({{firstID + index, "Result"}, {firstID + index + 1, "A"}});
			if (index % 2 == 0 && index + 2 < nodeCount)
			{
				graph.Links.push_back({{firstID + index, "Then"}, {firstID + index + 2, "Execute"}});
			}
		}
		for (uint32_t index = 0; index < nodeCount / 100; index++)
		{
			VisualScriptComment comment;
			comment.ID = firstID + nodeCount + index;
			comment.Text = "Block " + std::to_string(index);
			comment.Position = glm::vec2(static_cast<float>(index) * 640.0f - 32.5f, -72.0f);
			comment.Size = glm::vec2(584.0f, 400.25f);
			comment.Color = glm::vec4(0.1f, 0.2f, 0.3f, 0.45f);
			graph.Comments.push_back(std::move(comment));
		}
		return graph;
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
	CHECK(VisualScriptSerializer::WriteText(document.GetValue()) == text);

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

TEST_CASE("VisualScript: the text writers write what the Json writers write, for every fixture, the FeatureTest graphs and 2,000 nodes")
{
	// The shared fixture documents (content form) and the fixture files, the old versions' as they load.
	constexpr std::array<std::string_view, 8> Documents = {"Data.json",      "Editor.json",   "Events.json",    "Flow.json",
	                                                       "Functions.json", "Language.json", "Reflected.json", "Variables.json"};
	for (std::string_view const name : Documents)
	{
		CAPTURE(name);
		CheckTextWriters(Load(Parse(ReadText(GetFixturePath("Documents") / FileSystem::PathFromUtf8(name)))).Document);
	}
	for (std::string_view const name : {"Example.sgraph", "Library.sgraph", "Version1/Example.sgraph", "Version2/Example.sgraph"})
	{
		CAPTURE(name);
		std::string const text = ReadText(GetFixturePath(name));
		Result<VisualScriptDocument> const document =
			VisualScriptSerializer::Deserialize(Parse(text), MakeContext(UnknownFieldPolicy::Warn));
		REQUIRE_MESSAGE(document.IsOk(), document.GetError());
		CheckTextWriters(document.GetValue());
		if (Parse(text)["Strada"]["Version"] == VisualScriptSerializer::FormatVersion)
		{
			CHECK(VisualScriptSerializer::WriteText(document.GetValue()) == text);
		}
	}

	// The FeatureTest project's graphs (FormatSample, Features, Contacts, Clicks): their files are the text written.
	std::filesystem::path const scripts = FileSystem::PathFromUtf8(STRADA_FEATURE_TEST_DIR) / "Assets" / "Scripts";
	size_t graphs = 0;
	for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(scripts))
	{
		if (entry.path().extension() != ".sgraph")
		{
			continue;
		}
		CAPTURE(FileSystem::PathToUtf8(entry.path()));
		graphs++;
		std::string const text = ReadText(entry.path());
		Result<VisualScriptDocument> const document =
			VisualScriptSerializer::Deserialize(Parse(text), MakeContext(UnknownFieldPolicy::Warn));
		REQUIRE_MESSAGE(document.IsOk(), document.GetError());
		CheckTextWriters(document.GetValue());
		CHECK(VisualScriptSerializer::WriteText(document.GetValue()) == text);
	}
	CHECK(graphs >= 4);

	// A library of 2,000 nodes in its event graph and 200 in a function, with variables, parameters and locals of every
	// optional key.
	VisualScriptDocument large;
	large.Kind = VisualScriptDocumentKind::Library;
	large.Description = "Generated \"large\" graph\nwith a second line, a tab\t, DEL \x7F and \xF0\x9F\x98\x80";
	for (uint32_t index = 0; index < 8; index++)
	{
		VisualScriptVariable variable;
		variable.ID = 1 + index;
		variable.Name = "Variable" + std::to_string(index);
		variable.Type = index % 2 == 0 ? "float" : "Strada.Vector3";
		if (index % 3 != 0)
		{
			variable.Value = index % 2 == 0 ? Json(0.1 * index) : Json::array({1, -2.5, 3e-5});
		}
		variable.Public = index % 2 == 0;
		variable.Tooltip = index % 4 == 0 ? "Meters per second" : "";
		if (index % 5 == 0)
		{
			variable.Range = glm::vec2(-0.5f, 20.75f);
		}
		if (index == 7)
		{
			variable.FormerNames = {"Old", "Older"};
		}
		large.Variables.push_back(std::move(variable));
	}
	VisualScriptFunction function;
	function.ID = 100;
	function.Name = "Clamp01";
	function.Pure = true;
	function.Inputs = {{"Value", "float", {"X", "Y"}}, {"Low", "float"}};
	function.Outputs = {{"Result", "float"}};
	VisualScriptLocal local;
	local.ID = 101;
	local.Name = "Scratch";
	local.Type = "int";
	local.Value = 7;
	function.Locals = {local, {102, "Empty", "string", Json()}};
	function.Graph = MakeWriterGraph(200, 200);
	large.Functions.push_back(std::move(function));
	large.Graph = MakeWriterGraph(1000, 2000);
	REQUIRE(large.Graph.Links.size() == 2998);
	CheckTextWriters(large);
	CheckSavesAndLoadsAgain(large);
	VisualScriptDocument script = large;
	script.Kind = VisualScriptDocumentKind::Script;
	CheckTextWriters(script);
	CheckTextWriters(VisualScriptDocument());
}

TEST_CASE("VisualScript: a file of version 1 loads without issues and saves as the current version, its content unchanged")
{
	// Version1/Example.sgraph keeps the bytes Example.sgraph had in version 1 of the format.
	std::string const old = ReadText(GetFixturePath("Version1/Example.sgraph"));
	std::string const current = ReadText(GetFixturePath("Example.sgraph"));
	Json const oldFile = Parse(old);
	REQUIRE(oldFile["Strada"]["Version"] == 1);
	CHECK(Parse(current)["Strada"]["Version"] == 3);

	std::vector<std::string> warnings;
	std::vector<VisualScriptIssue> issues;
	Result<VisualScriptDocument> migrated =
		VisualScriptSerializer::Deserialize(oldFile, MakeContext(UnknownFieldPolicy::Error, &warnings), &issues);
	REQUIRE_MESSAGE(migrated.IsOk(), migrated.GetError());
	CHECK(GetPaths(issues).empty());
	CHECK(warnings.empty());
	Result<VisualScriptDocument> example = VisualScriptSerializer::Deserialize(Parse(current), MakeContext(UnknownFieldPolicy::Error));
	REQUIRE_MESSAGE(example.IsOk(), example.GetError());
	CHECK(migrated.GetValue() == example.GetValue());
	CHECK(DumpJson(VisualScriptSerializer::Serialize(migrated.GetValue())) == current);

	// Only the digit of the header differs.
	constexpr std::string_view OldVersion = "\"Version\": 1,";
	std::string withNewHeader = old;
	size_t const digit = withNewHeader.find(OldVersion);
	REQUIRE(digit != std::string::npos);
	withNewHeader.replace(digit, OldVersion.size(), "\"Version\": 3,");
	CHECK(withNewHeader == current);

	// Every node of a file of version 1 has the defaults of the keys version 2 added.
	VisualScriptDocument const& document = migrated.GetValue();
	std::vector<VisualScriptGraph const*> graphs = {&document.Graph};
	for (VisualScriptFunction const& function : document.Functions)
	{
		graphs.push_back(&function.Graph);
	}
	for (VisualScriptGraph const* graph : graphs)
	{
		for (VisualScriptNode const& node : graph->Nodes)
		{
			CAPTURE(node.ID);
			CHECK(node.Split.IsEmpty());
			CHECK(node.Comment.empty());
			CHECK_FALSE(node.Collapsed);
			CHECK(node.Enabled == VisualScriptNodeEnabled::Enabled);
		}
	}
}

TEST_CASE("VisualScript: a file of version 2 loads without issues and saves as the current version, its content unchanged")
{
	// Version2/Example.sgraph keeps the bytes Example.sgraph had in version 2 of the format.
	std::string const old = ReadText(GetFixturePath("Version2/Example.sgraph"));
	std::string const current = ReadText(GetFixturePath("Example.sgraph"));
	Json const oldFile = Parse(old);
	REQUIRE(oldFile["Strada"]["Version"] == 2);

	std::vector<std::string> warnings;
	std::vector<VisualScriptIssue> issues;
	Result<VisualScriptDocument> migrated =
		VisualScriptSerializer::Deserialize(oldFile, MakeContext(UnknownFieldPolicy::Error, &warnings), &issues);
	REQUIRE_MESSAGE(migrated.IsOk(), migrated.GetError());
	CHECK(GetPaths(issues).empty());
	CHECK(warnings.empty());
	Result<VisualScriptDocument> example = VisualScriptSerializer::Deserialize(Parse(current), MakeContext(UnknownFieldPolicy::Error));
	REQUIRE_MESSAGE(example.IsOk(), example.GetError());
	CHECK(migrated.GetValue() == example.GetValue());
	CHECK(migrated.GetValue().Kind == VisualScriptDocumentKind::Script);
	CHECK(DumpJson(VisualScriptSerializer::Serialize(migrated.GetValue())) == current);

	// Only the digit of the header differs.
	constexpr std::string_view OldVersion = "\"Version\": 2,";
	std::string withNewHeader = old;
	size_t const digit = withNewHeader.find(OldVersion);
	REQUIRE(digit != std::string::npos);
	withNewHeader.replace(digit, OldVersion.size(), "\"Version\": 3,");
	CHECK(withNewHeader == current);
}

TEST_CASE("VisualScript: the shared fixture documents load strictly, round trip and keep the writer's layout")
{
	constexpr std::array<std::string_view, 8> Documents = {"Data.json",      "Editor.json",   "Events.json",    "Flow.json",
	                                                       "Functions.json", "Language.json", "Reflected.json", "Variables.json"};
	for (std::string_view const name : Documents)
	{
		CAPTURE(name);
		std::string const text = ReadText(GetFixturePath("Documents") / FileSystem::PathFromUtf8(name));
		LoadedDocument const loaded = Load(Parse(text));
		CHECK(GetPaths(loaded.Issues).empty());
		CHECK(DumpJson(VisualScriptSerializer::SerializeDocument(loaded.Document)) == text);
		CHECK_FALSE(loaded.Document.Graph.Comments.empty());
	}

	// The library fixture is a file of version 3: its Kind is written first, its renamed parameter's former names last.
	std::string const text = ReadText(GetFixturePath("Library.sgraph"));
	std::vector<VisualScriptIssue> issues;
	Result<VisualScriptDocument> library = VisualScriptSerializer::Deserialize(Parse(text), MakeContext(UnknownFieldPolicy::Warn), &issues);
	REQUIRE_MESSAGE(library.IsOk(), library.GetError());
	CHECK(GetPaths(issues).empty());
	CHECK(DumpJson(VisualScriptSerializer::Serialize(library.GetValue())) == text);
	CheckSavesAndLoadsAgain(library.GetValue());
	VisualScriptDocument const& document = library.GetValue();
	CHECK(document.Kind == VisualScriptDocumentKind::Library);
	CHECK(document.Variables.empty());
	CHECK(document.Graph.Nodes.empty());
	CHECK_FALSE(document.Graph.Comments.empty());
	REQUIRE(document.Functions.size() == 3);
	CHECK(document.Functions[0].Inputs == std::vector<VisualScriptParameter>{{"Value", "float", {"X"}}});
	size_t unplaced = 0;
	for (VisualScriptFunction const& function : document.Functions)
	{
		for (VisualScriptNode const& node : function.Graph.Nodes)
		{
			unplaced += node.Position.has_value() ? 0 : 1;
		}
	}
	CHECK(unplaced >= 2);
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

	// Every node key of version 2: split inputs on one node and nested split outputs on another, comments, collapsed nodes
	// and both states other than Enabled, on placed and on unplaced nodes and in a function graph.
	auto const hasVersion2Key = [](VisualScriptNode const& node)
	{
		return !node.Split.IsEmpty() || !node.Comment.empty() || node.Collapsed || node.Enabled != VisualScriptNodeEnabled::Enabled;
	};
	VisualScriptNode const* splitInputs = nullptr;
	VisualScriptNode const* nestedSplitOutputs = nullptr;
	bool comment = false;
	bool collapsed = false;
	bool disabled = false;
	bool developmentOnly = false;
	bool version2Placed = false;
	bool version2Unplaced = false;
	for (VisualScriptGraph const* graph : graphs)
	{
		for (VisualScriptNode const& node : graph->Nodes)
		{
			if (!node.Split.Inputs.empty())
			{
				splitInputs = &node;
			}
			if (std::any_of(node.Split.Outputs.begin(), node.Split.Outputs.end(),
			                [](std::string const& pin)
			                {
								return pin.find('.') != std::string::npos;
							}))
			{
				nestedSplitOutputs = &node;
			}
			comment = comment || !node.Comment.empty();
			collapsed = collapsed || node.Collapsed;
			disabled = disabled || node.Enabled == VisualScriptNodeEnabled::Disabled;
			developmentOnly = developmentOnly || node.Enabled == VisualScriptNodeEnabled::DevelopmentOnly;
			version2Placed = version2Placed || (hasVersion2Key(node) && node.Position.has_value());
			version2Unplaced = version2Unplaced || (hasVersion2Key(node) && !node.Position.has_value());
		}
	}
	REQUIRE(splitInputs != nullptr);
	REQUIRE(nestedSplitOutputs != nullptr);
	CHECK(splitInputs != nestedSplitOutputs);
	CHECK(comment);
	CHECK(collapsed);
	CHECK(disabled);
	CHECK(developmentOnly);
	CHECK(version2Placed);
	CHECK(version2Unplaced);
	CHECK(std::any_of(sample.Functions.begin(), sample.Functions.end(),
	                  [&hasVersion2Key](VisualScriptFunction const& function)
	                  {
						  return std::any_of(function.Graph.Nodes.begin(), function.Graph.Nodes.end(), hasVersion2Key);
					  }));
}

TEST_CASE("VisualScript: the editor fixture document uses every node key of version 2 where the editor shows it")
{
	LoadedDocument const loaded = Load(Parse(ReadText(GetFixturePath("Documents/Editor.json"))));
	CHECK(GetPaths(loaded.Issues).empty());
	VisualScriptDocument const& document = loaded.Document;
	std::vector<VisualScriptGraph const*> graphs = {&document.Graph};
	for (VisualScriptFunction const& function : document.Functions)
	{
		graphs.push_back(&function.Graph);
	}
	std::vector<std::pair<VisualScriptGraph const*, VisualScriptNode const*>> nodes;
	size_t unplaced = 0;
	for (VisualScriptGraph const* graph : graphs)
	{
		for (VisualScriptNode const& node : graph->Nodes)
		{
			nodes.emplace_back(graph, &node);
			unplaced += node.Position.has_value() ? 0 : 1;
		}
	}
	CHECK(unplaced >= 2);
	auto const has = [&nodes](auto const& predicate)
	{
		return std::any_of(nodes.begin(), nodes.end(),
		                   [&predicate](std::pair<VisualScriptGraph const*, VisualScriptNode const*> const& entry)
		                   {
							   return predicate(*entry.first, *entry.second);
						   });
	};

	// Each state on an impure node, on a latent node and on an event; a disabled function entry (which ignores it).
	auto const hasState = [&has](std::string_view typePrefix, VisualScriptNodeEnabled enabled)
	{
		return has(
			[typePrefix, enabled](VisualScriptGraph const&, VisualScriptNode const& node)
			{
				return node.Type.starts_with(typePrefix) && node.Enabled == enabled;
			});
	};
	for (VisualScriptNodeEnabled const enabled :
	     {VisualScriptNodeEnabled::Enabled, VisualScriptNodeEnabled::Disabled, VisualScriptNodeEnabled::DevelopmentOnly})
	{
		CAPTURE(VisualScriptNodeEnabledToString(enabled));
		CHECK(hasState("Call:Strada.Log.Info(string)", enabled));
		CHECK(hasState("Flow.Delay", enabled));
		CHECK(hasState("Event.", enabled));
	}
	CHECK(hasState("Function.Entry", VisualScriptNodeEnabled::Disabled));

	// The value of a transform setter split into fields, with literals and a link; a raycast's hit split, and its point.
	CHECK(has(
		[](VisualScriptGraph const& graph, VisualScriptNode const& node)
		{
			bool const linkedField = std::any_of(graph.Links.begin(), graph.Links.end(),
		                                         [&node](VisualScriptLink const& link)
		                                         {
													 return link.To.Node == node.ID && link.To.Pin.starts_with("Value.");
												 });
			bool fieldLiteral = false;
			for (auto const& item : node.Inputs.items())
			{
				fieldLiteral = fieldLiteral || item.key().starts_with("Value.");
			}
			return node.Type == "Set:Strada.TransformComponent.Translation" && node.Split.Inputs == std::vector<std::string>{"Value"} &&
		           linkedField && fieldLiteral;
		}));
	CHECK(has(
		[](VisualScriptGraph const& graph, VisualScriptNode const& node)
		{
			bool const linkedField = std::any_of(graph.Links.begin(), graph.Links.end(),
		                                         [&node](VisualScriptLink const& link)
		                                         {
													 return link.From.Node == node.ID && link.From.Pin.starts_with("Hit.Point.");
												 });
			return node.Type.starts_with("Call:Strada.Physics.Raycast(") &&
		           node.Split.Outputs == std::vector<std::string>{"Hit", "Hit.Point"} && linkedField;
		}));
	CHECK(has(
		[](VisualScriptGraph const&, VisualScriptNode const& node)
		{
			return !node.Comment.empty();
		}));
	CHECK(has(
		[](VisualScriptGraph const&, VisualScriptNode const& node)
		{
			return node.Collapsed;
		}));
}

TEST_CASE("VisualScript: the FeatureTest project's graphs load without issues and are files of the current version in the writer's layout")
{
	std::filesystem::path const scripts = FileSystem::PathFromUtf8(STRADA_FEATURE_TEST_DIR) / "Assets" / "Scripts";
	size_t graphs = 0;
	for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(scripts))
	{
		if (entry.path().extension() != ".sgraph")
		{
			continue;
		}
		CAPTURE(FileSystem::PathToUtf8(entry.path()));
		graphs++;
		std::string const text = ReadText(entry.path());
		Json const file = Parse(text);
		CHECK(file["Strada"]["Version"] == VisualScriptSerializer::FormatVersion);
		std::vector<VisualScriptIssue> issues;
		Result<VisualScriptDocument> loaded = VisualScriptSerializer::Deserialize(file, MakeContext(UnknownFieldPolicy::Warn), &issues);
		REQUIRE_MESSAGE(loaded.IsOk(), loaded.GetError());
		CHECK(GetPaths(issues).empty());
		CHECK(DumpJson(VisualScriptSerializer::Serialize(loaded.GetValue())) == text);
	}
	// FormatSample, Features (the scene's), Contacts and Clicks at least.
	CHECK(graphs >= 4);
}

TEST_CASE("VisualScript: files that are not visual scripts of this version are refused")
{
	Testing::TemporaryDirectory const directory;
	CHECK(GetFileError(directory, MakeFileText(ValidHeader, "{}")).empty());
	CHECK(GetFileError(directory, MakeFileText(Version2Header, "{}")).empty());
	CHECK(GetFileError(directory, MakeFileText(Version3Header, "{}")).empty());

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
	for (std::string_view const version : {"4", "2147483648", "4294967297", "18446744073709551615"})
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
	// The content form has no version: it knows the node keys of version 2, which the file of version 1 does not.
	CHECK(VisualScriptSerializer::DeserializeDocument(content, MakeContext(UnknownFieldPolicy::Error)).GetError() ==
	      "Graph.Links[0].Color: unknown key");
}

TEST_CASE("VisualScript: the node keys of version 2 are unknown keys in a file of version 1")
{
	constexpr std::string_view Content = R"({
		"Functions": [{"ID": 1, "Name": "F", "Graph": {"Nodes": [{"ID": 2, "Type": "Function.Entry", "Collapsed": true}]}}],
		"Graph": {
			"Nodes": [{"ID": 3, "Type": "Flow.Branch", "Split": {"Inputs": ["Condition"]}, "Comment": "c", "Collapsed": true, "Enabled": "Disabled"}]
		}
	})";
	Json const version1 = Parse(MakeFileText(ValidHeader, Content));
	Json const version2 = Parse(MakeFileText(Version2Header, Content));

	// A file of version 2 has them.
	std::vector<VisualScriptIssue> issues;
	Result<VisualScriptDocument> read = VisualScriptSerializer::Deserialize(version2, MakeContext(UnknownFieldPolicy::Error), &issues);
	REQUIRE_MESSAGE(read.IsOk(), read.GetError());
	CHECK(GetPaths(issues).empty());
	VisualScriptNode const& branch = read.GetValue().Graph.Nodes.at(0);
	CHECK(branch.Split == VisualScriptSplitPins{{"Condition"}, {}});
	CHECK(branch.Comment == "c");
	CHECK(branch.Collapsed);
	CHECK(branch.Enabled == VisualScriptNodeEnabled::Disabled);
	CHECK(read.GetValue().Functions.at(0).Graph.Nodes.at(0).Collapsed);

	// A file of version 1 does not: each is an issue under Warn, and the nodes keep the defaults.
	std::vector<std::string> warnings;
	issues.clear();
	Result<VisualScriptDocument> warned =
		VisualScriptSerializer::Deserialize(version1, MakeContext(UnknownFieldPolicy::Warn, &warnings), &issues);
	REQUIRE_MESSAGE(warned.IsOk(), warned.GetError());
	CHECK(GetPaths(issues) == "Functions[0].Graph.Nodes[0].Collapsed, Graph.Nodes[0].Split, Graph.Nodes[0].Comment, "
	                          "Graph.Nodes[0].Collapsed, Graph.Nodes[0].Enabled");
	CHECK(warnings.size() == issues.size());
	VisualScriptNode plainBranch;
	plainBranch.ID = 3;
	plainBranch.Type = "Flow.Branch";
	VisualScriptDocument expected = read.GetValue();
	expected.Graph.Nodes.at(0) = plainBranch;
	expected.Functions.at(0).Graph.Nodes.at(0).Collapsed = false;
	CHECK(warned.GetValue() == expected);
	CheckSavesAndLoadsAgain(warned.GetValue());

	// They are skipped under Ignore and refuse the file under Error.
	warnings.clear();
	issues.clear();
	Result<VisualScriptDocument> ignored =
		VisualScriptSerializer::Deserialize(version1, MakeContext(UnknownFieldPolicy::Ignore, &warnings), &issues);
	REQUIRE(ignored.IsOk());
	CHECK(issues.empty());
	CHECK(warnings.empty());
	CHECK(ignored.GetValue() == expected);
	CHECK(VisualScriptSerializer::Deserialize(version1, MakeContext(UnknownFieldPolicy::Error)).GetError() ==
	      "Functions[0].Graph.Nodes[0].Collapsed: unknown key");
}

TEST_CASE("VisualScript: the keys of version 3 are unknown keys in a file of version 1 or 2")
{
	constexpr std::string_view Content = R"({
		"Kind": "Library",
		"Functions": [
			{
				"ID": 1, "Name": "Clamp01", "Pure": true,
				"Inputs": [{"Name": "Value", "Type": "float", "FormerNames": ["X"]}],
				"Outputs": [{"Name": "Result", "Type": "float", "FormerNames": ["Clamped", "Out"]}]
			}
		]
	})";

	// A file of version 3 has them, as the content form does.
	std::vector<VisualScriptIssue> issues;
	Result<VisualScriptDocument> read =
		VisualScriptSerializer::Deserialize(Parse(MakeFileText(Version3Header, Content)), MakeContext(UnknownFieldPolicy::Error), &issues);
	REQUIRE_MESSAGE(read.IsOk(), read.GetError());
	CHECK(GetPaths(issues).empty());
	CHECK(read.GetValue().Kind == VisualScriptDocumentKind::Library);
	VisualScriptFunction const& clamp = read.GetValue().Functions.at(0);
	CHECK(clamp.Inputs == std::vector<VisualScriptParameter>{{"Value", "float", {"X"}}});
	CHECK(clamp.Outputs == std::vector<VisualScriptParameter>{{"Result", "float", {"Clamped", "Out"}}});
	CHECK(IsStrictlyValid(Parse(Content)));
	CHECK(Load(Parse(Content)).Document == read.GetValue());

	// Files of versions 1 and 2 do not: each is an issue under Warn, the document is a script and the parameters have no
	// former names; they are skipped under Ignore and refuse the file under Error.
	VisualScriptDocument expected = read.GetValue();
	expected.Kind = VisualScriptDocumentKind::Script;
	expected.Functions.at(0).Inputs.at(0).FormerNames.clear();
	expected.Functions.at(0).Outputs.at(0).FormerNames.clear();
	for (std::string_view const header : {ValidHeader, Version2Header})
	{
		CAPTURE(header);
		Json const file = Parse(MakeFileText(header, Content));
		std::vector<std::string> warnings;
		issues.clear();
		Result<VisualScriptDocument> warned =
			VisualScriptSerializer::Deserialize(file, MakeContext(UnknownFieldPolicy::Warn, &warnings), &issues);
		REQUIRE_MESSAGE(warned.IsOk(), warned.GetError());
		CHECK(GetPaths(issues) == "Kind, Functions[0].Inputs[0].FormerNames, Functions[0].Outputs[0].FormerNames");
		CHECK(warnings.size() == issues.size());
		CHECK(warned.GetValue() == expected);
		CheckSavesAndLoadsAgain(warned.GetValue());

		warnings.clear();
		issues.clear();
		Result<VisualScriptDocument> ignored =
			VisualScriptSerializer::Deserialize(file, MakeContext(UnknownFieldPolicy::Ignore, &warnings), &issues);
		REQUIRE(ignored.IsOk());
		CHECK(issues.empty());
		CHECK(warnings.empty());
		CHECK(ignored.GetValue() == expected);
		CHECK(VisualScriptSerializer::Deserialize(file, MakeContext(UnknownFieldPolicy::Error)).GetError() == "Kind: unknown key");
		Json withoutKind = file;
		withoutKind["VisualScript"].erase("Kind");
		CHECK(VisualScriptSerializer::Deserialize(withoutKind, MakeContext(UnknownFieldPolicy::Error)).GetError() ==
		      "Functions[0].Inputs[0].FormerNames: unknown key");
	}
}

TEST_CASE("VisualScript: a document is a script unless it says it is a library, which the writer writes first")
{
	CHECK(VisualScriptDocument().Kind == VisualScriptDocumentKind::Script);
	CHECK(VisualScriptDocumentKindToString(VisualScriptDocumentKind::Script) == "Script");
	CHECK(VisualScriptDocumentKindToString(VisualScriptDocumentKind::Library) == "Library");
	for (VisualScriptDocumentKind const kind : {VisualScriptDocumentKind::Script, VisualScriptDocumentKind::Library})
	{
		CHECK(VisualScriptDocumentKindFromString(VisualScriptDocumentKindToString(kind)) == kind);
	}
	for (std::string_view const text : {"", "library", "Library ", "Scripts", "Module"})
	{
		CAPTURE(text);
		CHECK_FALSE(VisualScriptDocumentKindFromString(text).has_value());
	}
	CHECK(VisualScriptDocumentKindToString(static_cast<VisualScriptDocumentKind>(2)).empty());

	// Repair 16: anything but the two names makes the document a script.
	for (Json const& kind : {Json(5), Json("library"), Json(""), Json(), Json::array({"Library"}), Json("Module")})
	{
		CAPTURE(kind.dump());
		Json content = Json::object({{"Kind", kind}, {"Description", "Clamps"}});
		LoadedDocument const loaded = Load(content);
		REQUIRE(loaded.Issues.size() == 1);
		CHECK(loaded.Issues[0] == VisualScriptIssue{"Kind", "expected \"Script\" or \"Library\"; the document is a script"});
		CHECK(loaded.Document.Kind == VisualScriptDocumentKind::Script);
		CHECK(loaded.Document.Description == "Clamps");
		CHECK_FALSE(IsStrictlyValid(content));
	}

	// "Script" written out is the default: no issue, and the writer omits it.
	Json const script = Parse(R"({"Kind": "Script", "Description": "Moves"})");
	CHECK(IsStrictlyValid(script));
	LoadedDocument const loadedScript = Load(script);
	CHECK(loadedScript.Document.Kind == VisualScriptDocumentKind::Script);
	CHECK_FALSE(VisualScriptSerializer::SerializeDocument(loadedScript.Document).contains("Kind"));

	// A library starts with its Kind, in the content form and in the file's content object.
	VisualScriptDocument library = MakeVisualScriptTemplate(VisualScriptTemplate::Library);
	CHECK(library.Kind == VisualScriptDocumentKind::Library);
	library.Description = "Clamps";
	std::string const expected = DumpJson(Parse(R"({
		"Kind": "Library",
		"Description": "Clamps",
		"Variables": [],
		"Functions": [],
		"Graph": {"Nodes": [], "Links": [], "Comments": []}
	})"));
	Json const content = VisualScriptSerializer::SerializeDocument(library);
	CHECK(DumpJson(content) == expected);
	CHECK(content.begin().key() == "Kind");
	Json const file = VisualScriptSerializer::Serialize(library);
	CHECK(DumpJson(file["VisualScript"]) == expected);
	CHECK(IsStrictlyValid(content));
	CheckSavesAndLoadsAgain(library);
}

TEST_CASE("VisualScript: parameters' former names are names of no parameter of their list, each once in it, at most 16, written last")
{
	LoadedDocument const loaded = Load(Parse(R"({
		"Functions": [
			{
				"ID": 1, "Name": "Mix",
				"Inputs": [
					{"Name": "A", "Type": "float", "FormerNames": ["X", 3, "Bad.Name", "B", "X", "A", "Y"]},
					{"Name": "B", "Type": "float", "FormerNames": ["Y", "Z"]},
					{"Name": "C", "Type": "float", "FormerNames": "Old"}
				],
				"Outputs": [{"Name": "Result", "Type": "float", "FormerNames": ["X", "A", "Result"]}]
			}
		]
	})"));
	CHECK(GetPaths(loaded.Issues) == "Functions[0].Inputs[0].FormerNames[1], Functions[0].Inputs[0].FormerNames[2], "
	                                 "Functions[0].Inputs[0].FormerNames[3], Functions[0].Inputs[0].FormerNames[4], "
	                                 "Functions[0].Inputs[0].FormerNames[5], Functions[0].Inputs[1].FormerNames[0], "
	                                 "Functions[0].Inputs[2].FormerNames, Functions[0].Outputs[0].FormerNames[2]");
	auto const message = [&loaded](std::string_view path)
	{
		auto const issue = std::find_if(loaded.Issues.begin(), loaded.Issues.end(),
		                                [path](VisualScriptIssue const& candidate)
		                                {
											return candidate.Path == path;
										});
		return issue == loaded.Issues.end() ? std::string() : issue->Message;
	};
	CHECK(message("Functions[0].Inputs[0].FormerNames[1]") == "expected a string; dropped");
	CHECK(Contains(message("Functions[0].Inputs[0].FormerNames[2]"), "expected a name of 1 to 64 bytes"));
	// A later parameter's name counts as much as an earlier one's.
	CHECK(message("Functions[0].Inputs[0].FormerNames[3]") == "'B' is the name of a parameter of this list; dropped");
	CHECK(message("Functions[0].Inputs[0].FormerNames[4]") == "'X' is already a former name; dropped");
	CHECK(message("Functions[0].Inputs[0].FormerNames[5]") == "'A' is the name of a parameter of this list; dropped");
	CHECK(message("Functions[0].Inputs[1].FormerNames[0]") == "'Y' is already a former name; dropped");
	CHECK(message("Functions[0].Inputs[2].FormerNames") == "expected an array; replaced by []");
	CHECK(message("Functions[0].Outputs[0].FormerNames[2]") == "'Result' is the name of a parameter of this list; dropped");

	// Each list is its own: an output may keep an input's name or former name.
	VisualScriptFunction const& mix = loaded.Document.Functions.at(0);
	CHECK(mix.Inputs == std::vector<VisualScriptParameter>{{"A", "float", {"X", "Y"}}, {"B", "float", {"Z"}}, {"C", "float", {}}});
	CHECK(mix.Outputs == std::vector<VisualScriptParameter>{{"Result", "float", {"X", "A"}}});

	// At most 16: the later ones are dropped.
	Json formerNames = Json::array();
	for (size_t i = 0; i <= VisualScriptSerializer::MaxFormerNames; i++)
	{
		formerNames.push_back(fmt::format("Old{}", i));
	}
	Json const many = Json::object(
		{{"Functions", Json::array({Json::object(
						   {{"ID", 1},
	                        {"Name", "F"},
	                        {"Inputs", Json::array({Json::object({{"Name", "P"}, {"Type", "int"}, {"FormerNames", formerNames}})})}})})}});
	LoadedDocument const limited = Load(many);
	CHECK(GetPaths(limited.Issues) == "Functions[0].Inputs[0].FormerNames[16]");
	CHECK(limited.Issues.at(0).Message == "more than 16 former names; dropped");
	CHECK(limited.Document.Functions.at(0).Inputs.at(0).FormerNames.size() == VisualScriptSerializer::MaxFormerNames);
	Json atLimit = many;
	atLimit["Functions"][0]["Inputs"][0]["FormerNames"].erase(VisualScriptSerializer::MaxFormerNames);
	CHECK(IsStrictlyValid(atLimit));

	// Written after Type, and only when there are any.
	VisualScriptDocument document;
	VisualScriptFunction function;
	function.ID = 1;
	function.Name = "Clamp01";
	function.Inputs = {{"Value", "float", {"X", "Input"}}, {"Low", "float"}};
	function.Outputs = {{"Result", "float"}};
	document.Functions = {function};
	Json const content = VisualScriptSerializer::SerializeDocument(document);
	CHECK(DumpJson(content["Functions"][0]["Inputs"]) ==
	      DumpJson(Parse(R"([{"Name": "Value", "Type": "float", "FormerNames": ["X", "Input"]}, {"Name": "Low", "Type": "float"}])")));
	CHECK(DumpJson(content["Functions"][0]["Outputs"]) == DumpJson(Parse(R"([{"Name": "Result", "Type": "float"}])")));
	CHECK(IsStrictlyValid(content));
	CheckSavesAndLoadsAgain(document);
}

TEST_CASE("VisualScript: node comments, collapsed and enabled states of the wrong kind take their defaults")
{
	Json nodes = Parse(R"([
		{"ID": 1, "Type": "N", "Comment": 5, "Collapsed": "yes", "Enabled": "Off"},
		{"ID": 2, "Type": "N", "Comment": ["c"], "Collapsed": 1, "Enabled": false},
		{"ID": 3, "Type": "N", "Comment": null, "Collapsed": null, "Enabled": "disabled"},
		{"ID": 4, "Type": "N", "Comment": "Kept", "Collapsed": true, "Enabled": "DevelopmentOnly"},
		{"ID": 5, "Type": "N", "Collapsed": false, "Enabled": "Disabled"},
		{"ID": 6, "Type": "N", "Enabled": "Enabled"}
	])");
	std::string const maxComment(65536, 'c');
	nodes.push_back(Json::object({{"ID", 7}, {"Type", "N"}, {"Comment", maxComment}}));
	nodes.push_back(Json::object({{"ID", 8}, {"Type", "N"}, {"Comment", maxComment + "c"}}));

	LoadedDocument const loaded = Load(MakeContent(Json::array(), Json::array(), MakeGraph(nodes)));
	CHECK(GetPaths(loaded.Issues) == "Graph.Nodes[0].Comment, Graph.Nodes[0].Collapsed, Graph.Nodes[0].Enabled, Graph.Nodes[1].Comment, "
	                                 "Graph.Nodes[1].Collapsed, Graph.Nodes[1].Enabled, Graph.Nodes[2].Comment, Graph.Nodes[2].Collapsed, "
	                                 "Graph.Nodes[2].Enabled, Graph.Nodes[7].Comment");
	std::vector<VisualScriptNode> const& kept = loaded.Document.Graph.Nodes;
	REQUIRE(kept.size() == 8);
	for (size_t const i : std::array<size_t, 4>{0, 1, 2, 7})
	{
		CAPTURE(i);
		CHECK(kept[i].Comment.empty());
		CHECK_FALSE(kept[i].Collapsed);
		CHECK(kept[i].Enabled == VisualScriptNodeEnabled::Enabled);
	}
	CHECK(kept[3].Comment == "Kept");
	CHECK(kept[3].Collapsed);
	CHECK(kept[3].Enabled == VisualScriptNodeEnabled::DevelopmentOnly);
	CHECK_FALSE(kept[4].Collapsed);
	CHECK(kept[4].Enabled == VisualScriptNodeEnabled::Disabled);
	CHECK(kept[5].Enabled == VisualScriptNodeEnabled::Enabled);
	CHECK(kept[6].Comment == maxComment);
}

TEST_CASE("VisualScript: split pins keep pin paths once each, parents ahead of their fields, at most 64 per list")
{
	std::string const longest(128, 'p');
	Json nodes = Json::array();
	nodes.push_back(Parse(R"({"ID": 1, "Type": "N", "Split": ["Hit"]})"));
	nodes.push_back(Parse(R"({"ID": 2, "Type": "N", "Split": {"Inputs": "Hit", "Outputs": {"Hit": 1}}})"));
	// A pin name holding a '.' (a string switch's case "Case:a.b") reads as a path too, here a field of "Case:a".
	Json paths = Json::array({5, "", ".Hit", "Hit.", "Hit..Point", "Tab\tPin", longest + "p", longest, "Hit", "Hit", "Hit.Point",
	                          "Hit.Point", "Force.X", "Hit.Normal.X", "Hit.Normal", "Case:a.b", nullptr});
	nodes.push_back(Json::object({{"ID", 3}, {"Type", "N"}, {"Split", Json::object({{"Inputs", paths}})}}));
	// Each list is its own: an output's field needs the output split, not an input of the same name.
	nodes.push_back(Parse(R"({"ID": 4, "Type": "N", "Split": {"Inputs": ["Value"], "Outputs": ["Value.X", "Value"]}})"));
	Json many = Json::array();
	for (size_t i = 0; i < 65; i++)
	{
		many.push_back(fmt::format("Pin{}", i));
	}
	many.push_back("Pin0.X");
	nodes.push_back(Json::object({{"ID", 5}, {"Type", "N"}, {"Split", Json::object({{"Outputs", many}})}}));
	nodes.push_back(Parse(R"({"ID": 6, "Type": "N", "Split": {"Inputs": ["A"], "Fields": ["B"]}})"));

	LoadedDocument const loaded = Load(MakeContent(Json::array(), Json::array(), MakeGraph(nodes)));
	CHECK(GetPaths(loaded.Issues) ==
	      "Graph.Nodes[0].Split, Graph.Nodes[1].Split.Inputs, Graph.Nodes[1].Split.Outputs, Graph.Nodes[2].Split.Inputs[0], "
	      "Graph.Nodes[2].Split.Inputs[1], Graph.Nodes[2].Split.Inputs[2], Graph.Nodes[2].Split.Inputs[3], "
	      "Graph.Nodes[2].Split.Inputs[4], Graph.Nodes[2].Split.Inputs[5], Graph.Nodes[2].Split.Inputs[6], "
	      "Graph.Nodes[2].Split.Inputs[9], Graph.Nodes[2].Split.Inputs[11], Graph.Nodes[2].Split.Inputs[12], "
	      "Graph.Nodes[2].Split.Inputs[13], Graph.Nodes[2].Split.Inputs[15], Graph.Nodes[2].Split.Inputs[16], "
	      "Graph.Nodes[3].Split.Outputs[0], Graph.Nodes[4].Split.Outputs[64], Graph.Nodes[4].Split.Outputs[65], "
	      "Graph.Nodes[5].Split.Fields");

	std::vector<VisualScriptNode> const& kept = loaded.Document.Graph.Nodes;
	REQUIRE(kept.size() == 6);
	CHECK(kept[0].Split.IsEmpty());
	CHECK(kept[1].Split.IsEmpty());
	CHECK(kept[2].Split.Inputs == std::vector<std::string>{longest, "Hit", "Hit.Point", "Hit.Normal"});
	CHECK(kept[2].Split.Outputs.empty());
	CHECK(kept[3].Split == VisualScriptSplitPins{{"Value"}, {"Value"}});
	REQUIRE(kept[4].Split.Outputs.size() == VisualScriptSerializer::MaxSplitPins);
	CHECK(kept[4].Split.Outputs.back() == "Pin63");
	CHECK(kept[5].Split == VisualScriptSplitPins{{"A"}, {}});

	// The messages say why an entry is dropped; an unknown member of Split refuses the document under Error.
	auto const message = [&loaded](std::string_view path)
	{
		auto const issue = std::find_if(loaded.Issues.begin(), loaded.Issues.end(),
		                                [path](VisualScriptIssue const& candidate)
		                                {
											return candidate.Path == path;
										});
		return issue == loaded.Issues.end() ? std::string() : issue->Message;
	};
	CHECK(Contains(message("Graph.Nodes[2].Split.Inputs[11]"), "'Hit.Point' is already split"));
	CHECK(Contains(message("Graph.Nodes[2].Split.Inputs[13]"), "'Hit.Normal.X' is a field of 'Hit.Normal', which is not split before it"));
	CHECK(Contains(message("Graph.Nodes[2].Split.Inputs[15]"), "'Case:a.b' is a field of 'Case:a'"));
	CHECK(Contains(message("Graph.Nodes[4].Split.Outputs[64]"), "more than 64 split pins"));
	CHECK(Contains(message("Graph.Nodes[0].Split"), "expected an object"));
	CHECK(VisualScriptSerializer::DeserializeDocument(MakeContent(Json::array(), Json::array(), MakeGraph(nodes)),
	                                                  MakeContext(UnknownFieldPolicy::Error))
	          .GetError() == "Graph.Nodes[5].Split.Fields: unknown key");
}

TEST_CASE("VisualScript: a split path dropped from its list drops the paths of its fields")
{
	auto const loadInputs = [](Json paths)
	{
		Json const node = Json::object({{"ID", 1}, {"Type", "N"}, {"Split", Json::object({{"Inputs", std::move(paths)}})}});
		return Load(MakeContent(Json::array(), Json::array(), MakeGraph(Json::array({node}))));
	};

	LoadedDocument const fieldFirst = loadInputs(Json::array({"A.B", "A", "A.B.C"}));
	CHECK(GetPaths(fieldFirst.Issues) == "Graph.Nodes[0].Split.Inputs[0], Graph.Nodes[0].Split.Inputs[2]");
	CHECK(fieldFirst.Document.Graph.Nodes.at(0).Split.Inputs == std::vector<std::string>{"A"});

	LoadedDocument const parentsFirst = loadInputs(Json::array({"A", "A.B", "A.B.C"}));
	CHECK(GetPaths(parentsFirst.Issues).empty());
	CHECK(parentsFirst.Document.Graph.Nodes.at(0).Split.Inputs == std::vector<std::string>{"A", "A.B", "A.B.C"});

	LoadedDocument const invalidParent = loadInputs(Json::array({"A", "B.", "B.C", "B.C.D", "A.X"}));
	CHECK(GetPaths(invalidParent.Issues) ==
	      "Graph.Nodes[0].Split.Inputs[1], Graph.Nodes[0].Split.Inputs[2], Graph.Nodes[0].Split.Inputs[3]");
	CHECK(invalidParent.Document.Graph.Nodes.at(0).Split.Inputs == std::vector<std::string>{"A", "A.X"});

	// A parent beyond the limit takes its fields with it.
	Json full = Json::array();
	for (size_t i = 0; i < VisualScriptSerializer::MaxSplitPins; i++)
	{
		full.push_back(fmt::format("Pin{}", i));
	}
	full.push_back("Extra");
	full.push_back("Extra.X");
	LoadedDocument const limited = loadInputs(full);
	CHECK(GetPaths(limited.Issues) == "Graph.Nodes[0].Split.Inputs[64], Graph.Nodes[0].Split.Inputs[65]");
	CHECK(limited.Document.Graph.Nodes.at(0).Split.Inputs.size() == VisualScriptSerializer::MaxSplitPins);
	CHECK(std::find(limited.Document.Graph.Nodes.at(0).Split.Inputs.begin(), limited.Document.Graph.Nodes.at(0).Split.Inputs.end(),
	                "Extra.X") == limited.Document.Graph.Nodes.at(0).Split.Inputs.end());
}

TEST_CASE("VisualScript: the content form carries the node keys of version 2, and strict use refuses their repairs")
{
	Json const content = Parse(R"({
		"Functions": [{"ID": 1, "Name": "F", "Graph": {"Nodes": [{"ID": 2, "Type": "Function.Entry", "Comment": "Starts here"}]}}],
		"Graph": {
			"Nodes": [
				{"ID": 3, "Type": "N", "Position": [0.0, 0.0], "Split": {"Inputs": ["Force"], "Outputs": ["Hit", "Hit.Point"]}, "Comment": "c", "Collapsed": true, "Enabled": "DevelopmentOnly"},
				{"ID": 4, "Type": "N", "Split": {"Outputs": ["Value"]}, "Enabled": "Disabled"}
			],
			"Links": [{"From": [4, "Value.X"], "To": [3, "Force.Y"]}]
		}
	})");
	CHECK(IsStrictlyValid(content));
	LoadedDocument const loaded = Load(content);
	VisualScriptNode const& node = loaded.Document.Graph.Nodes.at(0);
	CHECK(node.Split == VisualScriptSplitPins{{"Force"}, {"Hit", "Hit.Point"}});
	CHECK(node.Enabled == VisualScriptNodeEnabled::DevelopmentOnly);
	CHECK(loaded.Document.Functions.at(0).Graph.Nodes.at(0).Comment == "Starts here");

	// What the writer gives back is the content as given (it is in the writer's order).
	Json const written = VisualScriptSerializer::SerializeDocument(loaded.Document);
	CHECK(written["Graph"]["Nodes"] == content["Graph"]["Nodes"]);
	CHECK(written["Functions"][0]["Graph"]["Nodes"] == content["Functions"][0]["Graph"]["Nodes"]);

	// Defaults written out are no issues; every repair is.
	Json explicitDefaults = content;
	explicitDefaults["Graph"]["Nodes"][1].update(Parse(R"({"Comment": "", "Collapsed": false})"));
	explicitDefaults["Functions"][0]["Graph"]["Nodes"][0].update(
		Parse(R"({"Split": {"Inputs": [], "Outputs": []}, "Collapsed": false, "Enabled": "Enabled"})"));
	CHECK(IsStrictlyValid(explicitDefaults));
	CHECK(Load(explicitDefaults).Document == loaded.Document);
	for (std::string_view const damage :
	     {R"({"Comment": 1})", R"({"Collapsed": "true"})", R"({"Enabled": "On"})", R"({"Split": []})", R"({"Split": {"Inputs": "Force"}})",
	      R"({"Split": {"Inputs": ["Force.X"]}})", R"({"Split": {"Inputs": ["Force", "Force"]}})", R"({"Split": {"Pins": []}})"})
	{
		CAPTURE(damage);
		Json damaged = content;
		damaged["Graph"]["Nodes"][1].update(Parse(damage));
		CHECK_FALSE(IsStrictlyValid(damaged));
	}
}

TEST_CASE("VisualScript: the writer appends the node keys of version 2 in order and omits them at their defaults")
{
	VisualScriptNode full;
	full.ID = 1;
	full.Type = "N";
	full.Enabled = VisualScriptNodeEnabled::Disabled;
	full.Collapsed = true;
	full.Comment = "Note";
	full.Split.Outputs = {"Hit", "Hit.Point"};
	full.Split.Inputs = {"Value"};
	full.Inputs = Json::object({{"Value.X", 1}});
	full.Position = glm::vec2(16.0f, 32.0f);
	VisualScriptNode outputsOnly;
	outputsOnly.ID = 2;
	outputsOnly.Type = "N";
	outputsOnly.Split.Outputs = {"Hit"};
	outputsOnly.Enabled = VisualScriptNodeEnabled::DevelopmentOnly;
	VisualScriptNode defaults;
	defaults.ID = 3;
	defaults.Type = "N";
	VisualScriptDocument document;
	document.Graph.Nodes = {full, outputsOnly, defaults};

	std::string const expected = DumpJson(Parse(R"([
		{"ID": 1, "Type": "N", "Position": [16.0, 32.0], "Inputs": {"Value.X": 1}, "Split": {"Inputs": ["Value"], "Outputs": ["Hit", "Hit.Point"]}, "Comment": "Note", "Collapsed": true, "Enabled": "Disabled"},
		{"ID": 2, "Type": "N", "Split": {"Outputs": ["Hit"]}, "Enabled": "DevelopmentOnly"},
		{"ID": 3, "Type": "N"}
	])"));
	CHECK(DumpJson(VisualScriptSerializer::SerializeDocument(document)["Graph"]["Nodes"]) == expected);
	CHECK(DumpJson(VisualScriptSerializer::Serialize(document)["VisualScript"]["Graph"]["Nodes"]) == expected);
	CheckSavesAndLoadsAgain(document);
}

TEST_CASE("VisualScript: the enabled states of nodes convert to and from their names")
{
	CHECK(VisualScriptNodeEnabledToString(VisualScriptNodeEnabled::Enabled) == "Enabled");
	CHECK(VisualScriptNodeEnabledToString(VisualScriptNodeEnabled::Disabled) == "Disabled");
	CHECK(VisualScriptNodeEnabledToString(VisualScriptNodeEnabled::DevelopmentOnly) == "DevelopmentOnly");
	for (VisualScriptNodeEnabled const enabled :
	     {VisualScriptNodeEnabled::Enabled, VisualScriptNodeEnabled::Disabled, VisualScriptNodeEnabled::DevelopmentOnly})
	{
		CHECK(VisualScriptNodeEnabledFromString(VisualScriptNodeEnabledToString(enabled)) == enabled);
	}
	for (std::string_view const text : {"", "enabled", "Disabled ", "Development Only", "None"})
	{
		CAPTURE(text);
		CHECK_FALSE(VisualScriptNodeEnabledFromString(text).has_value());
	}
	CHECK(VisualScriptNodeEnabledToString(static_cast<VisualScriptNodeEnabled>(3)).empty());
	CHECK(VisualScriptNode().Enabled == VisualScriptNodeEnabled::Enabled);
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
	// The first two have the shape of a name; the loader drops the variables named by the others.
	std::vector<std::string> const names = {std::string(64, 'a'),
	                                        std::string("Gr\xC3\xB6\xC3\x9F"
	                                                    "e der Welt"),
	                                        std::string(65, 'b'),
	                                        std::string(),
	                                        std::string(" Lead"),
	                                        std::string("Trail "),
	                                        std::string("Split.Pin"),
	                                        std::string("Tab\tName"),
	                                        std::string("Delete\x7F"),
	                                        std::string("\xC3")};
	Json variables = Json::array();
	uint32_t id = 1;
	for (std::string const& name : names)
	{
		// The rule is public, so editors check names as they are typed against the loader's own.
		CAPTURE(id);
		CHECK(VisualScriptSerializer::IsValidName(name) == (id <= 2));
		variables.push_back(MakeVariable(id++, name));
	}
	// Spaces may only be inside a name, and its limit counts bytes.
	CHECK(VisualScriptSerializer::IsValidName("Max Speed"));
	CHECK_FALSE(VisualScriptSerializer::IsValidName(" "));
	std::string umlauts;
	for (size_t character = 0; character < VisualScriptSerializer::MaxNameBytes / 2; character++)
	{
		umlauts += "\xC3\xB6";
	}
	CHECK(VisualScriptSerializer::IsValidName(umlauts));
	CHECK_FALSE(VisualScriptSerializer::IsValidName(umlauts + "s"));
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
	explicitDefaults["Kind"] = "Script";
	explicitDefaults["Description"] = "";
	explicitDefaults["Functions"][0]["Inputs"][0]["FormerNames"] = Json::array();
	explicitDefaults["Variables"][0].update(Parse(R"({"Value": null, "Public": false, "Tooltip": "", "FormerNames": []})"));
	explicitDefaults["Graph"]["Nodes"][0].update(
		Parse(R"({"Settings": {}, "Inputs": {}, "Split": {}, "Comment": "", "Collapsed": false, "Enabled": "Enabled"})"));
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
	CHECK_FALSE(IsStrictlyValid(damaged(
		[](Json& content)
		{
			content["Kind"] = "Module";
		})));
	CHECK_FALSE(IsStrictlyValid(damaged(
		[](Json& content)
		{
			content["Functions"][0]["Inputs"][0]["FormerNames"] = Json::array({"P"});
		})));
	CHECK_FALSE(IsStrictlyValid(damaged(
		[](Json& content)
		{
			content["Graph"]["Nodes"][0]["Split"] = Json::object({{"Inputs", Json::array({"Condition.X"})}});
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

TEST_CASE("VisualScript: text, settings, values and ranges at their limits are kept, one more is repaired")
{
	static_assert(VisualScriptSerializer::MaxTextBytes == 65536 && VisualScriptSerializer::MaxSettingsMembers == 256);
	static_assert(VisualScriptSerializer::MaxValueDepth == 16 && VisualScriptSerializer::MaxNameBytes == 64);
	static_assert(VisualScriptSerializer::MaxFloatMagnitude == 3.4028235e38);

	std::string const maxText(65536, 't');
	Json variables =
		Json::array({MakeVariable(1, "A"), MakeVariable(2, "B"), MakeVariable(6, "C"), MakeVariable(7, "D"), MakeVariable(8, "E")});
	variables[0]["Tooltip"] = maxText;
	variables[1]["Tooltip"] = maxText + "t";
	// Ranges are floats up to float's largest value written with the fewest digits, a little above its exact value; past
	// it, a number is no float even where it would round to the largest one.
	variables[2]["Range"] = Json::array({-3.4028235e38, 3.4028235e38});
	variables[3]["Range"] = Json::array({0.0, 3.40282355e38});
	variables[4]["Range"] = Json::array({-3.40282355e38, 0.0});
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
	CHECK(GetPaths(loaded.Issues) == "Description, Variables[1].Tooltip, Variables[3].Range, Variables[4].Range, "
	                                 "Graph.Nodes[0].Settings.S256, Graph.Comments[1].Text");
	VisualScriptDocument const& document = loaded.Document;
	CHECK(document.Description.empty());
	CHECK(document.Variables[0].Tooltip == maxText);
	CHECK(document.Variables[1].Tooltip.empty());
	float const largest = std::numeric_limits<float>::max();
	CHECK(document.Variables[2].Range == std::optional<glm::vec2>(glm::vec2(-largest, largest)));
	CHECK_FALSE(document.Variables[3].Range);
	CHECK_FALSE(document.Variables[4].Range);
	// Written as the float it is, which reads back as itself (Load saves and loads every document again).
	CHECK(VisualScriptSerializer::SerializeDocument(document)["Variables"][2]["Range"] ==
	      Json::array({-3.4028234663852886e38, 3.4028234663852886e38}));
	CHECK(document.Graph.Comments[0].Text == maxText);
	CHECK(document.Graph.Comments[1].Text.empty());
	CHECK(document.Graph.Nodes[0].Settings.size() == 256);
	CHECK_FALSE(document.Graph.Nodes[0].Settings.contains("S256"));
	CHECK(document.Graph.Nodes[0].Inputs.size() == 256);

	content["Description"] = maxText;
	content["Variables"][1].erase("Tooltip");
	content["Variables"][3].erase("Range");
	content["Variables"][4].erase("Range");
	content["Graph"]["Comments"].erase(1);
	content["Graph"]["Nodes"][0]["Settings"].erase("S256");
	CHECK(IsStrictlyValid(content));
}

TEST_CASE("VisualScript: the format's rules for texts, pin paths and values are the loader's, for documents built in memory too")
{
	using Format = VisualScriptSerializer;
	std::string const invalidUtf8 = "\xC3\x28";

	CHECK(Format::IsValidText("Then", Format::MaxPinNameBytes));
	CHECK(Format::IsValidText("\xC3\x9C"
	                          "ber",
	                          Format::MaxPinNameBytes));
	CHECK(Format::IsValidText(std::string(128, 'p'), Format::MaxPinNameBytes));
	CHECK_FALSE(Format::IsValidText(std::string(129, 'p'), Format::MaxPinNameBytes));
	CHECK_FALSE(Format::IsValidText("", Format::MaxPinNameBytes));
	CHECK_FALSE(Format::IsValidText("A\nB", Format::MaxPinNameBytes));
	CHECK_FALSE(Format::IsValidText("A\x7F", Format::MaxPinNameBytes));
	CHECK_FALSE(Format::IsValidText(invalidUtf8, Format::MaxPinNameBytes));

	CHECK(Format::IsValidPinPath("Hit"));
	CHECK(Format::IsValidPinPath("Hit.Point.X"));
	CHECK_FALSE(Format::IsValidPinPath(""));
	CHECK_FALSE(Format::IsValidPinPath(".Hit"));
	CHECK_FALSE(Format::IsValidPinPath("Hit."));
	CHECK_FALSE(Format::IsValidPinPath("Hit..Point"));
	CHECK_FALSE(Format::IsValidPinPath(std::string(129, 'p')));

	double const nan = std::numeric_limits<double>::quiet_NaN();
	double const infinity = std::numeric_limits<double>::infinity();
	for (Json const& valid : {Json(), Json(true), Json(-3), Json(18446744073709551615u), Json(0.25), Json("text"),
	                          Json::array({1, "two", Json::object({{"Three", 3.0}})}), Json(std::string(65536, 't')),
	                          Json::object({{std::string(65536, 'k'), 1}}), MakeNestedValue(16)})
	{
		CHECK_MESSAGE(Format::IsValidValue(valid), valid.dump());
	}
	for (Json const& invalid :
	     {Json(nan), Json(infinity), Json(-infinity), Json::array({1.0, nan}), Json::object({{"X", infinity}}), Json(invalidUtf8),
	      Json::array({invalidUtf8}), Json::object({{invalidUtf8, 1}}), Json(std::string(65537, 't')),
	      Json::object({{std::string(65537, 'k'), 1}}), MakeNestedValue(17), Json::binary({1, 2})})
	{
		CHECK_FALSE(Format::IsValidValue(invalid));
	}

	// What the writer does with a value the format refuses, which only a document built in memory can hold: a number that is
	// not finite is written as null and invalid UTF-8 is replaced, so the file would not load back as the document. The
	// loader refuses both in memory, so the strict check before every write fails such an edit instead.
	VisualScriptDocument document;
	VisualScriptVariable notFinite;
	notFinite.ID = 1;
	notFinite.Name = "NotFinite";
	notFinite.Type = "float";
	notFinite.Value = nan;
	VisualScriptVariable badText;
	badText.ID = 2;
	badText.Name = "BadText";
	badText.Type = "string";
	badText.Value = invalidUtf8;
	document.Variables = {notFinite, badText};
	Json const content = VisualScriptSerializer::SerializeDocument(document);
	Json const written = Parse(DumpJson(content));
	CHECK(written["Variables"][0]["Value"].is_null());
	CHECK(written["Variables"][1]["Value"] == "\xEF\xBF\xBD(");
	std::vector<VisualScriptIssue> issues;
	Result<VisualScriptDocument> const reloaded =
		VisualScriptSerializer::DeserializeDocument(content, MakeContext(UnknownFieldPolicy::Error), &issues);
	REQUIRE_MESSAGE(reloaded.IsOk(), reloaded.GetError());
	CHECK(GetPaths(issues) == "Variables[0].Value, Variables[1].Value");
	CHECK(reloaded.GetValue().Variables[0].Value.is_null());
	CHECK(reloaded.GetValue().Variables[1].Value.is_null());

	// Texts are UTF-8 too: the description, a variable's tooltip, a node's comment and a comment box's text that are not are
	// replaced by "", which a parsed file never needs.
	VisualScriptDocument texts;
	texts.Description = invalidUtf8;
	VisualScriptVariable tooltip;
	tooltip.ID = 1;
	tooltip.Name = "Tipped";
	tooltip.Type = "int";
	tooltip.Tooltip = "Bad " + invalidUtf8;
	texts.Variables = {tooltip};
	VisualScriptNode noted;
	noted.ID = 2;
	noted.Type = "Flow.Branch";
	noted.Comment = invalidUtf8;
	texts.Graph.Nodes = {noted};
	texts.Graph.Comments = {VisualScriptComment{3, invalidUtf8 + "!", glm::vec2(0.0f), glm::vec2(400.0f, 200.0f), glm::vec4(0.5f)}};
	issues.clear();
	Result<VisualScriptDocument> const repaired = VisualScriptSerializer::DeserializeDocument(
		VisualScriptSerializer::SerializeDocument(texts), MakeContext(UnknownFieldPolicy::Error), &issues);
	REQUIRE_MESSAGE(repaired.IsOk(), repaired.GetError());
	CHECK(GetPaths(issues) == "Description, Variables[0].Tooltip, Graph.Nodes[0].Comment, Graph.Comments[0].Text");
	for (VisualScriptIssue const& issue : issues)
	{
		CAPTURE(issue.Path);
		CHECK(issue.Message == "expected a string of UTF-8 of at most 65536 bytes; replaced by \"\"");
	}
	CHECK(repaired.GetValue().Description.empty());
	CHECK(repaired.GetValue().Variables.at(0).Tooltip.empty());
	CHECK(repaired.GetValue().Graph.Nodes.at(0).Comment.empty());
	CHECK(repaired.GetValue().Graph.Comments.at(0).Text.empty());
	CHECK_FALSE(IsStrictlyValid(VisualScriptSerializer::SerializeDocument(texts)));
	texts.Description = "\xC3\x9C"
						"ber";
	texts.Variables[0].Tooltip = "Tip";
	texts.Graph.Nodes[0].Comment = "Line\nbreak";
	texts.Graph.Comments[0].Text = "Box";
	CHECK(IsStrictlyValid(VisualScriptSerializer::SerializeDocument(texts)));
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
	CHECK(file["Strada"] == Parse(R"({"Version": 3, "Type": "VisualScript"})"));
	CHECK(file.size() == 2);
	CHECK(VisualScriptSerializer::FormatVersion == 3);
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

	// A function library and nothing else.
	VisualScriptDocument library = MakeVisualScriptTemplate(VisualScriptTemplate::Library);
	CHECK(library.Kind == VisualScriptDocumentKind::Library);
	library.Kind = VisualScriptDocumentKind::Script;
	CHECK(library == VisualScriptDocument{});
	CheckSavesAndLoadsAgain(MakeVisualScriptTemplate(VisualScriptTemplate::Library));
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

	// The document is a shared object. The shared overload keeps the object it is given, and the one before it stays as
	// it was for whoever keeps it (an undo step, an editor).
	Ref<VisualScriptDocument const> const empty = asset.GetSharedDocument();
	REQUIRE(empty != nullptr);
	CHECK(&asset.GetDocument() == empty.get());
	Ref<VisualScriptDocument const> const shared = CreateRef<VisualScriptDocument const>(changed);
	asset.SetDocument(shared);
	CHECK(asset.GetVersion() == 3);
	CHECK(asset.GetSharedDocument() == shared);
	CHECK(&asset.GetDocument() == shared.get());
	CHECK(*empty == VisualScriptDocument{});

	// The document overload makes a new object; the shared one keeps its contents.
	VisualScriptDocument turned = changed;
	turned.Description = "Turns";
	asset.SetDocument(turned);
	CHECK(asset.GetVersion() == 4);
	CHECK(asset.GetSharedDocument() != shared);
	CHECK(asset.GetDocument() == turned);
	CHECK(*shared == changed);

	// Giving the same object again is still a change.
	asset.SetDocument(shared);
	asset.SetDocument(shared);
	CHECK(asset.GetVersion() == 6);
	CHECK(asset.GetSharedDocument() == shared);

	// Every asset has a document object, from its start.
	VisualScriptAsset const fresh;
	REQUIRE(fresh.GetSharedDocument() != nullptr);
	CHECK(fresh.GetVersion() == 0);
	VisualScriptAsset const constructed(changed);
	REQUIRE(constructed.GetSharedDocument() != nullptr);
	CHECK(constructed.GetVersion() == 0);
	CHECK(constructed.GetDocument() == changed);
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
	REQUIRE(FileSystem::WriteTextFile(assets / "Scripts" / "Newer.sgraph", MakeFileText(R"({"Version": 4, "Type": "VisualScript"})", "{}"))
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
