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

		// Whether a pin path is the pin's name or the path of one of its fields.
		bool IsPathOf(std::string_view path, std::string_view pin)
		{
			return path == pin || (path.size() > pin.size() && path.starts_with(pin) && path[pin.size()] == '.');
		}

		// Whether the node's links, inputs or Split name the pin of the rename (or a field of it).
		bool UsesPin(VisualScriptGraph const& graph, VisualScriptNode const& node, VisualScriptPinRename const& rename)
		{
			bool const input = rename.Direction == VisualScriptPinDirection::In;
			bool const linked = std::any_of(graph.Links.begin(), graph.Links.end(),
			                                [&node, &rename, input](VisualScriptLink const& link)
			                                {
												VisualScriptPinReference const& end = input ? link.To : link.From;
												return end.Node == node.ID && IsPathOf(end.Pin, rename.From);
											});
			if (linked)
			{
				return true;
			}
			if (input)
			{
				for (auto const& item : node.Inputs.items())
				{
					if (IsPathOf(item.key(), rename.From))
					{
						return true;
					}
				}
			}
			std::vector<std::string> const& split = input ? node.Split.Inputs : node.Split.Outputs;
			return std::any_of(split.begin(), split.end(),
			                   [&rename](std::string const& path)
			                   {
								   return IsPathOf(path, rename.From);
							   });
		}

		// Records the renames of one list of a function's parameters (Inputs or Outputs) as former names, by the in-order
		// rule of SetFunctionSignature: only when the list kept its length, a parameter whose name differs from the stored
		// one at its index was renamed.
		void RecordParameterFormerNames(std::string const& function, std::vector<VisualScriptParameter>& parameters,
		                                std::vector<VisualScriptParameter> const& storedParameters, std::vector<std::string>& warnings)
		{
			if (parameters.size() != storedParameters.size())
			{
				return;
			}
			// A former name is no parameter's current name and no other former name of the list.
			std::unordered_set<std::string> taken;
			for (VisualScriptParameter const& parameter : parameters)
			{
				taken.insert(parameter.Name);
				taken.insert(parameter.FormerNames.begin(), parameter.FormerNames.end());
			}
			for (size_t index = 0; index < parameters.size(); index++)
			{
				VisualScriptParameter& parameter = parameters[index];
				std::string const& formerName = storedParameters[index].Name;
				if (formerName == parameter.Name || !taken.insert(formerName).second)
				{
					continue;
				}
				parameter.FormerNames.push_back(formerName);
				if (parameter.FormerNames.size() > VisualScriptSerializer::MaxFormerNames)
				{
					taken.erase(parameter.FormerNames.front());
					parameter.FormerNames.erase(parameter.FormerNames.begin());
				}
				warnings.push_back(fmt::format("parameter renamed: {}'s {} → {} ({} is kept in FormerNames)", function, formerName,
				                               parameter.Name, formerName));
			}
		}

		// A list's parameters never keep a name one of them has now as a former name, which the loader would drop.
		void RemoveCurrentNamesFromFormerNames(std::vector<VisualScriptParameter>& parameters)
		{
			std::unordered_set<std::string_view> names;
			for (VisualScriptParameter const& parameter : parameters)
			{
				names.insert(parameter.Name);
			}
			for (VisualScriptParameter& parameter : parameters)
			{
				std::erase_if(parameter.FormerNames,
				              [&names](std::string const& formerName)
				              {
								  return names.contains(formerName);
							  });
			}
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

		std::unordered_map<uint32_t, VisualScriptFunction const*> storedFunctions;
		for (VisualScriptFunction const& function : stored.Functions)
		{
			storedFunctions.emplace(function.ID, &function);
		}
		for (VisualScriptFunction& function : document.Functions)
		{
			RemoveCurrentNamesFromFormerNames(function.Inputs);
			RemoveCurrentNamesFromFormerNames(function.Outputs);
			// Only a library's functions are called from other files, by links and literals that name their parameters.
			auto const storedFunction = storedFunctions.find(function.ID);
			if (document.Kind != VisualScriptDocumentKind::Library || storedFunction == storedFunctions.end())
			{
				continue;
			}
			RecordParameterFormerNames(function.Name, function.Inputs, storedFunction->second->Inputs, warnings);
			RecordParameterFormerNames(function.Name, function.Outputs, storedFunction->second->Outputs, warnings);
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
			auto const place = places.find(analyzed.ID);
			if (place == places.end())
			{
				continue;
			}
			VisualScriptGraph* const graph = place->second.first;
			VisualScriptNode& node = graph->Nodes[place->second.second];
			if (analyzed.Resolved == VisualScriptResolution::Exact)
			{
				// A node of its own type whose pins are called otherwise now (a library call naming a former parameter name):
				// it keeps its type and stores the current names.
				if (node.Type != analyzed.Type)
				{
					continue;
				}
				std::vector<VisualScriptPinRename> used;
				for (VisualScriptPinRename const& rename : analyzed.PinRenames)
				{
					if (UsesPin(*graph, node, rename))
					{
						used.push_back(rename);
						warnings.push_back(
							fmt::format("pin renamed: {} (node {}) {} → {}", analyzed.Title, node.ID, rename.From, rename.To));
					}
				}
				if (!used.empty())
				{
					RenamePins(*graph, node, used);
					renamedPins.insert(graph);
				}
				continue;
			}
			bool const inexact =
				analyzed.Resolved == VisualScriptResolution::Redirect || analyzed.Resolved == VisualScriptResolution::Unique;
			if (!inexact || analyzed.CanonicalType.empty())
			{
				continue;
			}
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
