#pragma once

#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptView.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Strada
{
	// What index queries cost, added up over the queries given it (a frame's, a hit test's).
	struct VisualScriptIndexStats
	{
		// Grid cells looked at: those of the queried rectangle, or the occupied ones when there are fewer.
		uint32_t CellsVisited = 0;
		// Items whose bounds were tested, each once per query, large and dynamic items included.
		uint32_t Candidates = 0;
		// Items returned.
		uint32_t Results = 0;
	};

	// A uniform grid over the visual model's nodes, comments and wires, so culling and hit testing touch only what is near a
	// rectangle: a query costs O(cells + candidates), not O(graph). It is rebuilt when the visual model's geometry changes
	// (VisualScriptCanvasModel::Sync returns true). While a gesture moves nodes, they and their wires are dynamic: queries leave
	// them out and the caller tests them at their preview places, so dragging never rebuilds the index. Main thread only.
	class VisualScriptSpatialIndex
	{
	public:
		// Canvas units: a node (about 200 × 120) touches 1 to 4 cells.
		static constexpr float CellSize = 512.0f;
		// Items spanning more cells than this in x or y (comments up to 1e6 units, long wires) are not in the grid: every
		// query tests them, so an item in the grid occupies at most LargeSpan × LargeSpan cells.
		static constexpr int LargeSpan = 8;

		enum class ItemKind : uint8_t
		{
			Node = 0,
			Comment = 1,
			Link = 2
		};

		// Query masks: the kinds of items a query returns.
		static constexpr uint8_t NodeMask = 1u << 0;
		static constexpr uint8_t CommentMask = 1u << 1;
		static constexpr uint8_t LinkMask = 1u << 2;
		static constexpr uint8_t AllMask = NodeMask | CommentMask | LinkMask;

		// Items are packed in a uint32_t: the kind in the top 2 bits, the index into the visual model's nodes, comments or
		// links in the others.
		static constexpr uint32_t MakeItem(ItemKind kind, uint32_t index)
		{
			return (static_cast<uint32_t>(kind) << 30) | (index & IndexMask);
		}
		static constexpr ItemKind GetKind(uint32_t item) { return static_cast<ItemKind>(item >> 30); }
		static constexpr uint32_t GetIndex(uint32_t item) { return item & IndexMask; }
		// What the index holds for an item, from the visual model: a node's VisualBounds, a comment's rectangle, a link's
		// VisualScriptGeometry::GetLinkBounds.
		static VisualScriptRect GetItemBounds(VisualScriptCanvasModel const& model, uint32_t item);

		// Indexes every node, comment and link of the model, and clears the dynamic items. Reuses its buffers: rebuilding an
		// unchanged model allocates nothing.
		void Build(VisualScriptCanvasModel const& model);
		// Leaves these nodes and links (model indices) out of query results until the next SetDynamic or Build; empty spans
		// clear it.
		void SetDynamic(std::span<uint32_t const> nodes, std::span<uint32_t const> links);
		// The items of the kinds in kindMask whose bounds intersect rect (canvas units), each once, dynamic ones left out, in
		// no particular order: replaces out's contents. Adds to stats when given.
		void Query(VisualScriptRect const& rect, uint8_t kindMask, std::vector<uint32_t>& out,
		           VisualScriptIndexStats* stats = nullptr) const;
		// Query of the square of half-size radius around point: a hit test's candidates. A radius under CellSize / 2 visits at
		// most 4 cells.
		void QueryPoint(glm::vec2 point, float radius, uint8_t kindMask, std::vector<uint32_t>& out,
		                VisualScriptIndexStats* stats = nullptr) const;

		// The bounds the index holds for an item: what GetItemBounds gave at the last Build.
		VisualScriptRect const& GetBounds(uint32_t item) const;
		// How many items each kind has, as of the last Build.
		uint32_t GetItemCount(ItemKind kind) const;
		uint32_t GetOccupiedCellCount() const { return static_cast<uint32_t>(m_CellKeys.size()); }
		uint32_t GetLargeItemCount() const { return static_cast<uint32_t>(m_Large.size()); }
		// Bytes its vectors have reserved (capacity, not size): rebuilding an unchanged model does not grow it.
		size_t GetCapacityBytes() const;

	private:
		static constexpr uint32_t IndexMask = (1u << 30) - 1u;
		static constexpr uint32_t EmptySlot = 0xFFFFFFFFu;

		uint32_t GetFlatIndex(uint32_t item) const;
		// The occupied cell's slot in m_CellKeys, or EmptySlot.
		uint32_t FindCell(uint32_t key) const;
		void Visit(uint32_t item, VisualScriptRect const& rect, uint8_t kindMask, std::vector<uint32_t>& out,
		           VisualScriptIndexStats* stats) const;

		// Sorted occupied cells: (cx + 32768) << 16 | (cy + 32768), the cell coordinates kept within ±32767.
		std::vector<uint32_t> m_CellKeys;
		// CSR offsets into m_Entries: m_CellStarts.size() == m_CellKeys.size() + 1.
		std::vector<uint32_t> m_CellStarts;
		// Packed items, by cell.
		std::vector<uint32_t> m_Entries;
		// Open addressing (load at most 0.5): cell key to its slot in m_CellKeys.
		std::vector<uint32_t> m_Table;
		// Items spanning more than LargeSpan cells.
		std::vector<uint32_t> m_Large;
		// Per item, by flat index (nodes, then comments, then links).
		std::vector<VisualScriptRect> m_Bounds;
		std::vector<uint8_t> m_Dynamic;
		// Flat indices of the dynamic items, to clear them.
		std::vector<uint32_t> m_DynamicItems;
		// Per item: the query that last visited it, so items in several cells are tested once.
		mutable std::vector<uint32_t> m_Stamps;
		mutable uint32_t m_Stamp = 0;
		uint32_t m_NodeCount = 0;
		uint32_t m_CommentCount = 0;
		uint32_t m_LinkCount = 0;
		// Build's (cell key << 32 | item) pairs, and the buffer it sorts them through.
		std::vector<uint64_t> m_Pairs;
		std::vector<uint64_t> m_SortBuffer;
	};
}
