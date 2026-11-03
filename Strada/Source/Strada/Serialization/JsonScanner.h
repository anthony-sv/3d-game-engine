#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace Strada
{
	// A member of a JSON object text's top level. Ranges are [offset, length) in the text scanned.
	struct JsonScanMember
	{
		// The key's bytes between its quotes as written: escapes are not decoded, so "Nod\u0065s" is not "Nodes".
		std::string_view Key;
		// The value, from its first character to its last (whitespace around it excluded).
		std::pair<uint32_t, uint32_t> Value;
		// When the value is an array: each element alike, in order. Empty for any other value and for an empty array.
		std::vector<std::pair<uint32_t, uint32_t>> Elements;
	};

	// The most brackets ScanJsonTopLevel lets be open at once: ParseJson's MaxJsonDepth, so a text the scanner delimits
	// never fails ParseJson for its nesting alone.
	inline constexpr size_t MaxJsonScanDepth = 256;

	// The byte ranges of a JSON text's top-level members and of the elements of the arrays among them, found by walking strings
	// (with escapes), brackets and separators without building values. The text must be one object, with only JSON whitespace
	// (space, tab, line feed, carriage return) around it. Fails at the first character the grammar does not allow in what it
	// walks: the top level's structure, every string's escapes and control characters, every element's first character,
	// bracket pairs, characters outside strings that no JSON token has, and nesting deeper than MaxJsonScanDepth. Element
	// interiors are only delimited, never validated (an element may still fail to parse, and strings' UTF-8 is not checked),
	// and duplicate keys are listed as they occur. Texts of 4 GiB or more fail. Pure; any thread.
	[[nodiscard]] std::optional<std::vector<JsonScanMember>> ScanJsonTopLevel(std::string_view text);
}
