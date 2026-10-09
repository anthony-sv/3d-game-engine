#include "Editor/Panels/InspectorPanel.h"

#include "Editor/ComponentInspection.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/AudioClipAsset.h"
#include "Strada/Asset/EnvironmentAsset.h"
#include "Strada/Asset/FontAsset.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/MeshSource.h"
#include "Strada/Asset/TextureAsset.h"
#include "Strada/ImGui/ImGuiRenderer.h"
#include "Strada/Renderer/Renderer.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <string_view>

namespace Strada
{
	namespace
	{
		// Merge keys of this panel ("INSP" in the high bytes, a running count below).
		constexpr uint64_t MergeKeyPrefix = 0x494E535000000000ull;

		// Texture previews fill the panel's width up to this height (in font sizes, so it follows the display scale).
		constexpr float MaxPreviewHeightInFontSizes = 16.0f;
		constexpr ImVec4 ErrorColor = ImVec4(0.95f, 0.40f, 0.35f, 1.0f);

		void TextError(std::string const& message)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ErrorColor);
			ImGui::TextWrapped("%s", message.c_str());
			ImGui::PopStyleColor();
		}

		// Loads an asset to show its details (assets load on first use; failures are remembered, not retried every frame).
		template<typename T>
		Ref<T> LoadForInspection(AssetHandle asset)
		{
			Result<Ref<T>> loaded = AssetManager::TryGetAsset<T>(asset);
			if (!loaded)
			{
				TextError(loaded.GetError());
				return nullptr;
			}
			return loaded.TakeValue();
		}

		// Read-only rows laid out like the field editor's property table.
		bool BeginDetails()
		{
			if (!ImGui::BeginTable("##details", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings))
			{
				return false;
			}
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.4f);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.6f);
			return true;
		}

		void DetailRow(char const* label, std::string const& value)
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(label);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(value.c_str());
		}

		std::string FormatByteSize(size_t bytes)
		{
			constexpr double Kilobyte = 1024.0;
			if (static_cast<double>(bytes) < Kilobyte)
			{
				return fmt::format("{} B", bytes);
			}
			if (static_cast<double>(bytes) < Kilobyte * Kilobyte)
			{
				return fmt::format("{:.1f} KB", static_cast<double>(bytes) / Kilobyte);
			}
			return fmt::format("{:.1f} MB", static_cast<double>(bytes) / (Kilobyte * Kilobyte));
		}

		void DrawTexturePreview(AssetHandle asset, uint32_t width, uint32_t height)
		{
			if (!Renderer::IsInitialized() || width == 0 || height == 0)
			{
				return;
			}
			// Sampled without sRGB decoding, so the stored (already sRGB-encoded) values reach the 8-bit swapchain unchanged.
			nvrhi::ITexture* texture = Renderer::GetTexture(asset, false, AssetHandle());
			if (texture == nullptr)
			{
				return;
			}
			float const maxHeight = ImGui::GetFontSize() * MaxPreviewHeightInFontSizes;
			float const scale =
				std::min(ImGui::GetContentRegionAvail().x / static_cast<float>(width), maxHeight / static_cast<float>(height));
			ImGui::Spacing();
			ImGui::Image(ImGuiRenderer::GetTextureID(texture),
			             ImVec2(std::max(1.0f, static_cast<float>(width) * scale), std::max(1.0f, static_cast<float>(height) * scale)));
		}

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
		: m_MaterialFields(GetFieldDescriptors<StructTraits<MaterialData>, MaterialData>()),
		  m_EditSession(MergeKeyPrefix)
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
			if (context.GetSelectedAsset().IsValid())
			{
				DrawAsset(operations, context.GetSelectedAsset());
			}
			else
			{
				ImGui::TextDisabled("Select an entity or an asset to inspect it.");
			}
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

	void InspectorPanel::DrawAsset(EditorOperations& operations, AssetHandle asset)
	{
		std::optional<AssetMetadata> const metadata = AssetManager::IsInitialized() ? AssetManager::GetMetadata(asset) : std::nullopt;
		if (!metadata)
		{
			// The asset was deleted, or its project was closed.
			operations.GetContext().SelectAsset(AssetHandle());
			ImGui::TextDisabled("Select an entity or an asset to inspect it.");
			return;
		}

		ImGui::PushID("##asset");
		std::string const name = metadata->GetDisplayName();
		std::string const type = UI::FormatDisplayName(AssetTypeToString(metadata->Type));
		ImGui::TextUnformatted(name.c_str());
		ImGui::TextDisabled("%s", type.c_str());
		std::string const reference = AssetManager::GetReference(asset);
		ImGui::TextDisabled("%s", reference.c_str());
		if (ImGui::BeginPopupContextItem("##referenceMenu"))
		{
			if (ImGui::MenuItem("Copy Reference"))
			{
				ImGui::SetClipboardText(reference.c_str());
			}
			if (ImGui::MenuItem("Copy ID"))
			{
				ImGui::SetClipboardText(asset.ToString().c_str());
			}
			ImGui::EndPopup();
		}
		if (metadata->IsBuiltIn())
		{
			ImGui::TextDisabled("Built into the engine.");
		}
		else if (metadata->IsSubAsset())
		{
			std::optional<AssetMetadata> const parent = AssetManager::GetMetadata(metadata->Parent);
			ImGui::TextDisabled("Imported with %s.", parent ? parent->GetDisplayName().c_str() : metadata->Parent.ToString().c_str());
		}
		ImGui::Spacing();

		if (AssetManager::IsMissing(asset))
		{
			TextError("The file is missing. Restore it and refresh the assets, or delete the asset.");
			ImGui::PopID();
			return;
		}
		switch (metadata->Type)
		{
			case AssetType::Material:
				DrawMaterial(operations, *metadata);
				break;
			case AssetType::Mesh:
				DrawMesh(operations, asset);
				break;
			case AssetType::Texture:
				if (Ref<TextureAsset> const texture = LoadForInspection<TextureAsset>(asset))
				{
					if (BeginDetails())
					{
						DetailRow("Size", fmt::format("{} x {}", texture->GetWidth(), texture->GetHeight()));
						ImGui::EndTable();
					}
					DrawTexturePreview(asset, texture->GetWidth(), texture->GetHeight());
				}
				break;
			case AssetType::Environment:
				if (Ref<EnvironmentAsset> const environment = LoadForInspection<EnvironmentAsset>(asset))
				{
					if (BeginDetails())
					{
						DetailRow("Size", fmt::format("{} x {}", environment->GetWidth(), environment->GetHeight()));
						ImGui::EndTable();
					}
				}
				break;
			case AssetType::AudioClip:
				if (Ref<AudioClipAsset> const clip = LoadForInspection<AudioClipAsset>(asset))
				{
					if (BeginDetails())
					{
						DetailRow("Format", AudioFormatToString(clip->GetFormat()));
						DetailRow("File Size", FormatByteSize(clip->GetData().size()));
						ImGui::EndTable();
					}
				}
				break;
			case AssetType::Font:
				if (Ref<FontAsset> const font = LoadForInspection<FontAsset>(asset))
				{
					if (BeginDetails())
					{
						DetailRow("File Size", FormatByteSize(font->GetData().size()));
						ImGui::EndTable();
					}
				}
				break;
			case AssetType::Scene:
				ImGui::TextDisabled("Double-click the scene in the content browser to open it.");
				break;
			case AssetType::Prefab:
			case AssetType::None:
				break;
		}
		ImGui::PopID();
	}

	void InspectorPanel::DrawMaterial(EditorOperations& operations, AssetMetadata const& metadata)
	{
		Ref<MaterialAsset> const material = LoadForInspection<MaterialAsset>(metadata.Handle);
		if (!material)
		{
			return;
		}
		// Only material files keep changes: built-in and imported materials are recreated from their source.
		bool const editable = metadata.IsFileAsset();
		if (!editable)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextWrapped("This material is read-only. Create a material asset to customize it.");
			ImGui::PopStyleColor();
			ImGui::Spacing();
		}
		Json const values = SerializeFields<StructTraits<MaterialData>>(material->GetData());
		ImGui::BeginDisabled(!editable);
		std::optional<FieldChange> const change = m_FieldEditor.Draw(m_MaterialFields, values);
		ImGui::EndDisabled();
		if (change && editable)
		{
			UI::ReportFailure(operations.SetMaterialFields(metadata.Handle, change->MakePatch(values), m_EditSession.GetMergeKey()),
			                  "Editing the material");
		}
	}

	void InspectorPanel::DrawMesh(EditorOperations& operations, AssetHandle asset)
	{
		Ref<MeshSource> const mesh = LoadForInspection<MeshSource>(asset);
		if (!mesh)
		{
			return;
		}
		if (BeginDetails())
		{
			DetailRow("Vertices", fmt::format("{}", mesh->GetVertices().size()));
			DetailRow("Triangles", fmt::format("{}", mesh->GetTriangleCount()));
			DetailRow("Submeshes", fmt::format("{}", mesh->GetSubmeshes().size()));
			if (AABB const& bounds = mesh->GetBounds(); bounds.IsValid())
			{
				glm::vec3 const size = bounds.Max - bounds.Min;
				DetailRow("Size", fmt::format("{:.3g} x {:.3g} x {:.3g}", size.x, size.y, size.z));
			}
			ImGui::EndTable();
		}

		std::vector<AssetHandle> const& materials = mesh->GetMaterials();
		if (materials.empty())
		{
			return;
		}
		ImGui::SeparatorText("Materials");
		for (size_t i = 0; i < materials.size(); i++)
		{
			std::optional<AssetMetadata> const material = AssetManager::GetMetadata(materials[i]);
			std::string const label = fmt::format("{}: {}", i, material ? material->GetDisplayName() : materials[i].ToString());
			ImGui::PushID(static_cast<int>(i));
			// Selecting shows the material (read-only); dragging assigns it like any material asset.
			if (ImGui::Selectable(label.c_str()) && material)
			{
				operations.GetContext().SelectAsset(materials[i]);
			}
			if (material)
			{
				UI::AssetDragSource(materials[i], label);
			}
			ImGui::PopID();
		}
	}
}
