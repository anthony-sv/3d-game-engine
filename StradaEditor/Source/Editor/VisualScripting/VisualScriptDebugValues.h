#pragma once

#include "Editor/VisualScripting/VisualScriptCanvasModel.h"

#include "Strada/Core/UUID.h"
#include "Strada/Script/VisualScriptDebugger.h"
#include "Strada/Script/VisualScriptLiterals.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// How the editor shows a value of a running or stopped graph (spec-slice4 §13.4): one pure formatter for every place that shows
// one (My Script, the Debugger panel, the canvas's value pills, receipts), from the runtime's encoding (spec-slice4 §5.4), which
// formats nothing for display. Pure; main thread only.
namespace Strada
{
	// Every fixed text the formatter writes, for the glyph test (VisualScriptTextTests). "\xE2\x80\x94" is the em dash,
	// "\xE2\x80\xA6" the ellipsis; the arrow of Edited is drawn as a shape (UI::DrawArrowText).
	namespace VisualScriptDebugValueTexts
	{
		inline constexpr std::string_view NotComputed = "\xE2\x80\x94";
		inline constexpr std::string_view NotComputedTip = "Not computed: this execution has not run the node yet";
		inline constexpr std::string_view EarlierTip = "Computed earlier, not by this execution";
		inline constexpr std::string_view UnknownTip = "Computed by a run that was not traced; it may be older";
		inline constexpr std::string_view ChangedElsewhere = "changed elsewhere";
		inline constexpr std::string_view Edited = "edited: ";
		inline constexpr std::string_view Arrow = " \xE2\x86\x92 ";
		inline constexpr std::string_view None = "None";
		inline constexpr std::string_view Ellipsis = "\xE2\x80\xA6";
		inline constexpr std::string_view More = " more";
		inline constexpr std::string_view Characters = " characters";
		inline constexpr std::string_view Newline = "\\n";

		inline constexpr std::array<std::string_view, 12> All = {NotComputed,      NotComputedTip, EarlierTip, UnknownTip,
		                                                         ChangedElsewhere, Edited,         Arrow,      None,
		                                                         Ellipsis,         More,           Characters, Newline};
	}

	enum class VisualScriptValueCellKind : uint8_t
	{
		Text = 0,
		// A colour: drawn as a swatch of Color, without text.
		Swatch,
		// An entity (also a component's or a script instance's): its name, which a click selects.
		Entity,
		// Not computed: "—".
		None
	};

	// A value as a cell shows it.
	struct VisualScriptValueCell
	{
		VisualScriptValueCellKind Kind = VisualScriptValueCellKind::Text;
		// What the cell shows, on one line: an entity's name, "[3] 1, 2, 3", "\"Hello\"", "—".
		std::string Text;
		// Byte offsets in Text where a newline shown as the two characters \n starts: drawn dimmed.
		std::vector<uint32_t> Escapes;
		// The full value: every array element (up to the 32 the runtime sends, then "… 968 more"), a string's lines, an entity's
		// name with its ID ("Enemy 3 (1234)"), and why a value is dimmed.
		std::string Tooltip;
		// Swatch: sRGB as chips encode colours (IM_COL32 layout).
		std::optional<uint32_t> Color;
		// Entity: what a click selects.
		UUID Entity = UUID::Invalid();
		// Computed earlier, or by a run that was not traced (spec-slice4 §5.2); the tooltip says which.
		bool Dimmed = false;
		// "changed elsewhere", "edited: 10 → 50" (drawn with UI::DrawArrowText), or empty.
		std::string Note;

		bool operator==(VisualScriptValueCell const& other) const = default;
	};

	// Formats a value of the runtime's encoding: colours as swatches, entities by name (describeEntity names an entity of the
	// running scene, "Missing (1234…)" when it is absent; it is not called for None), arrays as "[n] a, b, c", strings quoted on
	// one line (a newline shown as the two characters \n, dimmed), enums and assets as their literals' texts
	// (FormatVisualScriptLiteral at 3 significant digits; the tooltip at 6), the runtime's Text otherwise (non-finite numbers,
	// objects without a literal form), and "—" for a value not computed. library, enums and assetName may be empty: types are
	// then read from the literals alone.
	[[nodiscard]] VisualScriptValueCell FormatVisualScriptDebugValue(VisualScriptDebugValue const& value,
	                                                                 VisualScriptLibrary const* library,
	                                                                 VisualScriptEnumLookup const& enums,
	                                                                 std::function<std::string(UUID)> const& describeEntity,
	                                                                 VisualScriptAssetNameLookup const& assetName);
}
