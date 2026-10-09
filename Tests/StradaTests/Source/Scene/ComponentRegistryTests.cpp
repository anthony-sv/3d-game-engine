#include "Strada/Scene/ComponentRegistry.h"
#include "Strada/Scene/ComponentSerialization.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>

#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

TEST_CASE("Components: every component is registered once with a unique name")
{
	std::span<ComponentInfo const> const components = ComponentRegistry::GetComponents();
	REQUIRE(components.size() == 21);

	std::set<std::string_view> names;
	for (ComponentInfo const& info : components)
	{
		CAPTURE(info.Name);
		CHECK(names.insert(info.Name).second);
		CHECK(ComponentRegistry::Find(info.Name) == &info);
		CHECK(info.Has != nullptr);
		CHECK(info.Add != nullptr);
		CHECK(info.Remove != nullptr);
		CHECK(info.Serialize != nullptr);
		CHECK(info.Deserialize != nullptr);
		CHECK(info.Copy != nullptr);
		CHECK_FALSE(info.Description.empty());
		CHECK_FALSE(info.Fields.empty());
		CHECK(info.Describe()["Fields"].size() == info.Fields.size());
	}

	CHECK(ComponentRegistry::Find("DoesNotExist") == nullptr);
	CHECK(ComponentRegistry::Get<TransformComponent>().Name == "Transform");
	CHECK(ComponentRegistry::Get<TransformComponent>().IsCore());
	CHECK_FALSE(ComponentRegistry::Get<MeshComponent>().IsCore());
	CHECK(ComponentRegistry::Get<RelationshipComponent>().IsInternal());
	CHECK(ComponentRegistry::Get<PrefabComponent>().IsInternal());
}

TEST_CASE("Components: every component round trips through JSON with its default values")
{
	Scene scene;
	Entity const entity = scene.CreateEntity("Source");
	entt::registry& registry = scene.GetRegistry();

	for (ComponentInfo const& info : ComponentRegistry::GetComponents())
	{
		CAPTURE(info.Name);
		info.Add(registry, entity.GetHandle());
		REQUIRE(info.Has(registry, entity.GetHandle()));

		Json const json = info.Serialize(registry, entity.GetHandle());
		REQUIRE(json.is_object());

		Entity const target = scene.CreateEntity("Target");
		info.Remove(registry, target.GetHandle());
		if (!info.IsCore())
		{
			CHECK_FALSE(info.Has(registry, target.GetHandle()));
		}
		Result<void> const result = info.Deserialize(registry, target.GetHandle(), json, DeserializationContext{});
		REQUIRE_MESSAGE(result.IsOk(), result.GetError());
		CHECK(info.Serialize(registry, target.GetHandle()) == json);
	}
}

TEST_CASE("Components: field values survive a round trip")
{
	RigidBodyComponent body;
	body.Type = RigidBodyType::Dynamic;
	body.Mass = 12.5f;
	body.Layer = 3;
	body.LockRotation = {true, false, true};
	body.InitialLinearVelocity = {1.0f, 2.0f, 3.0f};

	RigidBodyComponent loaded;
	REQUIRE(DeserializeComponent(SerializeComponent(body), loaded, DeserializationContext{}).IsOk());
	CHECK(loaded.Type == RigidBodyType::Dynamic);
	CHECK(loaded.Mass == doctest::Approx(12.5f));
	CHECK(loaded.Layer == 3u);
	CHECK(loaded.LockRotation == glm::bvec3(true, false, true));
	CHECK(loaded.InitialLinearVelocity == glm::vec3(1.0f, 2.0f, 3.0f));

	ScriptComponent script;
	script.ClassName = "Game.Player";
	script.Fields["Speed"] = ScriptFieldValue::FromFloat(4.5f);
	script.Fields["Target"] = ScriptFieldValue::FromEntity(UUID(77));
	script.Fields["Score"] = ScriptFieldValue::FromInt64(-9007199254740993LL);
	ScriptComponent loadedScript;
	REQUIRE(DeserializeComponent(SerializeComponent(script), loadedScript, DeserializationContext{}).IsOk());
	CHECK(loadedScript.ClassName == "Game.Player");
	CHECK(loadedScript.Fields == script.Fields);
}

