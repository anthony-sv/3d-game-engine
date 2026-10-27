#include "Editor/VisualScripting/VisualScriptDrawer.h"

#include "Editor/VisualScripting/VisualScriptTheme.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Log.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		// Messages are wrapped at this many font sizes.
		constexpr float MessageWrapInFontSizes = 36.0f;

		uint32_t GetSeverityColor(VisualScriptSeverity severity)
		{
			return severity == VisualScriptSeverity::Error ? VisualScriptErrorColor : VisualScriptWarningColor;
		}

		// The kind of element an ID names in the document.
		enum class ElementKind : uint8_t
		{
			None = 0,
			Variable,
			Local,
			Function,
			Node
		};

		ElementKind FindElementKind(VisualScriptDocument const& document, uint32_t id, uint32_t& owner)
		{
			owner = 0;
			for (VisualScriptVariable const& variable : document.Variables)
			{
				if (variable.ID == id)
				{
					return ElementKind::Variable;
				}
			}
			for (VisualScriptFunction const& function : document.Functions)
			{
				if (function.ID == id)
				{
					return ElementKind::Function;
				}
				for (VisualScriptLocal const& local : function.Locals)
				{
					if (local.ID == id)
					{
						owner = function.ID;
						return ElementKind::Local;
					}
				}
			}
			if (std::optional<uint32_t> const graph = FindVisualScriptElementGraph(document, id))
			{
				owner = *graph;
				return ElementKind::Node;
			}
			return ElementKind::None;
		}
	}

	std::vector<VisualScriptDiagnostic> GetVisualScriptNodeProblems(VisualScriptAnalysis const* analysis, uint32_t node)
	{
		std::vector<VisualScriptDiagnostic> problems;
		if (analysis == nullptr)
		{
			return problems;
		}
		for (VisualScriptDiagnostic const& diagnostic : analysis->Diagnostics)
		{
			if (diagnostic.Node == node)
			{
				problems.push_back(diagnostic);
			}
		}
		return problems;
	}

	std::vector<VisualScriptDiagnostic> GetVisualScriptVariableProblems(VisualScriptAnalysis const* analysis, uint32_t variable)
	{
		std::vector<VisualScriptDiagnostic> problems;
		if (analysis == nullptr)
		{
			return problems;
		}
		for (VisualScriptDiagnostic const& diagnostic : analysis->Diagnostics)
		{
			if (diagnostic.Variable == variable && !diagnostic.Node)
			{
				problems.push_back(diagnostic);
			}
		}
		return problems;
	}

	std::vector<VisualScriptDiagnostic> GetVisualScriptFunctionProblems(VisualScriptAnalysis const* analysis, uint32_t function)
	{
		std::vector<VisualScriptDiagnostic> problems;
		if (analysis == nullptr || function == 0)
		{
			return problems;
		}
		for (VisualScriptDiagnostic const& diagnostic : analysis->Diagnostics)
		{
			if (diagnostic.Function == function && !diagnostic.Node && !diagnostic.Variable && !diagnostic.Link)
			{
				problems.push_back(diagnostic);
			}
		}
		return problems;
	}

	void ApplyVisualScriptQuickFixChoice(VisualScriptPaneContext& context, VisualScriptDiagnostic const& diagnostic,
	                                     VisualScriptQuickFix const& fix, VisualScriptWidgetCommits& commits,
	                                     VisualScriptPaneResult& result)
	{
		VisualScriptEditorModel& model = context.Model;
		VisualScriptDocument const& document = commits.GetDocument(model);
		if (fix.Navigates)
		{
			uint32_t owner = 0;
			switch (FindElementKind(document, fix.Target, owner))
			{
				case ElementKind::Node:
					result.Open = MakeVisualScriptJump(context.Graph, owner, {fix.Target});
					if (fix.Kind == VisualScriptFixKind::EditSetting)
					{
						result.FocusSetting = fix.Argument;
					}
					else if (fix.Kind == VisualScriptFixKind::Rename)
					{
						// A node is renamed by its setting: a custom event by its Name.
						result.FocusSetting = "Name";
					}
					break;
				case ElementKind::Variable:
					SelectVisualScriptElement(context, {VisualScriptElementKind::Variable, fix.Target});
					context.Shared.Rename = VisualScriptElement{VisualScriptElementKind::Variable, fix.Target};
					break;
				case ElementKind::Local:
					result.ShowFunction = owner;
					SelectVisualScriptElement(context, {VisualScriptElementKind::Local, fix.Target});
					context.Shared.Rename = VisualScriptElement{VisualScriptElementKind::Local, fix.Target};
					break;
				case ElementKind::Function:
					SelectVisualScriptElement(context, {VisualScriptElementKind::Function, fix.Target});
					context.Shared.Rename = VisualScriptElement{VisualScriptElementKind::Function, fix.Target};
					break;
				case ElementKind::None:
					result.Notes.push_back(fmt::format("{}: what it goes to is gone", fix.Label));
					break;
			}
			return;
		}
		VisualScriptAnalysis const* const analysis = model.GetAnalysis();
		if (analysis == nullptr)
		{
			result.Notes.push_back(fmt::format("{} needs the scripting runtime", fix.Label));
			return;
		}
		VisualScriptDocument edited = document;
		if (Result<void> const applied = ApplyVisualScriptQuickFix(edited, *analysis, diagnostic, fix); !applied)
		{
			result.Notes.push_back(fmt::format("Could not apply {}: {}", fix.Label, applied.GetError()));
			return;
		}
		commits.Commit(context, std::move(edited), "Fix: " + fix.Label, 0, result);
	}

	bool DrawVisualScriptQuickFixButtons(VisualScriptPaneContext& context, VisualScriptDiagnostic const& diagnostic,
	                                     VisualScriptWidgetCommits& commits, VisualScriptPaneResult& result,
	                                     std::optional<std::pair<glm::vec2, glm::vec2>>* firstButton)
	{
		VisualScriptAnalysis const* const analysis = context.Model.GetAnalysis();
		Ref<VisualScriptLibrary const> const library = context.Semantics.GetLibrary();
		if (analysis == nullptr || library == nullptr)
		{
			return false;
		}
		std::vector<VisualScriptQuickFix> const fixes =
			GetVisualScriptQuickFixes(commits.GetDocument(context.Model), *analysis, diagnostic, *library);
		bool chosen = false;
		bool const readOnly = context.Model.IsReadOnly();
		for (size_t i = 0; i < fixes.size(); i++)
		{
			VisualScriptQuickFix const& fix = fixes[i];
			ImGui::PushID(static_cast<int>(i));
			if (i > 0)
			{
				ImGui::SameLine();
			}
			// Navigating needs no write; the other fixes edit.
			ImGui::BeginDisabled(readOnly && !fix.Navigates);
			if (ImGui::SmallButton(fix.Label.c_str()))
			{
				ApplyVisualScriptQuickFixChoice(context, diagnostic, fix, commits, result);
				chosen = true;
			}
			if (i == 0 && firstButton != nullptr)
			{
				ImVec2 const min = ImGui::GetItemRectMin();
				ImVec2 const max = ImGui::GetItemRectMax();
				*firstButton = std::pair(glm::vec2(min.x, min.y), glm::vec2(max.x, max.y));
			}
			ImGui::EndDisabled();
			ImGui::PopID();
		}
		return chosen;
	}

	void DrawVisualScriptProblems(VisualScriptPaneContext& context, std::span<VisualScriptDiagnostic const> diagnostics,
	                              VisualScriptWidgetCommits& commits, VisualScriptPaneResult& result)
	{
		float const wrap = ImGui::GetFontSize() * MessageWrapInFontSizes;
		for (size_t i = 0; i < diagnostics.size(); i++)
		{
			VisualScriptDiagnostic const& diagnostic = diagnostics[i];
			ImGui::PushID(static_cast<int>(i));
			if (i > 0)
			{
				ImGui::Separator();
			}
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(GetSeverityColor(diagnostic.Severity)));
			ImFont* const monospace = context.Services.MonospaceFont;
			if (monospace != nullptr)
			{
				ImGui::PushFont(monospace, 0.0f);
			}
			ImGui::TextUnformatted(diagnostic.Code.c_str());
			if (monospace != nullptr)
			{
				ImGui::PopFont();
			}
			ImGui::PopStyleColor();
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
			ImGui::TextUnformatted(diagnostic.Message.c_str());
			ImGui::PopTextWrapPos();
			DrawVisualScriptQuickFixButtons(context, diagnostic, commits, result);
			ImGui::PopID();
		}
	}

	// --- VisualScriptDrawer ---

	namespace
	{
		// The order of a sub-graph in the problems list: the event graph, then the functions in document order, then what the
		// document does not have.
		size_t GetGraphOrder(VisualScriptDocument const& document, uint32_t function)
		{
			if (function == 0)
			{
				return 0;
			}
			for (size_t i = 0; i < document.Functions.size(); i++)
			{
				if (document.Functions[i].ID == function)
				{
					return i + 1;
				}
			}
			return document.Functions.size() + 1;
		}

		// The node a diagnostic is placed at: its node, else its link's input node.
		std::optional<uint32_t> GetDiagnosticNode(VisualScriptDiagnostic const& diagnostic)
		{
			if (diagnostic.Node)
			{
				return diagnostic.Node;
			}
			if (diagnostic.Link)
			{
				return diagnostic.Link->To.Node;
			}
			return std::nullopt;
		}

		// "AddScore > Set Score": the sub-graph and the element a diagnostic names.
		std::string DescribePlace(VisualScriptDocument const& document, VisualScriptAnalysis const* analysis,
		                          VisualScriptDiagnostic const& diagnostic)
		{
			std::string place = GetVisualScriptGraphName(document, diagnostic.Function);
			if (std::optional<uint32_t> const node = GetDiagnosticNode(diagnostic))
			{
				if (VisualScriptNode const* const found = FindVisualScriptNode(document, *node))
				{
					place += " \xE2\x80\xBA " + GetVisualScriptNodeTitle(document, analysis, *found);
				}
			}
			else if (diagnostic.Variable)
			{
				for (VisualScriptVariable const& variable : document.Variables)
				{
					if (variable.ID == *diagnostic.Variable)
					{
						place += " \xE2\x80\xBA " + variable.Name;
					}
				}
				for (VisualScriptFunction const& function : document.Functions)
				{
					for (VisualScriptLocal const& local : function.Locals)
					{
						if (local.ID == *diagnostic.Variable)
						{
							place = function.Name + " \xE2\x80\xBA " + local.Name;
						}
					}
				}
			}
			return place;
		}

		std::string DescribeMatch(VisualScriptDocument const& document, VisualScriptAnalysis const* analysis,
		                          VisualScriptFindResult const& match)
		{
			if (match.Node != 0)
			{
				VisualScriptNode const* const node = FindVisualScriptNode(document, match.Node);
				std::string const title = node != nullptr ? GetVisualScriptNodeTitle(document, analysis, *node) : match.Text;
				return match.Field == VisualScriptFindField::Title || title == match.Text ? title
				                                                                          : fmt::format("{}: {}", title, match.Text);
			}
			if (match.Comment != 0)
			{
				return "Comment: " + match.Text;
			}
			if (match.Variable != 0)
			{
				return (match.Function != 0 ? "Local " : "Variable ") + match.Text;
			}
			return "Function " + match.Text;
		}

		std::pair<glm::vec2, glm::vec2> GetLastItemRect()
		{
			ImVec2 const min = ImGui::GetItemRectMin();
			ImVec2 const max = ImGui::GetItemRectMax();
			return {glm::vec2(min.x, min.y), glm::vec2(max.x, max.y)};
		}

		template<typename TRects>
		std::optional<std::pair<glm::vec2, glm::vec2>> FindRect(TRects const& rects, size_t index)
		{
			auto const found = std::find_if(rects.begin(), rects.end(),
			                                [index](auto const& rect)
			                                {
												return rect.Index == index;
											});
			return found != rects.end() ? std::optional(std::pair(found->Min, found->Max)) : std::nullopt;
		}
	}

	void VisualScriptDrawer::ShowFind(std::string text)
	{
		m_Mode = VisualScriptDrawerMode::Find;
		m_Text = std::move(text);
		m_Reference.reset();
		m_FocusText = true;
	}

	void VisualScriptDrawer::ShowReferences(VisualScriptReference const& reference)
	{
		m_Mode = VisualScriptDrawerMode::Find;
		m_Text.clear();
		m_Reference = reference;
	}

	std::optional<std::pair<glm::vec2, glm::vec2>> VisualScriptDrawer::FindProblemRow(size_t index) const
	{
		return FindRect(m_ProblemRows, index);
	}

	std::optional<std::pair<glm::vec2, glm::vec2>> VisualScriptDrawer::FindFixButton(size_t index) const
	{
		return FindRect(m_FixButtons, index);
	}

	std::optional<std::pair<glm::vec2, glm::vec2>> VisualScriptDrawer::FindFindRow(size_t index) const
	{
		return FindRect(m_FindRects, index);
	}

	VisualScriptPaneResult VisualScriptDrawer::Draw(VisualScriptPaneContext& context)
	{
		VisualScriptPaneResult result;
		m_Commits.BeginFrame(context, result);
		m_ProblemRows.clear();
		m_FixButtons.clear();
		m_FindRects.clear();
		if (context.Model.GetStoredDocument() == nullptr)
		{
			ImGui::TextDisabled("No script is shown.");
			return result;
		}
		if (m_Mode == VisualScriptDrawerMode::Find)
		{
			DrawFind(context, result);
		}
		else
		{
			m_Mode = VisualScriptDrawerMode::Problems;
			DrawProblems(context, result);
		}
		return result;
	}

	void VisualScriptDrawer::DrawProblems(VisualScriptPaneContext& context, VisualScriptPaneResult& result)
	{
		VisualScriptEditorModel const& model = context.Model;
		VisualScriptAnalysis const* const analysis = model.GetAnalysis();
		m_Problems.clear();
		if (analysis == nullptr)
		{
			ImGui::TextDisabled("Not analyzed: problems need the scripting runtime, which is not running.");
			return;
		}
		// The rows are found in the document the analysis was made of; a widget's pending value does not move them.
		VisualScriptDocument const& document = model.GetDisplayDocument();
		m_Problems = analysis->Diagnostics;
		auto const key = [&document](VisualScriptDiagnostic const& diagnostic)
		{
			std::optional<uint32_t> const node = GetDiagnosticNode(diagnostic);
			VisualScriptNode const* const found = node ? FindVisualScriptNode(document, *node) : nullptr;
			glm::vec2 const position = found != nullptr ? found->Position.value_or(glm::vec2(0.0f)) : glm::vec2(0.0f);
			return std::tuple(diagnostic.Severity != VisualScriptSeverity::Error, GetGraphOrder(document, diagnostic.Function),
			                  found == nullptr, position.y, position.x);
		};
		std::stable_sort(m_Problems.begin(), m_Problems.end(),
		                 [&key](VisualScriptDiagnostic const& a, VisualScriptDiagnostic const& b)
		                 {
							 return key(a) < key(b);
						 });
		if (m_Problems.empty())
		{
			ImGui::TextDisabled("No problems");
			return;
		}
		ImFont* const monospace = context.Services.MonospaceFont;
		ImGuiStyle const& style = ImGui::GetStyle();
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(m_Problems.size()), ImGui::GetFrameHeightWithSpacing());
		while (clipper.Step())
		{
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
			{
				auto const index = static_cast<size_t>(i);
				VisualScriptDiagnostic const& diagnostic = m_Problems[index];
				ImGui::PushID(i);
				ImGui::SetNextItemAllowOverlap();
				ImVec2 const start = ImGui::GetCursorScreenPos();
				if (ImGui::Selectable("##row", false, ImGuiSelectableFlags_None, ImVec2(0.0f, ImGui::GetFrameHeight())))
				{
					GoTo(context, diagnostic, result);
				}
				std::pair<glm::vec2, glm::vec2> const rect = GetLastItemRect();
				m_ProblemRows.push_back({index, rect.first, rect.second});
				ImGui::SetCursorScreenPos(start);
				// The severity's glyph: a dot for errors, a triangle for warnings.
				float const size = ImGui::GetFontSize() * 0.6f;
				ImVec2 const center(start.x + style.FramePadding.x + size * 0.5f, start.y + ImGui::GetFrameHeight() * 0.5f);
				ImDrawList* const drawList = ImGui::GetWindowDrawList();
				if (diagnostic.Severity == VisualScriptSeverity::Error)
				{
					drawList->AddCircleFilled(center, size * 0.5f, GetSeverityColor(diagnostic.Severity));
				}
				else
				{
					drawList->AddTriangleFilled(
						ImVec2(center.x, center.y - size * 0.5f), ImVec2(center.x + size * 0.5f, center.y + size * 0.5f),
						ImVec2(center.x - size * 0.5f, center.y + size * 0.5f), GetSeverityColor(diagnostic.Severity));
				}
				ImGui::SetCursorScreenPos(ImVec2(start.x + style.FramePadding.x * 2.0f + size, start.y));
				ImGui::AlignTextToFramePadding();
				if (monospace != nullptr)
				{
					ImGui::PushFont(monospace, 0.0f);
				}
				ImGui::TextUnformatted(diagnostic.Code.c_str());
				if (monospace != nullptr)
				{
					ImGui::PopFont();
				}
				ImGui::SameLine();
				ImGui::TextUnformatted(diagnostic.Message.c_str());
				ImGui::SameLine();
				ImGui::TextDisabled("%s", DescribePlace(document, analysis, diagnostic).c_str());
				ImGui::SameLine();
				// In a group, which ends the row's line whether it has buttons or not.
				std::optional<std::pair<glm::vec2, glm::vec2>> firstFix;
				ImGui::BeginGroup();
				DrawVisualScriptQuickFixButtons(context, diagnostic, m_Commits, result, &firstFix);
				ImGui::EndGroup();
				if (firstFix)
				{
					m_FixButtons.push_back({index, firstFix->first, firstFix->second});
				}
				ImGui::PopID();
			}
		}
	}

	void VisualScriptDrawer::UpdateFindRows(VisualScriptPaneContext& context)
	{
		VisualScriptEditorModel const& model = context.Model;
		bool const everyScript = m_Scope == VisualScriptFindScope::AllScripts ||
		                         (m_Reference && m_Reference->ReferenceKind == VisualScriptReference::Kind::CustomEvent);
		// What to look in: this script, then the project's other scripts by path.
		std::vector<std::pair<AssetHandle, Ref<VisualScriptDocument const>>> documents;
		std::vector<std::pair<AssetHandle, std::string>> errors;
		documents.emplace_back(context.Graph, model.GetStoredDocument());
		std::vector<AssetMetadata> scripts;
		if (everyScript)
		{
			scripts = AssetManager::GetAssets(AssetType::VisualScript);
			std::sort(scripts.begin(), scripts.end(),
			          [](AssetMetadata const& a, AssetMetadata const& b)
			          {
						  return a.Path < b.Path;
					  });
			for (AssetMetadata const& script : scripts)
			{
				if (script.Handle == context.Graph)
				{
					continue;
				}
				Result<Ref<VisualScriptAsset>> const asset = AssetManager::TryGetAsset<VisualScriptAsset>(script.Handle);
				if (asset)
				{
					documents.emplace_back(script.Handle, asset.GetValue()->GetSharedDocument());
				}
				else
				{
					documents.emplace_back(script.Handle, nullptr);
					errors.emplace_back(script.Handle, asset.GetError());
				}
			}
		}
		if (m_Found && m_FoundText == m_Text && m_FoundReference == m_Reference && m_FoundScope == m_Scope && m_FoundDocuments == documents)
		{
			return;
		}
		m_Found = true;
		m_FoundText = m_Text;
		m_FoundReference = m_Reference;
		m_FoundScope = m_Scope;
		m_FoundDocuments = documents;
		m_FindRows.clear();
		VisualScriptFindQuery query;
		if (m_Reference)
		{
			query.References = m_Reference;
		}
		else
		{
			query.Text = m_Text;
		}
		if (!query.References && query.Text.empty())
		{
			return;
		}
		auto const pathOf = [&scripts, &context](AssetHandle handle)
		{
			for (AssetMetadata const& script : scripts)
			{
				if (script.Handle == handle)
				{
					return script.Path.empty() ? script.Name : script.Path;
				}
			}
			std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(handle);
			return metadata ? (metadata->Path.empty() ? metadata->Name : metadata->Path) : std::string(context.ScriptName);
		};
		for (auto const& [handle, document] : documents)
		{
			if (document == nullptr)
			{
				continue;
			}
			bool const own = handle == context.Graph;
			// A variable's or a function's references are of this script; a custom event's are found by name everywhere.
			if (!own && m_Reference && m_Reference->ReferenceKind != VisualScriptReference::Kind::CustomEvent)
			{
				continue;
			}
			VisualScriptDocument const& searched = own ? model.GetDisplayDocument() : *document;
			VisualScriptAnalysis const* const analysis = own ? model.GetAnalysis() : nullptr;
			std::string const script = pathOf(handle);
			for (VisualScriptFindResult const& match : FindInVisualScript(searched, analysis, query))
			{
				m_FindRows.push_back({handle, script, match, DescribeMatch(searched, analysis, match), {}});
			}
		}
		for (auto const& [handle, error] : errors)
		{
			VisualScriptDrawerFindRow row;
			row.Graph = handle;
			row.Script = pathOf(handle);
			row.Error = "could not be read: " + error;
			m_FindRows.push_back(std::move(row));
		}
	}

	void VisualScriptDrawer::DrawFind(VisualScriptPaneContext& context, VisualScriptPaneResult& result)
	{
		ImGuiStyle const& style = ImGui::GetStyle();
		if (m_Reference)
		{
			std::string what;
			switch (m_Reference->ReferenceKind)
			{
				case VisualScriptReference::Kind::Variable:
					what = "the variable";
					break;
				case VisualScriptReference::Kind::Function:
					what = "the callers of the function";
					break;
				case VisualScriptReference::Kind::CustomEvent:
					what = fmt::format("the custom event {}: its senders in every script", m_Reference->Name);
					break;
			}
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(fmt::format("References of {}", what).c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton("Find Text"))
			{
				ShowFind();
			}
		}
		else
		{
			float const combo = ImGui::CalcTextSize("This Script").x + ImGui::GetFrameHeight() + style.FramePadding.x * 4.0f;
			if (m_FocusText)
			{
				ImGui::SetKeyboardFocusHere();
				m_FocusText = false;
			}
			ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - combo - style.ItemSpacing.x, ImGui::GetFontSize() * 4.0f));
			ImGui::InputTextWithHint("##text", "Find in nodes, comments, values and names", &m_Text);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(combo);
			constexpr std::array<char const*, 2> Scopes = {"This Script", "All Scripts"};
			int scope = static_cast<int>(m_Scope);
			if (ImGui::Combo("##scope", &scope, Scopes.data(), static_cast<int>(Scopes.size())))
			{
				m_Scope = static_cast<VisualScriptFindScope>(scope);
			}
		}
		UpdateFindRows(context);
		if (m_FindRows.empty())
		{
			if (m_Reference || !m_Text.empty())
			{
				ImGui::TextDisabled("Nothing found");
			}
			return;
		}
		ImGui::BeginChild("##results", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
		AssetHandle shownScript;
		std::optional<uint32_t> shownGraph;
		for (size_t i = 0; i < m_FindRows.size(); i++)
		{
			VisualScriptDrawerFindRow const& row = m_FindRows[i];
			ImGui::PushID(static_cast<int>(i));
			if (i == 0 || row.Graph != shownScript)
			{
				ImGui::TextDisabled("%s", row.Script.c_str());
				shownScript = row.Graph;
				shownGraph.reset();
			}
			if (!row.Error.empty())
			{
				ImGui::Indent();
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(VisualScriptErrorColor));
				ImGui::TextWrapped("%s", row.Error.c_str());
				ImGui::PopStyleColor();
				ImGui::Unindent();
				ImGui::PopID();
				continue;
			}
			if (shownGraph != row.Match.Function)
			{
				ImGui::Indent();
				std::optional<Ref<VisualScriptDocument const>> document;
				for (auto const& [handle, found] : m_FoundDocuments)
				{
					if (handle == row.Graph && found != nullptr)
					{
						document = found;
					}
				}
				ImGui::TextDisabled("%s", document ? GetVisualScriptGraphName(**document, row.Match.Function).c_str() : "");
				ImGui::Unindent();
				shownGraph = row.Match.Function;
			}
			ImGui::Indent(style.IndentSpacing * 2.0f);
			if (ImGui::Selectable(row.Label.c_str()))
			{
				GoTo(context, row, result);
			}
			std::pair<glm::vec2, glm::vec2> const rect = GetLastItemRect();
			m_FindRects.push_back({i, rect.first, rect.second});
			ImGui::Unindent(style.IndentSpacing * 2.0f);
			ImGui::PopID();
		}
		ImGui::EndChild();
	}

	void VisualScriptDrawer::GoTo(VisualScriptPaneContext& context, VisualScriptDiagnostic const& diagnostic,
	                              VisualScriptPaneResult& result)
	{
		if (std::optional<uint32_t> const node = GetDiagnosticNode(diagnostic))
		{
			uint32_t const function = FindVisualScriptElementGraph(context.Model.GetDisplayDocument(), *node).value_or(diagnostic.Function);
			result.Open = MakeVisualScriptJump(context.Graph, function, {*node});
			return;
		}
		if (diagnostic.Variable)
		{
			bool const local = diagnostic.Function != 0;
			if (local)
			{
				result.ShowFunction = diagnostic.Function;
			}
			SelectVisualScriptElement(context,
			                          {local ? VisualScriptElementKind::Local : VisualScriptElementKind::Variable, *diagnostic.Variable});
			return;
		}
		if (diagnostic.Function != 0)
		{
			result.ShowFunction = diagnostic.Function;
			SelectVisualScriptElement(context, {VisualScriptElementKind::Function, diagnostic.Function});
		}
	}

	void VisualScriptDrawer::GoTo(VisualScriptPaneContext& context, VisualScriptDrawerFindRow const& row, VisualScriptPaneResult& result)
	{
		VisualScriptFindResult const& match = row.Match;
		uint32_t const element = match.Node != 0 ? match.Node : match.Comment;
		bool const own = row.Graph == context.Graph;
		if (element != 0 || !own)
		{
			VisualScriptOpenRequest request =
				MakeVisualScriptJump(row.Graph, match.Function, element != 0 ? std::vector<uint32_t>{element} : std::vector<uint32_t>());
			result.Open = std::move(request);
			return;
		}
		if (match.Variable != 0)
		{
			if (match.Function != 0)
			{
				result.ShowFunction = match.Function;
			}
			SelectVisualScriptElement(
				context, {match.Function != 0 ? VisualScriptElementKind::Local : VisualScriptElementKind::Variable, match.Variable});
			return;
		}
		result.ShowFunction = match.Function;
		SelectVisualScriptElement(context, {VisualScriptElementKind::Function, match.Function});
	}
}
