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
#include <glm/gtc/constants.hpp>
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
		// Icon sizes are in pixels at a UI scale of 1.
		constexpr float IconRadius = 12.0f;
		// A press and release closer than this (in pixels) is a click; farther is a drag.
		constexpr float ClickDragThreshold = 4.0f;
		// Merge keys of gizmo drags ("GIZMO" in the high bytes); each drag adds its sequence number.
		constexpr uint64_t GizmoMergeKeyBase = 0x47495A4D4F000000ull;
		constexpr ImU32 IconBackground = IM_COL32(24, 24, 26, 215);
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
			DirectionalLight,
			PointLight,
			SpotLight,
			SkyLight
		};

		IconShape GetIconShape(Entity entity)
		{
			if (entity.HasComponent<CameraComponent>())
			{
				return IconShape::Camera;
			}
			if (entity.HasComponent<DirectionalLightComponent>())
			{
				return IconShape::DirectionalLight;
			}
			if (entity.HasComponent<SpotLightComponent>())
			{
				return IconShape::SpotLight;
			}
			if (entity.HasComponent<PointLightComponent>())
			{
				return IconShape::PointLight;
			}
			if (entity.HasComponent<SkyLightComponent>())
			{
				return IconShape::SkyLight;
			}
			return IconShape::None;
		}

		// A billboard: a dark disc with a ring and a symbol of the kind of entity, in the icon's color.
		void DrawIcon(ImDrawList* drawList, ImVec2 center, IconShape shape, ImU32 color, float scale)
		{
			auto const at = [center, scale](float x, float y)
			{
				return ImVec2(center.x + x * scale, center.y + y * scale);
			};
			drawList->AddCircleFilled(center, IconRadius * scale, IconBackground);
			drawList->AddCircle(center, IconRadius * scale, color, 0, 1.5f * scale);
			// Filled shapes list their points clockwise (screen Y points down), as ImGui's anti-aliasing expects.
			switch (shape)
			{
				case IconShape::Camera:
					// A camera body and its lens.
					drawList->AddRectFilled(at(-6.5f, -4.0f), at(2.0f, 4.0f), color, 1.5f * scale);
					drawList->AddTriangleFilled(at(2.0f, 0.0f), at(7.0f, -4.0f), at(7.0f, 4.0f), color);
					break;
				case IconShape::DirectionalLight:
					// The sun.
					drawList->AddCircleFilled(center, 3.5f * scale, color);
					for (int i = 0; i < 8; i++)
					{
						float const angle = static_cast<float>(i) * 0.7853982f;
						float const x = std::cos(angle);
						float const y = std::sin(angle);
						drawList->AddLine(at(x * 5.5f, y * 5.5f), at(x * 8.0f, y * 8.0f), color, 1.5f * scale);
					}
					break;
				case IconShape::PointLight:
					// A light bulb.
					drawList->AddCircleFilled(at(0.0f, -1.5f), 4.75f * scale, color);
					drawList->AddRectFilled(at(-2.5f, 3.5f), at(2.5f, 7.0f), color, 1.0f * scale);
					break;
				case IconShape::SpotLight:
					// A lamp housing and its beam.
					drawList->AddRectFilled(at(-3.0f, -7.5f), at(3.0f, -3.5f), color, 1.0f * scale);
					drawList->AddTriangleFilled(at(0.0f, -4.0f), at(6.5f, 6.5f), at(-6.5f, 6.5f), color);
					break;
				case IconShape::SkyLight:
					// The sky's dome over the horizon.
					drawList->PathArcTo(at(0.0f, 3.5f), 6.5f * scale, glm::pi<float>(), glm::two_pi<float>());
					drawList->PathStroke(color, 2.0f * scale);
					drawList->AddLine(at(-8.0f, 3.5f), at(8.0f, 3.5f), color, 2.0f * scale);
					break;
				case IconShape::None:
					break;
			}
		}

		// A kind of overlay shape: whether it is drawn, and for the selection or every entity.
		void DrawShapeSetting(char const* label, char const* tooltip, OverlayShapeSetting& setting, float choicesColumn)
		{
			ImGui::PushID(label);
			ImGui::Checkbox(label, &setting.Enabled);
			ImGui::SetItemTooltip("%s", tooltip);
			ImGui::SameLine(choicesColumn);
			ImGui::BeginDisabled(!setting.Enabled);
			if (ImGui::RadioButton("Selected", !setting.AllEntities))
			{
				setting.AllEntities = false;
			}
			ImGui::SameLine();
			if (ImGui::RadioButton("All", setting.AllEntities))
			{
				setting.AllEntities = true;
			}
			ImGui::EndDisabled();
			ImGui::PopID();
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
		bool const visible = ImGui::Begin("Scene View", &open);
		ImGui::PopStyleVar();
		if (!visible)
		{
			EndGizmoDrag(context);
			ImGui::End();
			return;
		}
		DrawToolbar();
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

		// Popups of the toolbar (opened over the image) keep the clicks.
		bool const hovered =
			ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(ToImVec2(imagePosition), ToImVec2(imagePosition + imageSize));
		HandleCameraInput(hovered);
		HandleShortcuts(context, hovered);
		m_Camera.SetViewportSize(width, height);
		m_Renderer->SetViewportSize(width, height);
		m_ViewportSize = glm::uvec2(width, height);
		ApplyPickResult(operations);
		Render(context);

		ImGui::Image(ImGuiRenderer::GetTextureID(m_Renderer->GetFinalImage()), ToImVec2(imageSize));
		AssetHandle droppedAsset;
		if (ImGui::BeginDragDropTarget())
		{
			constexpr std::array<AssetType, 5> DroppableTypes = {AssetType::Mesh, AssetType::Material, AssetType::Environment,
			                                                     AssetType::Scene, AssetType::Prefab};
			droppedAsset = UI::AcceptAssetDrop(DroppableTypes);
			ImGui::EndDragDropTarget();
		}
		glm::vec2 const dropPixel = ToVec2(ImGui::GetMousePos()) - imagePosition;
		UUID const hoveredIcon = m_Overlays.Icons ? DrawEntityIcons(context, imagePosition, imageSize, hovered) : UUID::Invalid();
		UpdateGizmo(operations, imagePosition, imageSize);
		HandleSelectionClicks(operations, imagePosition, hovered, hoveredIcon);
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
		if (ImGui::IsKeyPressed(ImGuiKey_Q, false))
		{
			m_GizmoOperation = GizmoOperation::Select;
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
			m_Overlays.Grid = !m_Overlays.Grid;
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
		float const uiScale = ImGui::GetStyle().FontScaleDpi;

		SceneRenderOptions options;
		options.GetPickingId = [this](UUID entity)
		{
			return m_PickingIds.GetId(entity);
		};
		if (m_Overlays.SelectionOutline)
		{
			options.IsSelected = [&selection](UUID entity)
			{
				return selection.Contains(entity);
			};
		}
		options.DrawDebugLines = m_Overlays.DebugLines;
		options.SubmitOverlays = [&](SceneRenderer& renderer)
		{
			SceneRendererOverlays overlays;
			overlays.Grid.Enabled = m_Overlays.Grid;
			overlays.LineWidth = ViewportOverlays::LineWidth * uiScale;
			overlays.LineOutlineWidth = ViewportOverlays::LineOutlineWidth * uiScale;
			overlays.LineOutlineColor = ViewportOverlays::LineOutlineColor;
			overlays.OccludedLineOpacity = ViewportOverlays::OccludedLineOpacity;
			renderer.SetOverlays(overlays);

			std::vector<OverlayLine> lines;
			ViewportOverlays::AppendSceneShapes(scene, selection.GetEntities(), m_Overlays, aspectRatio, m_Camera.GetPosition(), lines,
			                                    &m_MeshColliderOutlines);
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
		float const scale = ImGui::GetStyle().FontScaleDpi;
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->PushClipRect(ToImVec2(imagePosition), ToImVec2(imagePosition + imageSize), true);

		UUID hoveredIcon = UUID::Invalid();
		float closestDistance = IconRadius * scale;
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
			DrawIcon(drawList, ToImVec2(screen), GetIconShape(entity), color, scale);
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
			case GizmoOperation::Select:
				EndGizmoDrag(context);
				return;
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

	void ViewportPanel::DrawToolbar()
	{
		// A strip above the image, like the game view's.
		ImGuiStyle const& style = ImGui::GetStyle();
		ImGui::SetCursorPos(ImVec2(style.WindowPadding.x + 6.0f, ImGui::GetCursorPosY() + 4.0f));
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

		struct OperationButton
		{
			GizmoOperation Operation;
			char const* Label;
			char const* Tooltip;
		};
		constexpr std::array<OperationButton, 4> Operations = {{
			{GizmoOperation::Select, "Select", "Select without a gizmo (Q)"},
			{GizmoOperation::Translate, "Move", "Move the selection (W)"},
			{GizmoOperation::Rotate, "Rotate", "Rotate the selection (E)"},
			{GizmoOperation::Scale, "Scale", "Scale the selection (R)"},
		}};
		for (OperationButton const& button : Operations)
		{
			if (toggleButton(button.Label, m_GizmoOperation == button.Operation, button.Tooltip))
			{
				m_GizmoOperation = button.Operation;
			}
			ImGui::SameLine();
		}
		ImGui::SameLine(0.0f, style.ItemSpacing.x * 3.0f);
		if (toggleButton(m_GizmoSpace == GizmoSpace::Local ? "Local" : "World", false, "Gizmo orientation: local or world axes (X)"))
		{
			m_GizmoSpace = m_GizmoSpace == GizmoSpace::Local ? GizmoSpace::World : GizmoSpace::Local;
		}
		ImGui::SameLine();
		if (toggleButton("Snap", m_SnapEnabled, "Snap translation, rotation and scale (hold Ctrl to toggle while dragging)"))
		{
			m_SnapEnabled = !m_SnapEnabled;
		}
		ImGui::SameLine();
		if (ImGui::Button("Steps"))
		{
			ImGui::OpenPopup("SnapSteps");
		}
		ImGui::SetItemTooltip("Snapping steps");
		if (ImGui::BeginPopup("SnapSteps"))
		{
			ImGui::SetNextItemWidth(120.0f);
			ImGui::DragFloat("Move (m)", &m_TranslationSnap, 0.01f, 0.001f, 1000.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SetNextItemWidth(120.0f);
			ImGui::DragFloat("Rotate (degrees)", &m_RotationSnap, 0.5f, 0.1f, 180.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SetNextItemWidth(120.0f);
			ImGui::DragFloat("Scale", &m_ScaleSnap, 0.01f, 0.001f, 100.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::EndPopup();
		}
		ImGui::SameLine(0.0f, style.ItemSpacing.x * 3.0f);
		if (ImGui::Button("Overlays"))
		{
			ImGui::OpenPopup("Overlays");
		}
		ImGui::SetItemTooltip("What the scene view draws over the scene");
		if (ImGui::BeginPopup("Overlays"))
		{
			ImGui::Checkbox("Grid (G)", &m_Overlays.Grid);
			ImGui::Checkbox("Selection outline", &m_Overlays.SelectionOutline);
			ImGui::Checkbox("Light and camera gizmos", &m_Overlays.Icons);
			ImGui::SetItemTooltip("Icons of cameras and lights; click one to select its entity");

			struct ShapeRow
			{
				char const* Label;
				char const* Tooltip;
				OverlayShapeSetting& Setting;
			};
			ShapeRow const rows[] = {
				{"Camera frustums", "What cameras see, up to a few meters", m_Overlays.Cameras},
				{"Light shapes", "Directional light rays, point light ranges and spot light cones", m_Overlays.Lights},
				{"Colliders", "Collider outlines (triggers in blue) and character capsules", m_Overlays.Colliders},
				{"Joints", "Joint anchors, hinge and slider axes, and what they connect to", m_Overlays.Joints},
				{"Post-process volumes", "Boxes of local post-process volumes and where their blending ends", m_Overlays.Volumes},
			};
			// The choices line up after the longest label.
			float labelWidth = 0.0f;
			for (ShapeRow const& row : rows)
			{
				labelWidth = std::max(labelWidth, ImGui::CalcTextSize(row.Label).x);
			}
			float const choicesColumn =
				ImGui::GetCursorPosX() + ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + labelWidth + style.ItemSpacing.x * 3.0f;
			ImGui::SeparatorText("Shapes");
			for (ShapeRow const& row : rows)
			{
				DrawShapeSetting(row.Label, row.Tooltip, row.Setting, choicesColumn);
			}
			ImGui::Separator();
			ImGui::Checkbox("Debug drawing (scripts)", &m_Overlays.DebugLines);
			ImGui::SetItemTooltip("Lines scripts draw with Debug.DrawLine and its relatives");
			ImGui::EndPopup();
		}
		ImGui::Dummy(ImVec2(0.0f, 2.0f));
	}

	void ViewportPanel::FocusEntities(Scene& scene, std::span<UUID const> entities)
	{
		m_Camera.Focus(ComputeEntityBounds(scene, entities));
	}
}
