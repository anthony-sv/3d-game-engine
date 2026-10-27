#pragma once

#include "Editor/VisualScripting/VisualScriptPalette.h"
#include "Editor/VisualScripting/VisualScriptPanes.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Base.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct ImGuiInputTextCallbackData;

namespace Strada
{
	// What graph.palette asks of a window's palette: opened as if Tab was pressed at At (the view's centre without it) and
	// Query typed, or, with From, as if a wire dragged from that pin (of Direction, named as the canvas names it) was released
	// at At. Limit caps the entries VisualScriptPaletteState reports.
	struct VisualScriptPaletteRequest
	{
		std::string Query;
		std::optional<glm::vec2> At;
		std::optional<VisualScriptPinReference> From;
		VisualScriptPinDirection Direction = VisualScriptPinDirection::Out;
		uint32_t Limit = 50;
	};

	// What the palette showed in its last frame, as graph.palette reports it.
	struct VisualScriptPaletteState
	{
		bool Open = false;
		std::string Query;
		std::optional<VisualScriptPinReference> From;
		// The rows listed, in their order, at most the request's limit: the ranking of the query or the context, else the
		// recent entries (at most RecentRows). Their Entry indices are of GetIndex()'s entries, valid until the next Draw.
		std::vector<VisualScriptPaletteMatch> Entries;
		// How many rows there are before the limit.
		uint32_t Total = 0;
	};

	// The node palette of a graph window: a popup at the normal UI size with a search field that has the keyboard, the rows of
	// the palette index (VisualScriptPaletteIndex) ranked by the query (RankVisualScriptPalette) or, without one, the recent
	// entries and the category tree, and a strip that describes the highlighted row. Choosing a row adds its node where the
	// palette was opened, in one step ("Add Branch in Player"), selects it, and in context mode (a wire released on empty
	// canvas) links it to the wire's pin. Without the scripting runtime it does not open. Main thread only.
	class VisualScriptPalettePopup
	{
	public:
		// The popup's size, in font sizes.
		static constexpr float WidthInFontSizes = 28.0f;
		static constexpr float HeightInFontSizes = 32.0f;
		// Recent entries listed above the category tree.
		static constexpr size_t RecentRows = 8;
		// What a new comment box says until it is edited.
		static constexpr char const* NewCommentText = "Comment";
		// The base names of new custom events and variables, made unique.
		static constexpr char const* NewEventName = "NewEvent";
		static constexpr char const* NewVariableName = "NewVar";

		// Opens the palette at the next Draw for the canvas's palette intent, at the mouse: its node goes to the intent's
		// Position, linked to its From pin in context mode, and the search field starts with its Query (the characters typed
		// before the field has the keyboard).
		void Open(VisualScriptPaletteIntent const& intent);
		// Opens the palette as graph.palette asks, at the screen point of At.
		void Open(VisualScriptPaletteRequest const& request);
		void Close();
		bool IsOpen() const { return m_Open; }

		// What the palette showed in the last Draw.
		VisualScriptPaletteState const& GetState() const { return m_State; }
		// The entries the state's matches index; valid until the next Draw.
		VisualScriptPaletteIndex const& GetIndex() const { return m_Index; }
		// The entries of the rows the last Draw listed, in their order (what Up, Down and Enter move through), and the
		// highlighted one, which the details strip describes; nothing while no row is listed.
		std::vector<uint32_t> GetRowEntries() const;
		std::optional<uint32_t> GetHighlightedEntry() const;

		// Draws the popup while it is open, in the window that draws the canvas (its ID scope opens the popup). Returns the
		// commit of a chosen row, or, for an event the graph has already, the node to select and frame.
		VisualScriptPaneResult Draw(VisualScriptPaneContext& context);
		// The palette commits nothing while a widget is in use.
		std::optional<VisualScriptCommitRequest> TakePendingEdit() { return std::nullopt; }

	private:
		// The rows of the list in the order they are drawn: what Up, Down and Enter move through.
		struct Row
		{
			uint32_t Entry = 0;
			// The ranked match's index in the ranking; -1 for the recent entries and the tree.
			int32_t Match = -1;
		};

