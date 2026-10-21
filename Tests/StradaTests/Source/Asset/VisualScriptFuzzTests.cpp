#include "Fuzzing.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/VisualScriptEngine.h"

#include <doctest/doctest.h>

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
}

TEST_CASE("VisualScript: damaged files are refused or load and save again")
{
	Json const original = Testing::LoadFeatureTestDocument("Assets/Scripts/FormatSample.sgraph");
	std::vector<std::string> warnings;
	std::vector<VisualScriptIssue> issues;
	REQUIRE(VisualScriptSerializer::Deserialize(original, Testing::MakeTolerantContext(warnings), &issues).IsOk());
	REQUIRE(issues.empty());

	Testing::DocumentMutator mutator(10000);
	int loaded = 0;
	int refused = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(300); round < rounds; round++)
	{
		CAPTURE(round);
		if (LoadDamagedFile(mutator.Damage(original, round)))
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
	Json const original = Testing::LoadFeatureTestDocument("Assets/Scripts/Features.sgraph");
	Testing::AssetManagerScope assets;
	Testing::ScriptEngineScope scripting;

	// Each document that loads runs on an entity of a scene that starts, updates and stops: whatever the runtime makes of
	// it, including failures, nothing crashes or hangs.
	Testing::DocumentMutator mutator(10100);
	int ran = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(100); round < rounds; round++)
	{
		CAPTURE(round);
		std::optional<VisualScriptDocument> document = LoadDamagedFile(mutator.Damage(original, round));
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
				ran++;
			}
			for (int frame = 0; frame < 5; frame++)
			{
				scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
			}
			scene.OnRuntimeStop();
		}
		AssetManager::RemoveMemoryAsset(graph);
	}
	CHECK(ran > 0);
}
