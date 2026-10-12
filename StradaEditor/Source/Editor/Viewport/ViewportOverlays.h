#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/UUID.h"
#include "Strada/Scene/Entity.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Strada
{
	class MeshSource;
	class Scene;

	struct OverlayLine
	{
		glm::vec3 From = glm::vec3(0.0f);
		glm::vec3 To = glm::vec3(0.0f);
		// sRGB-encoded with straight alpha.
		glm::vec4 Color = glm::vec4(1.0f);
		// Dimmed where surfaces hide it (ViewportOverlays::OccludedLineOpacity); otherwise drawn over everything: joints,
		// whose anchors lie inside bodies, directional lights, whose position does not matter (they are usually placed on or
		// under the ground), local post-process volumes, which the camera is often inside, the capsules of selected
		// characters, which their models enclose, and the outlines of UI canvases (CanvasOverlay).
		bool DepthTest = true;
	};

	// Which entities the scene view draws one kind of shape for.
	struct OverlayShapeSetting
	{
		bool Enabled = true;
		// Every entity's shapes, not only the selected entities'.
		bool AllEntities = false;

		bool operator==(OverlayShapeSetting const&) const = default;
	};

	// What the scene view draws over the scene (the Overlays menu of its toolbar, and viewport.overlays).
	struct SceneViewOverlays
	{
		bool Grid = true;
		bool SelectionOutline = true;
		// Billboard icons of cameras and lights (they can be clicked to select).
		bool Icons = true;
		// Camera frustums.
		OverlayShapeSetting Cameras;
		// Directional light rays, point light ranges and spot light cones.
		OverlayShapeSetting Lights;
		// Collider outlines and character controller capsules.
		OverlayShapeSetting Colliders;
		// Joint anchors, axes and connections.
		OverlayShapeSetting Joints;
		// Boxes of local post-process volumes and where their blending ends.
		OverlayShapeSetting Volumes;
		// Lines scripts draw (Debug.DrawLine and friends).
		bool DebugLines = true;
		// The UI of each canvas in its entity's plane with the canvas's outline (CanvasOverlay).
		bool Canvases = true;

		bool operator==(SceneViewOverlays const&) const = default;
	};

	namespace ViewportOverlays
	{
		// The outlines of mesh colliders (MeshColliderOutline), built once per mesh and kind of shape: a convex hull takes a
		// while to build for a large mesh. Main thread only.
		class MeshColliderOutlineCache
		{
		public:
			// What physics collides with for the mesh, in the mesh's space (the ends of each edge in turn): its convex hull,
			// or its triangles. Empty when physics cannot build the shape either.
			std::span<glm::vec3 const> Get(Ref<MeshSource> const& mesh, bool convex);
			// Forgets the outlines of meshes that are gone (unloaded, or replaced when their file changed).
			void RemoveReleased();
			size_t GetSize() const { return m_Outlines.size(); }

		private:
			struct Outline
			{
				std::weak_ptr<MeshSource> Mesh;
				std::vector<glm::vec3> Points;
			};

			std::map<std::pair<MeshSource const*, bool>, Outline> m_Outlines;
		};

		// Kinds of shapes AppendEntityShapes draws (bit flags).
		namespace Shapes
		{
			inline constexpr uint32_t Cameras = 1u << 0;
			inline constexpr uint32_t Lights = 1u << 1;
			inline constexpr uint32_t Colliders = 1u << 2;
			inline constexpr uint32_t Joints = 1u << 3;
			// Boxes of local post-process volumes.
			inline constexpr uint32_t Volumes = 1u << 4;
			inline constexpr uint32_t All = Cameras | Lights | Colliders | Joints | Volumes;
			// Not a kind but how they are drawn: the entity is selected, so a character controller's capsule shows through
			// the model around it. Otherwise surfaces in front of the capsule dim it, as they dim other colliders.
			inline constexpr uint32_t Selected = 1u << 5;
		}

		// Distance up to which camera frustums are drawn (their far planes are usually far away).
		inline constexpr float CameraFrustumLength = 3.0f;
		// Directional lights: a disc facing along the light with rays from its rim and an arrow through its center. Their
		// position does not matter, so the shape keeps its size on screen when a view is given: these sizes are multiplied
		// by DirectionalLightViewScale per meter between the view and the light.
		inline constexpr float DirectionalLightRadius = 0.3f;
		inline constexpr uint32_t DirectionalLightRays = 8;
		inline constexpr float DirectionalLightRayLength = 0.9f;
		inline constexpr float DirectionalLightArrowLength = 1.5f;
		inline constexpr float DirectionalLightViewScale = 0.13f;
		inline constexpr uint32_t CircleSegments = 32;
		// Spot lights: segments of each arc of the cone's cap.
		inline constexpr uint32_t SpotCapSegments = 16;

		// Light colors that read on dark surfaces; the lines' dark outline keeps them visible on bright ones.
		inline constexpr glm::vec4 CameraColor = glm::vec4(0.94f, 0.94f, 0.94f, 1.0f);
		inline constexpr glm::vec4 LightColor = glm::vec4(1.0f, 0.9f, 0.45f, 1.0f);
		// Spot lights' inner cone, where their light starts to fade.
		inline constexpr glm::vec4 LightInnerColor = glm::vec4(1.0f, 0.9f, 0.45f, 0.55f);
		inline constexpr glm::vec4 ColliderColor = glm::vec4(0.5f, 1.0f, 0.55f, 1.0f);
		inline constexpr glm::vec4 TriggerColor = glm::vec4(0.5f, 0.85f, 1.0f, 1.0f);
		inline constexpr glm::vec4 CharacterColor = glm::vec4(0.74f, 0.62f, 1.0f, 1.0f);
		inline constexpr glm::vec4 JointColor = glm::vec4(1.0f, 0.55f, 0.95f, 1.0f);
		// Post-process volumes: a deeper violet than characters'.
		inline constexpr glm::vec4 VolumeColor = glm::vec4(0.68f, 0.48f, 1.0f, 1.0f);
		// The outline where a local volume's blend distance ends.
		inline constexpr glm::vec4 VolumeBlendColor = glm::vec4(0.68f, 0.48f, 1.0f, 0.55f);
		// Length of the drawn hinge and slider axes, and size of the anchor's cross.
		inline constexpr float JointAxisLength = 0.6f;
		inline constexpr float JointAnchorSize = 0.1f;

		// How the scene view draws its lines (SceneRendererOverlays), in pixels at a UI scale of 1: wide enough to see at a
		// glance, with a dark outline for bright backgrounds, and dimmed where surfaces hide them.
		inline constexpr float LineWidth = 2.0f;
		inline constexpr float LineOutlineWidth = 1.0f;
		inline constexpr glm::vec4 LineOutlineColor = glm::vec4(0.0f, 0.0f, 0.0f, 0.55f);
		inline constexpr float OccludedLineOpacity = 0.3f;

		// Shapes of an entity's camera, lights, colliders, joint and post-process volume (the kinds in shapes): camera
		// frustums up to CameraFrustumLength, directional light rays, point light ranges, spot light cones (their outer cone
		// up to the range with its cap, and the inner cone's rim), collider outlines (mesh colliders as the convex hull or
		// the triangles physics collides with, of the collider's mesh or else the entity's; a character controller's upright
		// capsule, through its model with Shapes::Selected), the joint's anchor (a cross), its hinge or slider axis and a
		// line to where it joins the connected body (or the world), and a local volume's box with the rounded outline where
		// its blend distance ends. aspectRatio is used by cameras without a fixed aspect ratio; viewPosition, where the
		// shapes are seen from, sizes directional lights; meshOutlines keeps the outlines of mesh colliders between calls
		// (without it they are built for each call).
		void AppendEntityShapes(Scene& scene, Entity entity, float aspectRatio, std::vector<OverlayLine>& lines,
		                        uint32_t shapes = Shapes::All, std::optional<glm::vec3> const& viewPosition = std::nullopt,
		                        MeshColliderOutlineCache* meshOutlines = nullptr);
		// The kinds of shapes (Shapes flags) the overlays draw for an entity that is selected, or not.
		uint32_t GetShapeKinds(SceneViewOverlays const& overlays, bool selected);
		// The shapes the overlays draw: those of the selected entities (drawn as selected: Shapes::Selected), and of every
		// entity for the kinds shown for all, in hierarchy order when any is. meshOutlines also forgets the outlines of meshes
		// that are gone.
		void AppendSceneShapes(Scene& scene, std::span<UUID const> selection, SceneViewOverlays const& overlays, float aspectRatio,
		                       glm::vec3 const& viewPosition, std::vector<OverlayLine>& lines,
		                       MeshColliderOutlineCache* meshOutlines = nullptr);
	}
}
