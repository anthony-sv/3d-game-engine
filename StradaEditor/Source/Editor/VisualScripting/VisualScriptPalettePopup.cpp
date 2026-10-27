#include "Editor/VisualScripting/VisualScriptPalettePopup.h"

#include "Editor/UI/EditorUI.h"
#include "Editor/VisualScripting/VisualScriptCanvasMetrics.h"
#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptEditing.h"
#include "Editor/VisualScripting/VisualScriptTheme.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string_view>
#include <utility>

namespace Strada
{
	namespace
	{
		// Category paths of the document's entries (VisualScriptPaletteIndex::SetDocument), drawn before the library's
		// categories unless the library has a category of the same path.
		constexpr std::array<std::string_view, 3> DocumentCategories = {"Variables", "Functions", "Events"};
		constexpr char const* PopupName = "##VisualScriptPalette";
		constexpr char const* SearchName = "##search";
		// The scale of the canvas a hovered row's node is previewed at.
		constexpr float PreviewScale = 0.75f;

		VisualScriptPinDirection Opposite(VisualScriptPinDirection direction)
		{
			return direction == VisualScriptPinDirection::In ? VisualScriptPinDirection::Out : VisualScriptPinDirection::In;
		}

		bool IsExec(VisualScriptPin const& pin)
		{
			return pin.Type == VisualScriptExecType;
		}

		glm::vec2 SnapToGrid(glm::vec2 position)
		{
			return glm::round(position / VisualScriptCanvasMetrics::GridSize) * VisualScriptCanvasMetrics::GridSize;
		}

		// The first segment of a slash-separated category path, which header colours take their quieter forms from.
		std::string_view GetTopCategory(std::string_view category)
		{
			return category.substr(0, category.find('/'));
		}

		VisualScriptGraph* FindGraph(VisualScriptDocument& document, uint32_t function)
		{
			if (function == 0)
			{
				return &document.Graph;
			}
			auto const found = std::find_if(document.Functions.begin(), document.Functions.end(),
			                                [function](VisualScriptFunction const& candidate)
			                                {
												return candidate.ID == function;
											});
			return found != document.Functions.end() ? &found->Graph : nullptr;
		}

		std::optional<VisualScriptLink> FindLinkFrom(VisualScriptGraph const& graph, uint32_t node, std::string_view pin)
		{
			auto const found = std::find_if(graph.Links.begin(), graph.Links.end(),
			                                [node, pin](VisualScriptLink const& link)
			                                {
												return link.From.Node == node && link.From.Pin == pin;
											});
			return found != graph.Links.end() ? std::optional<VisualScriptLink>(*found) : std::nullopt;
		}

		VisualScriptPin const* FindFirstExec(std::span<VisualScriptPin const> pins, VisualScriptPinDirection direction)
		{
			auto const found = std::find_if(pins.begin(), pins.end(),
			                                [direction](VisualScriptPin const& pin)
			                                {
												return pin.Direction == direction && IsExec(pin);
											});
			return found != pins.end() ? &*found : nullptr;
		}

		// With the Context box unchecked, a node chosen from every entry is still linked to the wire's pin when one of its pins
		// of a decided type connects to it (a wildcard only its settings decide would take anything).
		VisualScriptPin const* FindLooseConnectPin(VisualScriptPaletteEntry const& entry, VisualScriptPin const& from,
		                                           VisualScriptPinDirection fromDirection, VisualScriptLibrary const& library)
		{
			for (VisualScriptPin const& pin : entry.Pins)
			{
				if (pin.Direction == fromDirection || pin.Type == VisualScriptWildcardType)
				{
					continue;
				}
				bool const connects =
					fromDirection == VisualScriptPinDirection::Out ? library.CanConnect(from, pin) : library.CanConnect(pin, from);
				if (connects)
				{
					return &pin;
				}
			}
			return nullptr;
		}

		// Where a pin of a node of the type would sit relative to the node's top-left corner, as the canvas lays the node out
		// without an analysis (its library template's pins, which a new node shows until it is analyzed).
		glm::vec2 MeasurePinAnchor(VisualScriptDocument const& document, uint32_t function, VisualScriptNode node, std::string const& pin,
		                           VisualScriptPinDirection direction, VisualScriptLibrary const& library,
		                           VisualScriptTextMeasurer const& measure)
		{
			// The node alone, with what its pins come from: the variables and the functions' signatures.
			VisualScriptDocument scratch;
			scratch.Variables = document.Variables;
			scratch.Functions = document.Functions;
			for (VisualScriptFunction& owner : scratch.Functions)
			{
				owner.Graph = {};
			}
			VisualScriptGraph* const graph = FindGraph(scratch, function);
			if (graph == nullptr)
			{
				return glm::vec2(0.0f);
			}
			node.Position = glm::vec2(0.0f);
			uint32_t const id = node.ID;
			graph->Nodes.push_back(std::move(node));

			VisualScriptCanvasSource source;
			source.Display = &scratch;
			source.Function = function;
			source.Library = &library;
			source.EnumLookup = [&library](std::string_view type)
			{
				return library.FindEnum(type);
			};
			VisualScriptCanvasModel canvas;
			canvas.Sync(source, measure, VisualScriptThemeAnchors{});
			std::optional<uint32_t> const index = canvas.FindNode(id);
			std::optional<uint32_t> const found = index ? canvas.FindPin(*index, pin, direction) : std::nullopt;
			return found ? canvas.GetPins()[*found].Anchor : glm::vec2(0.0f);
		}

