#pragma once

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <string>
#include <vector>

// What the editor does to a visual script document it is given to write (graph.set) or to check (graph.check), besides
// checking its structure: plain functions of documents. Each returns a warning for every change it makes, which automation
// reports. They keep no state.
namespace Strada
{
	// Keeps the values components store for variables renamed since the stored document: a variable the stored document has
	// under another name (by ID) gets that name appended to its FormerNames, under which scenes and the inspector still apply
	// stored values; the oldest is dropped beyond VisualScriptSerializer::MaxFormerNames. A name that is a current name of a
	// variable, or a former name in the document already, is not added, so the document stays valid. Warnings read
	// "variable renamed: <old> → <new> (<old> is kept in FormerNames)".
	// In a library (VisualScriptDocumentKind::Library), whose functions graphs of other files call, keeps those calls
	// working the same way: for each function the stored document has (by ID) and each of its parameter lists (Inputs,
	// Outputs) that kept its length, a parameter named otherwise at its index (SetFunctionSignature's in-order rule) gets
	// the stored name appended to its FormerNames, under the same rules within the list. Warnings read "parameter renamed:
	// <function>'s <old> → <new> (<old> is kept in FormerNames)", after the variables'. In every document a former name of a
	// parameter that a parameter of its list has now is removed.
	std::vector<std::string> RecordFormerNames(VisualScriptDocument& document, VisualScriptDocument const& stored);

	// Stores the nodes the analysis of the document resolved inexactly (through a redirect of an API member that was renamed
	// or changed its signature, or as the only overload of their member with their pins) under their canonical types. The
	// links of their renamed pins, the inputs stored for them and their Split entries are renamed too, with the paths of the
	// pins' fields ("Force.X" follows Force; "Forces.X" does not): a renamed input replaces one that already has its new
	// name, and a link or a Split entry that then repeats another one is dropped. Warnings read
	// "node updated: <old> → <new>".
	// A node it resolved exactly whose links, inputs or Split name pins under a former name (a call of a library function
	// whose parameter was renamed) keeps its type and has those pins renamed the same way, with a warning per renamed pin:
	// "pin renamed: <title> (node <ID>) <old> → <new>".
	std::vector<std::string> ApplyCanonicalNodeTypes(VisualScriptDocument& document, VisualScriptAnalysis const& analysis);
}
