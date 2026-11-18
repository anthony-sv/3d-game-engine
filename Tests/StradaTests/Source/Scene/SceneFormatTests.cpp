#include "Fuzzing.h"
#include "TestUtilities.h"

#include "Strada/Asset/PrefabAsset.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"
#include "Strada/Script/ScriptFieldSerialization.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

// The scene and prefab formats' versions (Docs/Architecture.md §7.3, "Versions and migrations"), with the fixtures of
// Tests/Data/Scenes and Tests/Data/Prefabs.

namespace
{
	Json Relabeled(Json document, int version)
	{
		document["Strada"]["Version"] = version;
		return document;
	}

	Ref<Scene> Load(Json const& document)
	{
		Result<Ref<Scene>> scene = SceneSerializer::Deserialize(document, DeserializationContext{});
		REQUIRE_MESSAGE(scene.IsOk(), scene.GetError());
		return scene.GetValue();
	}

	Entity Find(Scene& scene, uint64_t id)
	{
		Entity const entity = scene.GetEntityByUUID(UUID(id));
		REQUIRE(entity);
		return entity;
	}

	ScriptFieldValue EntityList(std::vector<UUID> const& ids)
	{
		std::vector<ScriptFieldValue> elements;
		for (UUID const id : ids)
		{
			elements.push_back(ScriptFieldValue::FromEntity(id));
		}
		return ScriptFieldValue::FromList(ScriptFieldType::Entity, std::move(elements));
	}

	// The Components object of an entity of a document, by ID.
	Json ComponentsOf(Json const& document, std::string_view id)
	{
		for (Json const& entity : document["Entities"])
		{
			if (entity["ID"] == Json(id))
			{
				return entity["Components"];
			}
		}
		FAIL("no entity " << id);
		return Json();
	}
}

TEST_CASE("SceneSerializer: scenes and prefabs are written in version 2, the version readers read up to")
{
	CHECK(SceneSerializer::FormatVersion == 2);
	CHECK(SceneSerializer::GetFormat().GetVersion() == 2);
	CHECK(SceneSerializer::GetFormat().Type == "Scene");
	CHECK(PrefabAsset::FormatVersion == 2);
	CHECK(PrefabSerializer::FormatVersion == 2);
	CHECK(PrefabAsset::GetFormat().GetVersion() == 2);
	CHECK(PrefabAsset::GetFormat().Type == "Prefab");
	// Version 2 needs no migration: it only added lists, which version-1 files cannot hold.
	CHECK(SceneSerializer::GetFormat().Versions[0].Migration == nullptr);
	CHECK(PrefabAsset::GetFormat().Versions[0].Migration == nullptr);

	Scene scene("Written");
	Entity const root = scene.CreateEntity("Root");
	CHECK(SceneSerializer::Serialize(scene)["Strada"] == MakeFileHeader("Scene", 2));
	CHECK(PrefabSerializer::Serialize(scene, root)["Strada"] == MakeFileHeader("Prefab", 2));
}

TEST_CASE("SceneSerializer: a version-1 scene loads unchanged and saves as version 2")
{
	Json const original = Testing::LoadTestDataDocument("Scenes/Version1.sscene");
	REQUIRE(original["Strada"]["Version"] == 1);
	Ref<Scene> const scene = Load(original);
	CHECK(scene->GetName() == "Version 1");
	CHECK(scene->GetSettings().Physics.Gravity == glm::vec3(0.0f, -9.5f, 0.0f));
	REQUIRE(scene->GetEntityCount() == 2);

	Entity const holder = Find(*scene, 1001);
	Entity const target = Find(*scene, 1002);
	CHECK(scene->GetParent(target) == holder);
	ScriptComponent const& script = holder.GetComponent<ScriptComponent>();
	CHECK(script.ClassName == "Fixture.Holder");
	CHECK(script.Fields == ScriptFieldMap{
							   {"Speed", ScriptFieldValue::FromFloat(3.5f)},
							   {"Count", ScriptFieldValue::FromInt32(-7)},
							   {"Label", ScriptFieldValue::FromString("first")},
							   {"Target", ScriptFieldValue::FromEntity(UUID(1002))},
							   {"Elsewhere", ScriptFieldValue::FromEntity(UUID(424242))},
							   {"Crate", ScriptFieldValue::FromPrefab(AssetHandle(UUID(17)))},
						   });
	CHECK(target.GetComponent<VisualScriptComponent>().Variables == ScriptFieldMap{
																		{"Owner", ScriptFieldValue::FromEntity(UUID(1001))},
																		{"Height", ScriptFieldValue::FromDouble(0.125)},
																	});

	// Saved, it is version 2, and what it holds is what the same content holds read as version 2: the migration
	// changes nothing.
	Json const saved = SceneSerializer::Serialize(*scene);
	CHECK(saved["Strada"] == MakeFileHeader("Scene", 2));
	CHECK(SceneSerializer::Serialize(*Load(Relabeled(original, 2))) == saved);
	for (char const* id : {"1001", "1002"})
	{
		CAPTURE(id);
		Json const originalComponents = ComponentsOf(original, id);
		Json const savedComponents = ComponentsOf(saved, id);
		for (auto const& [name, component] : originalComponents.items())
		{
			CAPTURE(name);
			if (name == "Script" || name == "VisualScript" || name == "Tag" || name == "Relationship")
			{
				CHECK(savedComponents[name] == component);
			}
		}
	}

	// A file is migrated in place as it loads, to the same scene.
	Testing::TemporaryDirectory directory;
	std::filesystem::path const file = directory.GetPath() / "Version1.sscene";
	REQUIRE(FileSystem::Copy(Testing::GetTestDataPath("Scenes/Version1.sscene"), file, false).IsOk());
	Result<Ref<Scene>> const loaded = SceneSerializer::LoadFromFile(file, DeserializationContext{});
	REQUIRE_MESSAGE(loaded.IsOk(), loaded.GetError());
	CHECK(SceneSerializer::Serialize(*loaded.GetValue()) == saved);
}

