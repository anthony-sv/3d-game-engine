#include "Editor/Panels/SceneHierarchyPanel.h"

#include "Editor/EntityPresets.h"
#include "Editor/HierarchyEditing.h"
#include "Editor/UI/EditorUI.h"

#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cstring>
#include <string_view>

namespace Strada
{
	namespace
	{
		// Entities linked to a prefab are drawn in this color.
		constexpr ImVec4 PrefabColor = ImVec4(0.45f, 0.70f, 1.0f, 1.0f);
		// The upper and lower quarter of a row place dropped entities before or after it.
		constexpr float DropEdgeFraction = 0.25f;

		void const* ToImGuiID(UUID id)
		{
			return reinterpret_cast<void const*>(static_cast<uintptr_t>(id.GetValue()));
		}

		std::vector<UUID> ReadEntityPayload(ImGuiPayload const& payload)
		{
			size_t const count = static_cast<size_t>(payload.DataSize) / sizeof(uint64_t);
			std::vector<UUID> entities;
			entities.reserve(count);
			for (size_t i = 0; i < count; i++)
			{
				uint64_t value = 0;
				std::memcpy(&value, static_cast<char const*>(payload.Data) + i * sizeof(uint64_t), sizeof(value));
				entities.emplace_back(value);
			}
			return entities;
		}

		void DrawDropIndicator(ImVec2 const& rowMin, ImVec2 const& rowMax, DropPosition position)
		{
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			ImU32 const color = ImGui::GetColorU32(ImGuiCol_DragDropTarget);
			switch (position)
			{
				case DropPosition::Before:
					drawList->AddLine(ImVec2(rowMin.x, rowMin.y), ImVec2(rowMax.x, rowMin.y), color, 2.0f);
					break;
				case DropPosition::After:
					drawList->AddLine(ImVec2(rowMin.x, rowMax.y), ImVec2(rowMax.x, rowMax.y), color, 2.0f);
					break;
				case DropPosition::Inside:
					drawList->AddRect(rowMin, rowMax, color, 0.0f, 2.0f);
					break;
			}
		}
	}