		void UpdateIndex(VisualScriptPaneContext const& context, Ref<VisualScriptLibrary const> const& library);
		void UpdateRanking(VisualScriptPaneContext const& context, VisualScriptLibrary const& library);
		void UpdateRecents(VisualScriptPaneContext const& context);
		void UpdateState();
		// The pin the palette was opened from, with its analyzed type; nothing outside context mode.
		std::optional<VisualScriptPin> FindContextPin(VisualScriptPaneContext const& context, VisualScriptLibrary const& library) const;
		void DrawSearch(bool& confirmed);
		void DrawRankedRows(VisualScriptPaneContext const& context, std::optional<uint32_t>& chosen);
		void DrawEmptyList(VisualScriptPaneContext& context);
		void DrawTree(VisualScriptPaneContext const& context, VisualScriptLibrary const& library, bool appearing,
		              std::optional<uint32_t>& chosen);
		void DrawCategory(VisualScriptPaneContext const& context, VisualScriptLibrary const& library, VisualScriptCategory const& category,
		                  bool appearing, bool firstLevel, std::optional<uint32_t>& chosen);
		void DrawEntryRows(VisualScriptPaneContext const& context, std::string const& category, std::optional<uint32_t>& chosen);
		// One row: the kind's glyph, the title (matched characters highlighted), the subtitle, and in context mode the pin it
		// connects and the conversion. Returns true when clicked.
		bool DrawRow(VisualScriptPaneContext const& context, Row const& row, int index);
		void DrawDetails(VisualScriptLibrary const& library);
		void DrawPreviewTooltip(VisualScriptPaneContext const& context, VisualScriptPaletteEntry const& entry) const;
		bool HasEntries(VisualScriptCategory const& category) const;
		// Adds the entry's node (or does its action) as one commit request; GoToNode selects and frames instead.
		void Choose(VisualScriptPaneContext& context, VisualScriptLibrary const& library, uint32_t entry,
		            VisualScriptPaletteMatch const* match, VisualScriptPaneResult& result);
		static int OnSearchKey(ImGuiInputTextCallbackData* data);

		bool m_Open = false;
		// ImGui::OpenPopup is still to be called (in Draw's ID scope).
		bool m_OpenPending = false;
		// Canvas units: where the node goes.
		glm::vec2 m_Position{0.0f};
		// Screen pixels: where the popup shows; none shows it at the mouse.
		std::optional<glm::vec2> m_ScreenPosition;
		std::optional<VisualScriptPinReference> m_From;
		VisualScriptPinDirection m_Direction = VisualScriptPinDirection::Out;
		uint32_t m_Limit = 50;
		// Opened by graph.palette (shown at the screen point of m_Position), and without a point (at the view's centre).
		bool m_FromRequest = false;
		bool m_RequestedAt = false;
		std::string m_Query;
		// The search field still has to get the keyboard; its first active frame puts the cursor after the text typed so far
		// (ImGui selects all of a field the keyboard is given to, and the next character would replace it).
		bool m_FocusSearch = false;
		bool m_PlaceCursor = false;
		// Up and Down this frame; the highlighted row; whether the list scrolls to it.
		int m_Move = 0;
		int m_Highlight = 0;
		bool m_ScrollToHighlight = false;

		// What the index was made from: the library object, the stored document object and the sub-graph shown.
		Ref<VisualScriptLibrary const> m_Library;
		Ref<VisualScriptDocument const> m_IndexedDocument;
		uint32_t m_IndexedFunction = 0;
		uint64_t m_IndexGeneration = 0;
		VisualScriptPaletteIndex m_Index;
		// The index's entries by RecentKey, and by category path.
		std::unordered_map<std::string, uint32_t> m_EntriesByKey;
		std::unordered_map<std::string, std::vector<uint32_t>> m_EntriesByCategory;

		// The ranking and what it was made of.
		std::vector<VisualScriptPaletteMatch> m_Matches;
		bool m_Ranked = false;
		std::string m_RankedQuery;
		bool m_RankedContext = false;
		uint64_t m_RankedGeneration = 0;
		std::vector<std::string> m_RankedRecents;
		std::optional<VisualScriptPin> m_ContextPin;

		// The recent entries the index has, the most recent first (at most RecentRows).
		std::vector<uint32_t> m_RecentEntries;
		// The rows drawn (in the last frame until the list is drawn again), which the keys act on.
		std::vector<Row> m_Rows;
		VisualScriptPaletteState m_State;
	};
}