TEST_CASE("SceneSerializer: a version-2 scene holds lists of every element type, and saves them as it read them")
{
	Json const original = Testing::LoadTestDataDocument("Scenes/Version2.sscene");
	Ref<Scene> const scene = Load(original);
	Entity const holder = Find(*scene, 1001);
	Entity const target = Find(*scene, 1002);

	ScriptFieldMap const& fields = holder.GetComponent<ScriptComponent>().Fields;
	std::vector<bool> listed(static_cast<size_t>(ScriptFieldType::Asset) + 1, false);
	for (auto const& [name, value] : fields)
	{
		if (value.IsList())
		{
			listed[static_cast<size_t>(value.GetElementType())] = true;
		}
	}
	for (int i = 1; i <= static_cast<int>(ScriptFieldType::Asset); i++)
	{
		CAPTURE(ScriptFieldTypeToString(static_cast<ScriptFieldType>(i)));
		CHECK(listed[static_cast<size_t>(i)]);
	}
	CHECK(fields.at("Speed") == ScriptFieldValue::FromFloat(3.5f));
	CHECK(fields.at("Int64s") == ScriptFieldValue::FromList(ScriptFieldType::Int64, {ScriptFieldValue::FromInt64(-9007199254740993LL),
	                                                                                 ScriptFieldValue::FromInt64(4)}));
	CHECK(fields.at("Strings") ==
	      ScriptFieldValue::FromList(ScriptFieldType::String, {ScriptFieldValue::FromString(""), ScriptFieldValue::FromString("hello"),
	                                                           ScriptFieldValue::FromString("two words")}));
	CHECK(fields.at("Entities") == EntityList({UUID(1002), UUID::Invalid(), UUID(424242), UUID(1001)}));
	CHECK(fields.at("Empty") == ScriptFieldValue::FromList(ScriptFieldType::String, {}));
	ScriptFieldMap const& variables = target.GetComponent<VisualScriptComponent>().Variables;
	CHECK(variables.at("Path") ==
	      ScriptFieldValue::FromList(ScriptFieldType::Vector3, {ScriptFieldValue::FromVector3({0.0f, 1.0f, 0.0f}),
	                                                            ScriptFieldValue::FromVector3({4.0f, 1.0f, -2.0f})}));
	CHECK(variables.at("Steps") == ScriptFieldValue::FromList(ScriptFieldType::Int64, {}));

	// Saved, every list is written as the fixture holds it, and what is saved loads to the same document again.
	Json const saved = SceneSerializer::Serialize(*scene);
	CHECK(saved["Strada"] == MakeFileHeader("Scene", 2));
	CHECK(ComponentsOf(saved, "1001")["Script"] == ComponentsOf(original, "1001")["Script"]);
	CHECK(ComponentsOf(saved, "1002")["VisualScript"] == ComponentsOf(original, "1002")["VisualScript"]);
	CHECK(SceneSerializer::Serialize(*Load(saved)) == saved);
}

