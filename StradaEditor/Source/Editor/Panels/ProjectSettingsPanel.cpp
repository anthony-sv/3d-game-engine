#include "Editor/Panels/ProjectSettingsPanel.h"

#include "Editor/UI/EditorUI.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Physics/PhysicsTypes.h"

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

		// Whether the change edits the list of physics layers as a list (adds or removes a layer).
		bool IsLayerListEdit(FieldChange const& change)
		{
			return change.ArrayEdit && change.Path.size() == 2 && change.Path[0] == FieldPathStep{"Physics", std::nullopt} &&
			       change.Path[1] == FieldPathStep{"Layers", std::nullopt};
		}

		// A name for a layer added to the list: layer names are unique and not empty.
		std::string MakeLayerName(std::vector<std::string> const& layers)
		{
			for (size_t number = layers.size();; number++)
			{
				std::string name = fmt::format("Layer {}", number);
				if (std::find(layers.begin(), layers.end(), name) == layers.end())
				{
					return name;
				}
			}
		}

		// The ignored pairs after a list edit of the layers: pairs naming a removed layer are dropped, and the others name
		// their layers where the edit put them.
		std::vector<PhysicsLayerPair> RenumberPairs(std::vector<PhysicsLayerPair> pairs, FieldArrayEdit const& edit, size_t layerCount)
		{
			if (edit.Kind == FieldArrayEditKind::Remove)
			{
				std::erase_if(pairs,
				              [&edit](PhysicsLayerPair const& pair)
				              {
								  return pair.First == edit.Index || pair.Second == edit.Index;
							  });
			}
			auto const renumber = [&edit, layerCount](uint32_t layer) -> uint32_t
			{
				switch (edit.Kind)
				{
					case FieldArrayEditKind::Insert:
						// An insertion past the end appends, after every layer.
						return edit.Index < layerCount && layer >= edit.Index ? layer + 1 : layer;
					case FieldArrayEditKind::Duplicate:
						return layer > edit.Index ? layer + 1 : layer;
					case FieldArrayEditKind::Remove:
						return layer > edit.Index ? layer - 1 : layer;
					case FieldArrayEditKind::Move:
						if (layer == edit.Index)
						{
							return edit.Target;
						}
						if (edit.Index < edit.Target && layer > edit.Index && layer <= edit.Target)
						{
							return layer - 1;
						}
						if (edit.Target < edit.Index && layer >= edit.Target && layer < edit.Index)
						{
							return layer + 1;
						}
						return layer;
				}
				return layer;
			};
			for (PhysicsLayerPair& pair : pairs)
			{
				pair.First = renumber(pair.First);
				pair.Second = renumber(pair.Second);
			}
			return pairs;
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
				// The panel adds no layer past the most a project takes (ValidateProjectSettings refuses more).
				for (FieldDescriptor& nested : field.Fields)
				{
					if (nested.Name == "Layers")
					{
						nested.Hints.MaxCount = MaxPhysicsLayers;
					}
				}
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
			ProjectPhysicsSettings const& physics = project->GetSettings().Physics;
			FieldChange edit = *change;
			bool const layerListEdit = IsLayerListEdit(edit);
			if (layerListEdit && edit.ArrayEdit->Kind == FieldArrayEditKind::Insert && edit.Value == "")
			{
				edit.Value = MakeLayerName(physics.Layers);
			}
			Json patch = edit.MakePatch(values);
			// The ignored collisions name layers by index: they follow the layers a list edit moves, and go with a removed one.
			if (layerListEdit && patch.contains("Physics"))
			{
				patch["Physics"]["IgnoredCollisions"] =
					SerializePairs(RenumberPairs(physics.IgnoredCollisions, *edit.ArrayEdit, physics.Layers.size()));
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
