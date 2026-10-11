#include "Editor/Viewport/ViewportOverlays.h"

#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneCamera.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_set>

namespace Strada::ViewportOverlays
{
	namespace
	{
		void AddLine(std::vector<OverlayLine>& lines, glm::vec3 const& from, glm::vec3 const& to, glm::vec4 const& color, bool depthTest)
		{
			lines.push_back({from, to, color, depthTest});
		}

		// Circle (or arc from startAngle to endAngle) spanned by the unit axes a and b around center.
		void AddArc(std::vector<OverlayLine>& lines, glm::vec3 const& center, glm::vec3 const& a, glm::vec3 const& b, float radius,
		            float startAngle, float endAngle, uint32_t segments, glm::vec4 const& color, bool depthTest)
		{
			glm::vec3 previous = center + radius * (a * std::cos(startAngle) + b * std::sin(startAngle));
			for (uint32_t i = 1; i <= segments; i++)
			{
				float const angle = startAngle + (endAngle - startAngle) * static_cast<float>(i) / static_cast<float>(segments);
				glm::vec3 const point = center + radius * (a * std::cos(angle) + b * std::sin(angle));
				AddLine(lines, previous, point, color, depthTest);
				previous = point;
			}
		}

		void AddCircle(std::vector<OverlayLine>& lines, glm::vec3 const& center, glm::vec3 const& a, glm::vec3 const& b, float radius,
		               glm::vec4 const& color, bool depthTest)
		{
			AddArc(lines, center, a, b, radius, 0.0f, glm::two_pi<float>(), CircleSegments, color, depthTest);
		}

		// The 12 edges of a box given by its 8 corners (bit 0 = +x, bit 1 = +y, bit 2 = +z).
		void AddBox(std::vector<OverlayLine>& lines, std::array<glm::vec3, 8> const& corners, glm::vec4 const& color, bool depthTest)
		{
			for (uint32_t i = 0; i < 8; i++)
			{
				for (uint32_t bit : {1u, 2u, 4u})
				{
					if ((i & bit) == 0)
					{
						AddLine(lines, corners[i], corners[i | bit], color, depthTest);
					}
				}
			}
		}

		glm::vec3 TransformPoint(glm::mat4 const& transform, glm::vec3 const& point)
		{
			return glm::vec3(transform * glm::vec4(point, 1.0f));
		}

		glm::vec3 TransformDirection(glm::mat4 const& transform, glm::vec3 const& direction)
		{
			glm::vec3 const result = glm::vec3(transform * glm::vec4(direction, 0.0f));
			float const length = glm::length(result);
			return length > 1e-6f ? result / length : direction;
		}

		// Largest axis scale of a transform (spheres and capsules stay round under non-uniform scale).
		float MaxScale(glm::mat4 const& transform)
		{
			return std::max(
				{glm::length(glm::vec3(transform[0])), glm::length(glm::vec3(transform[1])), glm::length(glm::vec3(transform[2]))});
		}

		void AddCameraFrustum(std::vector<OverlayLine>& lines, glm::mat4 const& world, CameraComponent const& camera, float aspectRatio)
		{
			float const aspect = GetCameraAspectRatio(camera, aspectRatio);
			auto const cornersAt = [&](float distance, float halfHeight)
			{
				std::array<glm::vec3, 4> corners;
				float const halfWidth = halfHeight * aspect;
				corners[0] = TransformPoint(world, {-halfWidth, -halfHeight, -distance});
				corners[1] = TransformPoint(world, {halfWidth, -halfHeight, -distance});
				corners[2] = TransformPoint(world, {halfWidth, halfHeight, -distance});
				corners[3] = TransformPoint(world, {-halfWidth, halfHeight, -distance});
				return corners;
			};

			std::array<glm::vec3, 4> nearCorners;
			std::array<glm::vec3, 4> farCorners;
			if (camera.Projection == ProjectionType::Perspective)
			{
				float const tanHalfFov = std::tan(glm::radians(std::clamp(camera.PerspectiveFOV, 1.0f, 179.0f)) * 0.5f);
				float const nearPlane = std::max(camera.PerspectiveNear, 1e-4f);
				float const farPlane = std::min(std::max(camera.PerspectiveFar, nearPlane), nearPlane + CameraFrustumLength);
				nearCorners = cornersAt(nearPlane, nearPlane * tanHalfFov);
				farCorners = cornersAt(farPlane, farPlane * tanHalfFov);
				glm::vec3 const apex = TransformPoint(world, glm::vec3(0.0f));
				for (glm::vec3 const& corner : nearCorners)
				{
					AddLine(lines, apex, corner, CameraColor, true);
				}
			}
			else
			{
				float const halfHeight = std::max(camera.OrthographicSize, 1e-4f) * 0.5f;
				float const nearPlane = camera.OrthographicNear;
				float const farPlane = std::min(std::max(camera.OrthographicFar, nearPlane), nearPlane + CameraFrustumLength);
				nearCorners = cornersAt(nearPlane, halfHeight);
				farCorners = cornersAt(farPlane, halfHeight);
			}
			for (uint32_t i = 0; i < 4; i++)
			{
				AddLine(lines, nearCorners[i], nearCorners[(i + 1) % 4], CameraColor, true);
				AddLine(lines, farCorners[i], farCorners[(i + 1) % 4], CameraColor, true);
				AddLine(lines, nearCorners[i], farCorners[i], CameraColor, true);
			}
		}

