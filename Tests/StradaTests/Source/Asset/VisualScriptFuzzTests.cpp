#include "Fuzzing.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/VisualScriptEngine.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	// One round of a visual-script fuzz case: loads a damaged file (file form) the way the asset manager does, with every
	// asset reference accepted. Nothing when the file is refused; otherwise the document, once it is known to save and
	// load again unchanged and without issues. Every case of this file loads its rounds here.
	std::optional<VisualScriptDocument> LoadDamagedFile(Json const& file)
	{
		std::vector<std::string> warnings;
		DeserializationContext const context = Testing::MakeTolerantContext(warnings);
		Result<VisualScriptDocument> document = VisualScriptSerializer::Deserialize(file, context);
		if (!document)
		{
			return std::nullopt;
		}

		std::vector<VisualScriptIssue> issues;
		Result<VisualScriptDocument> reloaded =
			VisualScriptSerializer::Deserialize(VisualScriptSerializer::Serialize(document.GetValue()), context, &issues);
		REQUIRE_MESSAGE(reloaded.IsOk(), reloaded.GetError());
		CHECK_MESSAGE(issues.empty(), (issues.empty() ? std::string() : issues.front().Path + ": " + issues.front().Message));
		CHECK(reloaded.GetValue() == document.GetValue());
		return document.TakeValue();
	}

	// What documents built in memory may hold and no parsed file does: numbers that are not finite or are out of every
	// range, strings and member names that are not UTF-8, values of any shape where the format expects others, and
	// enumerators out of range.
	class InMemoryDamage
	{
	public:
		explicit InMemoryDamage(uint64_t seed)
			: m_Random(seed)
		{
		}

		// Applies 1 to 6 injections, each to an element it adds when the document has none of its kind.
		void Damage(VisualScriptDocument& document)
		{
			for (size_t injection = 0, count = 1 + Pick(6); injection < count; injection++)
			{
				Inject(document);
			}
		}

	private:
		size_t Pick(size_t count) { return m_Random.Pick(count); }

		std::string MakeText()
		{
			constexpr std::array<uint8_t, 16> Interesting = {0x00, 0x09, 0x0A, 0x1F, 0x22, 0x2E, 0x5C, 0x7F,
			                                                 0x80, 0xBF, 0xC0, 0xC3, 0xED, 0xF0, 0xF8, 0xFF};
			std::string text;
			for (size_t length = Pick(9); text.size() < length;)
			{
				switch (Pick(3))
				{
					case 0:
						text.push_back(static_cast<char>(Interesting[Pick(Interesting.size())]));
						break;
					case 1:
						text.push_back(static_cast<char>(m_Random.Next() & 0xFF));
						break;
					default:
						text.append(Pick(2) == 0 ? "A" : "\xC3\xA9");
						break;
				}
			}
			return text;
		}

		double MakeDouble()
		{
			switch (Pick(7))
			{
				case 0:
					return std::numeric_limits<double>::quiet_NaN();
				case 1:
					return Pick(2) == 0 ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity();
				case 2:
					return std::bit_cast<double>(m_Random.Next());
				case 3:
					return static_cast<double>(static_cast<int64_t>(m_Random.Next() % 2000000000000001) - 1000000000000000);
				case 4:
					return Pick(2) == 0 ? -0.0 : 1.0e16;
				case 5:
					return static_cast<double>(std::bit_cast<float>(static_cast<uint32_t>(m_Random.Next())));
				default:
					return static_cast<double>(Pick(1000)) / 8.0;
			}
		}

		glm::vec2 MakeVector()
		{
			// One call after the other: the order of a call's arguments is unspecified.
			float const x = MakeFloat();
			float const y = MakeFloat();
			return glm::vec2(x, y);
		}

		float MakeFloat()
		{
			switch (Pick(4))
			{
				case 0:
					return std::numeric_limits<float>::quiet_NaN();
				case 1:
					return Pick(2) == 0 ? std::numeric_limits<float>::infinity() : 1.0e30f;
				case 2:
					return std::bit_cast<float>(static_cast<uint32_t>(m_Random.Next()));
				default:
					return static_cast<float>(MakeDouble());
			}
		}

		Json MakeValue(int depth = 0)
		{
			switch (Pick(depth < 4 ? 9 : 7))
			{
				case 0:
					return Json();
				case 1:
					return Pick(2) == 0;
				case 2:
					return static_cast<int64_t>(m_Random.Next());
				case 3:
					return m_Random.Next();
				case 4:
				case 5:
					return MakeDouble();
				case 6:
					return MakeText();
				case 7:
				{
					Json array = Json::array();
					for (size_t element = 0, count = Pick(4); element < count; element++)
					{
						array.push_back(MakeValue(depth + 1));
					}
					return array;
				}
				default:
				{
					Json object = Json::object();
					for (size_t member = 0, count = Pick(4); member < count; member++)
					{
						object[MakeText()] = MakeValue(depth + 1);
					}
					return object;
				}
			}
		}

		VisualScriptGraph& PickGraph(VisualScriptDocument& document)
		{
			size_t const graph = Pick(document.Functions.size() + 1);
			return graph == 0 ? document.Graph : document.Functions[graph - 1].Graph;
		}

		VisualScriptNode& PickNode(VisualScriptDocument& document)
		{
			VisualScriptGraph& graph = PickGraph(document);
			if (graph.Nodes.empty())
			{
				graph.Nodes.push_back({});
			}
			return graph.Nodes[Pick(graph.Nodes.size())];
		}

		VisualScriptVariable& PickVariable(VisualScriptDocument& document)
		{
			if (document.Variables.empty())
			{
				document.Variables.push_back({});
			}
			return document.Variables[Pick(document.Variables.size())];
		}

		VisualScriptFunction& PickFunction(VisualScriptDocument& document)
		{
			if (document.Functions.empty())
			{
				document.Functions.push_back({});
			}
			return document.Functions[Pick(document.Functions.size())];
		}

		void Inject(VisualScriptDocument& document)
		{
			switch (Pick(12))
			{
				case 0:
					PickNode(document).Settings = MakeValue();
					break;
				case 1:
					PickNode(document).Inputs = MakeValue();
					break;
				case 2:
					PickVariable(document).Value = MakeValue();
					break;
				case 3:
				{
					VisualScriptFunction& function = PickFunction(document);
					function.Locals.push_back({static_cast<uint32_t>(m_Random.Next()), MakeText(), MakeText(), MakeValue()});
					break;
				}
				case 4:
				{
					VisualScriptNode& node = PickNode(document);
					node.Position = Pick(4) == 0 ? std::nullopt : std::optional<glm::vec2>(MakeVector());
					break;
				}
				case 5:
				{
					VisualScriptGraph& graph = PickGraph(document);
					VisualScriptComment comment;
					comment.ID = static_cast<uint32_t>(m_Random.Next());
					comment.Text = MakeText();
					comment.Position = MakeVector();
					comment.Size = MakeVector();
					glm::vec2 const red = MakeVector();
					glm::vec2 const blue = MakeVector();
					comment.Color = glm::vec4(red, blue);
					graph.Comments.push_back(std::move(comment));
					break;
				}
				case 6:
				{
					VisualScriptVariable& variable = PickVariable(document);
					variable.Range = MakeVector();
					variable.Name = MakeText();
					variable.FormerNames.push_back(MakeText());
					break;
				}
				case 7:
				{
					VisualScriptFunction& function = PickFunction(document);
					function.Name = MakeText();
					std::string name = MakeText();
					std::string type = MakeText();
					function.Inputs.emplace_back(std::move(name), std::move(type), std::vector<std::string>{MakeText()});
					name = MakeText();
					type = MakeText();
					function.Outputs.emplace_back(std::move(name), std::move(type));
					break;
				}
				case 8:
					document.Description = MakeText();
					PickVariable(document).Tooltip = MakeText();
					break;
				case 9:
				{
					VisualScriptNode& node = PickNode(document);
					node.Type = MakeText();
					node.Comment = MakeText();
					node.Split.Inputs.push_back(MakeText());
					node.Split.Outputs.push_back(MakeText());
					break;
				}
				case 10:
				{
					VisualScriptGraph& graph = PickGraph(document);
					graph.Links.push_back({{static_cast<uint32_t>(m_Random.Next()), MakeText()}, {0, MakeText()}});
					break;
				}
				default:
				{
					// Enumerators out of range are written as nlohmann writes their empty names.
					document.Kind = static_cast<VisualScriptDocumentKind>(Pick(3));
					PickNode(document).Enabled = static_cast<VisualScriptNodeEnabled>(Pick(4));
					PickNode(document).Collapsed = Pick(2) == 0;
					break;
				}
			}
		}

		Testing::FuzzRandom m_Random;
	};
}

