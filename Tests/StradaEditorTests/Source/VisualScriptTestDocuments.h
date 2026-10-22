#pragma once

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptLayout.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

// Checks of the documents the visual script editor writes, for the tests of every part that edits them. Pure: no ImGui,
// no scripting runtime.
namespace Strada::Testing
{
	namespace Detail
	{
		// Whether a pin name of a node addresses a field of a pin that is not split: "<path>.<field>" whose path is not in
		// the node's Split list of the pin's direction. A Flow.Switch's case outputs are pins of their own whatever they
		// hold ("Case:a.b" of a string case), so they never address fields.
		inline bool AddressesUnsplitField(VisualScriptNode const& node, std::string_view pin, bool output)
		{
			size_t const dot = pin.rfind('.');
			if (dot == std::string_view::npos || (output && node.Type == "Flow.Switch" && pin.starts_with("Case:")))
			{
				return false;
			}
			std::vector<std::string> const& split = output ? node.Split.Outputs : node.Split.Inputs;
			return std::find(split.begin(), split.end(), pin.substr(0, dot)) == split.end();
		}

		inline void FindSplitInconsistencies(VisualScriptGraph const& graph, std::string const& path, std::vector<std::string>& found)
		{
			auto const findNode = [&graph](uint32_t id) -> VisualScriptNode const*
			{
				auto const node = std::find_if(graph.Nodes.begin(), graph.Nodes.end(),
				                               [id](VisualScriptNode const& candidate)
				                               {
												   return candidate.ID == id;
											   });
				return node != graph.Nodes.end() ? &*node : nullptr;
			};
			for (VisualScriptNode const& node : graph.Nodes)
			{
				if (!node.Inputs.is_object())
				{
					continue;
				}
				for (auto const& input : node.Inputs.items())
				{
					if (AddressesUnsplitField(node, input.key(), false))
					{
						found.push_back(path + ": node " + std::to_string(node.ID) + " has a literal for " + input.key() +
						                ", a field of an input that is not split");
					}
				}
			}
			for (VisualScriptLink const& link : graph.Links)
			{
				VisualScriptNode const* const from = findNode(link.From.Node);
				if (from != nullptr && AddressesUnsplitField(*from, link.From.Pin, true))
				{
					found.push_back(path + ": a link leaves node " + std::to_string(from->ID) + " at " + link.From.Pin +
					                ", a field of an output that is not split");
				}
				VisualScriptNode const* const to = findNode(link.To.Node);
				if (to != nullptr && AddressesUnsplitField(*to, link.To.Pin, false))
				{
					found.push_back(path + ": a link enters node " + std::to_string(to->ID) + " at " + link.To.Pin +
					                ", a field of an input that is not split");
				}
			}
		}
	}

	// The places a document addresses a field of a struct pin that is not split, which the editor never writes: link
	// endpoints and Inputs keys "<pin>.<field>" whose <pin> (itself a pin path) is not in the node's Split list of that
	// direction, each described with the graph it is in ("Graph", "Functions[0].Graph"). Empty when there is none.
	inline std::vector<std::string> FindVisualScriptSplitInconsistencies(VisualScriptDocument const& document)
	{
		std::vector<std::string> found;
		Detail::FindSplitInconsistencies(document.Graph, "Graph", found);
		for (size_t index = 0; index < document.Functions.size(); index++)
		{
			Detail::FindSplitInconsistencies(document.Functions[index].Graph, "Functions[" + std::to_string(index) + "].Graph", found);
		}
		return found;
	}

	// The document addresses fields only of the pins it splits (FindVisualScriptSplitInconsistencies finds nothing).
	inline void ExpectSplitConsistency(VisualScriptDocument const& document)
	{
		for (std::string const& inconsistency : FindVisualScriptSplitInconsistencies(document))
		{
			FAIL_CHECK(inconsistency);
		}
	}

	// What keeps a document from passing the check every write of a visual script makes before it writes: with its
	// unplaced nodes placed (PlaceUnplacedNodes), it must save and load again strictly, as graph.set reads documents (each
	// repair of the loader, an unknown key included, is an issue), to an equal document. Returns the loader's refusal, its
	// issues ("<path>: <message>") or that the document read back differs; empty when it passes.
	inline std::vector<std::string> FindVisualScriptRoundTripIssues(VisualScriptDocument const& document)
	{
		VisualScriptDocument placed = document;
		PlaceUnplacedNodes(placed);
		DeserializationContext context;
		context.UnknownFields = UnknownFieldPolicy::Warn;
		std::vector<VisualScriptIssue> issues;
		Result<VisualScriptDocument> const read =
			VisualScriptSerializer::DeserializeDocument(VisualScriptSerializer::SerializeDocument(placed), context, &issues);
		if (!read)
		{
			return {"the document does not load: " + read.GetError()};
		}
		std::vector<std::string> found;
		for (VisualScriptIssue const& issue : issues)
		{
			found.push_back(issue.Path + ": " + issue.Message);
		}
		if (found.empty() && read.GetValue() != placed)
		{
			found.push_back("the document loads back changed");
		}
		return found;
	}

	// The document passes the strict check every write makes (FindVisualScriptRoundTripIssues finds nothing).
	inline void ExpectStrictRoundTrip(VisualScriptDocument const& document)
	{
		for (std::string const& issue : FindVisualScriptRoundTripIssues(document))
		{
			FAIL_CHECK(issue);
		}
	}
}
