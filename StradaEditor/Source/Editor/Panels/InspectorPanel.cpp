#include "Editor/Panels/InspectorPanel.h"

#include "Editor/AssetDrops.h"
#include "Editor/AssetThumbnails.h"
#include "Editor/ComponentInspection.h"
#include "Editor/MaterialSlots.h"
#include "Editor/ScriptFieldInspection.h"
#include "Editor/Viewport/CanvasOverlay.h"

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
#include "Strada/Script/ScriptEngine.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		// Merge keys of this panel ("INSP" in the high bytes, a running count below).
		constexpr uint64_t MergeKeyPrefix = 0x494E535000000000ull;
		constexpr char const* AddComponentPopup = "##addComponent";

		// Texture previews fill the panel's width up to this height (in font sizes, so it follows the display scale).
		constexpr float MaxPreviewHeightInFontSizes = 16.0f;
		// Material files shown in the inspector get a square preview of this size, at most the panel's width.
		constexpr float MaterialPreviewSizeInFontSizes = 8.0f;
		constexpr ImVec4 ErrorColor = ImVec4(0.95f, 0.40f, 0.35f, 1.0f);
		// Headers of the material sections inside Mesh components: darker than component headers.
		constexpr ImVec4 MaterialHeaderColor = ImVec4(0.180f, 0.196f, 0.227f, 1.0f);
		// The Transform header of UI elements, and what it says about them.
		constexpr char const* PlacedByRectHeaderId = "###TransformPlacedByRect";
		constexpr char const* PlacedByRectNote = "The UI Rect places this UI element on the screen: its Transform is not used.";

		void TextError(std::string const& message)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ErrorColor);
			ImGui::TextWrapped("%s", message.c_str());
			ImGui::PopStyleColor();
		}

		// The component's description as the tooltip of the last item.
		void ShowDescription(ComponentInfo const& component)
		{
			if (!component.Description.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
			{
				ImGui::SetTooltip("%.*s", static_cast<int>(component.Description.size()), component.Description.data());
			}
		}

		// Why a material cannot be edited where it is.
		std::string DescribeReadOnlyMaterial(AssetMetadata const& material)
		{
			if (material.IsBuiltIn())
			{
				return "Built into the engine and read-only: create a material to change it.";
			}
			if (material.IsSubAsset())
			{
				std::optional<AssetMetadata> const parent = AssetManager::GetMetadata(material.Parent);
				return fmt::format("Imported with {} and read-only: create a material to change it.",
				                   parent ? parent->GetDisplayName() : material.Parent.ToString());
			}
			return "This material has no file of its own and is read-only.";
		}

		// A full-width "Create Material" button, disabled without a project to write the material to; true when clicked.
		bool DrawCreateMaterialButton(char const* tooltip)
		{
			bool const available = AssetManager::HasAssetDirectory();
			ImGui::BeginDisabled(!available);
			bool const clicked = ImGui::Button("Create Material", ImVec2(-FLT_MIN, 0.0f));
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
			{
				ImGui::SetTooltip("%s", available ? tooltip : "Open a project to create materials.");
			}
			ImGui::Spacing();
			return clicked;
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

	void InspectorPanel::SetThumbnails(AssetThumbnails* thumbnails)
	{
		m_Thumbnails = thumbnails;
		m_FieldEditor.SetAssetPreviews(thumbnails);
	}

	void InspectorPanel::ShowAddComponentMenu(bool open, std::string search)
	{
		m_AddComponentMenuRequest = open;
		if (open)
		{
			m_AddComponentFilter = std::move(search);
		}
	}

	void InspectorPanel::OnImGuiRender(EditorOperations& operations, bool& open)
	{
		// A request to show the Add Component menu applies to this frame only (nothing opens without selected entities).
		std::optional<bool> const menuRequest = std::exchange(m_AddComponentMenuRequest, std::nullopt);
		if (menuRequest.value_or(false))
		{
			ImGui::SetNextWindowFocus();
		}
		if (ImGui::Begin("Inspector", &open))
		{
			DrawContents(operations, menuRequest);
		}
		ImGui::End();
	}

	void InspectorPanel::DrawContents(EditorOperations& operations, std::optional<bool> menuRequest)
	{
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
		DrawAddComponent(operations, entities, menuRequest);

		// C# scripts dragged from the content browser onto the inspector go to the inspected entities.
		if (ImGui::BeginDragDropTargetCustom(ImGui::GetCurrentWindow()->Rect(), ImGui::GetID("##scriptDrop")))
		{
			if (std::optional<std::filesystem::path> const script = UI::AcceptScriptDrop())
			{
				m_Deferred.push_back(
					[&operations, entities, file = *script]
					{
						UI::ReportFailure(AssetDrops::AddScript(operations, entities, file), "Adding the script");
					});
			}
			ImGui::EndDragDropTarget();
		}

		std::vector<std::function<void()>> deferred;
		deferred.swap(m_Deferred);
		for (std::function<void()> const& action : deferred)
		{
			action();
		}
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

		// The UIRect of UI elements places them: their Transform is not used, so it starts folded and cannot be edited.
		bool const placedByRect =
			component.Name == ComponentTraits<TransformComponent>::Name && std::all_of(entities.begin(), entities.end(),
		                                                                               [&scene](UUID id)
		                                                                               {
																						   return CanvasOverlay::IsUIElement(scene, id);
																					   });

		ImGui::PushID(component.Name.data(), component.Name.data() + component.Name.size());
		float const rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
		std::string const name = UI::FormatDisplayName(component.Name);
		// A header of its own for UI elements, so that it starts folded whatever other entities' Transform headers are.
		std::string const label = placedByRect ? name + PlacedByRectHeaderId : name;
		ImGuiTreeNodeFlags const flags =
			placedByRect ? ImGuiTreeNodeFlags_AllowOverlap : ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap;
		bool const open = ImGui::CollapsingHeader(label.c_str(), flags);
		if (placedByRect)
		{
			ImGui::SetItemTooltip("%s", PlacedByRectNote);
		}
		else if (!component.Description.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
		{
			ImGui::SetTooltip("%.*s", static_cast<int>(component.Description.size()), component.Description.data());
		}
		ImGui::OpenPopupOnItemClick("##componentMenu", ImGuiPopupFlags_MouseButtonRight);
		if (placedByRect)
		{
			// A note follows the header's name on its line.
			float const noteX = ImGui::GetItemRectMin().x - ImGui::GetWindowPos().x + ImGui::GetTreeNodeToLabelSpacing() +
			                    ImGui::CalcTextSize(name.c_str()).x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
			ImGui::SameLine(noteX);
			ImGui::TextDisabled("placed by its UI Rect");
		}
		float const buttonWidth = ImGui::GetFrameHeight() * 1.5f;
		ImGui::SameLine(rightEdge - buttonWidth);
		if (ImGui::Button("...", ImVec2(buttonWidth, 0.0f)))
		{
			ImGui::OpenPopup("##componentMenu");
		}
		DrawComponentMenu(operations, component, entities, values.front());

		if (open && component.Name == ComponentTraits<ScriptComponent>::Name)
		{
			DrawScriptComponent(operations, component, entities, values);
			ImGui::Spacing();
		}
		else if (open && placedByRect)
		{
			ImGui::TextDisabled("%s", PlacedByRectNote);
			// Shown read-only: disabled fields report no changes.
			ImGui::BeginDisabled();
			m_FieldEditor.Draw(component.Fields, values.front(), ComponentInspection::FindMixedFields(values), &scene);
			ImGui::EndDisabled();
			ImGui::Spacing();
		}
		else if (open)
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
			// The materials the mesh renders with, right below the mesh.
			if (component.Name == ComponentTraits<MeshComponent>::Name)
			{
				DrawMeshMaterials(operations, entities);
			}
			ImGui::Spacing();
		}
		ImGui::PopID();
	}

	void InspectorPanel::DrawScriptComponent(EditorOperations& operations, ComponentInfo const& component,
	                                         std::vector<UUID> const& entities, std::vector<Json> const& values)
	{
		auto const getClassName = [](Json const& value)
		{
			auto const name = value.find("ClassName");
			return name != value.end() && name->is_string() ? name->get<std::string>() : std::string();
		};
		std::string const className = getClassName(values.front());
		bool const mixedClasses = std::any_of(values.begin() + 1, values.end(),
		                                      [&](Json const& value)
		                                      {
												  return getClassName(value) != className;
											  });

		// The fields of the class, when every selected entity runs the same class and the game's assembly has it.
		ScriptClassInfo const* const scriptClass = mixedClasses || className.empty() ? nullptr : ScriptEngine::FindClass(className);
		std::vector<FieldDescriptor> const fields =
			scriptClass != nullptr ? ScriptFieldInspection::DescribeFields(*scriptClass) : std::vector<FieldDescriptor>();

		// The class picker, laid out like the field rows below.
		std::optional<std::string> pickedClass;
		float const nameShare = FieldEditor::GetNameColumnShare(fields, ImGui::GetContentRegionAvail().x);
		if (ImGui::BeginTable("##scriptClass", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, nameShare);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f - nameShare);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted("Class");
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
			{
				ImGui::SetTooltip("The C# script class the entity runs.");
			}
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-FLT_MIN);
			std::string const preview = mixedClasses ? std::string("--") : className.empty() ? std::string("None") : className;
			if (ImGui::BeginCombo("##class", preview.c_str()))
			{
				if (ImGui::Selectable("None", !mixedClasses && className.empty()))
				{
					pickedClass = std::string();
				}
				if (ScriptEngine::IsInitialized())
				{
					for (ScriptClassInfo const& candidate : ScriptEngine::GetClasses())
					{
						if (ImGui::Selectable(candidate.Name.c_str(), !mixedClasses && candidate.Name == className))
						{
							pickedClass = candidate.Name;
						}
					}
				}
				ImGui::EndCombo();
			}
			ImGui::EndTable();
		}
		if (pickedClass)
		{
			ApplyPatch(operations, component, entities, Json::object({{"ClassName", *pickedClass}}), 0);
			return;
		}

		if (mixedClasses)
		{
			ImGui::TextDisabled("The selected entities run different scripts.");
			return;
		}
		if (className.empty())
		{
			return;
		}
		if (scriptClass == nullptr)
		{
			if (!ScriptEngine::IsInitialized())
			{
				TextError("Scripting is unavailable: the .NET 10 runtime was not found.");
			}
			else if (!ScriptEngine::HasGameAssembly())
			{
				ImGui::TextDisabled("The game's scripts are not built yet.");
			}
			else
			{
				TextError(fmt::format("There is no script class {}: its stored field values are kept.", className));
			}
			return;
		}

		if (fields.empty())
		{
			ImGui::TextDisabled("The script has no fields to edit.");
			return;
		}
		auto const getStoredFields = [](Json const& value)
		{
			auto const stored = value.find("Fields");
			return stored != value.end() ? *stored : Json::object();
		};
		std::vector<Json> editorValues;
		editorValues.reserve(values.size());
		for (Json const& value : values)
		{
			editorValues.push_back(ScriptFieldInspection::ToEditorValues(*scriptClass, getStoredFields(value)));
		}
		std::vector<std::string> const mixed = ComponentInspection::FindMixedFields(editorValues);
		std::optional<FieldChange> const change =
			m_FieldEditor.Draw(fields, editorValues.front(), mixed, &operations.GetContext().GetScene());
		if (!change || change->Path.empty())
		{
			return;
		}

		Result<std::vector<ComponentEdit>> edits = ScriptFieldInspection::MakeEdits(*scriptClass, entities, values, editorValues, *change);
		if (!edits)
		{
			UI::ReportFailure(edits, "Editing the script's fields");
			return;
		}
		UI::ReportFailure(operations.SetComponentFields(edits.TakeValue(), m_EditSession.GetMergeKey()), "Editing the script's fields");
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
		// The menu is drawn every frame and the clipboard may hold anything (megabytes of other text): it is only parsed when
		// the values are pasted.
		char const* clipboard = ImGui::GetClipboardText();
		std::string_view const clipboardText = clipboard != nullptr ? std::string_view(clipboard) : std::string_view();
		size_t const start = clipboardText.find_first_not_of(" \t\r\n");
		bool const holdsObject = start != std::string_view::npos && clipboardText[start] == '{';
		if (ImGui::MenuItem("Paste Values", nullptr, false, holdsObject))
		{
			// Text that starts with '{' and parses is an object.
			Result<Json> const pasted = ParseJson(clipboardText);
			if (pasted)
			{
				ApplyPatch(operations, component, entities, pasted.GetValue(), 0);
			}
			UI::ReportFailure(pasted, "Pasting the values");
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

	void InspectorPanel::DrawAddComponent(EditorOperations& operations, std::vector<UUID> const& entities, std::optional<bool> request)
	{
		float const width = ImGui::GetContentRegionAvail().x;
		if (ImGui::Button("Add Component", ImVec2(width, 0.0f)))
		{
			m_AddComponentFilter.clear();
			ImGui::OpenPopup(AddComponentPopup);
		}
		else if (request.value_or(false))
		{
			// The search text came with the request; the inspector scrolls down to the button, which the menu drops from.
			ImGui::SetScrollHereY(1.0f);
			ImGui::OpenPopup(AddComponentPopup);
		}

		// The menu drops down from the button, or up when there is more room above, as wide as the button.
		ImVec2 const buttonMin = ImGui::GetItemRectMin();
		ImVec2 const buttonMax = ImGui::GetItemRectMax();
		ImGuiViewport const* viewport = ImGui::GetWindowViewport();
		float const spaceBelow = viewport->WorkPos.y + viewport->WorkSize.y - buttonMax.y;
		float const spaceAbove = buttonMin.y - viewport->WorkPos.y;
		bool const below = spaceBelow >= spaceAbove;
		ImGui::SetNextWindowPos(below ? ImVec2(buttonMin.x, buttonMax.y) : buttonMin, ImGuiCond_Always,
		                        below ? ImVec2(0.0f, 0.0f) : ImVec2(0.0f, 1.0f));
		ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f),
		                                    ImVec2(width, std::max(below ? spaceBelow : spaceAbove, ImGui::GetFrameHeight() * 6.0f)));
		if (!ImGui::BeginPopup(AddComponentPopup))
		{
			return;
		}
		if (request == false)
		{
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}
		if (ImGui::IsWindowAppearing() || m_FocusComponentSearch)
		{
			ImGui::SetKeyboardFocusHere();
			m_FocusComponentSearch = false;
		}
		ImGui::SetNextItemWidth(-FLT_MIN);
		bool const confirmed =
			ImGui::InputTextWithHint("##search", "Search components", &m_AddComponentFilter, ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::Separator();

		Scene& scene = operations.GetContext().GetScene();
		std::vector<ComponentInfo const*> const addable = ComponentInspection::GetAddableComponents(scene, entities);
		ComponentInfo const* chosen = nullptr;
		if (addable.empty())
		{
			ImGui::TextDisabled("No components to add");
		}
		else if (m_AddComponentFilter.find_first_not_of(' ') == std::string::npos)
		{
			for (ComponentInspection::ComponentGroup const& group : ComponentInspection::GroupByCategory(addable))
			{
				if (!ImGui::BeginMenu(std::string(group.Category).c_str()))
				{
					continue;
				}
				for (ComponentInfo const* component : group.Components)
				{
					if (ImGui::MenuItem(UI::FormatDisplayName(component->Name).c_str()))
					{
						chosen = component;
					}
					ShowDescription(*component);
				}
				ImGui::EndMenu();
			}
		}
		else
		{
			std::vector<ComponentInfo const*> const matches = ComponentInspection::SearchComponents(addable, m_AddComponentFilter);
			if (matches.empty())
			{
				ImGui::TextDisabled("No components match");
			}
			for (size_t i = 0; i < matches.size(); i++)
			{
				ComponentInfo const& component = *matches[i];
				ImGui::PushID(component.Name.data(), component.Name.data() + component.Name.size());
				// Selectables grow by half the item spacing around their label: the category goes on the label's line.
				float const labelY = ImGui::GetCursorScreenPos().y;
				// The first match is highlighted: Enter adds it.
				if (ImGui::Selectable(UI::FormatDisplayName(component.Name).c_str(), i == 0))
				{
					chosen = &component;
				}
				ShowDescription(component);
				// The category, right-aligned and dimmed.
				std::string_view const category = component.Category;
				float const categoryWidth = ImGui::CalcTextSize(category.data(), category.data() + category.size()).x;
				ImGui::GetWindowDrawList()->AddText(
					ImVec2(ImGui::GetItemRectMax().x - categoryWidth - ImGui::GetStyle().ItemInnerSpacing.x, labelY),
					ImGui::GetColorU32(ImGuiCol_TextDisabled), category.data(), category.data() + category.size());
				ImGui::PopID();
			}
			if (confirmed && !matches.empty())
			{
				chosen = matches.front();
			}
		}

		if (chosen != nullptr)
		{
			std::vector<UUID> const targets = ComponentInspection::GetEntitiesWithout(scene, entities, *chosen);
			UI::ReportFailure(operations.AddComponent(std::span<UUID const>(targets), chosen->Name), "Adding the component");
			ImGui::CloseCurrentPopup();
		}
		else if (confirmed)
		{
			// Enter without a match ends the text input: keep typing in the search field.
			m_FocusComponentSearch = true;
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
		// The reference, shortened to the panel's width (all of it in the tooltip and the menu's Copy Reference).
		std::string const reference = AssetManager::GetReference(asset);
		std::string const shownReference = UI::FitPath(reference, ImGui::GetContentRegionAvail().x);
		ImGui::TextDisabled("%s", shownReference.c_str());
		if (shownReference != reference && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
		{
			ImGui::SetTooltip("%s", reference.c_str());
		}
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
		// The material on a sphere, as the content browser shows it; edits show as soon as they render.
		if (nvrhi::ITexture* preview = m_Thumbnails != nullptr ? m_Thumbnails->Get(metadata.Handle) : nullptr)
		{
			float const size = std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * MaterialPreviewSizeInFontSizes);
			ImGui::Image(ImGuiRenderer::GetTextureID(preview), ImVec2(size, size));
			ImGui::Spacing();
		}
		// Only material files keep changes: built-in and imported materials are recreated from their source.
		if (!MaterialSlots::IsEditable(metadata.Handle) &&
		    DrawCreateMaterialButton("Writes a copy of this material to Assets/Materials as a material file you can edit."))
		{
			Result<AssetHandle> const copy =
				MaterialSlots::CreateCopy(operations, metadata.Handle, MaterialSlots::GetMaterialName(metadata.Handle));
			if (copy)
			{
				operations.GetContext().SelectAsset(copy.GetValue());
			}
			UI::ReportFailure(copy, "Creating the material");
		}
		DrawMaterialFields(operations, metadata.Handle);
	}

	void InspectorPanel::DrawMaterialFields(EditorOperations& operations, AssetHandle material)
	{
		Ref<MaterialAsset> const loaded = LoadForInspection<MaterialAsset>(material);
		if (!loaded)
		{
			return;
		}
		bool const editable = MaterialSlots::IsEditable(material);
		Json const values = SerializeFields<StructTraits<MaterialData>>(loaded->GetData());
		ImGui::BeginDisabled(!editable);
		std::optional<FieldChange> const change = m_FieldEditor.Draw(m_MaterialFields, values);
		ImGui::EndDisabled();
		if (change && editable)
		{
			UI::ReportFailure(operations.SetMaterialFields(material, change->MakePatch(values), m_EditSession.GetMergeKey()),
			                  "Editing the material");
		}
	}

	void InspectorPanel::DrawMeshMaterials(EditorOperations& operations, std::vector<UUID> const& entities)
	{
		std::vector<AssetHandle> const materials = MaterialSlots::GetMaterials(operations.GetContext().GetScene(), entities);
		ImGui::PushID("##materials");
		for (size_t slot = 0; slot < materials.size(); slot++)
		{
			ImGui::PushID(static_cast<int>(slot));
			bool const open = DrawMaterialHeader(materials[slot], slot, materials.size());
			// Materials dropped onto the header go to the slot of every entity, as one undo step.
			if (ImGui::BeginDragDropTarget())
			{
				constexpr AssetType MaterialType = AssetType::Material;
				if (AssetHandle const dropped = UI::AcceptAssetDrop(std::span<AssetType const>(&MaterialType, 1)); dropped.IsValid())
				{
					UI::ReportFailure(MaterialSlots::Assign(operations, entities, slot, dropped), "Assigning the material");
				}
				ImGui::EndDragDropTarget();
			}
			if (open)
			{
				DrawSlotMaterial(operations, entities, slot, materials[slot]);
			}
			ImGui::Spacing();
			ImGui::PopID();
		}
		ImGui::PopID();
	}

	bool InspectorPanel::DrawMaterialHeader(AssetHandle material, size_t slot, size_t slotCount)
	{
		// A preview of the material on a sphere beside two lines: the material's name, then its slot (meshes with several) and
		// its path.
		ImGuiStyle const& style = ImGui::GetStyle();
		ImVec2 const padding = style.FramePadding;
		float const spacing = style.ItemInnerSpacing.x;
		float const fontSize = ImGui::GetFontSize();
		float const textHeight = fontSize * 2.0f + style.ItemInnerSpacing.y;
		float const height = textHeight + padding.y * 2.0f;
		ImGui::PushStyleColor(ImGuiCol_Header, MaterialHeaderColor);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padding.x, (height - fontSize) * 0.5f));
		bool const open = ImGui::TreeNodeEx("##header",
		                                    ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth |
		                                        ImGuiTreeNodeFlags_NoTreePushOnOpen,
		                                    "%s", "");
		ImGui::PopStyleVar();
		ImGui::PopStyleColor();
		std::optional<AssetMetadata> const metadata = material.IsValid() ? AssetManager::GetMetadata(material) : std::nullopt;
		bool const registered = metadata.has_value();
		// The whole path, which the header may shorten, and what dropping a material does.
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
		{
			std::string const reference = registered ? UI::DescribeAssetReference(material) + "\n" : std::string();
			ImGui::SetTooltip("%sDrop a material here to use it in this slot.", reference.c_str());
		}

		ImVec2 const min = ImGui::GetItemRectMin();
		ImVec2 const max = ImGui::GetItemRectMax();
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		// After the tree node's arrow.
		ImVec2 const previewMin(min.x + padding.x * 2.0f + fontSize, min.y + padding.y);
		ImVec2 const previewMax(previewMin.x + textHeight, max.y - padding.y);
		float const rounding = style.FrameRounding;
		if (nvrhi::ITexture* preview = registered && m_Thumbnails != nullptr ? m_Thumbnails->Get(material) : nullptr)
		{
			drawList->AddImageRounded(ImGuiRenderer::GetTextureID(preview), previewMin, previewMax, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
			                          IM_COL32_WHITE, rounding);
		}
		else
		{
			drawList->AddRectFilled(previewMin, previewMax, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
		}

		float const textX = previewMax.x + spacing * 2.0f;
		float const textY = min.y + padding.y;
		float const textMaxX = max.x - padding.x;
		drawList->PushClipRect(ImVec2(textX, min.y), ImVec2(textMaxX, max.y), true);
		// The name, then a badge for materials that cannot be edited here, right-aligned while the name keeps some room.
		float titleMaxX = textMaxX;
		if (registered && metadata->Type == AssetType::Material && !AssetManager::IsMissing(material) &&
		    !MaterialSlots::IsEditable(material))
		{
			constexpr std::string_view Badge = "Read-only";
			float const badgeWidth = ImGui::CalcTextSize(Badge.data(), Badge.data() + Badge.size()).x;
			if (textMaxX - textX >= badgeWidth + spacing * 2.0f + fontSize * 3.0f)
			{
				drawList->AddText(ImVec2(textMaxX - badgeWidth, textY), ImGui::GetColorU32(ImGuiCol_TextDisabled), Badge.data(),
				                  Badge.data() + Badge.size());
				titleMaxX -= badgeWidth + spacing * 2.0f;
			}
		}
		std::string title = material.IsValid() ? MaterialSlots::GetMaterialName(material) : std::string("Different materials");
		if (title.empty())
		{
			title = UI::DescribeAssetReference(material);
		}
		ImGui::RenderTextEllipsis(drawList, ImVec2(textX, textY), ImVec2(titleMaxX, textY + fontSize), titleMaxX, title.c_str(), nullptr,
		                          nullptr);
		// The slot (meshes with several), a middle dot and where the material comes from: its path, or the model's for materials
		// imported with one. Long paths lose their folders, then the middle of the name.
		std::string subtitle = slotCount > 1 || !material.IsValid() ? fmt::format("Slot {}", slot) : std::string();
		if (material.IsValid())
		{
			AssetHandle const source = registered && metadata->IsSubAsset() ? metadata->Parent : material;
			subtitle += subtitle.empty() ? "" : " \xC2\xB7 ";
			subtitle += UI::FitPath(UI::DescribeAssetReference(source), textMaxX - textX - ImGui::CalcTextSize(subtitle.c_str()).x);
		}
		drawList->AddText(ImVec2(textX, textY + fontSize + style.ItemInnerSpacing.y), ImGui::GetColorU32(ImGuiCol_TextDisabled),
		                  subtitle.c_str());
		drawList->PopClipRect();
		return open;
	}

	void InspectorPanel::DrawSlotMaterialReference(EditorOperations& operations, std::vector<UUID> const& entities, size_t slot,
	                                               AssetHandle material)
	{
		// A row lined up with the material's fields below it.
		float const nameShare = FieldEditor::GetNameColumnShare(m_MaterialFields, ImGui::GetContentRegionAvail().x);
		if (!ImGui::BeginTable("##slotMaterial", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings))
		{
			return;
		}
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, nameShare);
		ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f - nameShare);
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Material");
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
		{
			ImGui::SetTooltip("The material this slot renders with: the component's own choice, else the mesh's material.");
		}
		ImGui::TableNextColumn();
		ImGui::SetNextItemWidth(-FLT_MIN);
		AssetReferenceOptions options;
		options.Type = AssetType::Material;
		options.Mixed = !material.IsValid();
		// A slot always renders a material: "x" goes back to the mesh's own one.
		options.AllowNone = false;
		options.CanClear = MaterialSlots::IsOverridden(operations.GetContext().GetScene(), entities, slot);
		options.ClearTooltip = "Use the mesh's own material";
		std::optional<AssetHandle> const chosen = m_FieldEditor.DrawAssetReference(material, options);
		ImGui::EndTable();
		if (chosen)
		{
			// Changes the Mesh component's overrides only: the entities and their components stay as they are drawn.
			UI::ReportFailure(chosen->IsValid() ? MaterialSlots::Assign(operations, entities, slot, *chosen)
			                                    : MaterialSlots::Clear(operations, entities, slot),
			                  "Changing the slot's material");
		}
	}

	void InspectorPanel::DrawSlotMaterial(EditorOperations& operations, std::vector<UUID> const& entities, size_t slot,
	                                      AssetHandle material)
	{
		DrawSlotMaterialReference(operations, entities, slot, material);
		if (!material.IsValid())
		{
			ImGui::TextDisabled("The selected entities use different materials here.");
			return;
		}
		std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(material);
		if (!metadata)
		{
			TextError(fmt::format("Material {} is not registered.", material));
			return;
		}
		if (metadata->Type != AssetType::Material)
		{
			TextError(fmt::format("{} is not a material.", UI::DescribeAssetReference(material)));
			return;
		}
		if (AssetManager::IsMissing(material))
		{
			TextError("The material file is missing. Restore it and refresh the assets, or choose another material.");
			return;
		}
		if (!MaterialSlots::IsEditable(material))
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextWrapped("%s", DescribeReadOnlyMaterial(*metadata).c_str());
			ImGui::PopStyleColor();
			if (DrawCreateMaterialButton("Writes a copy of this material to Assets/Materials as a material file you can edit, and uses "
			                             "it here."))
			{
				UI::ReportFailure(MaterialSlots::CreateEditableCopy(operations, entities, slot), "Creating the material");
			}
		}
		DrawMaterialFields(operations, material);
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