TEST_CASE("VisualScript: damaged files are refused or load and save again")
{
	// The FeatureTest format sample (every key of a script) and the library fixture (Kind, and parameters' former names).
	Result<std::string> const libraryText =
		FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Library.sgraph");
	REQUIRE(libraryText.IsOk());
	Result<Json> const library = ParseJson(libraryText.GetValue());
	REQUIRE(library.IsOk());
	std::array<Json, 2> const seeds = {Testing::LoadFeatureTestDocument("Assets/Scripts/FormatSample.sgraph"), library.GetValue()};
	std::vector<std::string> warnings;
	for (Json const& seed : seeds)
	{
		std::vector<VisualScriptIssue> issues;
		REQUIRE(VisualScriptSerializer::Deserialize(seed, Testing::MakeTolerantContext(warnings), &issues).IsOk());
		REQUIRE(issues.empty());
	}

	// Each seed is damaged as a file of version 3, 2 and 1 in turn, where the keys of later versions it holds are unknown
	// keys: the sample's node keys of version 2 in a file of version 1, the library's Kind and former names in both.
	auto const withVersion = [](Json file, int version)
	{
		file["Strada"]["Version"] = version;
		return file;
	};
	for (auto const& [seed, version] : {std::pair<size_t, int>(0, 1), std::pair<size_t, int>(1, 2), std::pair<size_t, int>(1, 1)})
	{
		std::vector<VisualScriptIssue> issues;
		REQUIRE(
			VisualScriptSerializer::Deserialize(withVersion(seeds[seed], version), Testing::MakeTolerantContext(warnings), &issues).IsOk());
		CHECK_FALSE(issues.empty());
	}

	Testing::DocumentMutator mutator(10000);
	int loaded = 0;
	int refused = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(300); round < rounds; round++)
	{
		CAPTURE(round);
		int const version = VisualScriptSerializer::FormatVersion - (round / 2) % 3;
		if (LoadDamagedFile(mutator.Damage(withVersion(seeds[static_cast<size_t>(round % 2)], version), round)))
		{
			loaded++;
		}
		else
		{
			refused++;
		}
	}
	CHECK(loaded > 0);
	CHECK(refused > 0);
}