		void AddDirectionalLight(std::vector<OverlayLine>& lines, glm::mat4 const& world, std::optional<glm::vec3> const& viewPosition)
		{
			glm::vec3 const origin = TransformPoint(world, glm::vec3(0.0f));
			glm::vec3 const forward = TransformDirection(world, {0.0f, 0.0f, -1.0f});
			glm::vec3 const right = TransformDirection(world, {1.0f, 0.0f, 0.0f});
			glm::vec3 const up = TransformDirection(world, {0.0f, 1.0f, 0.0f});
			float const size = viewPosition ? std::max(glm::distance(*viewPosition, origin), 0.01f) * DirectionalLightViewScale : 1.0f;
			float const radius = DirectionalLightRadius * size;
			// Parallel rays: the light comes from infinitely far away, so only its direction matters.
			AddCircle(lines, origin, right, up, radius, LightColor, true);
			for (uint32_t i = 0; i < DirectionalLightRays; i++)
			{
				float const angle = glm::two_pi<float>() * static_cast<float>(i) / static_cast<float>(DirectionalLightRays);
				glm::vec3 const start = origin + radius * (right * std::cos(angle) + up * std::sin(angle));
				AddLine(lines, start, start + forward * (DirectionalLightRayLength * size), LightColor, true);
			}
			glm::vec3 const tip = origin + forward * (DirectionalLightArrowLength * size);
			float const head = DirectionalLightArrowLength * size * 0.15f;
			AddLine(lines, origin, tip, LightColor, true);
			for (glm::vec3 const& side : {right, -right, up, -up})
			{
				AddLine(lines, tip, tip - forward * head + side * head * 0.5f, LightColor, true);
			}
		}

		void AddPointLight(std::vector<OverlayLine>& lines, glm::mat4 const& world, PointLightComponent const& light)
		{
			glm::vec3 const center = TransformPoint(world, glm::vec3(0.0f));
			float const range = std::max(light.Range, 0.0f);
			AddCircle(lines, center, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, range, LightColor, true);
			AddCircle(lines, center, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, range, LightColor, true);
			AddCircle(lines, center, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, range, LightColor, true);
		}

		void AddSpotLight(std::vector<OverlayLine>& lines, glm::mat4 const& world, SpotLightComponent const& light)
		{
			glm::vec3 const origin = TransformPoint(world, glm::vec3(0.0f));
			glm::vec3 const forward = TransformDirection(world, {0.0f, 0.0f, -1.0f});
			glm::vec3 const right = TransformDirection(world, {1.0f, 0.0f, 0.0f});
			glm::vec3 const up = TransformDirection(world, {0.0f, 1.0f, 0.0f});
			float const range = std::max(light.Range, 0.0f);
			float const outer = glm::radians(std::clamp(light.OuterConeAngle, 0.1f, 89.9f));
			float const inner = std::min(glm::radians(std::clamp(light.InnerConeAngle, 0.0f, 89.9f)), outer);
			// The cone ends in the sphere cap at the light's range: its rim, the edges from the light to it, and arcs over
			// the cap through the cone's axis.
			glm::vec3 const baseCenter = origin + forward * (range * std::cos(outer));
			float const baseRadius = range * std::sin(outer);
			AddCircle(lines, baseCenter, right, up, baseRadius, LightColor, true);
			for (glm::vec3 const& side : {right, -right, up, -up})
			{
				AddLine(lines, origin, baseCenter + side * baseRadius, LightColor, true);
			}
			AddArc(lines, origin, forward, right, range, -outer, outer, SpotCapSegments, LightColor, true);
			AddArc(lines, origin, forward, up, range, -outer, outer, SpotCapSegments, LightColor, true);
			// Where the light starts to fade towards the outer cone.
			AddCircle(lines, origin + forward * (range * std::cos(inner)), right, up, range * std::sin(inner), LightInnerColor, true);
		}

