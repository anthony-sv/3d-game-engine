#pragma once

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Script/VisualScriptLibrary.h"
#include "Strada/Script/VisualScriptLiterals.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// The node palette's index and ranking: what the palette of the visual script editor offers (the node library's kinds, their
// typed expansions, the document's own variables, functions and custom events, and a few actions) and how a query ranks it.
// VisualScriptLibrary::Search, which graph.library answers with, stays the plain substring filter it is: the palette keeps
// its own index. No ImGui; main thread or any other, one thread per index.
namespace Strada
{
	// What choosing a palette entry does.
	enum class VisualScriptPaletteAction : uint8_t
	{
		// Adds a node of NodeType with the entry's Settings.
		AddNode = 0,
		// Frames and selects ExistingNode, the event node the entry would duplicate.
		GoToNode,
		AddCommentBox,
		AddReroute,
		AddCustomEvent,
		AddVariable
	};

	struct VisualScriptPaletteEntry
	{
		std::string Title;
		// Empty when none.
		std::string Subtitle;
		// Slash-separated ("Components/Rigid Body"); the document's entries are in "Variables", "Functions" and "Events";
		// empty for the actions.
		std::string Category;
		// The node type the entry adds (the actions that add a node name it too: "Reroute", "Event.Custom"); empty for the
		// actions that add none.
		std::string NodeType;
		std::string Description;
		std::vector<std::string> Keywords;
		// The settings the node starts with: those of a typed expansion ("Get Component: Rigid Body" has TypeArguments) or of
		// a document entry ("Get Speed" names its variable); an empty object for the library's kinds.
		Json Settings = Json::object();
		VisualScriptNodeKind Kind = VisualScriptNodeKind::Call;
		// The pins of the node the entry adds, typed as far as the entry decides them: what context filtering connects to.
		std::vector<VisualScriptPin> Pins;
		VisualScriptPaletteAction Action = VisualScriptPaletteAction::AddNode;
		// GoToNode: the event node of the shown graph the entry would duplicate.
		uint32_t ExistingNode = 0;
		// The document's own variables, functions and custom events.
		bool FromDocument = false;
		// What the palette remembers recent entries by: NodeType followed by the compact JSON of Settings
		// ("Flow.Branch{}", "Variable.Get{\"Variable\":12}"); "Action:<action>" for the actions that add no node of their own
		// settings (comment boxes, reroutes, custom events, variables). A GoToNode entry keeps the key of the node it would add.
		// The actions' keys must stay apart from NodeType + "{}": Add Reroute and Add Custom Event would otherwise share the
		// keys of the library's Reroute and Event.Custom entries. MakeVisualScriptRecentKey turns a stored record into a key.
		std::string RecentKey;
	};

	// The dragged pin of a wire released on empty canvas, with its analyzed type (a wildcard's resolved type when it has
	// one): the palette then lists what connects to it.
	struct VisualScriptPaletteContext
	{
		VisualScriptPin Pin;
	};

	// The prefix of the actions' RecentKey values.
	inline constexpr std::string_view VisualScriptPaletteActionKeyPrefix = "Action:";

	// The RecentKey of a node added from the palette, as the view settings keep it (VisualScriptRecentNode: a type and the
	// settings it was added with): type followed by the compact JSON of settings. An action is kept as its RecentKey with
	// empty settings ({ "Type": "Action:AddReroute", "Settings": {} }), and a type that starts with "Action:" is that key as
	// it is.
	[[nodiscard]] std::string MakeVisualScriptRecentKey(std::string_view type, Json const& settings);

	struct VisualScriptPaletteMatch
	{
		// The entry's index in VisualScriptPaletteIndex::GetEntries(): valid until the index's next Rebuild, or the next
		// SetDocument that changes what it reads. Rank again after either; never keep matches across them.
		uint32_t Entry = 0;
		int32_t Score = 0;
		// The bytes of the entry's title that the query matched, ascending.
		std::vector<uint16_t> Highlights;
		// Context mode: the entry's pin that connects to the dragged pin.
		std::string ConnectPin;
		// Context mode: the link converts its value implicitly (an int into a float pin, any value into a string pin).
		bool Converts = false;

		bool operator==(VisualScriptPaletteMatch const& other) const = default;
	};

