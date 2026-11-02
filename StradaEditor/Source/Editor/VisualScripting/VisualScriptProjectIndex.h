#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptReferences.h"
#include "Strada/Core/Base.h"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Strada
{
	// What one visual script of the project offers other graphs, read from its document without the scripting runtime.
	struct VisualScriptProjectGraph
	{
		AssetHandle Graph;
		// The asset path ("Scripts/Door.sgraph"); empty for memory assets.
		std::string Path;
		// The file stem ("Door"); a memory asset's name.
		std::string DisplayName;
		VisualScriptDocumentKind Kind = VisualScriptDocumentKind::Script;
		// Null when it does not load (its file is missing or broken).
		Ref<VisualScriptDocument const> Document;
		// Why it does not load; empty when it does.
		std::string LoadError;
		// CollectVisualScriptReferences of the document: the graphs its nodes name.
		std::vector<VisualScriptGraphReference> References;
	};

	// The visual scripts of the project, what each offers and who refers to whom, kept from the asset manager's registry and
	// the assets' shared documents: an unchanged project costs one lookup per script (the registry's version, then each
	// script's document pointer). The first refresh loads every visual script of the project. Main thread only.
	class VisualScriptProjectIndex
	{
	public:
		// Follows the asset registry (scripts added, deleted, moved or renamed) and each script's document (a commit, an undo,
		// a reload, a file that stopped or started loading). Without assets the index is empty.
		void Refresh();
		// The scripts by path (memory assets, which have none, by name first).
		std::span<VisualScriptProjectGraph const> GetGraphs() const { return m_Graphs; }
		// Null when the index has no such script.
		VisualScriptProjectGraph const* Find(AssetHandle graph) const;
		// The other graphs whose nodes refer to the graph, by path, each with the strongest use and how many of its nodes
		// name it. A graph's references to itself are left out.
		std::vector<VisualScriptGraphReference> FindReferrers(AssetHandle graph) const;
		// Changes with anything above; an idle Refresh keeps it.
		uint64_t GetRevision() const { return m_Revision; }
		// The resolver of titles, pins, quick fixes and name lookups: self (a window's own graph) resolves to working, its
		// displayed document, never to the stored asset (nothing while working is null); every other graph to the document
		// the index holds. The documents it gives, and the resolver itself, are valid until the index's next Refresh.
		VisualScriptGraphResolver MakeResolver(AssetHandle self = {}, VisualScriptDocument const* working = nullptr) const;

	private:
		std::vector<VisualScriptProjectGraph> m_Graphs;
		std::unordered_map<AssetHandle, size_t> m_Indices;
		// The asset registry's version the scripts were listed at; none before the first Refresh.
		uint64_t m_AssetVersion = 0;
		bool m_Listed = false;
		uint64_t m_Revision = 0;
	};

	// "Player.sgraph, Enemy.sgraph (5 nodes)": the other graphs whose nodes refer to the graph (FindReferrers), by file name
	// (a memory asset by its name), the first five named and the others counted ("and 3 more"), with how many of their nodes
	// do; empty when none does.
	[[nodiscard]] std::string DescribeVisualScriptReferrers(VisualScriptProjectIndex const& index, AssetHandle graph);
}