		void AddBoxCollider(std::vector<OverlayLine>& lines, glm::mat4 const& world, BoxColliderComponent const& collider)
		{
			glm::vec4 const color = collider.IsTrigger ? TriggerColor : ColliderColor;
			std::array<glm::vec3, 8> corners;
			for (uint32_t i = 0; i < 8; i++)
			{
				glm::vec3 const sign((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
				corners[i] = TransformPoint(world, collider.Offset + sign * collider.HalfExtents);
			}
			AddBox(lines, corners, color, true);
		}

		void AddSphereCollider(std::vector<OverlayLine>& lines, glm::mat4 const& world, SphereColliderComponent const& collider)
		{
			glm::vec4 const color = collider.IsTrigger ? TriggerColor : ColliderColor;
			glm::vec3 const center = TransformPoint(world, collider.Offset);
			float const radius = collider.Radius * MaxScale(world);
			glm::vec3 const x = TransformDirection(world, {1.0f, 0.0f, 0.0f});
			glm::vec3 const y = TransformDirection(world, {0.0f, 1.0f, 0.0f});
			glm::vec3 const z = TransformDirection(world, {0.0f, 0.0f, 1.0f});
			AddCircle(lines, center, x, y, radius, color, true);
			AddCircle(lines, center, x, z, radius, color, true);
			AddCircle(lines, center, y, z, radius, color, true);
		}

		void AddCapsuleCollider(std::vector<OverlayLine>& lines, glm::mat4 const& world, CapsuleColliderComponent const& collider)
		{
			glm::vec4 const color = collider.IsTrigger ? TriggerColor : ColliderColor;
			glm::vec3 const x = TransformDirection(world, {1.0f, 0.0f, 0.0f});
			glm::vec3 const y = TransformDirection(world, {0.0f, 1.0f, 0.0f});
			glm::vec3 const z = TransformDirection(world, {0.0f, 0.0f, 1.0f});
			// The cylinder part follows the entity's Y scale; the radius uses the largest scale of the other axes.
			float const scaleY = glm::length(glm::vec3(world[1]));
			float const radius = collider.Radius * std::max(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[2])));
			glm::vec3 const center = TransformPoint(world, collider.Offset);
			glm::vec3 const top = center + y * (collider.HalfHeight * scaleY);
			glm::vec3 const bottom = center - y * (collider.HalfHeight * scaleY);
			AddCircle(lines, top, x, z, radius, color, true);
			AddCircle(lines, bottom, x, z, radius, color, true);
			for (glm::vec3 const& side : {x, -x, z, -z})
			{
				AddLine(lines, bottom + side * radius, top + side * radius, color, true);
			}
			uint32_t const halfSegments = CircleSegments / 2;
			AddArc(lines, top, x, y, radius, 0.0f, glm::pi<float>(), halfSegments, color, true);
			AddArc(lines, top, z, y, radius, 0.0f, glm::pi<float>(), halfSegments, color, true);
			AddArc(lines, bottom, x, -y, radius, 0.0f, glm::pi<float>(), halfSegments, color, true);
			AddArc(lines, bottom, z, -y, radius, 0.0f, glm::pi<float>(), halfSegments, color, true);
		}
	}

