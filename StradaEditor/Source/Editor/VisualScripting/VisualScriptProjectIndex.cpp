#include "Editor/VisualScripting/VisualScriptProjectIndex.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <filesystem>
#include <optional>
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
			m_FailedFiles.clear();
			m_Listed = false;
			m_Revision += changed ? 1 : 0;
			return;
		}

		// The scripts registered, listed again only when the registry changed (any asset's registration, move or deletion);
		// the entries are replaced only when other scripts are listed, or in another order, so an unchanged list keeps what
		// GetGraphs and Find gave.
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
				graphs.push_back(std::move(graph));
			}
			std::sort(graphs.begin(), graphs.end(),
			          [](VisualScriptProjectGraph const& a, VisualScriptProjectGraph const& b)
			          {
						  return std::tie(a.Path, a.DisplayName) < std::tie(b.Path, b.DisplayName) ||
				                 (std::tie(a.Path, a.DisplayName) == std::tie(b.Path, b.DisplayName) &&
				                  a.Graph.GetUUID().GetValue() < b.Graph.GetUUID().GetValue());
					  });
			bool const sameGraphs = std::equal(graphs.begin(), graphs.end(), m_Graphs.begin(), m_Graphs.end(),
			                                   [](VisualScriptProjectGraph const& a, VisualScriptProjectGraph const& b)
			                                   {
												   return a.Graph == b.Graph;
											   });
			if (sameGraphs)
			{
				for (size_t index = 0; index < graphs.size(); index++)
				{
					VisualScriptProjectGraph& known = m_Graphs[index];
					if (known.Path != graphs[index].Path || known.DisplayName != graphs[index].DisplayName)
					{
						changed = true;
						known.Path = std::move(graphs[index].Path);
						known.DisplayName = std::move(graphs[index].DisplayName);
					}
				}
			}
			else
			{
				changed = true;
				for (VisualScriptProjectGraph& graph : graphs)
				{
					if (VisualScriptProjectGraph const* const known = Find(graph.Graph))
					{
						graph.Kind = known->Kind;
						graph.Document = known->Document;
						graph.LoadError = known->LoadError;
						graph.References = known->References;
					}
				}
				m_Graphs = std::move(graphs);
				m_Indices.clear();
				for (size_t index = 0; index < m_Graphs.size(); index++)
				{
					m_Indices.emplace(m_Graphs[index].Graph, index);
				}
				std::erase_if(m_FailedFiles,
				              [this](auto const& failed)
				              {
								  return !m_Indices.contains(failed.first);
							  });
			}
		}

		// Each script's document: a new shared document is a new program or layout; a load that failed may fail differently.
		for (VisualScriptProjectGraph& graph : m_Graphs)
		{
			// A file that did not load is read again only once it changed (or it loaded meanwhile, or went missing): an asset
			// refresh forgets failed loads (AssetManager::Refresh), and loading the file again would log its error again.
			std::optional<FileStamp> stamp;
			if (!AssetManager::IsLoaded(graph.Graph) && !AssetManager::IsMissing(graph.Graph))
			{
				auto const failed = m_FailedFiles.find(graph.Graph);
				if (failed != m_FailedFiles.end() && AssetManager::HasLoadFailure(graph.Graph))
				{
					continue;
				}
				stamp = ReadFileStamp(graph.Graph);
				if (failed != m_FailedFiles.end() && failed->second == *stamp)
				{
					continue;
				}
			}
			auto [document, error] = LoadDocument(graph.Graph);
			if (document == nullptr && stamp.has_value())
			{
				m_FailedFiles.insert_or_assign(graph.Graph, *stamp);
			}
			else
			{
				m_FailedFiles.erase(graph.Graph);
			}
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

	VisualScriptProjectIndex::FileStamp VisualScriptProjectIndex::ReadFileStamp(AssetHandle graph)
	{
		std::filesystem::path const file = AssetManager::GetAbsolutePath(graph);
		return {FileSystem::GetLastWriteTime(file), FileSystem::GetFileSize(file)};
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
