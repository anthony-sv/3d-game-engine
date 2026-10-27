#pragma once

#include "Editor/VisualScripting/VisualScriptFind.h"
#include "Editor/VisualScripting/VisualScriptPanes.h"

#include "Strada/Asset/VisualScriptAsset.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Strada
{
	// The find bar of a graph window (Ctrl+F), at the canvas's top-right: a text field that has the keyboard, "3 of 12", the
	// previous and next buttons, "All graphs" (every sub-graph of the script, else the shown one) and a close button. Enter
	// goes to the next match, Shift+Enter to the previous, Escape closes. Going to a match replaces the selection with it:
	// a node or comment is selected and framed (its sub-graph shown first), a variable, local or function is selected in
	// My Script; so what Delete, F2 or Ctrl+D do next acts on what the bar showed. The shown sub-graph's matching nodes and
	// comments are outlined (GetOutlined). Matches follow the document as it changes. Main thread only.
	class VisualScriptFindBar
	{
	public:
		// The bar's width: min(24 F, the canvas's width - 2 F), F the font size.
		static constexpr float WidthInFontSizes = 24.0f;
		[[nodiscard]] static float GetWidth(float canvasWidth, float fontSize);

		// Opens the bar with its field's keyboard; a text not empty replaces the one typed before. With goToFirst, its next Draw
		// goes to the first match (an open request's find, spec §13.2), as Enter would from no match.
		void Open(std::string text = {}, bool goToFirst = false);
		// Closes the bar; the selection it made stays.
		void Close();
		bool IsOpen() const { return m_Open; }

		// Draws the bar (when open) as wide as the current item width allows, in the window the panel gives it.
		VisualScriptPaneResult Draw(VisualScriptPaneContext& context);
		// The bar commits nothing.
		std::optional<VisualScriptCommitRequest> TakePendingEdit() { return std::nullopt; }

		std::string const& GetText() const { return m_Text; }
		bool IsAllGraphs() const { return m_AllGraphs; }
		void SetAllGraphs(bool allGraphs) { m_AllGraphs = allGraphs; }
		// The matches in document order (FindInVisualScript), of every sub-graph or the shown one.
		std::span<VisualScriptFindResult const> GetMatches() const { return m_Matches; }
		// The match gone to last, from 1 ("3 of 12"); 0 when none.
		uint32_t GetIndex() const { return m_Index < 0 ? 0u : static_cast<uint32_t>(m_Index) + 1u; }
		uint32_t GetCount() const { return static_cast<uint32_t>(m_Matches.size()); }
		// The nodes and comments of the shown sub-graph that match, which the canvas outlines in amber.
		std::span<uint32_t const> GetOutlined() const { return m_Outlined; }

	private:
		void UpdateMatches(VisualScriptPaneContext& context);
		void GoTo(VisualScriptPaneContext& context, int step, VisualScriptPaneResult& result);

		bool m_Open = false;
		// The field, and the window the bar is drawn in, take the keyboard at the next Draw.
		bool m_Focus = false;
		bool m_FocusWindow = false;
		// The next Draw goes to the first match.
		bool m_GoToFirst = false;
		std::string m_Text;
		bool m_AllGraphs = true;
		std::vector<VisualScriptFindResult> m_Matches;
		std::vector<uint32_t> m_Outlined;
		// The match gone to last; -1 when none.
		int m_Index = -1;
		// What the matches were found in.
		bool m_Found = false;
		std::string m_FoundText;
		bool m_FoundAllGraphs = true;
		uint32_t m_FoundFunction = 0;
		Ref<VisualScriptDocument const> m_FoundDocument;
		VisualScriptAnalysis const* m_FoundAnalysis = nullptr;
	};
}