	namespace
	{
		// Drawn over everything: anchors usually lie inside the bodies they join.
		void AddJoint(Scene& scene, std::vector<OverlayLine>& lines, glm::mat4 const& world, JointComponent const& joint)
		{
			glm::vec3 const anchor(world * glm::vec4(joint.Anchor, 1.0f));
			for (int axis = 0; axis < 3; axis++)
			{
				glm::vec3 offset(0.0f);
				offset[axis] = JointAnchorSize;
				AddLine(lines, anchor - offset, anchor + offset, JointColor, false);
			}
			if (joint.Type == JointType::Hinge || joint.Type == JointType::Slider)
			{
				glm::vec3 direction(world * glm::vec4(joint.Axis, 0.0f));
				float const length = glm::length(direction);
				if (length > 1e-6f)
				{
					direction *= JointAxisLength * 0.5f / length;
					AddLine(lines, anchor - direction, anchor + direction, JointColor, false);
				}
			}
			// To the far anchor of distance joints, or the connected body.
			glm::vec3 other = joint.ConnectedAnchor;
			Entity const connected = joint.ConnectedEntity.IsValid() ? scene.GetEntityByUUID(joint.ConnectedEntity) : Entity();
			if (connected)
			{
				glm::mat4 const connectedWorld = scene.GetWorldTransform(connected);
				other = joint.Type == JointType::Distance ? glm::vec3(connectedWorld * glm::vec4(joint.ConnectedAnchor, 1.0f))
				                                          : glm::vec3(connectedWorld[3]);
			}
			else if (joint.Type != JointType::Distance)
			{
				return;
			}
			AddLine(lines, anchor, other, JointColor, false);
		}
	}

	void AppendEntityShapes(Scene& scene, Entity entity, float aspectRatio, std::vector<OverlayLine>& lines, uint32_t shapes,
	                        std::optional<glm::vec3> const& viewPosition)
	{
		if (!entity || shapes == 0)
		{
			return;
		}
		glm::mat4 const world = scene.GetWorldTransform(entity);
		if ((shapes & Shapes::Cameras) != 0)
		{
			if (CameraComponent const* camera = entity.TryGetComponent<CameraComponent>())
			{
				AddCameraFrustum(lines, world, *camera, aspectRatio);
			}
		}
		if ((shapes & Shapes::Lights) != 0)
		{
			if (entity.HasComponent<DirectionalLightComponent>())
			{
				AddDirectionalLight(lines, world, viewPosition);
			}
			if (PointLightComponent const* light = entity.TryGetComponent<PointLightComponent>())
			{
				AddPointLight(lines, world, *light);
			}
			if (SpotLightComponent const* light = entity.TryGetComponent<SpotLightComponent>())
			{
				AddSpotLight(lines, world, *light);
			}
		}
		if ((shapes & Shapes::Colliders) != 0)
		{
			if (BoxColliderComponent const* collider = entity.TryGetComponent<BoxColliderComponent>())
			{
				AddBoxCollider(lines, world, *collider);
			}
			if (SphereColliderComponent const* collider = entity.TryGetComponent<SphereColliderComponent>())
			{
				AddSphereCollider(lines, world, *collider);
			}
			if (CapsuleColliderComponent const* collider = entity.TryGetComponent<CapsuleColliderComponent>())
			{
				AddCapsuleCollider(lines, world, *collider);
			}
		}
		if ((shapes & Shapes::Joints) != 0)
		{
			if (JointComponent const* joint = entity.TryGetComponent<JointComponent>())
			{
				AddJoint(scene, lines, world, *joint);
			}
		}
	}

	uint32_t GetShapeKinds(SceneViewOverlays const& overlays, bool selected)
	{
		auto const kind = [selected](OverlayShapeSetting const& setting, uint32_t flag)
		{
			return setting.Enabled && (selected || setting.AllEntities) ? flag : 0u;
		};
		return kind(overlays.Cameras, Shapes::Cameras) | kind(overlays.Lights, Shapes::Lights) |
		       kind(overlays.Colliders, Shapes::Colliders) | kind(overlays.Joints, Shapes::Joints);
	}

	void AppendSceneShapes(Scene& scene, std::span<UUID const> selection, SceneViewOverlays const& overlays, float aspectRatio,
	                       glm::vec3 const& viewPosition, std::vector<OverlayLine>& lines)
	{
		uint32_t const selectedKinds = GetShapeKinds(overlays, true);
		uint32_t const otherKinds = GetShapeKinds(overlays, false);
		if (otherKinds == 0)
		{
			for (UUID const id : selection)
			{
				AppendEntityShapes(scene, scene.GetEntityByUUID(id), aspectRatio, lines, selectedKinds, viewPosition);
			}
			return;
		}
		std::unordered_set<UUID> const selected(selection.begin(), selection.end());
		scene.ForEachEntityInHierarchyOrder(
			[&](Entity entity)
			{
				AppendEntityShapes(scene, entity, aspectRatio, lines, selected.contains(entity.GetUUID()) ? selectedKinds : otherKinds,
			                       viewPosition);
			});
	}
}