	// What a ranking did, which tests check to keep its cost proportional to the entries.
	struct VisualScriptPaletteRankStats
	{
		// The entries the ranking considered: every entry of the index.
		uint32_t EntriesScored = 0;
		// The subsequence searches it ran: at most one per token of an entry no other kind of match found.
		uint32_t SubsequenceSearches = 0;
	};

	class VisualScriptPaletteIndex;

	// Ranks the index's entries for a query typed into the palette, best first; entries of equal score keep the index's
	// order. The query's words (split at spaces, at most 8, ASCII letters compared in any case) must each match the entry; a
	// word scores the best of: the title starting with it (700), a word of the title starting with it (600, "force" in "Add
	// Force"), it starting the initials of the title's words from one of them (500, "af" for Add Force), the title holding
	// it (400), a keyword starting with it or holding it (350, 300), a word of the subtitle starting with it (250), a word of
	// the category starting with it (200), the node type holding it (150, "iskeydown"), and, for words of 3 characters or
	// more, the title holding its characters in order (100 less 5 per gap, at least 20). An entry scores the sum of its words,
	// 1000 more when the whole query is its title, 300 less 15 per place for the 20 most recent entries (recents: RecentKey
	// values, most recent first), 50 more for the document's entries, and 2 less per byte of its title.
	//
	// With a context (a wire released on empty canvas), only the entries with a pin of the other direction that connects to
	// the dragged pin are listed (VisualScriptLibrary::CanConnect: the dragged output to the entry's input, or the entry's
	// output to the dragged input), never through a wildcard only the entry's settings decide; an entry with a pin of exactly
	// the dragged type scores 250 more, one that connects through an implicit conversion 100 more. Convert's typed expansions
	// ("To String", "To Int") are listed only for the conversions every value has (to string) and numbers have (to another
	// numeric type), and without a query only when no other entry has a pin of exactly the dragged type: they bridge what
	// nothing takes as it is, and a query asks for them by name. GoToNode entries are not listed. Without a query, a context
	// lists every entry it lets through; without either, nothing is ranked (the palette shows its recent entries and
	// categories). The node type and keywords are compared without the scripting API's namespace ("Strada."), which every
	// reflected type holds.
	[[nodiscard]] std::vector<VisualScriptPaletteMatch>
	RankVisualScriptPalette(VisualScriptPaletteIndex const& index, std::string_view query, std::span<std::string const> recents,
	                        VisualScriptPaletteContext const* context, VisualScriptLibrary const& library,
	                        VisualScriptPaletteRankStats* stats = nullptr);

	// The entries the palette offers for the shown graph: every node kind of the library and its typed expansions, the
	// document's variables (and the shown function's locals), functions and custom events, and the actions. Entries the
	// shown graph cannot use are left out (events and ticking nodes in functions' graphs; impure nodes in pure functions';
	// a second Function.Entry; Function.Entry and Function.Return in the event graph), and an event the event graph handles
	// already (a unique event) is offered as GoToNode. Entries come in this order, which equal rankings keep: the library's
	// kinds in library order, each followed by its expansions, then the document's entries, then the actions.
	class VisualScriptPaletteIndex
	{
	public:
		// Reads the library's entries, when the library changes; the index keeps no reference to it. Until it is called the
		// index offers only the document's entries and the actions.
		void Rebuild(VisualScriptLibrary const& library);
		// Reads what the document offers for the graph shown (0 for the event graph, else a function's ID), when the document
		// or the shown graph changes; a function the document does not have is shown as the event graph is. Nothing changes
		// when what the palette reads from the document is unchanged.
		void SetDocument(VisualScriptDocument const& document, uint32_t function);

		// Valid, as are the entries' indices, until the next Rebuild or the next SetDocument that changes what the index reads
		// (either composes the entries again): never keep the span or an index across them.
		std::span<VisualScriptPaletteEntry const> GetEntries() const { return m_Entries; }

	private:
		// An entry's texts as the ranking compares them: ASCII letters in lower case, with where their words start.
		struct SearchKey
		{
			std::string Title;
			std::vector<uint16_t> TitleWords;
			// The first character of each word of the title.
			std::string Initials;
			std::string Subtitle;
			std::vector<uint16_t> SubtitleWords;
			std::string Category;
			std::vector<uint16_t> CategoryWords;
			// Without the scripting API's namespace ("strada."), which every reflected type and API type holds.
			std::vector<std::string> Keywords;
			std::string NodeType;
			// A bit for each byte value (modulo 64) the texts above hold: a word with a byte none of them holds matches nothing.
			uint64_t Characters = 0;
		};