		// Text with the bytes of highlights (ascending) in the highlight colour.
		void DrawHighlightedText(ImDrawList* drawList, ImVec2 position, std::string_view text, std::span<uint16_t const> highlights,
		                         ImU32 color, ImU32 highlightColor)
		{
			size_t start = 0;
			size_t next = 0;
			while (start < text.size())
			{
				bool const highlighted = next < highlights.size() && highlights[next] == start;
				size_t end = start + 1;
				if (highlighted)
				{
					next++;
					while (end < text.size() && next < highlights.size() && highlights[next] == end)
					{
						next++;
						end++;
					}
				}
				else
				{
					while (end < text.size() && !(next < highlights.size() && highlights[next] == end))
					{
						end++;
					}
				}
				// Keep UTF-8 sequences whole: a run ends only before a lead byte.
				while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
				{
					end++;
				}
				char const* const begin = text.data() + start;
				char const* const finish = text.data() + end;
				drawList->AddText(position, highlighted ? highlightColor : color, begin, finish);
				position.x += ImGui::CalcTextSize(begin, finish).x;
				while (next < highlights.size() && highlights[next] < end)
				{
					next++;
				}
				start = end;
			}
		}
	}

	void VisualScriptPalettePopup::Open(VisualScriptPaletteIntent const& intent)
	{
		m_Open = true;
		m_OpenPending = true;
		m_Position = intent.Position;
		m_ScreenPosition.reset();
		m_From = intent.From;
		m_Direction = intent.Direction;
		m_Limit = 50;
		m_Query = intent.Query;
	}

	void VisualScriptPalettePopup::Open(VisualScriptPaletteRequest const& request)
	{
		m_Open = true;
		m_OpenPending = true;
		m_Position = request.At.value_or(glm::vec2(0.0f));
		m_ScreenPosition.reset();
		m_RequestedAt = !request.At.has_value();
		m_From = request.From;
		m_Direction = request.Direction;
		m_Limit = std::max<uint32_t>(request.Limit, 1);
		m_Query = request.Query;
		m_FromRequest = true;
	}

	void VisualScriptPalettePopup::Close()
	{
		m_Open = false;
		m_OpenPending = false;
		m_FromRequest = false;
		m_RequestedAt = false;
		m_Query.clear();
		m_From.reset();
		m_Rows.clear();
	}

	std::vector<uint32_t> VisualScriptPalettePopup::GetRowEntries() const
	{
		std::vector<uint32_t> entries;
		entries.reserve(m_Rows.size());
		for (Row const& row : m_Rows)
		{
			entries.push_back(row.Entry);
		}
		return entries;
	}

	std::optional<uint32_t> VisualScriptPalettePopup::GetHighlightedEntry() const
	{
		if (m_Rows.empty())
		{
			return std::nullopt;
		}
		return m_Rows[static_cast<size_t>(std::clamp(m_Highlight, 0, static_cast<int>(m_Rows.size()) - 1))].Entry;
	}

