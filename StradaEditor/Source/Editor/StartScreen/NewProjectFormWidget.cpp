#include "Editor/StartScreen/NewProjectFormWidget.h"

#include "Editor/StartScreen/StartScreenCards.h"
#include "Editor/StartScreen/StartScreenThumbnails.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"

#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace Strada
{
	namespace
	{
		// How often the checks that look at the disk run again while the form stays the same (the target may appear).
		constexpr double CheckSeconds = 1.0;
		// Lines of a template card's description: as many as the longest takes, within these (a longer one is cut, and the card's
		// tooltip shows it).
		constexpr float MinimumDescriptionLines = 2.0f;
		constexpr float MaximumDescriptionLines = 6.0f;

		std::string GetLocationStart(NewProjectForm const& form, EditorLocations const& locations)
		{
			return form.Location.empty() ? FileSystem::PathToUtf8(locations.DefaultProjectLocation) : form.Location;
		}
	}

	void RecordStartScreenItem(std::vector<StartScreenItem>* items, std::string name, bool enabled)
	{
		RecordStartScreenItem(items, std::move(name), enabled, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
	}

	void RecordStartScreenItem(std::vector<StartScreenItem>* items, std::string name, bool enabled, ImVec2 itemMin, ImVec2 itemMax)
	{
		if (items == nullptr)
		{
			return;
		}
		ImGuiWindow const* const window = ImGui::GetCurrentWindowRead();
		ImRect const rect(itemMin, itemMax);
		ImGuiViewport const* const viewport = ImGui::GetMainViewport();
		ImRect const screen(viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y));
		StartScreenItem item;
		item.Name = std::move(name);
		item.Min = rect.Min;
		item.Max = rect.Max;
		item.Visible = window != nullptr && window->ClipRect.Contains(rect) && screen.Contains(rect);
		item.Enabled = enabled;
		items->push_back(std::move(item));
	}

	void NewProjectFormWidget::Invalidate()
	{
		m_Check.reset();
		m_TemplateThumbnails.clear();
	}

	NewProjectCheck const& NewProjectFormWidget::GetCheck(StartScreenModel const& model)
	{
		double const now = ImGui::GetTime();
		if (!m_Check || !(m_CheckedForm == model.GetForm()) || now - m_CheckedAt > CheckSeconds)
		{
			m_Check = model.CheckForm();
			m_CheckedForm = model.GetForm();
			m_CheckedAt = now;
		}
		return *m_Check;
	}

	std::vector<std::string> const& NewProjectFormWidget::GetAgentFiles(StartScreenModel const& model)
	{
		if (m_AgentFilesGitIgnore != model.GetForm().GitIgnore)
		{
			m_AgentFiles = model.ListAgentFiles();
			m_AgentFilesGitIgnore = model.GetForm().GitIgnore;
		}
		return m_AgentFiles;
	}

	void NewProjectFormWidget::DrawTemplateCards(StartScreenModel& model, StartScreenThumbnails* thumbnails,
	                                             std::vector<StartScreenItem>* items)
	{
		std::vector<ProjectTemplate> const& templates = model.GetTemplates().Templates;
		if (templates.empty())
		{
			return;
		}
		ImGuiStyle const& style = ImGui::GetStyle();
		float const available = ImGui::GetContentRegionAvail().x;
		float const spacing = style.ItemSpacing.x;
		float const minimumWidth = ImGui::GetFontSize() * 9.0f;
		size_t const fitting = static_cast<size_t>(std::max(1.0f, std::floor((available + spacing) / (minimumWidth + spacing))));
		size_t const perRow = std::min(fitting, templates.size());
		float const cardWidth = std::floor((available - spacing * static_cast<float>(perRow - 1)) / static_cast<float>(perRow));

		std::vector<StartScreenCard> cards;
		cards.reserve(templates.size());
		for (ProjectTemplate const& projectTemplate : templates)
		{
			StartScreenCard card;
			card.Name = projectTemplate.Name;
			card.Description = projectTemplate.Description;
			card.RequiresSdk = projectTemplate.RequiresSdk;
			if (thumbnails != nullptr && !projectTemplate.Thumbnail.empty())
			{
				auto [found, added] = m_TemplateThumbnails.try_emplace(projectTemplate.Id);
				if (added)
				{
					found->second = projectTemplate.GetThumbnailPath();
				}
				card.Thumbnail = thumbnails->Get(found->second);
			}
			cards.push_back(card);
		}
		// The cards are as tall as the longest text needs, so none is cut and the row stays even.
		StartScreenCardLayout const layout = StartScreenCardLayout::Vertical(cardWidth, MinimumDescriptionLines)
		                                         .FitText(cards, MinimumDescriptionLines, MaximumDescriptionLines);

		NewProjectForm& form = model.GetForm();
		for (size_t index = 0; index < templates.size(); index++)
		{
			ProjectTemplate const& projectTemplate = templates[index];
			if (index % perRow != 0)
			{
				ImGui::SameLine(0.0f, spacing);
			}
			ImGui::PushID(projectTemplate.Id.c_str());
			bool const selected = form.TemplateId == projectTemplate.Id;
			StartScreenCardResult const drawn = DrawStartScreenCard("##card", cards[index], layout, selected);
			if (drawn.Clicked)
			{
				form.TemplateId = projectTemplate.Id;
			}
			// The card as drawn, and whether its text shows whole.
			RecordStartScreenItem(items, "Template:" + projectTemplate.Id, true, drawn.Min, drawn.Max);
			if (items != nullptr)
			{
				items->back().TextWhole = drawn.TextWhole;
			}
			ImGui::PopID();
		}
	}

	void NewProjectFormWidget::DrawFields(StartScreenModel& model, StartScreenThumbnails* thumbnails, NewProjectFormActions const& actions,
	                                      std::vector<StartScreenItem>* items)
	{
		ImGui::PushID("NewProjectForm");
		DrawTemplateCards(model, thumbnails, items);
		ImGui::Spacing();

		NewProjectForm& form = model.GetForm();
		ImGuiStyle const& style = ImGui::GetStyle();
		ImGui::TextUnformatted("Name");
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputText("##name", &form.Name);
		RecordStartScreenItem(items, "Name", true);

		ImGui::TextUnformatted("Location");
		float const browseWidth = ImGui::CalcTextSize("Browse...").x + style.FramePadding.x * 2.0f;
		ImGui::SetNextItemWidth(-(browseWidth + style.ItemSpacing.x));
		ImGui::InputTextWithHint("##location", "The folder the project's folder goes into", &form.Location);
		RecordStartScreenItem(items, "Location", true);
		ImGui::SameLine();
		bool const canBrowse = static_cast<bool>(actions.PickFolder);
		ImGui::BeginDisabled(!canBrowse);
		if (ImGui::Button("Browse...") && canBrowse)
		{
			m_BrowseError.clear();
			Result<std::optional<std::filesystem::path>> chosen =
				actions.PickFolder(FileSystem::PathFromUtf8(GetLocationStart(form, model.GetLocations())));
			if (!chosen)
			{
				m_BrowseError = chosen.GetError();
				ST_ERROR("Choosing the location: {}", chosen.GetError());
			}
			else if (chosen.GetValue())
			{
				form.Location = FileSystem::PathToUtf8(*chosen.GetValue());
			}
		}
		ImGui::EndDisabled();
		RecordStartScreenItem(items, "Browse...", canBrowse);
		if (!m_BrowseError.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, GetStartScreenWarningColor());
			ImGui::TextWrapped("%s", m_BrowseError.c_str());
			ImGui::PopStyleColor();
		}

		NewProjectCheck const& check = GetCheck(model);
		ImGui::PushTextWrapPos(0.0f);
		if (!check.Directory.empty())
		{
			ImGui::TextDisabled("Creates %s", FileSystem::PathToUtf8(check.Directory).c_str());
		}
		else
		{
			ImGui::TextDisabled("Creates a folder named after the project in the location.");
		}
		ImGui::PopTextWrapPos();
		ImGui::Spacing();

		ImGui::Checkbox("Set up for coding agents", &form.SetUpAgents);
		RecordStartScreenItem(items, "Set up for coding agents", true);
		ImGui::SetItemTooltip("Writes AGENTS.md, CLAUDE.md, .mcp.json, the game-making skill and the guides, so that coding agents "
		                      "such as Claude Code can work in the project through this editor");
		if (form.SetUpAgents)
		{
			ImGui::Indent();
			bool const listed = ImGui::TreeNodeEx("Files it writes", ImGuiTreeNodeFlags_SpanAvailWidth);
			RecordStartScreenItem(items, "Files it writes", true);
			if (listed)
			{
				for (std::string const& file : GetAgentFiles(model))
				{
					ImGui::BulletText("%s", file.c_str());
				}
				ImGui::TreePop();
			}
			ImGui::Unindent();
		}
		ImGui::Checkbox("Add a .gitignore", &form.GitIgnore);
		RecordStartScreenItem(items, "Add a .gitignore", true);
		ImGui::SetItemTooltip("Writes a .gitignore that keeps build output, caches and machine-specific files out of git");
		if (!check.Warning.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, GetStartScreenWarningColor());
			ImGui::TextWrapped("%s", check.Warning.c_str());
			ImGui::PopStyleColor();
		}
		ImGui::PopID();
	}

	float NewProjectFormWidget::GetCreateRowHeight()
	{
		return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
	}

	bool NewProjectFormWidget::DrawCreateButton(StartScreenModel& model, NewProjectFormActions const& actions,
	                                            std::vector<StartScreenItem>* items, std::function<void()> const& besideCreate)
	{
		ImGui::PushID("NewProjectForm");
		NewProjectCheck const check = GetCheck(model);
		bool const enabled = check.CanCreate() && static_cast<bool>(actions.Create);
		bool clicked = false;
		ImGui::BeginDisabled(!enabled);
		if (ImGui::Button("Create", ImVec2(ImGui::GetFontSize() * 7.0f, 0.0f)) && enabled)
		{
			NewProjectRequest request;
			request.Form = model.GetForm();
			request.Directory = check.Directory;
			actions.Create(request);
			// The new project's folder exists now (or the editor asks about unsaved changes first): look again.
			Invalidate();
			clicked = true;
		}
		ImGui::EndDisabled();
		RecordStartScreenItem(items, "Create", enabled);
		if (besideCreate)
		{
			ImGui::SameLine();
			besideCreate();
		}
		if (!check.Reason.empty())
		{
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Text, GetStartScreenWarningColor());
			// One line: the row's height is fixed. The whole reason is in the tooltip.
			float const width = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
			ImVec2 const position = ImGui::GetCursorScreenPos();
			float const textY = position.y + ImGui::GetStyle().FramePadding.y;
			ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), ImVec2(position.x, textY),
			                          ImVec2(position.x + width, textY + ImGui::GetTextLineHeight()), position.x + width,
			                          check.Reason.c_str(), nullptr, nullptr);
			ImGui::Dummy(ImVec2(width, ImGui::GetFrameHeight()));
			ImGui::PopStyleColor();
			ImGui::SetItemTooltip("%s", check.Reason.c_str());
		}
		ImGui::PopID();
		return clicked;
	}
}
