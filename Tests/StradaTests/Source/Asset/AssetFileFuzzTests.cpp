#include "Fuzzing.h"

#include "Strada/Asset/AssetRegistry.h"
#include "Strada/Asset/MaterialAsset.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace Strada;

TEST_CASE("MaterialSerializer: damaged material files are refused or load and save again")
{
	std::vector<std::string> warnings;
	DeserializationContext const context = Testing::MakeTolerantContext(warnings);
	Json const original = Testing::LoadFeatureTestDocument("Assets/Materials/Painted.smat");
	REQUIRE(MaterialSerializer::Deserialize(original, context).IsOk());

	Testing::DocumentMutator mutator(3000);
	int loaded = 0;
	int refused = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(300); round < rounds; round++)
	{
		CAPTURE(round);
		Result<MaterialData> material = MaterialSerializer::Deserialize(mutator.Damage(original, round), context);
		if (!material)
		{
			refused++;
			continue;
		}
		loaded++;
		Result<MaterialData> reloaded = MaterialSerializer::Deserialize(MaterialSerializer::Serialize(material.GetValue()), context);
		REQUIRE_MESSAGE(reloaded.IsOk(), (reloaded ? std::string() : reloaded.GetError()));
		CHECK(reloaded.GetValue() == material.GetValue());
	}
	CHECK(loaded > 0);
	CHECK(refused > 0);
}

TEST_CASE("AssetRegistry: damaged registries are refused or load and save again")
{
	std::vector<std::string> warnings;
	DeserializationContext const context = Testing::MakeTolerantContext(warnings);
	Json const original = Testing::LoadFeatureTestDocument("Assets/AssetRegistry.sreg");
	REQUIRE(AssetRegistry::Deserialize(original, context).IsOk());

	Testing::DocumentMutator mutator(4000);
	int loaded = 0;
	int refused = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(300); round < rounds; round++)
	{
		CAPTURE(round);
		Result<AssetRegistry> registry = AssetRegistry::Deserialize(mutator.Damage(original, round), context);
		if (!registry)
		{
			refused++;
			continue;
		}
		loaded++;
		Result<AssetRegistry> reloaded = AssetRegistry::Deserialize(registry.GetValue().Serialize(), context);
		REQUIRE_MESSAGE(reloaded.IsOk(), (reloaded ? std::string() : reloaded.GetError()));
		CHECK(reloaded.GetValue().GetAll().size() == registry.GetValue().GetAll().size());
	}
	CHECK(loaded > 0);
	CHECK(refused > 0);
}