	void SceneHierarchyPanel::OnImGuiRender(EditorOperations& operations, bool& open)
	{
		if (!ImGui::Begin("Scene Hierarchy", &open))
		{
			ImGui::End();
			return;
		}

		EditorContext& context = operations.GetContext();
		Scene& scene = context.GetScene();
		if (context.GetSceneVersion() != m_SceneVersion)
		{
			m_SceneVersion = context.GetSceneVersion();
			m_Click.reset();
			m_PressedSelected = UUID::Invalid();
			m_RangeAnchor = UUID::Invalid();
			m_Renaming = UUID::Invalid();
		}

		// Selections made elsewhere (viewport picks, automation) scroll to the new primary entity and open its ancestors.
		UUID const primary = context.GetSelection().GetPrimary();
		if (primary != m_LastPrimary)
		{
			if (!m_SelectionFromPanel && primary.IsValid())
			{
				m_Reveal = primary;
				m_RevealAncestors = HierarchyEditing::GetAncestors(scene, primary);
			}
			m_LastPrimary = primary;
		}
		m_SelectionFromPanel = false;

		if (ImGui::Button("+"))
		{
			ImGui::OpenPopup("##create");
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
		{
			ImGui::SetTooltip("Create Entity");
		}
		if (ImGui::BeginPopup("##create"))
		{
			if (EntityPreset const* preset = UI::DrawEntityPresetMenuItems())
			{
				CreatePreset(operations, *preset, UUID::Invalid());
			}
			ImGui::EndPopup();
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint("##filter", "Search", &m_Filter);

		m_Displayed.clear();
		if (ImGui::BeginChild("##entities"))
		{
			if (m_Filter.empty())
			{
				for (Entity const root : scene.GetRootEntities())
				{
					DrawEntity(operations, root, true);
				}
			}
			else
			{
				for (UUID const id : HierarchyEditing::FindByName(scene, m_Filter))
				{
					DrawEntity(operations, scene.GetEntityByUUID(id), false);
				}
			}
			DrawBackground(operations);

			if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
			    ImGui::IsKeyPressed(ImGuiKey_F2, false) && primary.IsValid())
			{
				BeginRename(primary, scene.GetEntityByUUID(primary).GetName());
			}
		}
		ImGui::EndChild();

		if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		{
			m_PressedSelected = UUID::Invalid();
		}
		m_Reveal = UUID::Invalid();
		m_RevealAncestors.clear();

		ApplyClick(operations);
		std::vector<std::function<void()>> deferred;
		deferred.swap(m_Deferred);
		for (std::function<void()> const& action : deferred)
		{
			action();
		}
		ImGui::End();
	}

	void SceneHierarchyPanel::DrawEntity(EditorOperations& operations, Entity entity, bool showChildren)
	{
		EditorContext& context = operations.GetContext();
		Scene& scene = context.GetScene();
		UUID const id = entity.GetUUID();
		std::vector<Entity> const children = showChildren ? scene.GetChildren(entity) : std::vector<Entity>();
		bool const selected = context.GetSelection().Contains(id);
		bool const renaming = m_Renaming == id;

		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
		if (selected)
		{
			flags |= ImGuiTreeNodeFlags_Selected;
		}
		if (children.empty())
		{
			flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		}
		if (std::find(m_RevealAncestors.begin(), m_RevealAncestors.end(), id) != m_RevealAncestors.end())
		{
			ImGui::SetNextItemOpen(true);
		}

		ImVec2 const rowStart = ImGui::GetCursorScreenPos();
		glm::vec2 const labelPosition(rowStart.x + ImGui::GetTreeNodeToLabelSpacing(), rowStart.y);
		bool const isPrefab = entity.HasComponent<PrefabComponent>();
		if (isPrefab)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, PrefabColor);
		}
		bool const open = ImGui::TreeNodeEx(ToImGuiID(id), flags, "%s", renaming ? "" : entity.GetName().c_str());
		if (isPrefab)
		{
			ImGui::PopStyleColor();
		}
		m_Displayed.push_back(id);

		DrawRowInteractions(operations, entity, selected);
		DrawRowContextMenu(operations, entity);
		if (renaming)
		{
			DrawRenameField(operations, entity, labelPosition);
		}
		if (m_Reveal == id)
		{
			ImGui::SetScrollHereY();
		}

