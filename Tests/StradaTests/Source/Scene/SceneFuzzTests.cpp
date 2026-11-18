#include "Audio/AudioTestUtilities.h"
#include "Fuzzing.h"
#include "Physics/PhysicsTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Core/Input.h"
#include "Strada/Core/KeyCodes.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/PostProcessVolumes.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"
#include "Strada/Scene/UILayout.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
	// Input is global: runs that feed it restore it.
	class InputScope
	{
	public:
		InputScope() { Input::Reset(); }
		~InputScope() { Input::Reset(); }

		InputScope(InputScope const&) = delete;
		InputScope& operator=(InputScope const&) = delete;
	};

	// Where a fuzz case's seed document comes from: the FeatureTest project, or Tests/Data.
	enum class SeedSource : uint8_t
	{
		FeatureTestProject,
		TestDataFixture
	};

	struct SeedDocument
	{
		SeedSource Source = SeedSource::FeatureTestProject;
		std::string_view File;
		int Rounds = 0;

		Json Load() const
		{
			return Source == SeedSource::FeatureTestProject ? Testing::LoadFeatureTestDocument(File) : Testing::LoadTestDataDocument(File);
		}
	};

	bool IsFinite(glm::vec2 const& value)
	{
		return std::isfinite(value.x) && std::isfinite(value.y);
	}

	// A few frames of the scene's runtime, with physics and sound (scripts do not run without the script engine), on a
	// 1280 x 720 view whose UI is laid out first: every rectangle must be finite and not inverted. The mouse moves over the
	// middle of the screen (the feature test's Play button, then its panel) and presses and releases there, so the UI follows
	// it and clicks.
	void Run(Scene& scene)
	{
		InputScope input;
		glm::vec2 const viewport(1280.0f, 720.0f);
		scene.OnViewportResize(static_cast<uint32_t>(viewport.x), static_cast<uint32_t>(viewport.y));
		for (UIElement const& element : UILayout::LayoutScene(scene, viewport))
		{
			UIRectangle const& rectangle = element.Rectangle;
			CAPTURE(rectangle.Min.x);
			CAPTURE(rectangle.Min.y);
			CAPTURE(rectangle.Max.x);
			CAPTURE(rectangle.Max.y);
			CHECK((IsFinite(rectangle.Min) && IsFinite(rectangle.Max) && rectangle.Min.x <= rectangle.Max.x &&
			       rectangle.Min.y <= rectangle.Max.y));
			CHECK((std::isfinite(element.Scale) && element.Scale > 0.0f && element.Opacity >= 0.0f && element.Opacity <= 1.0f));
		}

		scene.OnRuntimeStart();
		for (int frame = 0; frame < 5; frame++)
		{
			Input::SetMousePosition(frame < 3 ? glm::vec2(640.0f, 400.0f) : glm::vec2(640.0f, 300.0f));
			Input::SetMouseButtonState(MouseButton::Left, frame % 2 == 1);
			scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
			Input::EndFrame();
		}
		scene.OnRuntimeStop();
	}
}

