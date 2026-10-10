#include "Editor/Panels/ViewportPanel.h"

#include "Editor/AssetDrops.h"
#include "Editor/EntityPresets.h"
#include "Editor/UI/EditorUI.h"
#include "Editor/Viewport/TransformEditing.h"
#include "Editor/Viewport/ViewportOverlays.h"

#include "Editor/EntityBounds.h"
#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/Log.h"
#include "Strada/ImGui/ImGuiRenderer.h"
#include "Strada/Renderer/Renderer.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/SceneRendering.h"

#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		constexpr float ToolbarPadding = 6.0f;
		constexpr float IconRadius = 10.0f;
		// A press and release closer than this (in pixels) is a click; farther is a drag.
		constexpr float ClickDragThreshold = 4.0f;
		// Merge keys of gizmo drags ("GIZMO" in the high bytes); each drag adds its sequence number.
		constexpr uint64_t GizmoMergeKeyBase = 0x47495A4D4F000000ull;
		constexpr ImU32 IconBackground = IM_COL32(30, 30, 30, 200);
		constexpr ImU32 IconColor = IM_COL32(235, 235, 235, 255);
		constexpr ImU32 IconHoveredColor = IM_COL32(255, 255, 255, 255);
		constexpr ImU32 IconSelectedColor = IM_COL32(255, 140, 26, 255);
		constexpr ImU32 PlayFrameColor = IM_COL32(64, 160, 255, 255);
		constexpr ImU32 SimulateFrameColor = IM_COL32(120, 200, 120, 255);
		constexpr float PlayFrameThickness = 2.0f;

		ImVec2 ToImVec2(glm::vec2 const& value)
		{
			return ImVec2(value.x, value.y);
		}

		glm::vec2 ToVec2(ImVec2 const& value)
		{
			return glm::vec2(value.x, value.y);
		}

		enum class IconShape : uint8_t
		{
			None = 0,
			Camera,
			Light
		};

		IconShape GetIconShape(Entity entity)
		{
			if (entity.HasComponent<CameraComponent>())
			{
				return IconShape::Camera;
			}
			if (entity.HasComponent<DirectionalLightComponent>() || entity.HasComponent<PointLightComponent>() ||
			    entity.HasComponent<SpotLightComponent>() || entity.HasComponent<SkyLightComponent>())
			{
				return IconShape::Light;
			}
			return IconShape::None;
		}

		void DrawIcon(ImDrawList* drawList, ImVec2 center, IconShape shape, ImU32 color)
		{
			drawList->AddCircleFilled(center, IconRadius, IconBackground);
			drawList->AddCircle(center, IconRadius, color, 0, 1.5f);
			if (shape == IconShape::Camera)
			{
				drawList->AddRectFilled(ImVec2(center.x - 5.5f, center.y - 3.5f), ImVec2(center.x + 2.0f, center.y + 3.5f), color, 1.0f);
				drawList->AddTriangleFilled(ImVec2(center.x + 2.0f, center.y), ImVec2(center.x + 6.0f, center.y - 3.5f),
				                            ImVec2(center.x + 6.0f, center.y + 3.5f), color);
			}
			else
			{
				drawList->AddCircleFilled(center, 3.0f, color);
				for (int i = 0; i < 8; i++)
				{
					float const angle = static_cast<float>(i) * 0.7853982f;
					ImVec2 const direction(std::cos(angle), std::sin(angle));
					drawList->AddLine(ImVec2(center.x + direction.x * 4.5f, center.y + direction.y * 4.5f),
					                  ImVec2(center.x + direction.x * 7.0f, center.y + direction.y * 7.0f), color, 1.2f);
				}
			}
		}
	}

	ViewportPanel::ViewportPanel()
	{
		if (Renderer::IsInitialized())
		{
			m_Renderer = CreateScope<SceneRenderer>();
			m_Renderer->SetEntityIdsEnabled(true);
		}
		m_Camera.SetView(glm::vec3(0.0f, 0.5f, 0.0f), 7.0f, 30.0f, -20.0f);
	}

	void ViewportPanel::OnImGuiRender(EditorOperations& operations, bool& open)
	{
		// ImGuizmo tracks hovering per frame.
		ImGuizmo::BeginFrame();
		EditorContext& context = operations.GetContext();
		if (context.GetSceneVersion() != m_PickingSceneVersion)
		{
			m_PickingIds.Clear();
			m_PickingSceneVersion = context.GetSceneVersion();
			m_DiscardPick = m_Renderer && m_Renderer->IsPickPending();
			m_PendingPick.reset();
			m_GizmoActive = false;
		}

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		bool const visible = ImGui::Begin("Viewport", &open);
		ImGui::PopStyleVar();
		if (!visible)
		{
			EndGizmoDrag(context);
			m_GameInputActive = false;
			ImGui::End();
			return;
		}
		if (!m_Renderer)
		{
			ImGui::TextDisabled("Rendering is unavailable (no GPU).");
			ImGui::End();
			return;
		}

		glm::vec2 const imagePosition = ToVec2(ImGui::GetCursorScreenPos());
		glm::vec2 const available = ToVec2(ImGui::GetContentRegionAvail());
		uint32_t const width = static_cast<uint32_t>(std::max(available.x, 1.0f));
		uint32_t const height = static_cast<uint32_t>(std::max(available.y, 1.0f));
		glm::vec2 const imageSize(static_cast<float>(width), static_cast<float>(height));

		bool const hovered = ImGui::IsWindowHovered() &&
		                     ImGui::IsMouseHoveringRect(ToImVec2(imagePosition), ToImVec2(imagePosition + imageSize)) &&
		                     !ImGui::IsMouseHoveringRect(ToImVec2(m_ToolbarMin), ToImVec2(m_ToolbarMax));

		// Play shows the game; its input goes to the game while the view is focused.
		bool const gameView = context.GetPlayState() == EditorPlayState::Play;
		m_GameInputActive = gameView && ImGui::IsWindowFocused();
		if (gameView)
		{
			m_GameMousePosition = ToVec2(ImGui::GetMousePos()) - imagePosition;
			EndGizmoDrag(context);
		}
		else
		{
			HandleCameraInput(hovered);
			HandleShortcuts(context, hovered);
		}
		m_Camera.SetViewportSize(width, height);
		m_Renderer->SetViewportSize(width, height);
		// Scripts read the scene's view size (Application.WindowWidth/Height, Camera.ScreenToWorldRay).
		context.GetScene().OnViewportResize(width, height);
		m_ViewportSize = glm::uvec2(width, height);
		ApplyPickResult(operations);
		bool const showsGame = gameView && RenderGame(context);
		if (!showsGame)
		{
			Render(context);
		}

		ImGui::Image(ImGuiRenderer::GetTextureID(m_Renderer->GetFinalImage()), ToImVec2(imageSize));
		if (gameView)
		{
			if (!showsGame)
			{
				ImGui::SetCursorScreenPos(ToImVec2(imagePosition + glm::vec2(ToolbarPadding)));
				ImGui::TextUnformatted("No primary camera: the editor camera shows the scene.");
			}
			DrawPlayFrame(imagePosition, imageSize, context.GetPlayState());
			ImGui::End();
			return;
		}
		AssetHandle droppedAsset;
		if (ImGui::BeginDragDropTarget())
		{
			constexpr std::array<AssetType, 5> DroppableTypes = {AssetType::Mesh, AssetType::Material, AssetType::Environment,
			                                                     AssetType::Scene, AssetType::Prefab};
			droppedAsset = UI::AcceptAssetDrop(DroppableTypes);
			ImGui::EndDragDropTarget();
		}
		glm::vec2 const dropPixel = ToVec2(ImGui::GetMousePos()) - imagePosition;
		UUID const hoveredIcon = DrawEntityIcons(context, imagePosition, imageSize, hovered);
		UpdateGizmo(operations, imagePosition, imageSize);
		HandleSelectionClicks(operations, imagePosition, hovered, hoveredIcon);
		DrawToolbar(imagePosition);
		if (context.IsPlaying())
		{
			DrawPlayFrame(imagePosition, imageSize, context.GetPlayState());
		}
		// Last: a drop may replace the scene (opening a scene asset) under the overlays drawn above.
		if (droppedAsset.IsValid())
		{
			ApplyAssetDrop(operations, droppedAsset, dropPixel, imageSize);
		}
		ImGui::End();
	}

	bool ViewportPanel::RenderGame(EditorContext& context)
	{
		return RenderSceneFromPrimaryCamera(context.GetScene(), *m_Renderer);
	}

	void ViewportPanel::DrawPlayFrame(glm::vec2 const& imagePosition, glm::vec2 const& imageSize, EditorPlayState state)
	{
		ImU32 const color = state == EditorPlayState::Play ? PlayFrameColor : SimulateFrameColor;
		glm::vec2 const inset(PlayFrameThickness * 0.5f);
		ImGui::GetWindowDrawList()->AddRect(ToImVec2(imagePosition + inset), ToImVec2(imagePosition + imageSize - inset), color, 0.0f,
		                                    PlayFrameThickness);
	}

	void ViewportPanel::HandleCameraInput(bool hovered)
	{
		ImGuiIO const& io = ImGui::GetIO();
		// A drag that starts in the viewport keeps controlling the camera until its button is released.
		auto const updateCapture = [hovered](bool& captured, ImGuiMouseButton button, bool condition)
		{
			if (hovered && condition && ImGui::IsMouseClicked(button))
			{
				captured = true;
			}
			if (!ImGui::IsMouseDown(button))
			{
				captured = false;
			}
		};
		updateCapture(m_LookCaptured, ImGuiMouseButton_Right, true);
		updateCapture(m_PanCaptured, ImGuiMouseButton_Middle, true);
		updateCapture(m_OrbitCaptured, ImGuiMouseButton_Left, io.KeyAlt);

		EditorCameraInput input;
		input.Look = m_LookCaptured;
		input.Pan = m_PanCaptured;
		input.Orbit = m_OrbitCaptured;
		if (input.Look || input.Pan || input.Orbit)
		{
			input.MouseDelta = glm::vec2(io.MouseDelta.x, io.MouseDelta.y);
		}
		if (input.Look)
		{
			input.Move.x = (ImGui::IsKeyDown(ImGuiKey_D) ? 1.0f : 0.0f) - (ImGui::IsKeyDown(ImGuiKey_A) ? 1.0f : 0.0f);
			input.Move.y = (ImGui::IsKeyDown(ImGuiKey_E) ? 1.0f : 0.0f) - (ImGui::IsKeyDown(ImGuiKey_Q) ? 1.0f : 0.0f);
			input.Move.z = (ImGui::IsKeyDown(ImGuiKey_W) ? 1.0f : 0.0f) - (ImGui::IsKeyDown(ImGuiKey_S) ? 1.0f : 0.0f);
			input.Fast = io.KeyShift;
		}
		if (hovered)
		{
			input.Scroll = io.MouseWheel;
		}
		m_Camera.Update(input, io.DeltaTime);
	}

	void ViewportPanel::HandleShortcuts(EditorContext& context, bool hovered)
	{
		// While flying, WASD/QE move the camera; text fields keep their keys.
		ImGuiIO const& io = ImGui::GetIO();
		if (!hovered || m_LookCaptured || io.WantTextInput || io.KeyCtrl || io.KeyAlt)
		{
			return;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_W, false))
		{
			m_GizmoOperation = GizmoOperation::Translate;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_E, false))
		{
			m_GizmoOperation = GizmoOperation::Rotate;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_R, false))
		{
			m_GizmoOperation = GizmoOperation::Scale;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_X, false))
		{
			m_GizmoSpace = m_GizmoSpace == GizmoSpace::Local ? GizmoSpace::World : GizmoSpace::Local;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_G, false))
		{
			m_ShowGrid = !m_ShowGrid;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_F, false))
		{
			FocusEntities(context.GetScene(), context.GetSelection().GetEntities());
		}
	}

	void ViewportPanel::Render(EditorContext& context)
	{
		Scene& scene = context.GetScene();
		EntitySelection const& selection = context.GetSelection();
		float const aspectRatio = m_Camera.GetAspectRatio();

		SceneRenderOptions options;
		options.GetPickingId = [this](UUID entity)
		{
			return m_PickingIds.GetId(entity);
		};
		options.IsSelected = [&selection](UUID entity)
		{
			return selection.Contains(entity);
		};
		options.SubmitOverlays = [&](SceneRenderer& renderer)
		{
			SceneRendererOverlays overlays;
			overlays.Grid.Enabled = m_ShowGrid;
			renderer.SetOverlays(overlays);

			std::vector<OverlayLine> lines;
			for (UUID const id : selection.GetEntities())
			{
				ViewportOverlays::AppendEntityShapes(scene, scene.GetEntityByUUID(id), aspectRatio, lines);
			}
			for (OverlayLine const& line : lines)
			{
				renderer.SubmitLine(line.From, line.To, line.Color, line.DepthTest);
			}
		};
		RenderScene(scene, *m_Renderer, m_Camera.GetRendererCamera(), options);
	}

	UUID ViewportPanel::DrawEntityIcons(EditorContext& context, glm::vec2 const& imagePosition, glm::vec2 const& imageSize, bool hovered)
	{
		Scene& scene = context.GetScene();
		glm::mat4 const viewProjection = m_Camera.GetProjection() * m_Camera.GetViewMatrix();
		glm::vec2 const mouse = ToVec2(ImGui::GetMousePos());
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->PushClipRect(ToImVec2(imagePosition), ToImVec2(imagePosition + imageSize), true);

		UUID hoveredIcon = UUID::Invalid();
		float closestDistance = IconRadius;
		std::vector<std::pair<UUID, glm::vec2>> icons;
		scene.ForEachEntityInHierarchyOrder(
			[&](Entity entity)
			{
				IconShape const shape = GetIconShape(entity);
				if (shape == IconShape::None)
				{
					return;
				}
				glm::vec4 const clip = viewProjection * glm::vec4(glm::vec3(scene.GetWorldTransform(entity)[3]), 1.0f);
				// Behind the camera (perspective w is the view distance).
				if (clip.w <= 1e-4f)
				{
					return;
				}
				glm::vec2 const ndc = glm::vec2(clip) / clip.w;
				glm::vec2 const screen = imagePosition + glm::vec2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f) * imageSize;
				icons.emplace_back(entity.GetUUID(), screen);
				float const distance = glm::length(mouse - screen);
				if (hovered && distance <= closestDistance)
				{
					closestDistance = distance;
					hoveredIcon = entity.GetUUID();
				}
			});

		for (auto const& [id, screen] : icons)
		{
			Entity const entity = scene.GetEntityByUUID(id);
			ImU32 const color =
				context.GetSelection().Contains(id) ? IconSelectedColor : (id == hoveredIcon ? IconHoveredColor : IconColor);
			DrawIcon(drawList, ToImVec2(screen), GetIconShape(entity), color);
		}
		drawList->PopClipRect();
		return hoveredIcon;
	}

	void ViewportPanel::UpdateGizmo(EditorOperations& operations, glm::vec2 const& imagePosition, glm::vec2 const& imageSize)
	{
		EditorContext& context = operations.GetContext();
		Scene& scene = context.GetScene();
		Entity const primary = scene.GetEntityByUUID(context.GetSelection().GetPrimary());
		if (!primary)
		{
			EndGizmoDrag(context);
			return;
		}

		ImGuizmo::SetOrthographic(false);
		ImGuizmo::SetDrawlist();
		ImGuizmo::SetRect(imagePosition.x, imagePosition.y, imageSize.x, imageSize.y);

		ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
		float snap = m_TranslationSnap;
		switch (m_GizmoOperation)
		{
			case GizmoOperation::Translate:
				break;
			case GizmoOperation::Rotate:
				operation = ImGuizmo::ROTATE;
				snap = m_RotationSnap;
				break;
			case GizmoOperation::Scale:
				operation = ImGuizmo::SCALE;
				snap = m_ScaleSnap;
				break;
		}
		// Scaling always happens along the entity's own axes.
		ImGuizmo::MODE const mode =
			m_GizmoSpace == GizmoSpace::World && m_GizmoOperation != GizmoOperation::Scale ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
		bool const snapping = m_SnapEnabled != ImGui::GetIO().KeyCtrl;
		float const snapValues[3] = {snap, snap, snap};

		glm::mat4 const view = m_Camera.GetViewMatrix();
		glm::mat4 const projection = m_Camera.GetGizmoProjection();
		glm::mat4 const world = scene.GetWorldTransform(primary);
		glm::mat4 manipulated = world;
		bool const changed = ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection), operation, mode,
		                                          glm::value_ptr(manipulated), nullptr, snapping ? snapValues : nullptr);
		if (!ImGuizmo::IsUsing())
		{
			EndGizmoDrag(context);
			return;
		}
		if (!m_GizmoActive)
		{
			m_GizmoActive = true;
			m_GizmoMergeKey = GizmoMergeKeyBase + ++m_GizmoDragCount;
		}
		if (!changed)
		{
			return;
		}

		// The same world-space change moves every selected root (pivoting around the primary selection).
		glm::mat4 const delta = manipulated * glm::inverse(world);
		std::vector<UUID> const roots = TransformEditing::GetSelectionRoots(scene, context.GetSelection().GetEntities());
		Result<std::vector<ComponentEdit>> edits = TransformEditing::ApplyWorldDelta(scene, roots, delta);
		if (!edits)
		{
			ST_WARN("Transform gizmo: {}", edits.GetError());
			return;
		}
		if (Result<void> result = operations.SetComponentFields(std::move(edits.GetValue()), m_GizmoMergeKey); !result)
		{
			ST_WARN("Transform gizmo: {}", result.GetError());
		}
	}

	void ViewportPanel::EndGizmoDrag(EditorContext& context)
	{
		if (m_GizmoActive)
		{
			m_GizmoActive = false;
			context.GetHistory().BreakMerge();
		}
	}

	void ViewportPanel::HandleSelectionClicks(EditorOperations& operations, glm::vec2 const& imagePosition, bool hovered, UUID hoveredIcon)
	{
		ImGuiIO const& io = ImGui::GetIO();
		if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			// Alt + left mouse orbits, and presses on the gizmo manipulate it.
			m_ClickCandidate = !io.KeyAlt && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing();
		}
		if (!m_ClickCandidate || !ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		{
			return;
		}
		m_ClickCandidate = false;
		if (m_GizmoActive || io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] > ClickDragThreshold * ClickDragThreshold || !hovered)
		{
			return;
		}

		SelectionMode const mode = io.KeyCtrl ? SelectionMode::Toggle : (io.KeyShift ? SelectionMode::Add : SelectionMode::Replace);
		if (hoveredIcon.IsValid())
		{
			UUID const entities[] = {hoveredIcon};
			(void)operations.Select(entities, mode, mode == SelectionMode::Toggle ? UUID::Invalid() : hoveredIcon);
			return;
		}
		// One pick at a time: a click while the previous result is still on its way (a frame or two) is ignored.
		glm::vec2 const pixel = ToVec2(io.MousePos) - imagePosition;
		if (pixel.x >= 0.0f && pixel.y >= 0.0f && !m_Renderer->IsPickPending())
		{
			m_Renderer->RequestPick(static_cast<uint32_t>(pixel.x), static_cast<uint32_t>(pixel.y));
			m_PendingPick = PendingPick{mode, AssetHandle()};
		}
	}

	void ViewportPanel::ApplyPickResult(EditorOperations& operations)
	{
		std::optional<uint32_t> const result = m_Renderer->TakePickResult();
		if (!result)
		{
			return;
		}
		std::optional<PendingPick> const request = std::exchange(m_PendingPick, std::nullopt);
		if (std::exchange(m_DiscardPick, false) || !request)
		{
			return;
		}

		UUID const entity = m_PickingIds.GetEntity(*result);
		EditorContext& context = operations.GetContext();
		bool const hit = entity.IsValid() && context.GetScene().HasEntity(entity);
		if (request->Material.IsValid())
		{
			// A material dropped onto empty space does nothing.
			if (hit)
			{
				UI::ReportFailure(AssetDrops::AssignMaterial(operations, entity, request->Material), "Assigning the material");
			}
			return;
		}
		SelectionMode const mode = request->Mode;
		if (!hit)
		{
			// Clicking empty space clears the selection (unless adding to it).
			if (mode == SelectionMode::Replace)
			{
				context.ClearSelection();
			}
			return;
		}
		UUID const entities[] = {entity};
		(void)operations.Select(entities, mode, mode == SelectionMode::Toggle ? UUID::Invalid() : entity);
	}

	void ViewportPanel::ApplyAssetDrop(EditorOperations& operations, AssetHandle asset, glm::vec2 const& pixel, glm::vec2 const& imageSize)
	{
		switch (AssetManager::GetAssetType(asset))
		{
			case AssetType::Mesh:
			case AssetType::Prefab:
			{
				glm::vec3 const position = m_Camera.GetPlacementPoint(pixel, imageSize);
				Result<UUID> created = AssetManager::GetAssetType(asset) == AssetType::Prefab
				                           ? operations.InstantiatePrefab(asset, UUID::Invalid(), position)
				                           : EntityPresets::CreateFromMesh(operations, asset, UUID::Invalid(), position);
				UI::ReportFailure(created, "Adding the asset");
				if (created)
				{
					std::array<UUID, 1> const selection = {created.GetValue()};
					UI::ReportFailure(operations.Select(selection), "Selecting the new entity");
				}
				break;
			}
			case AssetType::Material:
				// The entity under the cursor is known when the pick result arrives, a frame or two later. Picks run one at a
				// time, and a click's pick can only be pending if the drop happened right after a click.
				if (m_Renderer->IsPickPending())
				{
					ST_WARN("The material was not assigned because the viewport was busy; drop it again.");
				}
				else if (pixel.x >= 0.0f && pixel.y >= 0.0f)
				{
					m_Renderer->RequestPick(static_cast<uint32_t>(pixel.x), static_cast<uint32_t>(pixel.y));
					m_PendingPick = PendingPick{SelectionMode::Replace, asset};
				}
				break;
			case AssetType::Environment:
				UI::ReportFailure(AssetDrops::SetSkyEnvironment(operations, asset), "Setting the environment");
				break;
			case AssetType::Scene:
				if (m_OpenScene)
				{
					m_OpenScene(AssetManager::GetAbsolutePath(asset));
				}
				break;
			// Not accepted by the drop target.
			case AssetType::None:
			case AssetType::Texture:
			case AssetType::AudioClip:
			case AssetType::Font:
				break;
		}
	}

	void ViewportPanel::DrawToolbar(glm::vec2 const& imagePosition)
	{
		ImGui::SetCursorScreenPos(ToImVec2(imagePosition + glm::vec2(ToolbarPadding)));
		ImGui::BeginGroup();
		auto const toggleButton = [](char const* label, bool active, char const* tooltip)
		{
			if (active)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			}
			bool const pressed = ImGui::Button(label);
			if (active)
			{
				ImGui::PopStyleColor();
			}
			ImGui::SetItemTooltip("%s", tooltip);
			return pressed;
		};

		if (toggleButton("Move", m_GizmoOperation == GizmoOperation::Translate, "Translate the selection (W)"))
		{
			m_GizmoOperation = GizmoOperation::Translate;
		}
		ImGui::SameLine();
		if (toggleButton("Rotate", m_GizmoOperation == GizmoOperation::Rotate, "Rotate the selection (E)"))
		{
			m_GizmoOperation = GizmoOperation::Rotate;
		}
		ImGui::SameLine();
		if (toggleButton("Scale", m_GizmoOperation == GizmoOperation::Scale, "Scale the selection (R)"))
		{
			m_GizmoOperation = GizmoOperation::Scale;
		}
		ImGui::SameLine();
		if (toggleButton(m_GizmoSpace == GizmoSpace::Local ? "Local" : "World", false, "Gizmo orientation (X)"))
		{
			m_GizmoSpace = m_GizmoSpace == GizmoSpace::Local ? GizmoSpace::World : GizmoSpace::Local;
		}
		ImGui::SameLine();
		if (toggleButton("Snap", m_SnapEnabled, "Snap translation, rotation and scale (hold Ctrl to toggle while dragging)"))
		{
			m_SnapEnabled = !m_SnapEnabled;
		}
		ImGui::SameLine();
		if (toggleButton("Grid", m_ShowGrid, "Ground grid (G)"))
		{
			m_ShowGrid = !m_ShowGrid;
		}
		ImGui::EndGroup();
		m_ToolbarMin = ToVec2(ImGui::GetItemRectMin());
		m_ToolbarMax = ToVec2(ImGui::GetItemRectMax());
	}

	void ViewportPanel::FocusEntities(Scene& scene, std::span<UUID const> entities)
	{
		m_Camera.Focus(ComputeEntityBounds(scene, entities));
	}
}