TEST_CASE("SceneSerializer: scenes and prefabs of a version after 2 are refused, by every reader")
{
	Json const newer = Relabeled(Testing::LoadTestDataDocument("Scenes/Version2.sscene"), 3);
	CHECK(SceneSerializer::Deserialize(newer, DeserializationContext{}).GetError() ==
	      "file version 3 is newer than the supported version 2");

	Testing::TemporaryDirectory directory;
	std::filesystem::path const file = directory.GetPath() / "Newer.sscene";
	REQUIRE(FileSystem::WriteTextFile(file, DumpJson(newer)).IsOk());
	Result<Ref<Scene>> const loaded = SceneSerializer::LoadFromFile(file, DeserializationContext{});
	REQUIRE(loaded.IsError());
	CHECK(loaded.GetError().ends_with("': file version 3 is newer than the supported version 2"));

	Json const newerPrefab = Relabeled(Testing::LoadTestDataDocument("Prefabs/Version2.sprefab"), 3);
	CHECK(PrefabAsset::Create(newerPrefab).GetError() == "file version 3 is newer than the supported version 2");
	Scene scene("Instances");
	Result<Entity> const instance =
		PrefabSerializer::Instantiate(scene, newerPrefab, AssetHandle(UUID(5)), Entity(), DeserializationContext{});
	CHECK(instance.GetError() == "file version 3 is newer than the supported version 2");
	CHECK(scene.GetEntityCount() == 0);

	CHECK(SceneSerializer::Deserialize(Relabeled(newer, 0), DeserializationContext{}).GetError() == "invalid file version 0");
	CHECK(PrefabAsset::Create(Relabeled(newerPrefab, 0)).IsError());
}

TEST_CASE("SceneSerializer: a damaged list refuses the scene, and an entity it is applied to stays as it was")
{
	Json damaged = Testing::LoadTestDataDocument("Scenes/Version2.sscene");
	damaged["Entities"][0]["Components"]["Script"]["Fields"]["Floats"]["Value"][1] = "fast";
	CHECK(SceneSerializer::Deserialize(damaged, DeserializationContext{}).GetError() ==
	      "entity 1001: Script.Fields: field 'Floats': element 1: expected a number");

	Json tooLong = Testing::LoadTestDataDocument("Scenes/Version2.sscene");
	Json& elements = tooLong["Entities"][1]["Components"]["VisualScript"]["Variables"]["Steps"]["Value"];
	for (size_t i = 0; i <= MaxScriptFieldListElements; i++)
	{
		elements.push_back("1");
	}
	CHECK(SceneSerializer::Deserialize(tooLong, DeserializationContext{}).GetError() ==
	      "entity 1002: VisualScript.Variables: field 'Steps': expected an array of at most 4096 elements");

	// Applied to an entity of a loaded scene (as automation and undo apply components), the damaged list changes nothing.
	Ref<Scene> const scene = Load(Testing::LoadTestDataDocument("Scenes/Version2.sscene"));
	Entity const holder = Find(*scene, 1001);
	ScriptComponent const before = holder.GetComponent<ScriptComponent>();
	Json const patch = Json::object({{"Script", Json::object({{"Fields", damaged["Entities"][0]["Components"]["Script"]["Fields"]}})}});
	CHECK(SceneSerializer::DeserializeComponents(*scene, holder.GetHandle(), patch, DeserializationContext{}).IsError());
	CHECK(holder.GetComponent<ScriptComponent>().ClassName == before.ClassName);
	CHECK(holder.GetComponent<ScriptComponent>().Fields == before.Fields);
	CHECK(scene->GetEntityCount() == 2);
}

TEST_CASE("Prefab: prefabs of versions 1 and 2 instantiate, with the entity references of lists remapped")
{
	AssetHandle const prefab(UUID(5000));
	for (char const* file : {"Prefabs/Version1.sprefab", "Prefabs/Version2.sprefab"})
	{
		CAPTURE(file);
		Json const document = Testing::LoadTestDataDocument(file);
		Scene scene("Instances");
		Result<Entity> const instance = PrefabSerializer::Instantiate(scene, document, prefab, Entity(), DeserializationContext{});
		REQUIRE_MESSAGE(instance.IsOk(), instance.GetError());
		Entity const turret = instance.GetValue();
		std::vector<Entity> const children = scene.GetChildren(turret);
		REQUIRE(children.size() == 1);
		Entity const barrel = children[0];
		CHECK(turret.GetUUID() != UUID(2001));
		CHECK(barrel.GetUUID() != UUID(2002));

		ScriptFieldMap const& fields = turret.GetComponent<ScriptComponent>().Fields;
		ScriptFieldMap const& variables = barrel.GetComponent<VisualScriptComponent>().Variables;
		CHECK(fields.at("Barrel") == ScriptFieldValue::FromEntity(barrel.GetUUID()));
		CHECK(variables.at("Mount") == ScriptFieldValue::FromEntity(turret.GetUUID()));
		if (document["Strada"]["Version"] == 2)
		{
			// References into the prefab follow the instance, in lists as in single fields; others and none are kept.
			CHECK(fields.at("Entities") == EntityList({barrel.GetUUID(), turret.GetUUID(), UUID(999), UUID::Invalid()}));
			CHECK(variables.at("Linked") == EntityList({turret.GetUUID(), UUID(999)}));
			CHECK(fields.at("Vector4s") == ScriptFieldValue::FromList(ScriptFieldType::Vector4, {}));
			CHECK(fields.at("Assets") ==
			      ScriptFieldValue::FromList(ScriptFieldType::Asset, {ScriptFieldValue::FromAsset(AssetHandle(UUID(66))),
			                                                          ScriptFieldValue::FromAsset(AssetHandle(UUID(67)))}));
		}
		else
		{
			CHECK(fields.at("Elsewhere") == ScriptFieldValue::FromEntity(UUID(999)));
		}

		// Every other field arrives as the prefab holds it.
		Json const& sourceFields = document["Entities"][0]["Components"]["Script"]["Fields"];
		Json const instanceFields = JsonTraits<ScriptFieldMap>::ToJson(fields);
		for (auto const& [name, value] : sourceFields.items())
		{
			CAPTURE(name);
			if (value["Type"] != "Entity" && value["Type"] != "Entity[]")
			{
				CHECK(instanceFields[name] == value);
			}
		}

		// The scene that holds the instance saves and loads again.
		CHECK(SceneSerializer::Deserialize(SceneSerializer::Serialize(scene), DeserializationContext{}).IsOk());
	}
}