TEST_CASE("Components: partial updates only touch the given fields")
{
	PointLightComponent light;
	light.Range = 25.0f;

	Json patch = Json::object();
	patch["Intensity"] = 3.0;
	REQUIRE(DeserializeComponent(patch, light, DeserializationContext{}).IsOk());
	CHECK(light.Intensity == doctest::Approx(3.0f));
	CHECK(light.Range == doctest::Approx(25.0f));
	CHECK(light.Color == glm::vec3(1.0f));
}

TEST_CASE("Components: invalid input leaves the component untouched and explains the problem")
{
	TransformComponent transform;
	transform.Translation = {1.0f, 2.0f, 3.0f};

	Json patch = Json::object();
	patch["Scale"] = Json::array({2, 2, 2});
	patch["Translation"] = Json::array({1, 2});
	Result<void> const result = DeserializeComponent(patch, transform, DeserializationContext{});
	REQUIRE(result.IsError());
	CHECK(result.GetError() == "Transform.Translation: expected an array of 3 numbers");
	CHECK(transform.Translation == glm::vec3(1.0f, 2.0f, 3.0f));
	CHECK(transform.Scale == glm::vec3(1.0f));

	CHECK(DeserializeComponent(Json::array(), transform, DeserializationContext{}).GetError() ==
	      "component 'Transform' must be a JSON object");
}

TEST_CASE("Components: unknown fields follow the configured policy")
{
	Json patch = Json::object();
	patch["Translatoin"] = Json::array({1, 2, 3});

	TransformComponent transform;
	Result<void> const strict = DeserializeComponent(patch, transform, DeserializationContext{});
	REQUIRE(strict.IsError());
	CHECK(strict.GetError() == "component 'Transform' has no field 'Translatoin' (fields: Translation, Rotation, Scale)");

	std::vector<std::string> warnings;
	DeserializationContext lenient;
	lenient.UnknownFields = UnknownFieldPolicy::Warn;
	lenient.Warnings = &warnings;
	CHECK(DeserializeComponent(patch, transform, lenient).IsOk());
	REQUIRE(warnings.size() == 1);
	CHECK(warnings[0] == "ignored unknown field 'Translatoin' in component 'Transform'");

	DeserializationContext silent;
	silent.UnknownFields = UnknownFieldPolicy::Ignore;
	CHECK(DeserializeComponent(patch, transform, silent).IsOk());
}

TEST_CASE("Components: transforms accept Euler angles in degrees")
{
	Json patch = Json::object();
	patch["RotationEuler"] = Json::array({0, 90, 0});
	TransformComponent transform;
	REQUIRE(DeserializeComponent(patch, transform, DeserializationContext{}).IsOk());
	CHECK(transform.GetRotationEuler().y == doctest::Approx(90.0f).epsilon(1e-3));

	patch["RotationEuler"] = "sideways";
	CHECK(DeserializeComponent(patch, transform, DeserializationContext{}).IsError());
}

TEST_CASE("Components: failed registry deserialization does not add the component")
{
	Scene scene;
	Entity entity = scene.CreateEntity();
	ComponentInfo const& info = ComponentRegistry::Get<RigidBodyComponent>();

	Json bad = Json::object();
	bad["Mass"] = "heavy";
	Result<void> const result = info.Deserialize(scene.GetRegistry(), entity.GetHandle(), bad, DeserializationContext{});
	REQUIRE(result.IsError());
	CHECK(result.GetError() == "RigidBody.Mass: expected a number");
	CHECK_FALSE(entity.HasComponent<RigidBodyComponent>());
}

