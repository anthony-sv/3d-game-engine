#include "Editor/VisualScripting/VisualScriptDrawer.h"

#include "Editor/VisualScripting/VisualScriptTheme.h"

#include <imgui.h>

#include <algorithm>
#include <string>
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
	                                     VisualScriptWidgetCommits& commits, VisualScriptPaneResult& result)
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
}
