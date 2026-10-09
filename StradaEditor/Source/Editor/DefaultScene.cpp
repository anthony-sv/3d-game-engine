#include "Editor/DefaultScene.h"

#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Scene/Entity.h"

namespace Strada
{
	namespace
	{
		Entity AddMesh(Scene& scene, char const* name, BuiltInAsset mesh, glm::vec3 const& translation, glm::vec3 const& scale)
		{
			Entity entity = scene.CreateEntity(name);
			entity.AddComponent<MeshComponent>().Mesh = GetBuiltInHandle(mesh);
			TransformComponent& transform = entity.GetComponent<TransformComponent>();
			transform.Translation = translation;
			transform.Scale = scale;
			return entity;
		}
	}

	Ref<Scene> CreateDefaultScene()
	{
		Ref<Scene> scene = CreateRef<Scene>("Untitled");

		Entity camera = scene->CreateEntity("Main Camera");
		camera.AddComponent<CameraComponent>();
		TransformComponent& cameraTransform = camera.GetComponent<TransformComponent>();
		cameraTransform.Translation = {0.0f, 2.5f, 7.0f};
		cameraTransform.SetRotationEuler({-15.0f, 0.0f, 0.0f});

		Entity sun = scene->CreateEntity("Sun");
		sun.GetComponent<TransformComponent>().SetRotationEuler({-50.0f, 35.0f, 0.0f});
		sun.AddComponent<DirectionalLightComponent>();

		Entity sky = scene->CreateEntity("Sky");
		SkyLightComponent& skyLight = sky.AddComponent<SkyLightComponent>();
		skyLight.AmbientColor = {0.4f, 0.5f, 0.65f};
		skyLight.Intensity = 0.5f;

		AddMesh(*scene, "Floor", BuiltInAsset::PlaneMesh, {0.0f, 0.0f, 0.0f}, glm::vec3(20.0f));
		AddMesh(*scene, "Cube", BuiltInAsset::CubeMesh, {-1.5f, 0.5f, 0.0f}, glm::vec3(1.0f));
		AddMesh(*scene, "Sphere", BuiltInAsset::SphereMesh, {0.0f, 0.5f, 0.0f}, glm::vec3(1.0f));
		AddMesh(*scene, "Capsule", BuiltInAsset::CapsuleMesh, {1.5f, 1.0f, 0.0f}, glm::vec3(1.0f));
		return scene;
	}
}
