#include "Editor/Panels/InspectorPanel.h"

#include "Editor/ComponentInspection.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <string_view>

namespace Strada
{
	namespace
	{
		// Merge keys of this panel ("INSP" in the high bytes, a running count below).
		constexpr uint64_t MergeKeyPrefix = 0x494E535000000000ull;

		std::string DescribePrefab(AssetHandle prefab)
		{
			if (AssetManager::IsInitialized())
			{
				if (std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(prefab))
				{
					return metadata->GetDisplayName();
				}
			}
			return prefab.ToString();
		}
	}

	InspectorPanel::InspectorPanel()
		: m_EditSession(MergeKeyPrefix)
	{
	}

	void InspectorPanel::OnImGuiRender(EditorOperations& operations, bool& open)
	{
		if (!ImGui::Begin("Inspector", &open))
		{
			ImGui::End();
			return;
		}

		EditorContext& context = operations.GetContext();
		m_EditSession.Update(ImGui::IsAnyItemActive(), context.GetHistory());
		EntitySelection const& selection = context.GetSelection();
		if (selection.IsEmpty())
		{
			ImGui::TextDisabled("Select an entity to inspect it.");
			ImGui::End();
			return;
		}

		// The primary entity comes first: its values are the ones shown.
		UUID const primary = selection.GetPrimary().IsValid() ? selection.GetPrimary() : selection.GetEntities().front();
		std::vector<UUID> entities = {primary};
		for (UUID const id : selection.GetEntities())
		{
			if (id != primary)
			{
				entities.push_back(id);
			}
		}

		DrawHeader(operations, entities);
		for (ComponentInfo const* component : ComponentInspection::GetCommonComponents(context.GetScene(), entities))
		{
			DrawComponent(operations, *component, entities);
		}
		ImGui::Spacing();
		DrawAddComponent(operations, entities);

		std::vector<std::function<void()>> deferred;
		deferred.swap(m_Deferred);
		for (std::function<void()> const& action : deferred)
		{
			action();
		}
		ImGui::End();
	}

	void InspectorPanel::DrawHeader(EditorOperations& operations, std::vector<UUID> const& entities)
	{
		Scene& scene = operations.GetContext().GetScene();
		Entity const primary = scene.GetEntityByUUID(entities.front());
		std::string const name = primary.GetName();
		bool mixedNames = false;
		for (size_t i = 1; i < entities.size() && !mixedNames; i++)
		{
			mixedNames = scene.GetEntityByUUID(entities[i]).GetName() != name;
		}

		std::string buffer = mixedNames ? std::string() : name;
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::InputTextWithHint("##name", mixedNames ? "Multiple names" : "Name", &buffer) && !buffer.empty())
		{
			UI::ReportFailure(operations.RenameEntities(entities, buffer, m_EditSession.GetMergeKey()), "Renaming");
		}