TEST_CASE("Components: schemas describe names, types and defaults")
{
	Json const schema = ComponentRegistry::Get<SpotLightComponent>().Describe();
	CHECK(schema["Name"] == "SpotLight");
	REQUIRE(schema["Fields"].is_array());
	REQUIRE(schema["Fields"].size() == 6);
	CHECK(schema["Fields"][0]["Name"] == "Color");
	CHECK(schema["Fields"][0]["Type"] == "vec3");
	CHECK(schema["Fields"][0]["Default"] == Json::array({1.0f, 1.0f, 1.0f}));
	CHECK(schema["Fields"][3]["Name"] == "InnerConeAngle");
	CHECK(schema["Fields"][3]["Default"] == 20.0f);

	Json const body = ComponentRegistry::Get<RigidBodyComponent>().Describe();
	CHECK(body["Fields"][0]["Type"] == "Static|Dynamic|Kinematic");
	CHECK(body["Fields"][0]["Default"] == "Static");
}

TEST_CASE("Components: copying between registries replaces existing components")
{
	Scene source;
	Scene destination;
	Entity from = source.CreateEntity();
	from.AddComponent<PointLightComponent>().Intensity = 42.0f;
	Entity to = destination.CreateEntity();
	to.AddComponent<PointLightComponent>().Intensity = 1.0f;

	ComponentRegistry::Get<PointLightComponent>().Copy(source.GetRegistry(), from.GetHandle(), destination.GetRegistry(), to.GetHandle());
	CHECK(to.GetComponent<PointLightComponent>().Intensity == doctest::Approx(42.0f));

	// Copying a component the source does not have changes nothing.
	ComponentRegistry::Get<SpotLightComponent>().Copy(source.GetRegistry(), from.GetHandle(), destination.GetRegistry(), to.GetHandle());
	CHECK_FALSE(to.HasComponent<SpotLightComponent>());
}

TEST_CASE("Components: field ranges are enforced and explained")
{
	PointLightComponent light;
	CHECK(DeserializeComponent(Json::object({{"Range", -1.0}}), light, DeserializationContext{}).GetError() ==
	      "PointLight.Range: must be at least 0");
	CHECK(light.Range == doctest::Approx(10.0f));
	CHECK(DeserializeComponent(Json::object({{"Color", {1.5, 0, 0}}}), light, DeserializationContext{}).GetError() ==
	      "PointLight.Color: every component must be between 0 and 1");
	CHECK(light.Color == glm::vec3(1.0f));

	// Bounds are inclusive, also for bounds that floats cannot represent exactly.
	SpotLightComponent spot;
	REQUIRE(DeserializeComponent(Json::object({{"OuterConeAngle", 89.9}, {"InnerConeAngle", 0.0}}), spot, DeserializationContext{}).IsOk());
	CHECK(spot.OuterConeAngle == doctest::Approx(89.9f));
	CHECK(DeserializeComponent(Json::object({{"OuterConeAngle", 90.0}}), spot, DeserializationContext{}).GetError() ==
	      "SpotLight.OuterConeAngle: must be between 0.1 and 89.9");

	RigidBodyComponent body;
	CHECK(DeserializeComponent(Json::object({{"Layer", 16}}), body, DeserializationContext{}).GetError() ==
	      "RigidBody.Layer: must be between 0 and 15");
	REQUIRE(DeserializeComponent(Json::object({{"Layer", 15}}), body, DeserializationContext{}).IsOk());
	CHECK(body.Layer == 15u);

	// Every default value lies inside its field's range.
	for (ComponentInfo const& info : ComponentRegistry::GetComponents())
	{
		CAPTURE(info.Name);
		for (FieldDescriptor const& field : info.Fields)
		{
			if (!field.Hints.HasRange())
			{
				continue;
			}
			CAPTURE(field.Name);
			Scene scene;
			Entity const entity = scene.CreateEntity();
			Json patch = Json::object();
			patch[std::string(field.Name)] = field.Default;
			CHECK(info.Deserialize(scene.GetRegistry(), entity.GetHandle(), patch, DeserializationContext{}).IsOk());
		}
	}
}

