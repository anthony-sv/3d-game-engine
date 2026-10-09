#include "Editor/Panels/ProjectSettingsPanel.h"

#include "Editor/UI/EditorUI.h"

#include "Strada/Core/FileSystem.h"

#include <imgui.h>

#include <algorithm>
#include <optional>
#include <string>

namespace Strada
{
	namespace
	{
		bool IsIgnored(ProjectPhysicsSettings const& physics, uint32_t first, uint32_t second)
		{
			return std::any_of(physics.IgnoredCollisions.begin(), physics.IgnoredCollisions.end(),
			                   [first, second](PhysicsLayerPair const& pair)
			                   {
								   return (pair.First == first && pair.Second == second) || (pair.First == second && pair.Second == first);
							   });
		}

		Json SerializePairs(std::vector<PhysicsLayerPair> const& pairs)
		{
			return JsonTraits<std::vector<PhysicsLayerPair>>::ToJson(pairs);
		}
	}

	ProjectSettingsPanel::ProjectSettingsPanel()
	{
		for (FieldDescriptor field : GetFieldDescriptors<StructTraits<ProjectSettings>, ProjectSettings>())
		{
			if (field.Name == "AssetDirectory")
			{
				continue;
			}
			if (field.Name == "Physics")
			{
				std::erase_if(field.Fields,
				              [](FieldDescriptor const& nested)
				              {
								  return nested.Name == "IgnoredCollisions";
							  });
			}
			m_Fields.push_back(std::move(field));
		}
	}

	void ProjectSettingsPanel::OnImGuiRender(EditorOperations& operations, bool& open)
	{
		if (!ImGui::Begin("Project Settings", &open))
		{
			ImGui::End();
			return;
		}

		Project const* project = operations.GetContext().GetProject();
		if (project == nullptr)
		{
			ImGui::TextDisabled("No project is open.");
			ImGui::TextWrapped("Create or open a project from the File menu.");
			ImGui::End();
			return;
		}
		if (m_SavePending && !ImGui::IsAnyItemActive())
		{
			Flush(operations);
		}

		ImGui::TextDisabled("Project file: %s", FileSystem::PathToUtf8(project->GetFilePath()).c_str());
		ImGui::TextDisabled("Asset directory: %s", project->GetSettings().AssetDirectory.c_str());
		ImGui::Spacing();

		Json const values = SerializeFields<StructTraits<ProjectSettings>>(project->GetSettings());
		if (std::optional<FieldChange> const change = m_FieldEditor.Draw(m_Fields, values))
		{
			Json patch = change->MakePatch(values);
			// Removing layers drops the ignored collisions that named them.
			if (patch.contains("Physics") && patch["Physics"].contains("Layers"))
			{
				size_t const layerCount = patch["Physics"]["Layers"].size();
				std::vector<PhysicsLayerPair> pairs = project->GetSettings().Physics.IgnoredCollisions;
				std::erase_if(pairs,
				              [layerCount](PhysicsLayerPair const& pair)
				              {
								  return pair.First >= layerCount || pair.Second >= layerCount;
							  });
				patch["Physics"]["IgnoredCollisions"] = SerializePairs(pairs);
			}
			Apply(operations, std::move(patch));
		}

		ImGui::Spacing();
		ImGui::SeparatorText("Layer Collisions");
		DrawCollisionMatrix(operations, project->GetSettings());
		ImGui::End();
	}

	void ProjectSettingsPanel::Flush(EditorOperations& operations)
	{
		if (m_SavePending)
		{
			UI::ReportFailure(operations.SaveProject(), "Saving the project settings");
			m_SavePending = false;
		}
	}

	void ProjectSettingsPanel::DrawCollisionMatrix(EditorOperations& operations, ProjectSettings const& settings)
	{
		ImGui::TextDisabled("Checked layer pairs collide.");
		std::vector<std::string> const& layers = settings.Physics.Layers;
		int const count = static_cast<int>(layers.size());
		ImGuiTableFlags const flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV |
		                              ImGuiTableFlags_HighlightHoveredColumn | ImGuiTableFlags_NoSavedSettings;
		if (!ImGui::BeginTable("##collisions", count + 1, flags))
		{
			return;
		}
		ImGui::TableSetupColumn("##layer", ImGuiTableColumnFlags_NoHide);
		for (std::string const& layer : layers)
		{
			ImGui::TableSetupColumn(layer.c_str(), ImGuiTableColumnFlags_AngledHeader | ImGuiTableColumnFlags_WidthFixed);
		}
		ImGui::TableAngledHeadersRow();

		for (int row = 0; row < count; row++)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(layers[static_cast<size_t>(row)].c_str());
			// The matrix is symmetric: only the upper triangle is shown.
			for (int column = row; column < count; column++)
			{
				ImGui::TableSetColumnIndex(column + 1);
				ImGui::PushID(row * count + column);
				uint32_t const first = static_cast<uint32_t>(row);
				uint32_t const second = static_cast<uint32_t>(column);
				bool collides = !IsIgnored(settings.Physics, first, second);
				if (ImGui::Checkbox("##collides", &collides))
				{
					std::vector<PhysicsLayerPair> pairs = settings.Physics.IgnoredCollisions;
					std::erase_if(pairs,
					              [first, second](PhysicsLayerPair const& pair)
					              {
									  return (pair.First == first && pair.Second == second) ||
						                     (pair.First == second && pair.Second == first);
								  });
					if (!collides)
					{
						pairs.push_back({first, second});
					}
					Apply(operations, Json::object({{"Physics", Json::object({{"IgnoredCollisions", SerializePairs(pairs)}})}}));
				}
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
				{
					ImGui::SetTooltip("%s / %s", layers[first].c_str(), layers[second].c_str());
				}
				ImGui::PopID();
			}
		}
		ImGui::EndTable();
	}

	void ProjectSettingsPanel::Apply(EditorOperations& operations, Json patch)
	{
		Result<void> const applied = operations.ApplyProjectSettings(patch);
		UI::ReportFailure(applied, "Changing the project settings");
		m_SavePending = m_SavePending || applied.IsOk();
	}
}
