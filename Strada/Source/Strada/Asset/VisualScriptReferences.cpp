#include "stpch.h"
#include "Strada/Asset/VisualScriptReferences.h"

#include "Strada/Core/UUID.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Strada
{
	namespace
	{
		constexpr char const* GraphSetting = "Graph";

		// The use a node of this type makes of the graph its Graph setting names; nothing for a type that refers to no graph.
		std::optional<VisualScriptReferenceUse> GetReferenceUse(std::string_view type)
		{
			if (type == "Function.Call")
			{
				return VisualScriptReferenceUse::Library;
			}
			if (type == "Variable.Get" || type == "Variable.Set" || type == "RunsGraph")
			{
				return VisualScriptReferenceUse::Interface;
			}
			if (type == "Event.Send" || type == "Event.Broadcast")
			{
				return VisualScriptReferenceUse::Verification;
			}
			return std::nullopt;
		}

		void CollectReferences(VisualScriptGraph const& graph, std::vector<VisualScriptGraphReference>& references)
		{
			for (VisualScriptNode const& node : graph.Nodes)
			{
				std::optional<VisualScriptReferenceUse> const use = GetReferenceUse(node.Type);
				if (!use)
				{
					continue;
				}
				if (std::optional<AssetHandle> const target = ReadVisualScriptGraphSetting(node))
				{
					references.push_back({*target, *use, 1});
				}
			}
		}
	}

	std::optional<AssetHandle> ReadVisualScriptGraphSetting(VisualScriptNode const& node)
	{
		if (!node.Settings.is_object())
		{
			return std::nullopt;
		}
		auto const setting = node.Settings.find(GraphSetting);
		if (setting == node.Settings.end() || !setting->is_string())
		{
			return std::nullopt;
		}
		std::optional<UUID> const id = UUID::FromString(setting->get_ref<std::string const&>());
		if (!id || !id->IsValid())
		{
			return std::nullopt;
		}
		return AssetHandle(*id);
	}

	bool IsLocalVisualScriptReference(VisualScriptNode const& node)
	{
		return !node.Settings.is_object() || !node.Settings.contains(GraphSetting);
	}

	std::vector<VisualScriptGraphReference> CollectVisualScriptReferences(VisualScriptDocument const& document)
	{
		// One entry per node first, then merged per graph.
		std::vector<VisualScriptGraphReference> nodes;
		for (VisualScriptFunction const& function : document.Functions)
		{
			CollectReferences(function.Graph, nodes);
		}
		CollectReferences(document.Graph, nodes);
		std::sort(nodes.begin(), nodes.end(),
		          [](VisualScriptGraphReference const& a, VisualScriptGraphReference const& b)
		          {
					  return a.Graph.GetUUID().GetValue() < b.Graph.GetUUID().GetValue();
				  });
		std::vector<VisualScriptGraphReference> references;
		for (VisualScriptGraphReference const& node : nodes)
		{
			if (!references.empty() && references.back().Graph == node.Graph)
			{
				VisualScriptGraphReference& merged = references.back();
				merged.Use = std::max(merged.Use, node.Use);
				merged.Nodes++;
				continue;
			}
			references.push_back(node);
		}
		return references;
	}

	VisualScriptDocument MakeVisualScriptInterface(VisualScriptDocument const& document)
	{
		VisualScriptDocument result;
		result.Kind = document.Kind;
		result.Variables = document.Variables;
		for (VisualScriptNode const& node : document.Graph.Nodes)
		{
			if (node.Type != "Event.Custom")
			{
				continue;
			}
			VisualScriptNode event;
			event.ID = node.ID;
			event.Type = node.Type;
			event.Settings = node.Settings;
			event.Enabled = node.Enabled;
			result.Graph.Nodes.push_back(std::move(event));
		}
		return result;
	}

	bool HaveSameVisualScriptLinkInterface(VisualScriptDocument const& a, VisualScriptDocument const& b)
	{
		if (a.Kind != b.Kind || a.Variables.size() != b.Variables.size())
		{
			return false;
		}
		// Variables compared as programs compare them: documents that hold nothing else.
		VisualScriptDocument first;
		first.Kind = a.Kind;
		first.Variables = a.Variables;
		VisualScriptDocument second;
		second.Kind = b.Kind;
		second.Variables = b.Variables;
		return HaveSameVisualScriptProgram(first, second);
	}
}