TEST_CASE("Components: field descriptors carry kinds, hints and enum values")
{
	ComponentInfo const& camera = ComponentRegistry::Get<CameraComponent>();
	FieldDescriptor const* projection = camera.FindField("Projection");
	REQUIRE(projection != nullptr);
	CHECK(projection->Kind == FieldKind::Enum);
	CHECK(projection->EnumValues == std::vector<std::string_view>{"Perspective", "Orthographic"});
	FieldDescriptor const* fov = camera.FindField("PerspectiveFOV");
	REQUIRE(fov != nullptr);
	CHECK(fov->Kind == FieldKind::Float);
	CHECK(fov->Hints.Display == FieldDisplay::Angle);
	CHECK(fov->Hints.Min == 1.0);
	CHECK(fov->Hints.Max == 179.0);
	CHECK(fov->Default == 60.0f);
	CHECK(camera.FindField("Missing") == nullptr);

	ComponentInfo const& mesh = ComponentRegistry::Get<MeshComponent>();
	CHECK(mesh.FindField("Mesh")->Kind == FieldKind::Asset);
	CHECK(mesh.FindField("Mesh")->Hints.AssetTypeName == "Mesh");
	CHECK(mesh.FindField("Materials")->Kind == FieldKind::Array);
	CHECK(mesh.FindField("Materials")->ElementKind == FieldKind::Asset);
	CHECK(mesh.FindField("Materials")->Hints.AssetTypeName == "Material");

	ComponentInfo const& transform = ComponentRegistry::Get<TransformComponent>();
	CHECK(transform.FindField("Translation")->Kind == FieldKind::Vec3);
	CHECK(transform.FindField("Rotation")->Kind == FieldKind::Quat);
	CHECK(ComponentRegistry::Get<DirectionalLightComponent>().FindField("Color")->Hints.Display == FieldDisplay::Color);
	CHECK(ComponentRegistry::Get<SpriteRendererComponent>().FindField("Color")->Kind == FieldKind::Vec4);
	CHECK(ComponentRegistry::Get<RigidBodyComponent>().FindField("Layer")->Kind == FieldKind::UInt);
	CHECK(ComponentRegistry::Get<RigidBodyComponent>().FindField("LockRotation")->Kind == FieldKind::BVec3);
	CHECK(ComponentRegistry::Get<TextComponent>().FindField("Text")->Hints.Display == FieldDisplay::MultilineText);
	CHECK(ComponentRegistry::Get<ScriptComponent>().FindField("Fields")->Kind == FieldKind::Custom);
	CHECK(ComponentRegistry::Get<RelationshipComponent>().FindField("Parent")->Kind == FieldKind::UUID);
	CHECK(ComponentRegistry::Get<RelationshipComponent>().FindField("Children")->ElementKind == FieldKind::UUID);
	CHECK(ComponentRegistry::Get<TagComponent>().FindField("Tag")->Kind == FieldKind::String);
	CHECK(ComponentRegistry::Get<AudioListenerComponent>().FindField("Active")->Kind == FieldKind::Bool);
}

TEST_CASE("Components: schemas include ranges, value lists, presentation hints and descriptions")
{
	Json const spot = ComponentRegistry::Get<SpotLightComponent>().Describe();
	CHECK(spot["Description"] == "Cone of light shining along the entity's forward direction (-Z).");
	Json const& color = spot["Fields"][0];
	CHECK(color["Display"] == "Color");
	CHECK(color["Min"] == 0.0);
	CHECK(color["Max"] == 1.0);
	Json const& range = spot["Fields"][2];
	CHECK(range["Min"] == 0.0);
	CHECK_FALSE(range.contains("Max"));
	CHECK(range["Description"] == "Distance in meters at which the light fades out completely.");
	CHECK_FALSE(spot["Fields"][5].contains("Description"));

	Json const body = ComponentRegistry::Get<RigidBodyComponent>().Describe();
	CHECK(body["Fields"][0]["Values"] == Json::array({"Static", "Dynamic", "Kinematic"}));
	// Integer fields report integer bounds.
	Json const& layer = body["Fields"][5];
	CHECK(layer["Name"] == "Layer");
	CHECK(layer["Min"].is_number_integer());
	CHECK(layer["Max"] == 15);

	Json const mesh = ComponentRegistry::Get<MeshComponent>().Describe();
	CHECK(mesh["Fields"][0]["AssetType"] == "Mesh");
	CHECK(mesh["Fields"][1]["Type"] == "asset[]");
	CHECK(mesh["Fields"][1]["AssetType"] == "Material");
}
