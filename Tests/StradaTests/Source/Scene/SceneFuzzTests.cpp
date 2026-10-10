#include "Audio/AudioTestUtilities.h"
#include "Physics/PhysicsTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

using namespace Strada;

namespace
{
	// Damages JSON documents the way bad files and hand edits do: members and elements go missing or repeat, and values
	// turn into ones of another kind or extreme ones. Deterministic on every platform (its own generator), so a failure
	// reproduces from its file and round.
	class DocumentMutator
	{
	public:
		explicit DocumentMutator(uint64_t seed)
			: m_State(seed)
		{
		}

		void Mutate(Json& document)
		{
			std::vector<Json*> nodes;
			Collect(document, nodes);
			Json& node = *nodes[Pick(nodes.size())];
			switch (Pick(4))
			{
				case 0:
					if (node.is_object() && !node.empty())
					{
						auto member = node.begin();
						std::advance(member, static_cast<std::ptrdiff_t>(Pick(node.size())));
						node.erase(member);
						return;
					}
					if (node.is_array() && !node.empty())
					{
						node.erase(node.begin() + static_cast<std::ptrdiff_t>(Pick(node.size())));
						return;
					}
					break;
				case 1:
					if (node.is_array() && !node.empty())
					{
						Json const copy = node[Pick(node.size())];
						node.push_back(copy);
						return;
					}
					break;
				default:
					break;
			}
			node = MakeValue();
		}

	private:
		// SplitMix64.
		uint64_t Next()
		{
			uint64_t value = (m_State += 0x9E3779B97F4A7C15ull);
			value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
			value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
			return value ^ (value >> 31);
		}

		size_t Pick(size_t count) { return static_cast<size_t>(Next() % count); }

		Json MakeValue()
		{
			switch (Pick(16))
			{
				case 0:
					return Json();
				case 1:
					return true;
				case 2:
					return false;
				case 3:
					return 0;
				case 4:
					return -1;
				case 5:
					return std::numeric_limits<int64_t>::max();
				case 6:
					return std::numeric_limits<uint64_t>::max();
				case 7:
					return -1.0e30;
				case 8:
					return 1.0e30;
				case 9:
					return 0.5;
				case 10:
					return "";
				case 11:
					return "builtin://Nothing";
				case 12:
					return "18446744073709551615";
				case 13:
					return Json::array({1, 2, 3});
				case 14:
					return Json::array();
				default:
					return Json::object();
			}
		}

		static void Collect(Json& node, std::vector<Json*>& nodes)
		{
			nodes.push_back(&node);
			if (node.is_structured())
			{
				for (Json& child : node)
				{
					Collect(child, nodes);
				}
			}
		}

		uint64_t m_State;
	};

	// Accepts every reference, so damaged documents get past asset lookups into the rest of the loader.
	Result<AssetHandle> ResolveAnyReference(std::string_view reference)
	{
		return AssetHandle(UUID((std::hash<std::string_view>{}(reference) & 0x7FFFFFFFFFFFFFFFull) | 1u));
	}

	Json LoadFeatureTestScene(char const* file)
	{
		std::filesystem::path const path = std::filesystem::path(STRADA_FEATURE_TEST_DIR) / "Assets" / "Scenes" / file;
		Result<std::string> text = FileSystem::ReadTextFile(path);
		REQUIRE(text.IsOk());
		Result<Json> document = ParseJson(text.GetValue());
		REQUIRE(document.IsOk());
		return document.TakeValue();
	}

	DeserializationContext MakeTolerantContext(std::vector<std::string>& warnings)
	{
		DeserializationContext context;
		context.UnknownFields = UnknownFieldPolicy::Warn;
		context.ResolveAssetReference = ResolveAnyReference;
		context.Warnings = &warnings;
		return context;
	}
}

TEST_CASE("SceneSerializer: damaged scene files are rejected or repaired, never crash")
{
	std::vector<std::string> warnings;
	DeserializationContext const context = MakeTolerantContext(warnings);

	uint64_t seed = 1;
	for (auto const& [file, rounds] : {std::pair{"Main.sscene", 400}, std::pair{"Second.sscene", 150}})
	{
		Json const original = LoadFeatureTestScene(file);
		REQUIRE(SceneSerializer::Deserialize(original, context).IsOk());

		DocumentMutator mutator(seed++);
		int loaded = 0;
		int rejected = 0;
		for (int round = 0; round < rounds; round++)
		{
			CAPTURE(file);
			CAPTURE(round);
			Json document = original;
			for (int mutation = 0; mutation <= round % 3; mutation++)
			{
				mutator.Mutate(document);
			}
			warnings.clear();
			Result<Ref<Scene>> scene = SceneSerializer::Deserialize(document, context);
			if (!scene)
			{
				rejected++;
				continue;
			}
			loaded++;
			// What a repaired scene saves loads again.
			Json const saved = SceneSerializer::Serialize(*scene.GetValue());
			Result<Ref<Scene>> reloaded = SceneSerializer::Deserialize(saved, context);
			CHECK_MESSAGE(reloaded.IsOk(), (reloaded ? std::string() : reloaded.GetError()));
		}
		// Both outcomes happen, or the damage was too mild or too severe to tell anything.
		CHECK(loaded > 0);
		CHECK(rejected > 0);
	}
}

TEST_CASE("Scene: damaged scenes that load also run")
{
	// Scripts do not run without the script engine; physics and sound do.
	Testing::PhysicsSystemScope physics;
	Testing::AudioEngineScope audio;
	Testing::AssetManagerScope assets;
	std::vector<std::string> warnings;
	DeserializationContext const context = MakeTolerantContext(warnings);
	Json const original = LoadFeatureTestScene("Main.sscene");

	DocumentMutator mutator(1000);
	int ran = 0;
	for (int round = 0; round < 300; round++)
	{
		CAPTURE(round);
		Json document = original;
		for (int mutation = 0; mutation <= round % 3; mutation++)
		{
			mutator.Mutate(document);
		}
		Result<Ref<Scene>> loaded = SceneSerializer::Deserialize(document, context);
		if (!loaded)
		{
			continue;
		}
		Scene& scene = *loaded.GetValue();
		scene.OnRuntimeStart();
		for (int frame = 0; frame < 5; frame++)
		{
			scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
		}
		scene.OnRuntimeStop();
		ran++;
	}
	CHECK(ran > 0);
}
