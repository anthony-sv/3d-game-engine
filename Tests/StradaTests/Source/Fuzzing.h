#pragma once

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Hash.h"
#include "Strada/Core/Platform.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strada::Testing
{
	// The rounds a fuzz test runs: its default, times STRADA_FUZZ_SCALE (1 to 1000) when that is set, for deeper local
	// runs; the default keeps the suite fast.
	inline int GetFuzzRounds(int rounds)
	{
		int multiplier = 1;
		if (std::optional<std::string> const scale = Platform::ReadEnvironmentVariable("STRADA_FUZZ_SCALE"))
		{
			// Stays 1 when the text is not a number.
			static_cast<void>(std::from_chars(scale->data(), scale->data() + scale->size(), multiplier));
		}
		return rounds * std::clamp(multiplier, 1, 1000);
	}

	// Damages JSON documents the way bad files and hand edits do: members and elements go missing or repeat, and values
	// turn into ones of another kind or extreme ones. Deterministic on every platform (its own generator), so a failure
	// reproduces from its seed and round.
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

		// Applies 1 to maxMutations mutations, more in later rounds.
		Json Damage(Json const& original, int round, int maxMutations = 3)
		{
			Json document = original;
			for (int mutation = 0; mutation <= round % maxMutations; mutation++)
			{
				Mutate(document);
			}
			return document;
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

	// Accepts every reference (as a stable handle), so damaged documents get past asset lookups into the rest of a loader.
	inline DeserializationContext MakeTolerantContext(std::vector<std::string>& warnings)
	{
		DeserializationContext context;
		context.UnknownFields = UnknownFieldPolicy::Warn;
		context.ResolveAssetReference = [](std::string_view reference) -> Result<AssetHandle>
		{
			return AssetHandle(UUID((Hash::FNV1a(reference) & 0x7FFFFFFFFFFFFFFFull) | 1u));
		};
		context.Warnings = &warnings;
		return context;
	}

	// Parses a file of the FeatureTest project (relativePath uses forward slashes). Its files are the fuzz tests' seed
	// documents: together they use every feature of every format.
	inline Json LoadFeatureTestDocument(std::string_view relativePath)
	{
		std::filesystem::path const path = FileSystem::PathFromUtf8(STRADA_FEATURE_TEST_DIR) / FileSystem::PathFromUtf8(relativePath);
		Result<std::string> text = FileSystem::ReadTextFile(path);
		REQUIRE_MESSAGE(text.IsOk(), FileSystem::PathToUtf8(path));
		Result<Json> document = ParseJson(text.GetValue());
		REQUIRE_MESSAGE(document.IsOk(), FileSystem::PathToUtf8(path));
		return document.TakeValue();
	}
}