		// What context filtering and the graph's rules need to know of an entry besides its pins.
		struct EntryFacts
		{
			// One per pin: a wildcard of a generic kind whose type only its settings decide (a variable's Value, a Literal's,
			// a Convert's Result, a Make's or a Break's Value, a Switch's Selection, a TypeArguments call's), which would
			// accept every type: context filtering never connects to it.
			std::vector<bool> SettingsDependent;
			// Convert's typed expansions: the type converted to. Context mode lists them only for the conversions every value
			// has (to string) and numbers have (to another numeric type).
			std::string ConvertTo;
			bool Pure = true;
			bool Event = false;
			bool Ticks = false;
			bool Entry = false;
			bool Return = false;
			// The event graph handles it at most once: a standard event, or an input event of one key or button.
			bool Unique = false;
			// Unique: what tells its node apart, the setting (Key or Button) and its canonical literal; empty for events
			// unique by type.
			std::string UniqueSetting;
			std::string UniqueValue;
		};

		struct IndexedEntry
		{
			VisualScriptPaletteEntry Entry;
			EntryFacts Facts;
			SearchKey Key;
		};

		struct EventNode
		{
			uint32_t ID = 0;
			std::string Type;
			Json Settings;

			bool operator==(EventNode const& other) const = default;
		};

		// What the palette reads from a document.
		struct DocumentFacts
		{
			// The function shown; 0 for the event graph (or a function the document does not have).
			uint32_t Function = 0;
			bool Pure = false;
			bool HasEntry = false;
			bool HasReturn = false;
			std::vector<VisualScriptVariable> Variables;
			std::vector<VisualScriptLocal> Locals;
			struct FunctionFacts
			{
				uint32_t ID = 0;
				std::string Name;
				bool Pure = false;
				std::vector<VisualScriptParameter> Inputs;
				std::vector<VisualScriptParameter> Outputs;

				bool operator==(FunctionFacts const& other) const = default;
			};
			std::vector<FunctionFacts> Functions;
			struct CustomEvent
			{
				std::string Name;
				// As stored; null when the event stores none or none of their form.
				Json Parameters;
				std::vector<VisualScriptParameter> ParameterList;

				bool operator==(CustomEvent const& other) const = default;
			};
			std::vector<CustomEvent> CustomEvents;
			// The event graph's event nodes, which unique events go to; empty while a function's graph is shown.
			std::vector<EventNode> EventNodes;

			bool operator==(DocumentFacts const& other) const = default;
		};

		static SearchKey MakeKey(VisualScriptPaletteEntry const& entry);
		void Compose();
		void AddDocumentEntries();
		void AddActions();
		void Add(VisualScriptPaletteEntry entry, EntryFacts facts);
		bool IsShown(EntryFacts const& facts) const;
		std::string TitleOfType(std::string_view type) const;
		// The canonical literal of an input event's setting value (an enumerator's name), as UniqueValue holds it; empty
		// when the value is none of the enum's.
		std::string CanonicalEventValue(std::string const& type, Json const& value) const;

		friend std::vector<VisualScriptPaletteMatch> RankVisualScriptPalette(VisualScriptPaletteIndex const& index, std::string_view query,
		                                                                     std::span<std::string const> recents,
		                                                                     VisualScriptPaletteContext const* context,
		                                                                     VisualScriptLibrary const& library,
		                                                                     VisualScriptPaletteRankStats* stats);

		// From the library (Rebuild).
		std::vector<IndexedEntry> m_LibraryEntries;
		std::unordered_map<std::string, std::string> m_TypeTitles;
		// The input events' settings that tell their nodes apart (Key, Button) with their enums, by node type.
		struct EventSetting
		{
			std::string Name;
			VisualScriptEnumInfo Enum;
		};
		std::unordered_map<std::string, EventSetting> m_EventSettings;
		std::vector<VisualScriptPin> m_ReroutePins;
		std::vector<VisualScriptPin> m_CustomEventPins;
		// From the document (SetDocument).
		DocumentFacts m_Document;
		// What GetEntries returns, with the keys and facts of each entry at the same index.
		std::vector<VisualScriptPaletteEntry> m_Entries;
		std::vector<SearchKey> m_Keys;
		std::vector<EntryFacts> m_Facts;
	};

}
