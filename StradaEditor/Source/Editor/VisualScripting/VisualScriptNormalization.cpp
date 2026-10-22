#include "Editor/VisualScripting/VisualScriptNormalization.h"

#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/Result.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Strada
{
	namespace
	{
		std::vector<VisualScriptGraph*> GetGraphs(VisualScriptDocument& document)
		{
			std::vector<VisualScriptGraph*> graphs;
			graphs.reserve(document.Functions.size() + 1);
			for (VisualScriptFunction& function : document.Functions)
			{
				graphs.push_back(&function.Graph);
			}
			graphs.push_back(&document.Graph);
			return graphs;
		}

		// The new path of a pin path of the node (a pin's name, or a field pin of a split pin: "Hit.Point"), or nothing when it
		// keeps its path. A pin's rename renames the paths of its fields with it. A path that names a renamed pin is that
		// pin's (an exact name wins over a split reading), else it is a field of the longest renamed pin it continues with
		// a '.'. The renames apply at once: a pin renamed to the former name of another one does not take that one's links.
		std::optional<std::string> RenamePath(std::span<VisualScriptPinRename const> renames, VisualScriptPinDirection direction,
		                                      std::string const& path)
		{
			VisualScriptPinRename const* parent = nullptr;
			for (VisualScriptPinRename const& rename : renames)
			{
				if (rename.Direction != direction)
				{
					continue;
				}
				if (rename.From == path)
				{
					return rename.To;
				}
				bool const isField = path.size() > rename.From.size() && path.starts_with(rename.From) && path[rename.From.size()] == '.';
				if (isField && (parent == nullptr || rename.From.size() > parent->From.size()))
				{
					parent = &rename;
				}
			}
			if (parent == nullptr)
			{
				return std::nullopt;
			}
			return parent->To + path.substr(parent->From.size());
		}

		// A field stays behind its pin, renamed with it; of entries that end up the same, the first stays (the loader drops
		// the others).
		void RenameSplitEntries(std::vector<std::string>& entries, std::span<VisualScriptPinRename const> renames,
		                        VisualScriptPinDirection direction)
		{
			std::vector<std::string> renamed;
			renamed.reserve(entries.size());
			for (std::string& entry : entries)
			{
				std::optional<std::string> path = RenamePath(renames, direction, entry);
				std::string name = path ? std::move(*path) : std::move(entry);
				if (std::find(renamed.begin(), renamed.end(), name) == renamed.end())
				{
					renamed.push_back(std::move(name));
				}
			}
			entries = std::move(renamed);
		}

		void RenamePins(VisualScriptGraph& graph, VisualScriptNode& node, std::span<VisualScriptPinRename const> renames)
		{
			for (VisualScriptLink& link : graph.Links)
			{
				if (link.To.Node == node.ID)
				{
					if (std::optional<std::string> path = RenamePath(renames, VisualScriptPinDirection::In, link.To.Pin))
					{
						link.To.Pin = std::move(*path);
					}
				}
				if (link.From.Node == node.ID)
				{
					if (std::optional<std::string> path = RenamePath(renames, VisualScriptPinDirection::Out, link.From.Pin))
					{
						link.From.Pin = std::move(*path);
					}
				}
			}

			// In their stored order; a renamed input replaces one stored under its new name, which the old node kind did not
			// use.
			Json inputs = Json::object();
			for (auto const& [name, value] : node.Inputs.items())
			{
				if (std::optional<std::string> const path = RenamePath(renames, VisualScriptPinDirection::In, name))
				{
					inputs[*path] = value;
				}
				else if (!inputs.contains(name))
				{
					inputs[name] = value;
				}
			}
			node.Inputs = std::move(inputs);

			RenameSplitEntries(node.Split.Inputs, renames, VisualScriptPinDirection::In);
			RenameSplitEntries(node.Split.Outputs, renames, VisualScriptPinDirection::Out);
		}

		// Of links that are the same, the first stays (the loader drops the others).
		void DropRepeatedLinks(VisualScriptGraph& graph)
		{
			std::set<std::tuple<uint32_t, std::string, uint32_t, std::string>> seen;
			std::erase_if(graph.Links,
			              [&seen](VisualScriptLink const& link)
			              {
							  return !seen.emplace(link.From.Node, link.From.Pin, link.To.Node, link.To.Pin).second;
						  });
		}
	}

	std::vector<std::string> RecordFormerNames(VisualScriptDocument& document, VisualScriptDocument const& stored)
	{
		std::unordered_map<uint32_t, std::string const*> storedNames;
		for (VisualScriptVariable const& variable : stored.Variables)
		{
			storedNames.emplace(variable.ID, &variable.Name);
		}
		// A former name is no variable's current name and no other former name in the document.
		std::unordered_set<std::string> taken;
		for (VisualScriptVariable const& variable : document.Variables)
		{
			taken.insert(variable.Name);
			taken.insert(variable.FormerNames.begin(), variable.FormerNames.end());
		}

		std::vector<std::string> warnings;
		for (VisualScriptVariable& variable : document.Variables)
		{
			auto const storedName = storedNames.find(variable.ID);
			if (storedName == storedNames.end() || *storedName->second == variable.Name)
			{
				continue;
			}
			std::string const formerName = *storedName->second;
			if (!taken.insert(formerName).second)
			{
				continue;
			}
			variable.FormerNames.push_back(formerName);
			if (variable.FormerNames.size() > VisualScriptSerializer::MaxFormerNames)
			{
				taken.erase(variable.FormerNames.front());
				variable.FormerNames.erase(variable.FormerNames.begin());
			}
			warnings.push_back(fmt::format("variable renamed: {} → {} ({} is kept in FormerNames)", formerName, variable.Name, formerName));
		}
		return warnings;
	}

	std::vector<std::string> ApplyCanonicalNodeTypes(VisualScriptDocument& document, VisualScriptAnalysis const& analysis)
	{
		// Node IDs are unique in a document.
		std::unordered_map<uint32_t, std::pair<VisualScriptGraph*, size_t>> places;
		for (VisualScriptGraph* graph : GetGraphs(document))
		{
			for (size_t index = 0; index < graph->Nodes.size(); index++)
			{
				places.emplace(graph->Nodes[index].ID, std::pair<VisualScriptGraph*, size_t>(graph, index));
			}
		}

		std::vector<std::string> warnings;
		std::unordered_set<VisualScriptGraph*> renamedPins;
		for (VisualScriptAnalyzedNode const& analyzed : analysis.Nodes)
		{
			bool const inexact =
				analyzed.Resolved == VisualScriptResolution::Redirect || analyzed.Resolved == VisualScriptResolution::Unique;
			auto const place = places.find(analyzed.ID);
			if (!inexact || analyzed.CanonicalType.empty() || place == places.end())
			{
				continue;
			}
			VisualScriptGraph* const graph = place->second.first;
			VisualScriptNode& node = graph->Nodes[place->second.second];
			// Only what this document stores: an analysis of another version of it changes nothing.
			if (node.Type != analyzed.Type || node.Type == analyzed.CanonicalType)
			{
				continue;
			}
			std::string const previous = std::exchange(node.Type, analyzed.CanonicalType);
			if (!analyzed.PinRenames.empty())
			{
				RenamePins(*graph, node, analyzed.PinRenames);
				renamedPins.insert(graph);
			}
			warnings.push_back(fmt::format("node updated: {} → {}", previous, node.Type));
		}
		for (VisualScriptGraph* graph : renamedPins)
		{
			DropRepeatedLinks(*graph);
		}
		return warnings;
	}
}
