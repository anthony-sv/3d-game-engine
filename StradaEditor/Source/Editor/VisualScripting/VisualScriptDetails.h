#pragma once

#include "Editor/VisualScripting/VisualScriptLiteralEditor.h"
#include "Editor/VisualScripting/VisualScriptPanes.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Strada
{
	class AssetThumbnails;

	// The easing curves of Flow.Tween's Easing setting (spec v2 §4.2) as the scripting runtime computes them, at t in [0, 1]:
	// what Details plots beside each choice. For display only: the runtime eases the values the graph sees. Nothing for a
	// name that is no easing.
	[[nodiscard]] std::optional<double> EvaluateVisualScriptEasing(std::string_view easing, double t);

	// Where a type picked in Details goes, which decides the types the picker offers.
	enum class VisualScriptTypeUse : uint8_t
	{
		// A private variable, a local, a function's or a custom event's parameter: any type with values except arrays (v2 §3.2).
		Value = 0,
		// A public variable: a type components can store (v2 §3.3): one with a script field type, or an enum.
		PublicVariable,
		// A setting of the kind Type or TypeList: the types its Constraint names.
		Setting
	};

	// Whether the type picker offers a type for a use; constraint is the setting's Constraint for VisualScriptTypeUse::Setting
	// ("Literal", "Makeable", "Switch", ...; empty or unknown: any type with values).
	[[nodiscard]] bool IsVisualScriptTypeOffered(VisualScriptTypeInfo const& type, VisualScriptTypeUse use,
	                                             std::string_view constraint = {});

	// What Details shows.
	enum class VisualScriptDetailsSubject : uint8_t
	{
		// Nothing is selected: the script (and in a function graph, the function).
		Script = 0,
		Node,
		Comment,
		// Two or more nodes and comments.
		Several,
		// A My Script row.
		Variable,
		Local,
		Function,
		CustomEvent
	};

	// The Details pane of a graph window: what is selected, edited in place. The canvas's selection (its primary element, or a
	// summary of several), else the My Script row selected (VisualScriptPaneShared::Element), else the script with its
	// description and, in a function graph, the function. Nodes show their header, General (comment, enabled, collapsed),
	// their settings by kind, the literals of their unlinked inputs (VisualScriptLiteralEditor) and where their linked inputs
	// come from, their problems with quick fixes, and what the node library says of them. Variables, locals, functions and
	// comments show their properties. Every widget commits through VisualScriptWidgetCommits (one step per interaction),
	// names once (VisualScriptNameField); nothing is edited in read-only states. Main thread only.
	class VisualScriptDetails
	{
	public:
		// The labels of the rows take this share of the pane's width.
		static constexpr float LabelShare = 0.38f;
		// The Easing plots, in pixels at a UI scale of 1.
		static constexpr float EasingPlotWidth = 32.0f;
		static constexpr float EasingPlotHeight = 16.0f;
		// The type picker lists at most this many rows before it scrolls.
		static constexpr int PickerRows = 14;

		VisualScriptPaneResult Draw(VisualScriptPaneContext& context);
		std::optional<VisualScriptCommitRequest> TakePendingEdit() { return m_Commits.TakePendingEdit(); }

		// What the last Draw showed.
		VisualScriptDetailsSubject GetSubject() const { return m_Subject; }
		// The screen rectangle (min, max) of the widget of the row with the label in the last Draw ("Name", "Type", "Count",
		// "Then1", ...; the first row of that label); nothing when no such row was drawn. Tests and automation find widgets
		// by it.
		std::optional<std::pair<glm::vec2, glm::vec2>> FindRow(std::string_view label) const;
		// The text a row that shows text drew in the last Draw ("Placeholders", "Default Value", an input's "Self", ...);
		// nothing when no such row was drawn.
		std::optional<std::string> GetRowText(std::string_view label) const;
		// The type IDs the type picker listed in the last Draw, in their order; empty while it is closed.
		std::span<std::string const> GetPickerTypes() const { return m_PickerTypes; }
		// Whether the open type picker is the one of the row with the label.
		bool IsPickerOpen(std::string_view label) const { return !m_PickerTypes.empty() && m_PickerLabel == label; }

		// Asset literals show images of the assets.
		void SetAssetPreviews(AssetThumbnails* previews) { m_Literals.SetAssetPreviews(previews); }

	private:
		// An edit a widget made: the document and the step's description.
		struct Edit
		{
			VisualScriptDocument Document;
			std::string Action;
		};

		struct Frame;

		// The rows: a label in the first column and its widget, as wide as the second, in a group.
		void BeginRow(std::string_view label);
		void EndRow(std::string_view label);
		// A row that shows a text instead of a widget.
		void TextRow(std::string_view label, std::string_view text);
		// Draws a widget in a row and hands its edit to the commits with the widget's state (active, deactivated).
		template<typename TDraw>
		void Track(Frame& frame, std::string_view label, TDraw&& draw);

		void DrawScript(Frame& frame);
		void DrawNode(Frame& frame, VisualScriptNode const& node);
		void DrawNodeHeader(Frame& frame, VisualScriptNode const& node, std::string const& title);
		void DrawGeneral(Frame& frame, VisualScriptNode const& node, std::string const& title);
		void DrawSettings(Frame& frame, VisualScriptNode const& node, std::string const& title);
		void DrawSetting(Frame& frame, VisualScriptNode const& node, std::string const& title, VisualScriptSetting const& setting);
		void DrawSequenceOutputs(Frame& frame, VisualScriptNode const& node, std::string const& title, VisualScriptSetting const& setting);
		void DrawCases(Frame& frame, VisualScriptNode const& node, std::string const& title);
		void DrawChoice(Frame& frame, VisualScriptNode const& node, std::string const& title, VisualScriptSetting const& setting);
		void DrawInputs(Frame& frame, VisualScriptNode const& node, std::string const& title);
		void DrawNodeProblems(Frame& frame, VisualScriptNode const& node);
		void DrawAbout(Frame& frame, VisualScriptNode const& node);
		void DrawComment(Frame& frame, VisualScriptComment const& comment);
		void DrawSeveral(Frame& frame, std::span<uint32_t const> selection);
		void DrawVariable(Frame& frame, VisualScriptVariable const& variable);
		void DrawLocal(Frame& frame, VisualScriptFunction const& function, VisualScriptLocal const& local);
		void DrawFunction(Frame& frame, VisualScriptFunction const& function);
		// A list of parameters (a function's inputs or outputs, a custom event's or a sender's): rows of name and type, moved
		// up and down and removed, and an add button. Returns the list changed this frame and the change's description
		// (without the owner's name).
		std::optional<std::pair<std::vector<VisualScriptParameter>, std::string>>
		DrawParameters(Frame& frame, std::string_view id, std::vector<VisualScriptParameter> const& parameters, std::string_view noun);
		void DrawElementProblems(Frame& frame, std::optional<uint32_t> variable, std::optional<uint32_t> function);

		// A button showing a type that opens the type picker (focus opens it as if clicked), as wide as width (0: to the right
		// edge); returns the type chosen in the frame it is chosen.
		std::optional<std::string> DrawTypePicker(Frame& frame, std::string_view label, std::string const& current, VisualScriptTypeUse use,
		                                          std::string_view constraint = {}, float width = 0.0f, bool focus = false);
		// A literal of a type, for the row with the label; returns the canonical literal changed this frame.
		std::optional<Json> DrawLiteral(Frame& frame, std::string_view type, Json const& literal, Json const& fallback);

		VisualScriptWidgetCommits m_Commits;
		VisualScriptLiteralEditor m_Literals;
		VisualScriptNameField m_Names;
		VisualScriptDetailsSubject m_Subject = VisualScriptDetailsSubject::Script;
		// The rows drawn in the last Draw, by label.
		std::vector<std::pair<std::string, std::pair<glm::vec2, glm::vec2>>> m_Rows;
		std::vector<std::pair<std::string, std::string>> m_RowTexts;
		// The open type picker: its row, its search text, its highlighted row and the types it listed.
		std::string m_PickerLabel;
		std::string m_PickerSearch;
		int m_PickerHighlight = 0;
		bool m_PickerFocus = false;
		std::vector<std::string> m_PickerTypes;
	};
}