		if (entities.size() == 1)
		{
			std::string const id = primary.GetUUID().ToString();
			ImGui::TextDisabled("ID %s", id.c_str());
			if (ImGui::BeginPopupContextItem("##idMenu"))
			{
				if (ImGui::MenuItem("Copy ID"))
				{
					ImGui::SetClipboardText(id.c_str());
				}
				ImGui::EndPopup();
			}
		}
		else
		{
			ImGui::TextDisabled("%zu entities selected", entities.size());
		}
		if (primary.HasComponent<PrefabComponent>())
		{
			ImGui::TextDisabled("Prefab: %s", DescribePrefab(primary.GetComponent<PrefabComponent>().Prefab).c_str());
		}
		ImGui::Spacing();
	}

	void InspectorPanel::DrawComponent(EditorOperations& operations, ComponentInfo const& component, std::vector<UUID> const& entities)
	{
		Scene& scene = operations.GetContext().GetScene();
		std::vector<Json> values;
		values.reserve(entities.size());
		for (UUID const id : entities)
		{
			values.push_back(component.Serialize(scene.GetRegistry(), scene.GetEntityByUUID(id).GetHandle()));
		}

		ImGui::PushID(component.Name.data(), component.Name.data() + component.Name.size());
		float const rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
		std::string const label = UI::FormatDisplayName(component.Name);
		bool const open = ImGui::CollapsingHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
		if (!component.Description.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
		{
			ImGui::SetTooltip("%.*s", static_cast<int>(component.Description.size()), component.Description.data());
		}
		ImGui::OpenPopupOnItemClick("##componentMenu", ImGuiPopupFlags_MouseButtonRight);
		float const buttonWidth = ImGui::GetFrameHeight() * 1.5f;
		ImGui::SameLine(rightEdge - buttonWidth);
		if (ImGui::Button("...", ImVec2(buttonWidth, 0.0f)))
		{
			ImGui::OpenPopup("##componentMenu");
		}
		DrawComponentMenu(operations, component, entities, values.front());

		if (open)
		{
			std::vector<std::string> const mixed = ComponentInspection::FindMixedFields(values);
			if (std::optional<FieldChange> const change = m_FieldEditor.Draw(component.Fields, values.front(), mixed, &scene))
			{
				std::vector<ComponentEdit> edits;
				edits.reserve(entities.size());
				for (size_t i = 0; i < entities.size(); i++)
				{
					edits.push_back({entities[i], std::string(component.Name), change->MakePatch(values[i])});
				}
				UI::ReportFailure(operations.SetComponentFields(std::move(edits), m_EditSession.GetMergeKey()), "Editing the component");
			}
			ImGui::Spacing();
		}
		ImGui::PopID();
	}

	void InspectorPanel::DrawComponentMenu(EditorOperations& operations, ComponentInfo const& component, std::vector<UUID> const& entities,
	                                       Json const& primaryValues)
	{
		if (!ImGui::BeginPopup("##componentMenu"))
		{
			return;
		}
		if (ImGui::MenuItem("Reset"))
		{
			ApplyPatch(operations, component, entities, ComponentInspection::GetDefaultValues(component), 0);
		}
		if (ImGui::MenuItem("Copy Values"))
		{
			ImGui::SetClipboardText(DumpJson(primaryValues).c_str());
		}
		char const* clipboard = ImGui::GetClipboardText();
		Result<Json> pasted = clipboard != nullptr ? ParseJson(clipboard) : Result<Json>(Error{"the clipboard is empty"});
		if (ImGui::MenuItem("Paste Values", nullptr, false, pasted && pasted.GetValue().is_object()))
		{
			ApplyPatch(operations, component, entities, pasted.GetValue(), 0);
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Remove Component", nullptr, false, !component.IsCore()))
		{
			m_Deferred.push_back(
				[&operations, &component, entities]
				{
					UI::ReportFailure(operations.RemoveComponent(std::span<UUID const>(entities), component.Name),
				                      "Removing the component");
				});
		}
		ImGui::EndPopup();
	}

	void InspectorPanel::DrawAddComponent(EditorOperations& operations, std::vector<UUID> const& entities)
	{
		if (ImGui::Button("Add Component", ImVec2(-FLT_MIN, 0.0f)))
		{
			m_AddComponentFilter.clear();
			ImGui::OpenPopup("##addComponent");
		}
		if (!ImGui::BeginPopup("##addComponent"))
		{
			return;
		}
		if (ImGui::IsWindowAppearing())
		{
			ImGui::SetKeyboardFocusHere();
		}
		ImGui::SetNextItemWidth(240.0f);
		ImGui::InputTextWithHint("##filter", "Search", &m_AddComponentFilter);

		Scene& scene = operations.GetContext().GetScene();
		bool listed = false;
		for (ComponentInfo const* component : ComponentInspection::GetAddableComponents(scene, entities))
		{
			std::string const label = UI::FormatDisplayName(component->Name);
			if (!m_AddComponentFilter.empty() && ImStristr(label.c_str(), nullptr, m_AddComponentFilter.c_str(), nullptr) == nullptr)
			{
				continue;
			}
			listed = true;
			if (ImGui::Selectable(label.c_str()))
			{
				std::vector<UUID> const targets = ComponentInspection::GetEntitiesWithout(scene, entities, *component);
				UI::ReportFailure(operations.AddComponent(std::span<UUID const>(targets), component->Name), "Adding the component");
				ImGui::CloseCurrentPopup();
			}
			if (!component->Description.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
			{
				ImGui::SetTooltip("%.*s", static_cast<int>(component->Description.size()), component->Description.data());
			}
		}
		if (!listed)
		{
			ImGui::TextDisabled("No components to add");
		}
		ImGui::EndPopup();
	}

	void InspectorPanel::ApplyPatch(EditorOperations& operations, ComponentInfo const& component, std::vector<UUID> const& entities,
	                                Json const& patch, uint64_t mergeKey)
	{
		std::vector<ComponentEdit> edits;
		edits.reserve(entities.size());
		for (UUID const id : entities)
		{
			edits.push_back({id, std::string(component.Name), patch});
		}
		UI::ReportFailure(operations.SetComponentFields(std::move(edits), mergeKey), "Editing the component");
	}
}