TEST_CASE("VisualScript: damaged graphs that load also run")
{
	Json const features = Testing::LoadFeatureTestDocument("Assets/Scripts/Features.sgraph");
	// The editor's fixture document in a file of version 3: disabled, development-only and split nodes.
	Result<std::string> const text =
		FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Documents" / "Editor.json");
	REQUIRE(text.IsOk());
	Result<Json> const content = ParseJson(text.GetValue());
	REQUIRE(content.IsOk());
	std::vector<std::string> warnings;
	std::vector<VisualScriptIssue> issues;
	Result<VisualScriptDocument> editor =
		VisualScriptSerializer::DeserializeDocument(content.GetValue(), Testing::MakeTolerantContext(warnings), &issues);
	REQUIRE_MESSAGE(editor.IsOk(), (editor ? std::string() : editor.GetError()));
	REQUIRE(issues.empty());
	Json const seeds[] = {features, VisualScriptSerializer::Serialize(editor.GetValue())};
	REQUIRE(seeds[1]["Strada"]["Version"] == 3);
	Testing::AssetManagerScope assets;
	Testing::ScriptEngineScope scripting;

	// Each document that loads runs on an entity of a scene that starts, updates and stops: whatever the runtime makes of
	// it, including failures, nothing crashes or hangs. The seeds take turns.
	Testing::DocumentMutator mutator(10100);
	std::array<int, 2> ran{};
	for (int round = 0, rounds = Testing::GetFuzzRounds(100); round < rounds; round++)
	{
		CAPTURE(round);
		size_t const seed = static_cast<size_t>(round % 2);
		std::optional<VisualScriptDocument> document = LoadDamagedFile(mutator.Damage(seeds[seed], round));
		if (!document)
		{
			continue;
		}
		AssetHandle const graph = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(std::move(*document)), "Damaged");
		{
			Scene scene("Fuzz");
			Entity entity = scene.CreateEntity("Graph");
			entity.AddComponent<VisualScriptComponent>().Graph = graph;
			scene.OnRuntimeStart();
			if (VisualScriptEngine::HasInstance(entity.GetUUID()))
			{
				ran[seed]++;
			}
			for (int frame = 0; frame < 5; frame++)
			{
				scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
			}
			scene.OnRuntimeStop();
		}
		AssetManager::RemoveMemoryAsset(graph);
	}
	CHECK(ran[0] > 0);
	CHECK(ran[1] > 0);
}

