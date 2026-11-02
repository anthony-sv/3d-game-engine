#include "Editor/VisualScripting/VisualScriptProjectIndex.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <tuple>
#include <utility>

namespace Strada
{
	namespace
	{
		// The document a script's asset holds now, as the graph windows load it (a missing file is not read), or why there is
		// none.
		std::pair<Ref<VisualScriptDocument const>, std::string> LoadDocument(AssetHandle graph)
		{
			if (AssetManager::IsMissing(graph))
			{
				return {nullptr, "its file is missing"};
			}
			Result<Ref<VisualScriptAsset>> const asset = AssetManager::TryGetAsset<VisualScriptAsset>(graph);
			if (!asset)
			{
				return {nullptr, asset.GetError()};
			}
			return {asset.GetValue()->GetSharedDocument(), std::string()};
		}
	}

	void VisualScriptProjectIndex::Refresh()
	{
		bool changed = false;
		if (!AssetManager::IsInitialized())
		{
			changed = !m_Graphs.empty();
			m_Graphs.clear();
			m_Indices.clear();
			m_Listed = false;
			m_Revision += changed ? 1 : 0;
			return;
		}

		// The scripts registered, listed again only when the registry changed.
		uint64_t const version = AssetManager::GetVersion();
		if (!m_Listed || version != m_AssetVersion)
		{
			m_Listed = true;
			m_AssetVersion = version;
			std::vector<VisualScriptProjectGraph> graphs;
			for (AssetMetadata const& metadata : AssetManager::GetAssets(AssetType::VisualScript))
			{
				VisualScriptProjectGraph graph;
				graph.Graph = metadata.Handle;
				if (metadata.IsFileAsset())
				{
					graph.Path = metadata.Path;
					graph.DisplayName = FileSystem::PathToUtf8(FileSystem::PathFromUtf8(metadata.Path).stem());
				}
				else
				{
					graph.DisplayName = metadata.GetDisplayName();
				}
				if (VisualScriptProjectGraph const* const known = Find(metadata.Handle))
				{
					changed = changed || known->Path != graph.Path || known->DisplayName != graph.DisplayName;
					graph.Kind = known->Kind;
					graph.Document = known->Document;
					graph.LoadError = known->LoadError;
					graph.References = known->References;
				}
				else
				{
					changed = true;
				}
				graphs.push_back(std::move(graph));
			}
			std::sort(graphs.begin(), graphs.end(),
			          [](VisualScriptProjectGraph const& a, VisualScriptProjectGraph const& b)
			          {
						  return std::tie(a.Path, a.DisplayName) < std::tie(b.Path, b.DisplayName) ||
				                 (std::tie(a.Path, a.DisplayName) == std::tie(b.Path, b.DisplayName) &&
				                  a.Graph.GetUUID().GetValue() < b.Graph.GetUUID().GetValue());
					  });
			changed = changed || graphs.size() != m_Graphs.size() ||
			          !std::equal(graphs.begin(), graphs.end(), m_Graphs.begin(), m_Graphs.end(),
			                      [](VisualScriptProjectGraph const& a, VisualScriptProjectGraph const& b)
			                      {
									  return a.Graph == b.Graph;
								  });
			m_Graphs = std::move(graphs);
			m_Indices.clear();
			for (size_t index = 0; index < m_Graphs.size(); index++)
			{
				m_Indices.emplace(m_Graphs[index].Graph, index);
			}
		}

		// Each script's document: a new shared document is a new program or layout; a load that failed may fail differently.
		for (VisualScriptProjectGraph& graph : m_Graphs)
		{
			auto [document, error] = LoadDocument(graph.Graph);
			if (document == graph.Document && error == graph.LoadError)
			{
				continue;
			}
			changed = true;
			graph.Kind = document != nullptr ? document->Kind : VisualScriptDocumentKind::Script;
			graph.References = document != nullptr ? CollectVisualScriptReferences(*document) : std::vector<VisualScriptGraphReference>();
			graph.Document = std::move(document);
			graph.LoadError = std::move(error);
		}
		m_Revision += changed ? 1 : 0;
	}

	VisualScriptProjectGraph const* VisualScriptProjectIndex::Find(AssetHandle graph) const
	{
		auto const found = m_Indices.find(graph);
		return found != m_Indices.end() ? &m_Graphs[found->second] : nullptr;
	}

	std::vector<VisualScriptGraphReference> VisualScriptProjectIndex::FindReferrers(AssetHandle graph) const
	{
		std::vector<VisualScriptGraphReference> referrers;
		for (VisualScriptProjectGraph const& other : m_Graphs)
		{
			if (other.Graph == graph)
			{
				continue;
			}
			auto const reference = std::find_if(other.References.begin(), other.References.end(),
			                                    [graph](VisualScriptGraphReference const& candidate)
			                                    {
													return candidate.Graph == graph;
												});
			if (reference != other.References.end())
			{
				referrers.push_back({other.Graph, reference->Use, reference->Nodes});
			}
		}
		return referrers;
	}

	VisualScriptGraphResolver VisualScriptProjectIndex::MakeResolver(AssetHandle self, VisualScriptDocument const* working) const
	{
		return [this, self, working](AssetHandle graph) -> VisualScriptDocument const*
		{
			if (self.IsValid() && graph == self)
			{
				return working;
			}
			VisualScriptProjectGraph const* const found = Find(graph);
			return found != nullptr ? found->Document.get() : nullptr;
		};
	}

	std::string DescribeVisualScriptReferrers(VisualScriptProjectIndex const& index, AssetHandle graph)
	{
		constexpr size_t NamedReferrers = 5;
		std::vector<VisualScriptGraphReference> const referrers = index.FindReferrers(graph);
		if (referrers.empty())
		{
			return {};
		}
		std::string text;
		uint64_t nodes = 0;
		for (size_t position = 0; position < referrers.size(); position++)
		{
			nodes += referrers[position].Nodes;
			if (position >= NamedReferrers)
			{
				continue;
			}
			VisualScriptProjectGraph const* const referrer = index.Find(referrers[position].Graph);
			std::string const name = referrer == nullptr      ? referrers[position].Graph.ToString()
			                         : referrer->Path.empty() ? referrer->DisplayName
			                                                  : FileSystem::PathToUtf8(FileSystem::PathFromUtf8(referrer->Path).filename());
			text += text.empty() ? name : ", " + name;
		}
		if (referrers.size() > NamedReferrers)
		{
			text += fmt::format(" and {} more", referrers.size() - NamedReferrers);
		}
		return fmt::format("{} ({} {})", text, nodes, nodes == 1 ? "node" : "nodes");
	}
}