	int VisualScriptPalettePopup::OnSearchKey(ImGuiInputTextCallbackData* data)
	{
		auto* const palette = static_cast<VisualScriptPalettePopup*>(data->UserData);
		if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory)
		{
			palette->m_Move += data->EventKey == ImGuiKey_UpArrow ? -1 : 1;
		}
		else if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways && palette->m_PlaceCursor)
		{
			// After the text typed before the field had the keyboard, not over it.
			data->CursorPos = data->BufTextLen;
			data->SelectionStart = data->BufTextLen;
			data->SelectionEnd = data->BufTextLen;
			palette->m_PlaceCursor = false;
		}
		return 0;
	}

	void VisualScriptPalettePopup::UpdateIndex(VisualScriptPaneContext const& context, Ref<VisualScriptLibrary const> const& library)
	{
		VisualScriptEditorModel const& model = context.Model;
		bool changed = false;
		if (library != m_Library)
		{
			m_Library = library;
			m_Index.Rebuild(*library);
			m_IndexedDocument.reset();
			changed = true;
		}
		if (model.GetStoredDocument() != m_IndexedDocument || model.GetFunction() != m_IndexedFunction || changed)
		{
			m_IndexedDocument = model.GetStoredDocument();
			m_IndexedFunction = model.GetFunction();
			m_Index.SetDocument(model.GetDisplayDocument(), m_IndexedFunction);
			changed = true;
		}
		if (!changed)
		{
			return;
		}
		// The entries may be composed again: what refers to them is made again.
		m_IndexGeneration++;
		m_EntriesByKey.clear();
		m_EntriesByCategory.clear();
		std::span<VisualScriptPaletteEntry const> const entries = m_Index.GetEntries();
		for (uint32_t i = 0; i < entries.size(); i++)
		{
			m_EntriesByKey.try_emplace(entries[i].RecentKey, i);
			m_EntriesByCategory[entries[i].Category].push_back(i);
		}
	}

	void VisualScriptPalettePopup::UpdateRecents(VisualScriptPaneContext const& context)
	{
		m_RecentEntries.clear();
		for (VisualScriptRecentNode const& recent : context.Settings.GetRecents())
		{
			auto const found = m_EntriesByKey.find(MakeVisualScriptRecentKey(recent.Type, recent.Settings));
			if (found != m_EntriesByKey.end() &&
			    std::find(m_RecentEntries.begin(), m_RecentEntries.end(), found->second) == m_RecentEntries.end())
			{
				m_RecentEntries.push_back(found->second);
				if (m_RecentEntries.size() == RecentRows)
				{
					break;
				}
			}
		}
	}

	std::optional<VisualScriptPin> VisualScriptPalettePopup::FindContextPin(VisualScriptPaneContext const& context,
	                                                                        VisualScriptLibrary const& library) const
	{
		if (!m_From)
		{
			return std::nullopt;
		}
		return FindVisualScriptCanvasPin(context.Model, &library, m_From->Node, m_From->Pin, m_Direction);
	}

	void VisualScriptPalettePopup::UpdateRanking(VisualScriptPaneContext const& context, VisualScriptLibrary const& library)
	{
		std::vector<std::string> recents;
		for (VisualScriptRecentNode const& recent : context.Settings.GetRecents())
		{
			recents.push_back(MakeVisualScriptRecentKey(recent.Type, recent.Settings));
		}
		bool const contextOn = m_From.has_value() && context.Settings.IsContextPalette();
		bool const hasQuery = m_Query.find_first_not_of(' ') != std::string::npos;
		// What the ranking is made of; a context pin that is gone leaves the rows as they are until one of these changes.
		if (m_RankedGeneration == m_IndexGeneration && m_RankedQuery == m_Query && m_RankedContext == contextOn &&
		    m_RankedRecents == recents)
		{
			return;
		}
		bool const restart = m_RankedQuery != m_Query || m_RankedContext != contextOn;
		m_RankedGeneration = m_IndexGeneration;
		m_RankedQuery = m_Query;
		m_RankedContext = contextOn;
		m_RankedRecents = std::move(recents);
		m_ContextPin = m_From ? FindContextPin(context, library) : std::nullopt;
		std::optional<VisualScriptPaletteContext> rankContext;
		if (contextOn && m_ContextPin)
		{
			rankContext = VisualScriptPaletteContext{*m_ContextPin};
		}
		m_Ranked = hasQuery || rankContext.has_value();
		m_Matches.clear();
		if (m_Ranked)
		{
			m_Matches = RankVisualScriptPalette(m_Index, m_Query, m_RankedRecents, rankContext ? &*rankContext : nullptr, library);
		}
		// The rows are new: none of the last frame's indices stands for the same row.
		m_Rows.clear();
		if (restart)
		{
			m_Highlight = 0;
			m_ScrollToHighlight = true;
		}
	}

	void VisualScriptPalettePopup::UpdateState()
	{
		m_State = {};
		if (!m_Open)
		{
			return;
		}
		m_State.Open = true;
		m_State.Query = m_Query;
		m_State.From = m_From;
		if (m_Ranked)
		{
			m_State.Total = static_cast<uint32_t>(m_Matches.size());
			m_State.Entries.assign(m_Matches.begin(), m_Matches.begin() + std::min<size_t>(m_Matches.size(), m_Limit));
			return;
		}
		m_State.Total = static_cast<uint32_t>(m_RecentEntries.size());
		for (size_t i = 0; i < m_RecentEntries.size() && i < m_Limit; i++)
		{
			VisualScriptPaletteMatch match;
			match.Entry = m_RecentEntries[i];
			m_State.Entries.push_back(std::move(match));
		}
	}

	VisualScriptPaneResult VisualScriptPalettePopup::Draw(VisualScriptPaneContext& context)
	{
		VisualScriptPaneResult result;
		if (!m_Open)
		{
			m_State = {};
			return result;
		}
		ImGuiID const popupId = ImGui::GetID(PopupName);
		// Without the scripting runtime there is nothing to list: the window shows its notice instead.
		Ref<VisualScriptLibrary const> const library = context.Semantics.GetLibrary();
		if (library == nullptr || context.Model.GetStoredDocument() == nullptr)
		{
			if (ImGui::IsPopupOpen(popupId, ImGuiPopupFlags_None))
			{
				ImGui::ClosePopupToLevel(ImGui::GetCurrentContext()->BeginPopupStack.Size, true);
			}
			Close();
			m_State = {};
			return result;
		}

		bool const opening = m_OpenPending;
		if (opening)
		{
			m_OpenPending = false;
			m_FocusSearch = true;
			m_PlaceCursor = true;
			m_Highlight = 0;
			m_Rows.clear();
			VisualScriptView const& view = context.Model.GetView();
			if (m_RequestedAt)
			{
				m_Position = view.GetState().Center;
			}
			if (m_FromRequest)
			{
				m_ScreenPosition = view.ToScreen(m_Position);
			}
			ImGui::OpenPopupEx(popupId, ImGuiPopupFlags_None);
		}
		else if (!ImGui::IsPopupOpen(popupId, ImGuiPopupFlags_None))
		{
			// Closed by a click outside it.
			Close();
			m_State = {};
			return result;
		}

		UpdateIndex(context, library);
		UpdateRecents(context);

		// Normal UI size (popups are UI, not canvas), at the cursor, flipped left or up to stay in the work area.
		float const fontSize = ImGui::GetFontSize();
		ImVec2 const size(WidthInFontSizes * fontSize, HeightInFontSizes * fontSize);
		if (opening)
		{
			ImGuiViewport const* const viewport = ImGui::GetMainViewport();
			ImVec2 const anchor = m_ScreenPosition ? ImVec2(m_ScreenPosition->x, m_ScreenPosition->y) : ImGui::GetIO().MousePos;
			ImVec2 const workMin = viewport->WorkPos;
			ImVec2 const workMax(viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y);
			auto const place = [](float at, float extent, float min, float max)
			{
				float const placed = at + extent > max ? at - extent : at;
				return std::max(min, std::min(placed, max - extent));
			};
			ImGui::SetNextWindowPos(ImVec2(place(anchor.x, size.x, workMin.x, workMax.x), place(anchor.y, size.y, workMin.y, workMax.y)));
		}
		ImGui::SetNextWindowSize(size);
		ImGuiWindowFlags const flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar;
		if (!ImGui::BeginPopupEx(popupId, flags))
		{
			Close();
			m_State = {};
			return result;
		}

		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
		{
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			Close();
			m_State = {};
			return result;
		}

		bool confirmed = false;
		DrawSearch(confirmed);
		if (m_From)
		{
			bool contextOn = context.Settings.IsContextPalette();
			if (ImGui::Checkbox("Context", &contextOn))
			{
				context.Settings.SetContextPalette(contextOn);
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
			{
				ImGui::SetTooltip("List only what connects to the wire's pin");
			}
		}
		UpdateRanking(context, *library);

		// Up and Down move through last frame's rows (the rows are drawn after the field).
		if (m_Move != 0)
		{
			m_Highlight += m_Move;
			m_ScrollToHighlight = true;
			m_Move = 0;
		}
		m_Highlight = std::clamp(m_Highlight, 0, std::max(static_cast<int>(m_Rows.size()) - 1, 0));

		std::optional<uint32_t> chosen;
		ImGuiStyle const& style = ImGui::GetStyle();
		float const detailsHeight = ImGui::GetTextLineHeightWithSpacing() * 3.0f + style.ItemSpacing.y * 2.0f;
		ImGui::BeginChild("##rows", ImVec2(0.0f, -detailsHeight), ImGuiChildFlags_None, ImGuiWindowFlags_NoNavInputs);
		m_Rows.clear();
		if (m_Ranked)
		{
			DrawRankedRows(context, chosen);
		}
		else
		{
			DrawTree(context, *library, opening, chosen);
		}
		m_ScrollToHighlight = false;
		if (m_Rows.empty())
		{
			DrawEmptyList(context);
		}
		ImGui::EndChild();
		m_Highlight = std::clamp(m_Highlight, 0, std::max(static_cast<int>(m_Rows.size()) - 1, 0));
		ImGui::Separator();
		DrawDetails(*library);

		if (confirmed && !chosen && !m_Rows.empty())
		{
			chosen = m_Rows[static_cast<size_t>(m_Highlight)].Entry;
		}
		else if (confirmed)
		{
			// Enter without a row ended the text input: the field gets the keyboard back.
			m_FocusSearch = true;
		}
		if (chosen)
		{
			auto const row = std::find_if(m_Rows.begin(), m_Rows.end(),
			                              [&chosen](Row const& candidate)
			                              {
											  return candidate.Entry == *chosen;
										  });
			VisualScriptPaletteMatch const* const match =
				row != m_Rows.end() && row->Match >= 0 ? &m_Matches[static_cast<size_t>(row->Match)] : nullptr;
			Choose(context, *library, *chosen, match, result);
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			Close();
			m_State = {};
			return result;
		}
		ImGui::EndPopup();
		UpdateState();
		return result;
	}

	void VisualScriptPalettePopup::DrawSearch(bool& confirmed)
	{
		// Characters typed while the field does not have the keyboard (the frames before ImGui gives it) are the palette's.
		ImGuiID const fieldId = ImGui::GetID(SearchName);
		ImGuiIO& io = ImGui::GetIO();
		if (ImGui::GetActiveID() != fieldId && !io.InputQueueCharacters.empty())
		{
			for (ImWchar const character : io.InputQueueCharacters)
			{
				if (character >= 0x20 && character != 0x7F)
				{
					std::array<char, 5> utf8{};
					ImTextCharToUtf8(utf8.data(), character);
					m_Query += utf8.data();
				}
			}
			io.InputQueueCharacters.resize(0);
			m_FocusSearch = true;
			m_PlaceCursor = true;
		}
		// Asked again until the field has the keyboard (a popup's first frame is laid out hidden, and may not take it), and
		// never while it has it.
		if (m_FocusSearch && ImGui::GetActiveID() != fieldId)
		{
			ImGui::SetKeyboardFocusHere();
		}
		ImGui::SetNextItemWidth(-FLT_MIN);
		confirmed = ImGui::InputTextWithHint(SearchName, "Search nodes, variables and events", &m_Query,
		                                     ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
		                                         ImGuiInputTextFlags_CallbackAlways,
		                                     &VisualScriptPalettePopup::OnSearchKey, this);
		if (ImGui::IsItemActive())
		{
			m_FocusSearch = false;
			// Enter right after the field took the keyboard: ImGui routes the key to the field only from the next frame.
			confirmed = confirmed || ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
		}
	}

	void VisualScriptPalettePopup::DrawRankedRows(VisualScriptPaneContext const& context, std::optional<uint32_t>& chosen)
	{
		m_Rows.reserve(m_Matches.size());
		for (size_t i = 0; i < m_Matches.size(); i++)
		{
			m_Rows.push_back({m_Matches[i].Entry, static_cast<int32_t>(i)});
		}
		int const count = static_cast<int>(m_Rows.size());
		m_Highlight = std::clamp(m_Highlight, 0, std::max(count - 1, 0));
		ImGuiListClipper clipper;
		clipper.Begin(count, ImGui::GetTextLineHeightWithSpacing());
		if (m_ScrollToHighlight && count > 0)
		{
			clipper.IncludeItemByIndex(m_Highlight);
		}
		while (clipper.Step())
		{
			for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; index++)
			{
				if (DrawRow(context, m_Rows[static_cast<size_t>(index)], index))
				{
					chosen = m_Rows[static_cast<size_t>(index)].Entry;
				}
			}
		}
	}

	void VisualScriptPalettePopup::DrawEmptyList(VisualScriptPaneContext& context)
	{
		std::string const query = m_Query.find_first_not_of(' ') == std::string::npos ? std::string() : m_Query;
		ImGui::TextDisabled("%s", query.empty() ? "No nodes connect to this pin" : fmt::format("No nodes match '{}'", query).c_str());
		if (m_RankedContext && ImGui::Button("Show all nodes"))
		{
			context.Settings.SetContextPalette(false);
			m_FocusSearch = true;
		}
	}

	void VisualScriptPalettePopup::DrawTree(VisualScriptPaneContext const& context, VisualScriptLibrary const& library, bool appearing,
	                                        std::optional<uint32_t>& chosen)
	{
		bool const recentEmpty = m_RecentEntries.empty();
		if (!recentEmpty)
		{
			if (appearing)
			{
				ImGui::SetNextItemOpen(true);
			}
			if (ImGui::TreeNodeEx("Recent", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth))
			{
				for (uint32_t const entry : m_RecentEntries)
				{
					m_Rows.push_back({entry, -1});
					if (DrawRow(context, m_Rows.back(), static_cast<int>(m_Rows.size()) - 1))
					{
						chosen = entry;
					}
				}
				ImGui::TreePop();
			}
		}

		VisualScriptCategory const& root = library.GetCategories();
		for (std::string_view const name : DocumentCategories)
		{
			bool const libraryHasIt = std::any_of(root.Children.begin(), root.Children.end(),
			                                      [name](VisualScriptCategory const& child)
			                                      {
													  return child.Path == name;
												  });
			auto const entries = m_EntriesByCategory.find(std::string(name));
			if (libraryHasIt || entries == m_EntriesByCategory.end())
			{
				continue;
			}
			// While nothing is recent, the first level is open, and with it the document's own entries.
			if (appearing)
			{
				ImGui::SetNextItemOpen(recentEmpty);
			}
			if (ImGui::TreeNodeEx(std::string(name).c_str(), ImGuiTreeNodeFlags_SpanAvailWidth))
			{
				DrawEntryRows(context, std::string(name), chosen);
				ImGui::TreePop();
			}
		}
		for (VisualScriptCategory const& category : root.Children)
		{
			DrawCategory(context, library, category, appearing, true, chosen);
		}
		// Entries without a category (the actions) close the list.
		DrawEntryRows(context, std::string(), chosen);
	}

	bool VisualScriptPalettePopup::HasEntries(VisualScriptCategory const& category) const
	{
		auto const entries = m_EntriesByCategory.find(category.Path);
		if (entries != m_EntriesByCategory.end() && !entries->second.empty())
		{
			return true;
		}
		return std::any_of(category.Children.begin(), category.Children.end(),
		                   [this](VisualScriptCategory const& child)
		                   {
							   return HasEntries(child);
						   });
	}

	void VisualScriptPalettePopup::DrawCategory(VisualScriptPaneContext const& context, VisualScriptLibrary const& library,
	                                            VisualScriptCategory const& category, bool appearing, bool firstLevel,
	                                            std::optional<uint32_t>& chosen)
	{
		if (!HasEntries(category))
		{
			return;
		}
		if (appearing)
		{
			ImGui::SetNextItemOpen(firstLevel && m_RecentEntries.empty());
		}
		if (!ImGui::TreeNodeEx(category.Name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth))
		{
			return;
		}
		for (VisualScriptCategory const& child : category.Children)
		{
			DrawCategory(context, library, child, appearing, false, chosen);
		}
		DrawEntryRows(context, category.Path, chosen);
		ImGui::TreePop();
	}

	void VisualScriptPalettePopup::DrawEntryRows(VisualScriptPaneContext const& context, std::string const& category,
	                                             std::optional<uint32_t>& chosen)
	{
		auto const entries = m_EntriesByCategory.find(category);
		if (entries == m_EntriesByCategory.end())
		{
			return;
		}
		for (uint32_t const entry : entries->second)
		{
			m_Rows.push_back({entry, -1});
			if (DrawRow(context, m_Rows.back(), static_cast<int>(m_Rows.size()) - 1))
			{
				chosen = entry;
			}
		}
	}

	bool VisualScriptPalettePopup::DrawRow(VisualScriptPaneContext const& context, Row const& row, int index)
	{
		std::span<VisualScriptPaletteEntry const> const entries = m_Index.GetEntries();
		VisualScriptPaletteEntry const& entry = entries[row.Entry];
		VisualScriptPaletteMatch const* const match = row.Match >= 0 ? &m_Matches[static_cast<size_t>(row.Match)] : nullptr;
		bool const highlighted = index == m_Highlight;
		ImGuiStyle const& style = ImGui::GetStyle();
		ImGui::PushID(index);
		ImVec2 const min = ImGui::GetCursorScreenPos();
		float const lineHeight = ImGui::GetTextLineHeight();
		bool const clicked = ImGui::Selectable("##row", highlighted, ImGuiSelectableFlags_None, ImVec2(0.0f, lineHeight));
		ImVec2 const max = ImGui::GetItemRectMax();
		// The mouse points at rows while the search field keeps the keyboard (it is the active item, and the keyboard's
		// navigation highlight is on it).
		ImGuiHoveredFlags const hoverFlags = ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | ImGuiHoveredFlags_NoNavOverride;
		bool const hovered = ImGui::IsItemHovered(hoverFlags);
		ImGuiIO const& io = ImGui::GetIO();
		// The mouse moves the highlight as the keys do, but only when it moves: a mouse resting over the list does not take the
		// row back from the keys.
		if (hovered && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f))
		{
			m_Highlight = index;
		}
		if (highlighted && m_ScrollToHighlight)
		{
			ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
		}

		ImDrawList* const drawList = ImGui::GetWindowDrawList();
		drawList->PushClipRect(min, ImVec2(max.x, max.y), true);
		// The kind's glyph: a square in the kind's header colour.
		float const glyph = std::floor(lineHeight * 0.55f);
		float const glyphTop = min.y + (lineHeight - glyph) * 0.5f;
		drawList->AddRectFilled(ImVec2(min.x, glyphTop), ImVec2(min.x + glyph, glyphTop + glyph),
		                        GetVisualScriptHeaderColor(entry.Kind, GetTopCategory(entry.Category)), glyph * 0.25f);
		float const titleX = min.x + glyph + style.ItemInnerSpacing.x;
		std::span<uint16_t const> const highlights =
			match != nullptr ? std::span<uint16_t const>(match->Highlights) : std::span<uint16_t const>();
		DrawHighlightedText(drawList, ImVec2(titleX, min.y), entry.Title, highlights, ImGui::GetColorU32(ImGuiCol_Text),
		                    ImGui::GetColorU32(ImGuiCol_CheckMark));

		// Right-aligned: in context mode the pin it connects and the conversion, else the subtitle.
		float right = max.x - style.ItemInnerSpacing.x;
		if (match != nullptr && !match->ConnectPin.empty())
		{
			if (match->Converts && m_ContextPin)
			{
				auto const pin =
					std::find_if(entry.Pins.begin(), entry.Pins.end(),
				                 [match, this](VisualScriptPin const& candidate)
				                 {
									 return candidate.Name == match->ConnectPin && candidate.Direction == Opposite(m_Direction);
								 });
				if (pin != entry.Pins.end())
				{
					bool const fromOutput = m_Direction == VisualScriptPinDirection::Out;
					std::string const badge = fmt::format("{} \xE2\x86\x92 {}", fromOutput ? m_ContextPin->Type : pin->Type,
					                                      fromOutput ? pin->Type : m_ContextPin->Type);
					ImVec2 const badgeSize = UI::CalcArrowTextSize(badge);
					float const left = right - badgeSize.x - style.FramePadding.x * 2.0f;
					drawList->AddRectFilled(ImVec2(left, min.y), ImVec2(right, min.y + lineHeight),
					                        MultiplyVisualScriptAlpha(VisualScriptWarningColor, 0.25f), lineHeight * 0.3f);
					UI::DrawArrowText(drawList, ImVec2(left + style.FramePadding.x, min.y), VisualScriptWarningColor, badge);
					right = left - style.ItemInnerSpacing.x;
				}
			}
			std::string const label = "\xE2\x86\x92 " + match->ConnectPin;
			ImVec2 const labelSize = UI::CalcArrowTextSize(label);
			UI::DrawArrowText(drawList, ImVec2(right - labelSize.x, min.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
		}
		else if (!entry.Subtitle.empty())
		{
			float const width = ImGui::CalcTextSize(entry.Subtitle.c_str()).x;
			drawList->AddText(ImVec2(right - width, min.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), entry.Subtitle.c_str());
		}
		drawList->PopClipRect();

		if (hovered && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | hoverFlags))
		{
			DrawPreviewTooltip(context, entry);
		}
		ImGui::PopID();
		return clicked;
	}

	void VisualScriptPalettePopup::DrawDetails(VisualScriptLibrary const& library)
	{
		// What the highlighted row does, whether the keys or the mouse moved the highlight.
		if (m_Rows.empty())
		{
			return;
		}
		std::span<VisualScriptPaletteEntry const> const entries = m_Index.GetEntries();
		VisualScriptPaletteEntry const& entry = entries[m_Rows[static_cast<size_t>(m_Highlight)].Entry];
		ImGui::PushTextWrapPos(0.0f);
		std::string const description = entry.Description.empty() ? entry.Title : entry.Description;
		ImGui::TextUnformatted(description.substr(0, description.find('\n')).c_str());
		ImGui::PopTextWrapPos();
		ImGui::TextDisabled("%s", entry.Category.empty() ? "Actions" : entry.Category.c_str());
		// The pins, each in its type's colour: inputs, then a chevron and the outputs.
		bool first = true;
		for (VisualScriptPinDirection const direction : {VisualScriptPinDirection::In, VisualScriptPinDirection::Out})
		{
			bool firstOfDirection = true;
			for (VisualScriptPin const& pin : entry.Pins)
			{
				if (pin.Direction != direction)
				{
					continue;
				}
				if (!first)
				{
					ImGui::SameLine(0.0f, 0.0f);
					ImGui::TextDisabled("%s", firstOfDirection ? "  \xE2\x80\xBA  " : ", ");
					ImGui::SameLine(0.0f, 0.0f);
				}
				first = false;
				firstOfDirection = false;
				ImGui::PushStyleColor(ImGuiCol_Text, GetVisualScriptTypeColor(pin.Type, &library));
				ImGui::TextUnformatted(pin.Label.empty() ? pin.Name.c_str() : pin.Label.c_str());
				ImGui::PopStyleColor();
			}
		}
		if (first)
		{
			ImGui::TextDisabled("No pins");
		}
	}

	void VisualScriptPalettePopup::DrawPreviewTooltip(VisualScriptPaneContext const& context, VisualScriptPaletteEntry const& entry) const
	{
		bool const reflected =
			entry.NodeType.starts_with("Call:") || entry.NodeType.starts_with("Get:") || entry.NodeType.starts_with("Set:");
		if (!context.DrawNodePreview && !reflected)
		{
			return;
		}
		ImGui::BeginTooltip();
		if (context.DrawNodePreview)
		{
			// The canvas draws the node at 75 % of its size at zoom 1; the space reserved is the node's size at most.
			float const scale = PreviewScale * ImGui::GetStyle().FontScaleDpi;
			ImVec2 const position = ImGui::GetCursorScreenPos();
			ImGui::Dummy(
				ImVec2(VisualScriptCanvasMetrics::MaxNodeWidth * scale * 0.5f,
			           (VisualScriptCanvasMetrics::HeaderHeightWithSubtitle + VisualScriptCanvasMetrics::PinRowHeight * 4.0f) * scale));
			context.DrawNodePreview(entry, ImGui::GetWindowDrawList(), position, scale);
		}
		if (reflected)
		{
			if (context.Services.MonospaceFont != nullptr)
			{
				ImGui::PushFont(context.Services.MonospaceFont, 0.0f);
			}
			ImGui::TextUnformatted(entry.NodeType.c_str());
			if (context.Services.MonospaceFont != nullptr)
			{
				ImGui::PopFont();
			}
		}
		ImGui::EndTooltip();
	}

	void VisualScriptPalettePopup::Choose(VisualScriptPaneContext& context, VisualScriptLibrary const& library, uint32_t entryIndex,
	                                      VisualScriptPaletteMatch const* match, VisualScriptPaneResult& result)
	{
		VisualScriptPaletteEntry const entry = m_Index.GetEntries()[entryIndex];
		if (entry.Action == VisualScriptPaletteAction::GoToNode)
		{
			result.SelectAndFrame = {entry.ExistingNode};
			return;
		}
		VisualScriptEditorModel const& model = context.Model;
		uint32_t const function = model.GetFunction();
		VisualScriptDocument edited = model.GetDisplayDocument();
		std::string description = entry.Action == VisualScriptPaletteAction::AddNode ? "Add " + entry.Title : entry.Title;
		std::vector<uint32_t> select;
		std::string focusSetting;

		auto const fail = [&result, &entry](std::string const& error)
		{
			result.Notes.push_back(fmt::format("Could not add {}: {}", entry.Title, error));
		};

		if (entry.Action == VisualScriptPaletteAction::AddVariable)
		{
			std::string const name =
				VisualScriptEditing::MakeUniqueVisualScriptName(NewVariableName,
			                                                    [&edited](std::string_view candidate)
			                                                    {
																	return VisualScriptEditing::IsVariableNameTaken(edited, candidate);
																});
			Result<uint32_t> const added = VisualScriptEditing::AddVariable(edited, name, context.Settings.GetLastVariableType());
			if (!added)
			{
				fail(added.GetError());
				return;
			}
			description = "Add Variable " + name;
		}
		else if (entry.Action == VisualScriptPaletteAction::AddCommentBox)
		{
			Result<uint32_t> const added = VisualScriptEditing::AddComment(edited, function, NewCommentText, {}, {}, SnapToGrid(m_Position),
			                                                               context.Settings.GetCommentColor());
			if (!added)
			{
				fail(added.GetError());
				return;
			}
			select.push_back(added.GetValue());
		}
		else
		{
			std::string type = entry.NodeType;
			Json settings = entry.Settings.is_object() ? entry.Settings : Json::object();
			if (entry.Action == VisualScriptPaletteAction::AddCustomEvent)
			{
				settings["Name"] = VisualScriptEditing::MakeUniqueVisualScriptName(NewEventName,
				                                                                   [&edited](std::string_view candidate)
				                                                                   {
																					   return VisualScriptEditing::IsCustomEventNameTaken(
																						   edited, candidate);
																				   });
				focusSetting = "Name";
			}
			else if (VisualScriptNodeTemplate const* const kind = library.FindNode(type))
			{
				// A setting the node needs and the entry does not give: Details gives it the keyboard.
				for (VisualScriptSetting const& setting : kind->Settings)
				{
					if (setting.Required && !settings.contains(setting.Name))
					{
						focusSetting = setting.Name;
						break;
					}
				}
			}

			// In context mode the pin that connects sits where the wire was released.
			std::optional<VisualScriptPin> const contextPin = m_From ? FindContextPin(context, library) : std::nullopt;
			std::string connectPin;
			if (contextPin && match != nullptr && !match->ConnectPin.empty())
			{
				connectPin = match->ConnectPin;
			}
			else if (contextPin)
			{
				if (VisualScriptPin const* const loose = FindLooseConnectPin(entry, *contextPin, m_Direction, library))
				{
					connectPin = loose->Name;
				}
			}
			uint32_t const id = VisualScriptEditing::GetLargestVisualScriptID(edited) + 1;
			glm::vec2 position = m_Position;
			if (!connectPin.empty())
			{
				VisualScriptNode probe;
				probe.ID = id;
				probe.Type = type;
				probe.Settings = settings;
				position -=
					MeasurePinAnchor(edited, function, std::move(probe), connectPin, Opposite(m_Direction), library, context.Measure);
			}
			Result<uint32_t> const added = VisualScriptEditing::AddNode(edited, function, type, settings, SnapToGrid(position));
			if (!added)
			{
				fail(added.GetError());
				return;
			}
			uint32_t const node = added.GetValue();
			select.push_back(node);

			if (!connectPin.empty())
			{
				VisualScriptGraph const* const graph = FindGraph(edited, function);
				VisualScriptNodePins const pins = MakeVisualScriptNodePins(model, &library, VisualScriptNewNodePins{node, entry.Pins});
				VisualScriptAnalysis const* const analysis = model.GetAnalysis();
				std::string const fromPin = VisualScriptEditing::ToStoredVisualScriptPin(
					analysis != nullptr ? analysis->FindNode(m_From->Node) : nullptr, m_Direction, m_From->Pin);
				VisualScriptPinReference const from{m_From->Node, fromPin};
				VisualScriptPinReference const to{node, connectPin};
				Result<void> linked;
				if (m_Direction == VisualScriptPinDirection::In)
				{
					// An input: its link is replaced (data), or the new node's exec output joins it.
					if (Result<uint32_t> const connected = VisualScriptEditing::Connect(edited, function, {to, from}, pins); !connected)
					{
						linked = Error{connected.GetError()};
					}
				}
				else if (IsExec(*contextPin))
				{
					// An exec output: the node goes into its link, or takes it when it has none.
					VisualScriptPin const* const next = FindFirstExec(entry.Pins, VisualScriptPinDirection::Out);
					std::optional<VisualScriptLink> const former =
						graph != nullptr ? FindLinkFrom(*graph, from.Node, from.Pin) : std::nullopt;
					if (former && next != nullptr)
					{
						linked = VisualScriptEditing::SpliceNode(edited, function, node, *former, connectPin, next->Name, pins);
					}
					else if (Result<uint32_t> const connected = VisualScriptEditing::Connect(edited, function, {from, to}, pins);
					         !connected)
					{
						linked = Error{connected.GetError()};
					}
				}
				else
				{
					if (Result<uint32_t> const connected = VisualScriptEditing::Connect(edited, function, {from, to}, pins); !connected)
					{
						linked = Error{connected.GetError()};
					}
					else
					{
						// Exec follow-up: a value of an impure node whose flow ends there goes to an impure node, which comes
						// next in the flow too ("Add Set Speed" wires both).
						VisualScriptCanvasModel const& canvas = model.GetCanvas();
						std::optional<uint32_t> const source = canvas.FindNode(from.Node);
						VisualScriptPin const* const execute = FindFirstExec(entry.Pins, VisualScriptPinDirection::In);
						bool const impureSource = source && !canvas.GetNodes()[*source].HasFlag(VisualScriptNodeFlags::Pure);
						if (impureSource && execute != nullptr && graph != nullptr)
						{
							std::vector<VisualScriptNodePin> const sourcePins = pins(from.Node);
							auto const then =
								std::find_if(sourcePins.begin(), sourcePins.end(),
							                 [](VisualScriptNodePin const& pin)
							                 {
												 return pin.Direction == VisualScriptPinDirection::Out && pin.Type == VisualScriptExecType;
											 });
							if (then != sourcePins.end() && !FindLinkFrom(*graph, from.Node, then->Name))
							{
								VisualScriptLink const flow{{from.Node, then->Name}, {node, execute->Name}};
								if (Result<uint32_t> const followed = VisualScriptEditing::Connect(edited, function, flow, pins); !followed)
								{
									linked = Error{followed.GetError()};
								}
							}
						}
					}
				}
				if (!linked)
				{
					fail(linked.GetError());
					return;
				}
			}
		}

		Result<VisualScriptCommitRequest> request = MakeVisualScriptCommitRequest(*model.GetStoredDocument(), std::move(edited),
		                                                                          model.GetAnalysis(), description, context.ScriptName, 0);
		if (!request)
		{
			fail(request.GetError());
			return;
		}
		request.GetValue().Select = std::move(select);
		result.Commit = request.TakeValue();
		result.FocusSetting = std::move(focusSetting);
		// Remembered as it was added: an action by its key, a node by its type and settings.
		bool const isAction = entry.RecentKey.starts_with(VisualScriptPaletteActionKeyPrefix);
		context.Settings.AddRecent(isAction ? VisualScriptRecentNode{entry.RecentKey, Json::object()}
		                                    : VisualScriptRecentNode{entry.NodeType, entry.Settings});
	}
}
