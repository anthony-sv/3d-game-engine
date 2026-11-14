#include "Asset/TestDataAsset.h"
#include "Fuzzing.h"

#include "Strada/Asset/DataAsset.h"
#include "Strada/Core/FileSystem.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace Strada;

namespace
{
	struct Seed
	{
		std::string Name;
		Json Document;
	};

	// The FeatureTest project's files of the type's extension: together they set every field of every type the engine
	// registers away from its default (FindFieldsLeftAtDefault checks it).
	std::vector<Seed> FindFeatureTestSeeds(DataAssetType const& type)
	{
		std::vector<Seed> seeds;
		std::filesystem::path const root = FileSystem::PathFromUtf8(STRADA_FEATURE_TEST_DIR);
		std::error_code errorCode;
		for (std::filesystem::recursive_directory_iterator it(root / "Assets", errorCode), end; !errorCode && it != end;
		     it.increment(errorCode))
		{
			if (!it->is_regular_file(errorCode) ||
			    DataAssetRegistry::FindByExtension(FileSystem::PathToUtf8(it->path().extension())) != &type)
			{
				continue;
			}
			std::string const relative = FileSystem::PathToUtf8(it->path().lexically_relative(root));
			seeds.push_back({relative, Testing::LoadFeatureTestDocument(relative)});
		}
		REQUIRE_FALSE(errorCode);
		return seeds;
	}

	// The test type's seeds: the current version with every field set, and the older version the migration reads.
	std::vector<Seed> MakeTestDataSeeds()
	{
		Json content = Json::object();
		content["Shape"] = "Sphere";
		content["Size"] = 2.5;
		content["Texture"] = "asset://Textures/Bricks.png";
		content["Next"] = "8406934185603345521";
		content["Entries"] = Json::array({Json::object({{"Name", "First"}, {"Weight", 0.5}}),
		                                  Json::object({{"Name", "Second"}, {"Weight", 10.0}}), Json::object({{"Name", "Third"}})});
		Json current = Json::object();
		current["Strada"] = MakeFileHeader(Testing::TestDataName, static_cast<int>(Testing::TestDataFormatVersion));
		current["TestData"] = content;

		Json older = current;
		older["Strada"]["Version"] = 1;
		older["TestData"].erase("Size");
		older["TestData"]["Radius"] = 2.5;
		return {{"a TestData document", std::move(current)}, {"a version-1 TestData document", std::move(older)}};
	}

	// The names of the type's fields, in table order, that no seed sets away from the field's default, as the seeds load
	// (migrated to the current version): the fuzz test damages only what its seeds use.
	std::vector<std::string_view> FindFieldsLeftAtDefault(DataAssetType const& type, std::vector<Seed> const& seeds,
	                                                      DeserializationContext const& context)
	{
		std::vector<Json> loaded;
		for (Seed const& seed : seeds)
		{
			Result<Ref<DataAsset>> asset = DataAssetSerializer::Deserialize(type, seed.Document, context);
			REQUIRE_MESSAGE(asset.IsOk(), (asset ? std::string() : asset.GetError()));
			loaded.push_back(asset.GetValue()->SerializeFields());
		}
		Json const& defaults = type.GetDefaultFields();
		std::vector<std::string_view> left;
		for (FieldDescriptor const& field : type.GetFields())
		{
			std::string const name(field.Name);
			bool const set = std::ranges::any_of(loaded,
			                                     [&defaults, &name](Json const& fields)
			                                     {
													 return fields.at(name) != defaults.at(name);
												 });
			if (!set)
			{
				left.push_back(field.Name);
			}
		}
		return left;
	}
}

TEST_CASE("DataAssetSerializer: damaged documents of every registered type are refused, or load and save again")
{
	Testing::TestDataTypeScope const testData;
	std::vector<std::string> warnings;
	DeserializationContext const context = Testing::MakeTolerantContext(warnings);

	std::vector<DataAssetType const*> const types = DataAssetRegistry::GetTypes();
	REQUIRE_FALSE(types.empty());
	uint64_t seed = 9000;
	for (DataAssetType const* type : types)
	{
		CAPTURE(type->GetName());
		std::vector<Seed> seeds = FindFeatureTestSeeds(*type);
		// Every type the engine registers has a file in the FeatureTest project that the tests start from.
		CHECK_MESSAGE((!type->IsEngineType() || !seeds.empty()),
		              fmt::format("the FeatureTest project has no {} file (a file of {})", type->GetName(), type->GetExtension()));
		if (type->GetAssetType() == testData.GetType())
		{
			for (Seed& testSeed : MakeTestDataSeeds())
			{
				seeds.push_back(std::move(testSeed));
			}
			// The check below sees a seed that leaves fields at their defaults.
			Json empty = MakeTestDataSeeds().front().Document;
			empty["TestData"] = Json::object();
			CHECK(FindFieldsLeftAtDefault(*type, {{"an empty TestData document", empty}}, context) ==
			      std::vector<std::string_view>{"Shape", "Size", "Texture", "Next", "Entries"});
		}
		// Together the seeds set every field away from its default, so that every field is damaged.
		std::vector<std::string_view> const left = FindFieldsLeftAtDefault(*type, seeds, context);
		std::string leftNames;
		for (std::string_view const name : left)
		{
			leftNames += fmt::format("{}{}", leftNames.empty() ? "" : ", ", name);
		}
		CHECK_MESSAGE(left.empty(), fmt::format("no seed of {} sets {} away from its default", type->GetName(), leftNames));

		for (Seed const& original : seeds)
		{
			CAPTURE(original.Name);
			Result<Ref<DataAsset>> undamaged = DataAssetSerializer::Deserialize(*type, original.Document, context);
			REQUIRE_MESSAGE(undamaged.IsOk(), (undamaged ? std::string() : undamaged.GetError()));

			Testing::DocumentMutator mutator(seed++);
			int loaded = 0;
			int refused = 0;
			for (int round = 0, rounds = Testing::GetFuzzRounds(300); round < rounds; round++)
			{
				CAPTURE(round);
				Result<Ref<DataAsset>> asset = DataAssetSerializer::Deserialize(*type, mutator.Damage(original.Document, round), context);
				if (!asset)
				{
					refused++;
					continue;
				}
				loaded++;
				Json const fields = asset.GetValue()->SerializeFields();
				Json const saved = DataAssetSerializer::Serialize(*asset.GetValue());
				CHECK(saved.at("Strada") == MakeFileHeader(type->GetName(), static_cast<int>(type->GetFormatVersion())));
				// What loads saves in the current version and loads again to the same fields, strictly.
				Result<Ref<DataAsset>> reloaded = DataAssetSerializer::Deserialize(*type, saved, DeserializationContext{});
				REQUIRE_MESSAGE(reloaded.IsOk(), (reloaded ? std::string() : reloaded.GetError()));
				CHECK(reloaded.GetValue()->SerializeFields() == fields);
				CHECK(&reloaded.GetValue()->GetDataAssetType() == type);
			}
			CHECK(loaded > 0);
			CHECK(refused > 0);
		}
	}
}