TEST_CASE("PrefabAsset: a prefab's document is migrated to the current version as it loads")
{
	Json const version1 = Testing::LoadTestDataDocument("Prefabs/Version1.sprefab");
	Result<Ref<PrefabAsset>> const asset = PrefabAsset::Create(version1);
	REQUIRE_MESSAGE(asset.IsOk(), asset.GetError());
	Json const& document = asset.GetValue()->GetDocument();
	CHECK(document["Strada"] == MakeFileHeader("Prefab", 2));
	CHECK(document["Entities"] == version1["Entities"]);

	Json const version2 = Testing::LoadTestDataDocument("Prefabs/Version2.sprefab");
	Result<Ref<PrefabAsset>> const current = PrefabAsset::Create(version2);
	REQUIRE(current.IsOk());
	CHECK(current.GetValue()->GetDocument() == version2);

	Json other = version1;
	other["Strada"]["Type"] = "Scene";
	CHECK(PrefabAsset::Create(other).GetError() == "expected a Prefab file but found 'Scene'");
}

TEST_CASE("Scene: duplicating an entity remaps the entity references in its lists")
{
	Scene scene("Duplicates");
	Entity const outside = scene.CreateEntity("Outside");
	Entity holder = scene.CreateEntity("Holder");
	Entity child = scene.CreateEntity("Child", holder);
	holder.AddComponent<ScriptComponent>().Fields["Watched"] =
		EntityList({child.GetUUID(), holder.GetUUID(), outside.GetUUID(), UUID::Invalid()});
	holder.GetComponent<ScriptComponent>().Fields["Speed"] = ScriptFieldValue::FromFloat(2.0f);
	VisualScriptComponent& visualScript = child.AddComponent<VisualScriptComponent>();
	visualScript.Variables["Holders"] = EntityList({holder.GetUUID(), outside.GetUUID()});
	visualScript.Variables["Points"] = ScriptFieldValue::FromList(ScriptFieldType::Vector2, {ScriptFieldValue::FromVector2({1.0f, 2.0f})});

	Entity const copy = scene.DuplicateEntity(holder);
	REQUIRE(copy);
	std::vector<Entity> const copyChildren = scene.GetChildren(copy);
	REQUIRE(copyChildren.size() == 1);
	Entity const childCopy = copyChildren[0];

	CHECK(copy.GetComponent<ScriptComponent>().Fields.at("Watched") ==
	      EntityList({childCopy.GetUUID(), copy.GetUUID(), outside.GetUUID(), UUID::Invalid()}));
	CHECK(copy.GetComponent<ScriptComponent>().Fields.at("Speed") == ScriptFieldValue::FromFloat(2.0f));
	CHECK(childCopy.GetComponent<VisualScriptComponent>().Variables.at("Holders") == EntityList({copy.GetUUID(), outside.GetUUID()}));
	CHECK(childCopy.GetComponent<VisualScriptComponent>().Variables.at("Points") == visualScript.Variables.at("Points"));
	// The originals keep theirs.
	CHECK(holder.GetComponent<ScriptComponent>().Fields.at("Watched") ==
	      EntityList({child.GetUUID(), holder.GetUUID(), outside.GetUUID(), UUID::Invalid()}));
	CHECK(child.GetComponent<VisualScriptComponent>().Variables.at("Holders") == EntityList({holder.GetUUID(), outside.GetUUID()}));
}