		if (open && !children.empty())
		{
			for (Entity const child : children)
			{
				DrawEntity(operations, child, true);
			}
			ImGui::TreePop();
		}
	}

	void SceneHierarchyPanel::DrawRowInteractions(EditorOperations& operations, Entity entity, bool selected)
	{
		EditorContext& context = operations.GetContext();
		UUID const id = entity.GetUUID();
		ImGuiIO const& io = ImGui::GetIO();

		if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
		{
			if (selected && !io.KeyCtrl && !io.KeyShift)
			{
				// Decided on release: dragging a selected row must keep the rest of the selection.
				m_PressedSelected = id;
			}
			else
			{
				m_Click = Click{id, io.KeyCtrl, io.KeyShift};
			}
		}
		if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !selected)
		{
			m_Click = Click{id, false, false};
		}
		if (m_PressedSelected == id && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		{
			if (ImGui::IsItemHovered() && !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left))
			{
				m_Click = Click{id, false, false};
			}
			m_PressedSelected = UUID::Invalid();
		}
		if (m_FocusCallback && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
		{
			m_FocusCallback(id);
		}

		if (ImGui::BeginDragDropSource())
		{
			std::vector<UUID> const dragged = GetActionTargets(context, id);
			std::vector<uint64_t> payload;
			payload.reserve(dragged.size());
			for (UUID const entityID : dragged)
			{
				payload.push_back(entityID.GetValue());
			}
			ImGui::SetDragDropPayload(DragDropPayload::Entities, payload.data(), payload.size() * sizeof(uint64_t));
			if (dragged.size() == 1)
			{
				ImGui::TextUnformatted(entity.GetName().c_str());
			}
			else
			{
				ImGui::Text("%zu entities", dragged.size());
			}
			ImGui::EndDragDropSource();
		}

		if (ImGui::BeginDragDropTarget())
		{
			ImVec2 const rowMin = ImGui::GetItemRectMin();
			ImVec2 const rowMax = ImGui::GetItemRectMax();
			float const edge = (rowMax.y - rowMin.y) * DropEdgeFraction;
			float const mouseY = ImGui::GetMousePos().y;
			DropPosition const position = mouseY < rowMin.y + edge   ? DropPosition::Before
			                              : mouseY > rowMax.y - edge ? DropPosition::After
			                                                         : DropPosition::Inside;
			if (ImGuiPayload const* payload = ImGui::AcceptDragDropPayload(
					DragDropPayload::Entities, ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
			{
				std::vector<UUID> const dragged = ReadEntityPayload(*payload);
				std::optional<MoveTarget> const target = HierarchyEditing::ResolveDrop(context.GetScene(), dragged, id, position);
				if (target)
				{
					DrawDropIndicator(rowMin, rowMax, position);
					if (payload->IsDelivery())
					{
						Defer(
							[&operations, dragged, move = *target]
							{
								UI::ReportFailure(operations.MoveEntities(dragged, move.Parent, move.InsertBefore), "Moving entities");
							});
					}
				}
				else
				{
					ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
				}
			}
			ImGui::EndDragDropTarget();
		}
	}

	void SceneHierarchyPanel::DrawRowContextMenu(EditorOperations& operations, Entity entity)
	{
		if (!ImGui::BeginPopupContextItem())
		{
			return;
		}
		EditorContext& context = operations.GetContext();
		UUID const id = entity.GetUUID();
		std::vector<UUID> const targets = GetActionTargets(context, id);

		if (ImGui::BeginMenu("Create Child"))
		{
			if (EntityPreset const* preset = UI::DrawEntityPresetMenuItems())
			{
				CreatePreset(operations, *preset, id);
			}
			ImGui::EndMenu();
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Rename", "F2", false, targets.size() == 1))
		{
			BeginRename(id, entity.GetName());
		}
		if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
		{
			Defer(
				[&operations, targets]
				{
					Result<std::vector<UUID>> copies = operations.DuplicateEntities(targets);
					UI::ReportFailure(copies, "Duplicating entities");
					if (copies)
					{
						UI::ReportFailure(operations.Select(copies.GetValue()), "Selecting the copies");
					}
				});
		}
		if (ImGui::MenuItem("Delete", "Delete"))
		{
			Defer(
				[&operations, targets]
				{
					UI::ReportFailure(operations.DeleteEntities(targets), "Deleting entities");
				});
		}
		if (m_FocusCallback)
		{
			ImGui::Separator();
			if (ImGui::MenuItem("Frame in Viewport"))
			{
				m_FocusCallback(id);
			}
		}
		ImGui::EndPopup();
	}

	void SceneHierarchyPanel::DrawRenameField(EditorOperations& operations, Entity entity, glm::vec2 const& labelPosition)
	{
		ImGui::SameLine();
		ImGui::SetCursorScreenPos(ImVec2(labelPosition.x, labelPosition.y));
		if (m_FocusRenameField)
		{
			ImGui::SetKeyboardFocusHere();
			m_FocusRenameField = false;
		}
		ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x, 60.0f));
		// The field keeps the row height.
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));
		ImGui::InputText("##rename", &m_RenameBuffer, ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::PopStyleVar();

		if (ImGui::IsItemActive())
		{
			m_RenameFieldActivated = true;
			return;
		}
		// Ends when the field loses focus (Escape reverts the text); a field that never got focus is dropped.
		if (m_RenameFieldActivated || ImGui::GetFrameCount() > m_RenameStartFrame + 1)
		{
			std::string const& name = entity.GetName();
			if (m_RenameFieldActivated && !m_RenameBuffer.empty() && m_RenameBuffer != name)
			{
				Defer(
					[&operations, id = entity.GetUUID(), newName = m_RenameBuffer]
					{
						UI::ReportFailure(operations.RenameEntity(id, newName), "Renaming the entity");
					});
			}
			m_Renaming = UUID::Invalid();
		}
	}

	void SceneHierarchyPanel::DrawBackground(EditorOperations& operations)
	{
		// The empty space below the rows: clicking clears the selection, dropping makes entities root entities (last).
		ImVec2 size = ImGui::GetContentRegionAvail();
		size.x = std::max(size.x, 1.0f);
		size.y = std::max(size.y, ImGui::GetFrameHeight());
		ImGui::InvisibleButton("##background", size);
		ImGuiIO const& io = ImGui::GetIO();
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !io.KeyCtrl && !io.KeyShift)
		{
			operations.GetContext().ClearSelection();
			m_SelectionFromPanel = true;
		}
		if (ImGui::BeginDragDropTarget())
		{
			if (ImGuiPayload const* payload = ImGui::AcceptDragDropPayload(DragDropPayload::Entities))
			{
				std::vector<UUID> const dragged = ReadEntityPayload(*payload);
				Defer(
					[&operations, dragged]
					{
						UI::ReportFailure(operations.MoveEntities(dragged, UUID::Invalid()), "Moving entities");
					});
			}
			ImGui::EndDragDropTarget();
		}
		if (ImGui::BeginPopupContextItem("##backgroundMenu"))
		{
			if (EntityPreset const* preset = UI::DrawEntityPresetMenuItems())
			{
				CreatePreset(operations, *preset, UUID::Invalid());
			}
			ImGui::EndPopup();
		}
	}

	void SceneHierarchyPanel::ApplyClick(EditorOperations& operations)
	{
		if (!m_Click)
		{
			return;
		}
		Click const click = *m_Click;
		m_Click.reset();
		m_SelectionFromPanel = true;

		std::vector<UUID> const clicked = {click.Entity};
		if (click.Range && m_RangeAnchor.IsValid())
		{
			std::vector<UUID> const range = HierarchyEditing::GetRange(m_Displayed, m_RangeAnchor, click.Entity);
			UI::ReportFailure(operations.Select(range, click.Toggle ? SelectionMode::Add : SelectionMode::Replace, click.Entity),
			                  "Selecting");
			return;
		}
		m_RangeAnchor = click.Entity;
		if (click.Toggle)
		{
			UI::ReportFailure(operations.Select(clicked, SelectionMode::Toggle), "Selecting");
		}
		else
		{
			UI::ReportFailure(operations.Select(clicked, SelectionMode::Replace, click.Entity), "Selecting");
		}
	}

	void SceneHierarchyPanel::BeginRename(UUID entity, std::string name)
	{
		m_Renaming = entity;
		m_RenameBuffer = std::move(name);
		m_FocusRenameField = true;
		m_RenameFieldActivated = false;
		m_RenameStartFrame = ImGui::GetFrameCount();
	}

	void SceneHierarchyPanel::CreatePreset(EditorOperations& operations, EntityPreset const& preset, UUID parent)
	{
		glm::vec3 const position = m_SpawnPositionProvider ? m_SpawnPositionProvider() : glm::vec3(0.0f);
		Defer(
			[&operations, &preset, parent, position]
			{
				Result<UUID> created = EntityPresets::Create(operations, preset, parent, position);
				UI::ReportFailure(created, "Creating the entity");
				if (created)
				{
					std::vector<UUID> const selection = {created.GetValue()};
					UI::ReportFailure(operations.Select(selection), "Selecting the new entity");
				}
			});
	}

	std::vector<UUID> SceneHierarchyPanel::GetActionTargets(EditorContext& context, UUID entity) const
	{
		EntitySelection const& selection = context.GetSelection();
		if (!selection.Contains(entity))
		{
			return {entity};
		}
		// Hierarchy order, so moved and duplicated entities keep their relative order.
		std::vector<UUID> targets;
		context.GetScene().ForEachEntityInHierarchyOrder(
			[&](Entity candidate)
			{
				if (selection.Contains(candidate.GetUUID()))
				{
					targets.push_back(candidate.GetUUID());
				}
			});
		return targets;
	}
}