TEST_CASE("SceneSerializer: damaged scene files are rejected or repaired, never crash")
{
	std::vector<std::string> warnings;
	DeserializationContext const context = Testing::MakeTolerantContext(warnings);

	// The feature test's scenes, then the format's version fixtures (Tests/Data/Scenes): a scene of each version the reader
	// takes, the second with lists of every element type.
	uint64_t seed = 1;
	for (SeedDocument const& seedDocument : {SeedDocument{SeedSource::FeatureTestProject, "Assets/Scenes/Main.sscene", 400},
	                                         SeedDocument{SeedSource::FeatureTestProject, "Assets/Scenes/Second.sscene", 150},
	                                         SeedDocument{SeedSource::TestDataFixture, "Scenes/Version1.sscene", 150},
	                                         SeedDocument{SeedSource::TestDataFixture, "Scenes/Version2.sscene", 300}})
	{
		std::string const file(seedDocument.File);
		int const rounds = seedDocument.Rounds;
		Json const original = seedDocument.Load();
		REQUIRE(SceneSerializer::Deserialize(original, context).IsOk());

		Testing::DocumentMutator mutator(seed++);
		int loaded = 0;
		int rejected = 0;
		for (int round = 0, scaled = Testing::GetFuzzRounds(rounds); round < scaled; round++)
		{
			CAPTURE(file);
			CAPTURE(round);
			Result<Ref<Scene>> scene = SceneSerializer::Deserialize(mutator.Damage(original, round), context);
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
	Testing::PhysicsSystemScope physics;
	Testing::AudioEngineScope audio;
	Testing::AssetManagerScope assets;
	std::vector<std::string> warnings;
	DeserializationContext const context = Testing::MakeTolerantContext(warnings);
	Json const original = Testing::LoadFeatureTestDocument("Assets/Scenes/Main.sscene");

	Testing::DocumentMutator mutator(1000);
	int ran = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(300); round < rounds; round++)
	{
		CAPTURE(round);
		Result<Ref<Scene>> scene = SceneSerializer::Deserialize(mutator.Damage(original, round), context);
		if (scene)
		{
			// Post-process volumes resolve to valid renderer settings: in the feature test's volume, in its blend zone, and
			// where the camera starts.
			for (glm::vec3 const position : {glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 4.0f), glm::vec3(0.0f, 2.5f, 7.0f)})
			{
				Json const resolved =
					SerializeFields<StructTraits<SceneRendererSettings>>(PostProcessVolumes::Resolve(*scene.GetValue(), position));
				SceneRendererSettings settings;
				Result<void> const valid =
					DeserializeFields<StructTraits<SceneRendererSettings>>(resolved, settings, DeserializationContext{}, "settings");
				CHECK_MESSAGE(valid.IsOk(), (valid ? std::string() : valid.GetError()));
			}
			Run(*scene.GetValue());
			ran++;
		}
	}
	CHECK(ran > 0);
}

TEST_CASE("PrefabSerializer: damaged prefabs are refused or instantiate and run")
{
	Testing::PhysicsSystemScope physics;
	Testing::AudioEngineScope audio;
	Testing::AssetManagerScope assets;
	std::vector<std::string> warnings;
	DeserializationContext const context = Testing::MakeTolerantContext(warnings);
	AssetHandle const prefab(UUID(1234));

	// The feature test's crate, then the format's version fixtures (Tests/Data/Prefabs): a prefab of each version the
	// reader takes, the second with lists of every element type.
	uint64_t seed = 2000;
	for (SeedDocument const& seedDocument : {SeedDocument{SeedSource::FeatureTestProject, "Assets/Prefabs/Crate.sprefab", 300},
	                                         SeedDocument{SeedSource::TestDataFixture, "Prefabs/Version1.sprefab", 150},
	                                         SeedDocument{SeedSource::TestDataFixture, "Prefabs/Version2.sprefab", 300}})
	{
		std::string const file(seedDocument.File);
		CAPTURE(file);
		Json const original = seedDocument.Load();
		Testing::DocumentMutator mutator(seed++);
		int instantiated = 0;
		int refused = 0;
		for (int round = 0, rounds = Testing::GetFuzzRounds(seedDocument.Rounds); round < rounds; round++)
		{
			CAPTURE(round);
			Scene scene("Prefabs");
			Entity const parent = scene.CreateEntity("Parent");
			size_t const entityCount = scene.GetEntityCount();
			Result<Entity> instance =
				PrefabSerializer::Instantiate(scene, mutator.Damage(original, round), prefab, round % 2 == 0 ? parent : Entity(), context);
			if (!instance)
			{
				// Nothing is created on failure.
				CHECK(scene.GetEntityCount() == entityCount);
				refused++;
				continue;
			}
			instantiated++;
			Json const saved = SceneSerializer::Serialize(scene);
			Result<Ref<Scene>> reloaded = SceneSerializer::Deserialize(saved, context);
			CHECK_MESSAGE(reloaded.IsOk(), (reloaded ? std::string() : reloaded.GetError()));
			Run(scene);
		}
		CHECK(instantiated > 0);
		CHECK(refused > 0);
	}
}
