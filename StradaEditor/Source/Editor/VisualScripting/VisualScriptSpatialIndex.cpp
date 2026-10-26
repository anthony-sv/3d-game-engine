#include "Editor/VisualScripting/VisualScriptSpatialIndex.h"

#include "Editor/VisualScripting/VisualScriptGeometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Strada
{
	namespace
	{
		// Cells stay within ±MaxCell, so a cell's coordinates pack into 16 bits each. Documents keep coordinates within ±1e7
		// canvas units (cells within ±19532); anything beyond shares the outermost cells, which only gives queries there more
		// candidates, as every candidate's bounds are tested.
		constexpr int32_t MaxCell = 32767;
		constexpr int32_t CellBias = 32768;

		int32_t ToCell(float coordinate)
		{
			double const cell = std::floor(static_cast<double>(coordinate) / static_cast<double>(VisualScriptSpatialIndex::CellSize));
			if (!(cell >= -MaxCell))
			{
				// Below the range, or NaN (which lands on 0).
				return cell < -MaxCell ? -MaxCell : 0;
			}
			return static_cast<int32_t>(std::min(cell, static_cast<double>(MaxCell)));
		}

		uint32_t MakeKey(int32_t cx, int32_t cy)
		{
			return (static_cast<uint32_t>(cx + CellBias) << 16) | static_cast<uint32_t>(cy + CellBias);
		}

		int32_t GetCellX(uint32_t key)
		{
			return static_cast<int32_t>(key >> 16) - CellBias;
		}

		int32_t GetCellY(uint32_t key)
		{
			return static_cast<int32_t>(key & 0xFFFFu) - CellBias;
		}

		uint64_t HashKey(uint32_t key)
		{
			// SplitMix64's finalizer.
			uint64_t hash = key;
			hash = (hash ^ (hash >> 30)) * 0xBF58476D1CE4E5B9ull;
			hash = (hash ^ (hash >> 27)) * 0x94D049BB133111EBull;
			return hash ^ (hash >> 31);
		}

		struct CellRange
		{
			int32_t MinX = 0;
			int32_t MinY = 0;
			int32_t MaxX = 0;
			int32_t MaxY = 0;

			uint64_t GetCount() const
			{
				return static_cast<uint64_t>(static_cast<int64_t>(MaxX) - MinX + 1) *
				       static_cast<uint64_t>(static_cast<int64_t>(MaxY) - MinY + 1);
			}

			bool Contains(int32_t x, int32_t y) const { return x >= MinX && x <= MaxX && y >= MinY && y <= MaxY; }
		};

		CellRange GetCells(VisualScriptRect const& rect)
		{
			CellRange range{ToCell(rect.Min.x), ToCell(rect.Min.y), ToCell(rect.Max.x), ToCell(rect.Max.y)};
			range.MaxX = std::max(range.MaxX, range.MinX);
			range.MaxY = std::max(range.MaxY, range.MinY);
			return range;
		}

		template<typename T>
		size_t GetCapacity(std::vector<T> const& vector)
		{
			return vector.capacity() * sizeof(T);
		}
	}

	VisualScriptRect VisualScriptSpatialIndex::GetItemBounds(VisualScriptCanvasModel const& model, uint32_t item)
	{
		uint32_t const index = GetIndex(item);
		switch (GetKind(item))
		{
			case ItemKind::Node:
				return model.GetNodes()[index].VisualBounds;
			case ItemKind::Comment:
				return model.GetComments()[index].Rect;
			case ItemKind::Link:
				return VisualScriptGeometry::GetLinkBounds(model, index);
		}
		return {};
	}

	void VisualScriptSpatialIndex::Build(VisualScriptCanvasModel const& model)
	{
		m_NodeCount = static_cast<uint32_t>(model.GetNodes().size());
		m_CommentCount = static_cast<uint32_t>(model.GetComments().size());
		m_LinkCount = static_cast<uint32_t>(model.GetLinks().size());
		size_t const itemCount = static_cast<size_t>(m_NodeCount) + m_CommentCount + m_LinkCount;

		m_Bounds.resize(itemCount);
		m_Dynamic.assign(itemCount, 0);
		m_DynamicItems.clear();
		m_Stamps.assign(itemCount, 0);
		m_Stamp = 0;
		m_Pairs.clear();
		m_Large.clear();

		auto const add = [this, &model](ItemKind kind, uint32_t index)
		{
			uint32_t const item = MakeItem(kind, index);
			VisualScriptRect const bounds = GetItemBounds(model, item);
			m_Bounds[GetFlatIndex(item)] = bounds;
			CellRange const cells = GetCells(bounds);
			if (static_cast<int64_t>(cells.MaxX) - cells.MinX + 1 > LargeSpan ||
			    static_cast<int64_t>(cells.MaxY) - cells.MinY + 1 > LargeSpan)
			{
				m_Large.push_back(item);
				return;
			}
			for (int32_t x = cells.MinX; x <= cells.MaxX; x++)
			{
				for (int32_t y = cells.MinY; y <= cells.MaxY; y++)
				{
					m_Pairs.push_back((static_cast<uint64_t>(MakeKey(x, y)) << 32) | item);
				}
			}
		};
		for (uint32_t index = 0; index < m_NodeCount; index++)
		{
			add(ItemKind::Node, index);
		}
		for (uint32_t index = 0; index < m_CommentCount; index++)
		{
			add(ItemKind::Comment, index);
		}
		for (uint32_t index = 0; index < m_LinkCount; index++)
		{
			add(ItemKind::Link, index);
		}

		// By cell: a radix sort of the pairs' key halves, byte by byte, which is stable, so each cell keeps its items in item
		// order. Linear in the pairs, where a comparison sort of a large graph's few hundred thousand pairs took most of the
		// build.
		m_SortBuffer.resize(m_Pairs.size());
		for (uint32_t shift = 32; shift < 64; shift += 8)
		{
			std::array<uint32_t, 257> starts{};
			for (uint64_t const pair : m_Pairs)
			{
				starts[((pair >> shift) & 0xFFu) + 1]++;
			}
			// A byte every key shares (the high bytes of a graph's cells mostly) leaves the order as it is.
			if (std::find(starts.begin(), starts.end(), static_cast<uint32_t>(m_Pairs.size())) != starts.end())
			{
				continue;
			}
			for (size_t digit = 1; digit < starts.size(); digit++)
			{
				starts[digit] += starts[digit - 1];
			}
			for (uint64_t const pair : m_Pairs)
			{
				m_SortBuffer[starts[(pair >> shift) & 0xFFu]++] = pair;
			}
			m_Pairs.swap(m_SortBuffer);
		}
		m_CellKeys.clear();
		m_CellStarts.clear();
		m_Entries.clear();
		for (uint64_t const pair : m_Pairs)
		{
			uint32_t const key = static_cast<uint32_t>(pair >> 32);
			if (m_CellKeys.empty() || m_CellKeys.back() != key)
			{
				m_CellKeys.push_back(key);
				m_CellStarts.push_back(static_cast<uint32_t>(m_Entries.size()));
			}
			m_Entries.push_back(static_cast<uint32_t>(pair));
		}
		m_CellStarts.push_back(static_cast<uint32_t>(m_Entries.size()));

		size_t tableSize = 16;
		while (tableSize < m_CellKeys.size() * 2)
		{
			tableSize *= 2;
		}
		m_Table.assign(tableSize, EmptySlot);
		size_t const mask = tableSize - 1;
		for (uint32_t slot = 0; slot < m_CellKeys.size(); slot++)
		{
			size_t position = static_cast<size_t>(HashKey(m_CellKeys[slot])) & mask;
			while (m_Table[position] != EmptySlot)
			{
				position = (position + 1) & mask;
			}
			m_Table[position] = slot;
		}
	}

	void VisualScriptSpatialIndex::SetDynamic(std::span<uint32_t const> nodes, std::span<uint32_t const> comments,
	                                          std::span<uint32_t const> links)
	{
		for (uint32_t const flat : m_DynamicItems)
		{
			m_Dynamic[flat] = 0;
		}
		m_DynamicItems.clear();
		auto const mark = [this](ItemKind kind, uint32_t index, uint32_t count)
		{
			if (index >= count)
			{
				return;
			}
			uint32_t const flat = GetFlatIndex(MakeItem(kind, index));
			if (m_Dynamic[flat] == 0)
			{
				m_Dynamic[flat] = 1;
				m_DynamicItems.push_back(flat);
			}
		};
		for (uint32_t const node : nodes)
		{
			mark(ItemKind::Node, node, m_NodeCount);
		}
		for (uint32_t const comment : comments)
		{
			mark(ItemKind::Comment, comment, m_CommentCount);
		}
		for (uint32_t const link : links)
		{
			mark(ItemKind::Link, link, m_LinkCount);
		}
	}

	void VisualScriptSpatialIndex::Query(VisualScriptRect const& rect, uint8_t kindMask, std::vector<uint32_t>& out,
	                                     VisualScriptIndexStats* stats) const
	{
		out.clear();
		if (m_Bounds.empty())
		{
			return;
		}
		if (++m_Stamp == 0)
		{
			// The stamps wrapped around: forget every visit.
			std::fill(m_Stamps.begin(), m_Stamps.end(), 0u);
			m_Stamp = 1;
		}

		CellRange const cells = GetCells(rect);
		auto const visitCell = [&](uint32_t slot)
		{
			for (uint32_t entry = m_CellStarts[slot]; entry < m_CellStarts[slot + 1]; entry++)
			{
				Visit(m_Entries[entry], rect, kindMask, out, stats);
			}
		};
		uint64_t visited = 0;
		if (cells.GetCount() <= m_CellKeys.size())
		{
			for (int32_t x = cells.MinX; x <= cells.MaxX; x++)
			{
				for (int32_t y = cells.MinY; y <= cells.MaxY; y++)
				{
					visited++;
					uint32_t const slot = FindCell(MakeKey(x, y));
					if (slot != EmptySlot)
					{
						visitCell(slot);
					}
				}
			}
		}
		else
		{
			// A rectangle over more cells than are occupied (a zoomed-out view of a sparse graph) walks the occupied ones.
			for (uint32_t slot = 0; slot < m_CellKeys.size(); slot++)
			{
				visited++;
				uint32_t const key = m_CellKeys[slot];
				if (cells.Contains(GetCellX(key), GetCellY(key)))
				{
					visitCell(slot);
				}
			}
		}
		for (uint32_t const item : m_Large)
		{
			Visit(item, rect, kindMask, out, stats);
		}
		if (stats != nullptr)
		{
			stats->CellsVisited += static_cast<uint32_t>(std::min<uint64_t>(visited, std::numeric_limits<uint32_t>::max()));
			stats->Results += static_cast<uint32_t>(out.size());
		}
	}

	void VisualScriptSpatialIndex::QueryPoint(glm::vec2 point, float radius, uint8_t kindMask, std::vector<uint32_t>& out,
	                                          VisualScriptIndexStats* stats) const
	{
		float const extent = radius > 0.0f ? radius : 0.0f;
		Query({point - glm::vec2(extent), point + glm::vec2(extent)}, kindMask, out, stats);
	}

	VisualScriptRect const& VisualScriptSpatialIndex::GetBounds(uint32_t item) const
	{
		return m_Bounds[GetFlatIndex(item)];
	}

	uint32_t VisualScriptSpatialIndex::GetItemCount(ItemKind kind) const
	{
		switch (kind)
		{
			case ItemKind::Node:
				return m_NodeCount;
			case ItemKind::Comment:
				return m_CommentCount;
			case ItemKind::Link:
				return m_LinkCount;
		}
		return 0;
	}

	size_t VisualScriptSpatialIndex::GetCapacityBytes() const
	{
		return GetCapacity(m_CellKeys) + GetCapacity(m_CellStarts) + GetCapacity(m_Entries) + GetCapacity(m_Table) + GetCapacity(m_Large) +
		       GetCapacity(m_Bounds) + GetCapacity(m_Dynamic) + GetCapacity(m_DynamicItems) + GetCapacity(m_Stamps) + GetCapacity(m_Pairs) +
		       GetCapacity(m_SortBuffer);
	}

	uint32_t VisualScriptSpatialIndex::GetFlatIndex(uint32_t item) const
	{
		uint32_t const index = GetIndex(item);
		switch (GetKind(item))
		{
			case ItemKind::Node:
				return index;
			case ItemKind::Comment:
				return m_NodeCount + index;
			case ItemKind::Link:
				return m_NodeCount + m_CommentCount + index;
		}
		return index;
	}

	uint32_t VisualScriptSpatialIndex::FindCell(uint32_t key) const
	{
		size_t const mask = m_Table.size() - 1;
		size_t position = static_cast<size_t>(HashKey(key)) & mask;
		while (m_Table[position] != EmptySlot)
		{
			uint32_t const slot = m_Table[position];
			if (m_CellKeys[slot] == key)
			{
				return slot;
			}
			position = (position + 1) & mask;
		}
		return EmptySlot;
	}

	void VisualScriptSpatialIndex::Visit(uint32_t item, VisualScriptRect const& rect, uint8_t kindMask, std::vector<uint32_t>& out,
	                                     VisualScriptIndexStats* stats) const
	{
		if ((kindMask & (1u << static_cast<uint32_t>(GetKind(item)))) == 0)
		{
			return;
		}
		uint32_t const flat = GetFlatIndex(item);
		if (m_Stamps[flat] == m_Stamp)
		{
			return;
		}
		m_Stamps[flat] = m_Stamp;
		if (stats != nullptr)
		{
			stats->Candidates++;
		}
		if (m_Dynamic[flat] == 0 && VisualScriptGeometry::Intersects(m_Bounds[flat], rect))
		{
			out.push_back(item);
		}
	}
}
