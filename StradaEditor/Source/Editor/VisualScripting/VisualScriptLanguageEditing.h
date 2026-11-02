#pragma once

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The edits of the language's settings that rename or remove pins (spec-slice3 §6.3): an Array.Make's elements and an
// Expression's formula. Pure functions of documents under VisualScriptEditing.h's rules: each checks before it changes,
// leaves the document unchanged when it fails and names pins as the document stores them. No ImGui, no state; any thread.
namespace Strada::VisualScriptEditing
{
	// The most elements an Array.Make has (its Count's Max in the node library).
	inline constexpr int MaxArrayElements = 64;
	// The most bytes of a formula the scripting runtime reads; a longer one has no inputs.
	inline constexpr size_t MaxFormulaBytes = 4096;
	// The most inputs a formula has: later names are none.
	inline constexpr size_t MaxFormulaInputs = 32;

	// The input pin of an Array.Make's element at an index: "Element0", "Element1", ...
	[[nodiscard]] std::string GetVisualScriptArrayElementPin(int index);

	// Gives an Array.Make count elements (0 to 64) and renumbers Element<k> links, Inputs keys and split paths exactly as
	// SetSequenceOutputs renumbers Then<k>: without insertAt or removeAt the elements from count on go; insertAt inserts
	// count minus the current count elements at that index (the later elements move up: Element1 becomes Element2); removeAt
	// removes the current count minus count elements from that index (their links, literals and split pins go, the later
	// ones move down). Pins of elements the node did not have stay as they are. Returns how many links it removed.
	[[nodiscard]] Result<uint32_t> SetArrayElements(VisualScriptDocument& document, uint32_t node, int count, std::optional<int> insertAt,
	                                                std::optional<int> removeAt);

	// The input names of a formula by §3.3.1's input rule, in order of first use, each once: the pins the scripting runtime
	// gives an Expression with that Formula (identifiers by §3.3.2's lexer, which goes on after characters that are no
	// token). An identifier is an input unless a '(' follows it, a '.' comes before it, it is true, false, pi or tau, it is
	// longer than 64 bytes, or it is a reserved pin name or Result; the first MaxFormulaInputs names are the inputs, and a
	// formula over MaxFormulaBytes has none. Pure; any thread. Pinned to the VM by Tests/Data/VisualScripting/Formulas.json.
	[[nodiscard]] std::vector<std::string> ListVisualScriptFormulaInputs(std::string_view formula);

	// What SetFormula did to the node's input pins.
	struct VisualScriptFormulaChange
	{
		// (old, new), in input order.
		std::vector<std::pair<std::string, std::string>> RenamedInputs;
		uint32_t RemovedLinks = 0;
	};

	// Sets an Expression's Formula and updates the links, Inputs literals and split paths of its input pins as
	// SetFunctionSignature updates a function's: an input whose name the new formula keeps keeps them; when the formula's
	// input list keeps its length, the inputs whose names changed are renamed in order, with them; those of every other
	// input that went are removed. The input lists come from ListVisualScriptFormulaInputs of the node's current formula (none
	// when it has no Formula string) and of formula. Fails for a node that is not an Expression and for a formula over
	// MaxFormulaBytes or that is not UTF-8.
	[[nodiscard]] Result<VisualScriptFormulaChange> SetFormula(VisualScriptDocument& document, uint32_t node, std::string formula);
}