TEST_CASE("VisualScript: the text writer writes what the Json writer writes")
{
	// The FeatureTest format sample, the library fixture and the editor's fixture document, as files.
	Result<std::string> const libraryText =
		FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Library.sgraph");
	REQUIRE(libraryText.IsOk());
	Result<Json> const library = ParseJson(libraryText.GetValue());
	REQUIRE(library.IsOk());
	Result<std::string> const editorText =
		FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Documents" / "Editor.json");
	REQUIRE(editorText.IsOk());
	Result<Json> const editorContent = ParseJson(editorText.GetValue());
	REQUIRE(editorContent.IsOk());
	std::vector<std::string> warnings;
	Result<VisualScriptDocument> const editor =
		VisualScriptSerializer::DeserializeDocument(editorContent.GetValue(), Testing::MakeTolerantContext(warnings));
	REQUIRE(editor.IsOk());
	std::array<Json, 3> const seeds = {Testing::LoadFeatureTestDocument("Assets/Scripts/FormatSample.sgraph"), library.GetValue(),
	                                   VisualScriptSerializer::Serialize(editor.GetValue())};

	// Both writers, in the file form (pretty) and the content form (compact), against nlohmann's text of the Json ones: for
	// each document that loads, repaired or not, then for the same document with values injected in memory.
	auto const checkWriters = [](VisualScriptDocument const& document)
	{
		CHECK(VisualScriptSerializer::WriteText(document) == DumpJson(VisualScriptSerializer::Serialize(document)));
		CHECK(VisualScriptSerializer::WriteContentText(document) ==
		      VisualScriptSerializer::SerializeDocument(document).dump(-1, ' ', false, Json::error_handler_t::replace));
	};
	Testing::DocumentMutator mutator(10500);
	InMemoryDamage damage(10501);
	int loaded = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(300); round < rounds; round++)
	{
		CAPTURE(round);
		Result<VisualScriptDocument> document = VisualScriptSerializer::Deserialize(
			mutator.Damage(seeds[static_cast<size_t>(round % 3)], round), Testing::MakeTolerantContext(warnings));
		warnings.clear();
		if (!document)
		{
			continue;
		}
		loaded++;
		checkWriters(document.GetValue());
		VisualScriptDocument damaged = document.TakeValue();
		damage.Damage(damaged);
		checkWriters(damaged);
	}
	CHECK(loaded > 0);
}
