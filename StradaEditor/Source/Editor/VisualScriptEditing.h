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
	std::vector<std::string> RecordFormerNames(VisualScriptDocument& document, VisualScriptDocument const& stored);

	// Stores the nodes the analysis of the document resolved inexactly (through a redirect of an API member that was renamed
	// or changed its signature, or as the only overload of their member with their pins) under their canonical types. The
	// links of their renamed pins and the inputs stored for them are renamed too (a renamed input replaces one that already
	// has its new name), and a link that then repeats another one is dropped. Warnings read "node updated: <old> → <new>".
	std::vector<std::string> ApplyCanonicalNodeTypes(VisualScriptDocument& document, VisualScriptAnalysis const& analysis);
}
