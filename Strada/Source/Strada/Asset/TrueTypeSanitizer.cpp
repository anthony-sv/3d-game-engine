#include "stpch.h"
#include "Strada/Asset/TrueTypeSanitizer.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

// Every check below mirrors a read of stb_truetype (v1.26): the comments name the function that reads.
namespace Strada
{
	namespace
	{
		// Composite glyphs nest at most this deep and expand to at most this many outline vertices: stb_truetype recurses
		// into components and allocates their vertices, and no real glyph comes near either limit.
		constexpr uint32_t MaxCompositeDepth = 16;
		constexpr uint64_t MaxGlyphVertices = uint64_t(1) << 16;

		// Big-endian access to the font. Callers check bounds before reading.
		class FontBytes
		{
		public:
			explicit FontBytes(std::span<uint8_t> bytes)
				: m_Bytes(bytes)
			{
			}

			// Whether [offset, offset + size) lies within [begin, end), without overflowing.
			static bool Fits(uint64_t offset, uint64_t size, uint64_t begin, uint64_t end)
			{
				return offset >= begin && offset <= end && size <= end - offset;
			}

			bool Has(uint64_t offset, uint64_t size) const { return Fits(offset, size, 0, m_Bytes.size()); }

			uint8_t U8(uint64_t offset) const { return m_Bytes[offset]; }
			uint16_t U16(uint64_t offset) const { return static_cast<uint16_t>((m_Bytes[offset] << 8) | m_Bytes[offset + 1]); }
			int16_t S16(uint64_t offset) const { return static_cast<int16_t>(U16(offset)); }
			uint32_t U32(uint64_t offset) const { return (static_cast<uint32_t>(U16(offset)) << 16) | U16(offset + 2); }

			bool IsTag(uint64_t offset, std::string_view tag) const
			{
				for (size_t index = 0; index < 4; index++)
				{
					if (m_Bytes[offset + index] != static_cast<uint8_t>(tag[index]))
					{
						return false;
					}
				}
				return true;
			}

			void SetU16(uint64_t offset, uint16_t value)
			{
				m_Bytes[offset] = static_cast<uint8_t>(value >> 8);
				m_Bytes[offset + 1] = static_cast<uint8_t>(value);
			}

			void SetTag(uint64_t offset, std::string_view tag)
			{
				for (size_t index = 0; index < 4; index++)
				{
					m_Bytes[offset + index] = static_cast<uint8_t>(tag[index]);
				}
			}

		private:
			std::span<uint8_t> m_Bytes;
		};

		// A table of the font: its absolute offset, its declared length and the directory record naming it.
		struct Table
		{
			uint64_t Offset = 0;
			uint64_t Length = 0;
			uint64_t Record = 0;

			uint64_t GetEnd() const { return Offset + Length; }
			bool Holds(uint64_t offset, uint64_t size) const { return FontBytes::Fits(offset, size, Offset, GetEnd()); }
		};

		// What stbtt_GetGlyphShape makes of a glyph.
		struct Glyph
		{
			bool Valid = true;
			// Vertices stb_truetype allocates for the outline (components included).
			uint64_t Vertices = 0;
			std::vector<uint16_t> Components;
		};

		class Sanitizer
		{
		public:
			explicit Sanitizer(std::span<uint8_t> font)
				: m_Font(font)
			{
			}

			Result<TrueTypeRepairs> Run()
			{
				if (Result<void> header = ReadHeader(); !header)
				{
					return Error{header.GetError()};
				}
				// stbtt_InitFont takes the CFF path for fonts without glyf.
				if (!Find("glyf"))
				{
					return Error{"fonts with CFF outlines (OpenType .otf) are not supported: use a TrueType font"};
				}
				Result<Table> head = Require("head", 54);
				Result<Table> maxp = Require("maxp", 6);
				Result<Table> hhea = Require("hhea", 36);
				Result<Table> hmtx = Require("hmtx", 0);
				Result<Table> cmap = Require("cmap", 4);
				Result<Table> loca = Require("loca", 0);
				Result<Table> glyf = Require("glyf", 0);
				for (Result<Table> const* table : {&head, &maxp, &hhea, &hmtx, &cmap, &loca, &glyf})
				{
					if (!*table)
					{
						return Error{table->GetError()};
					}
					m_Regions.push_back(table->GetValue());
				}
				// Repairs write into the directory, the character map and the glyph locations: with no table overlapping
				// another, they never change what was already checked. (Valid fonts never overlap.)
				m_Regions.push_back(Table{m_FontStart, 12 + 16 * uint64_t(m_TableCount), 0});
				for (size_t first = 0; first < m_Regions.size(); first++)
				{
					for (size_t second = first + 1; second < m_Regions.size(); second++)
					{
						if (Overlap(m_Regions[first], m_Regions[second]))
						{
							return Error{"the font's tables overlap"};
						}
					}
				}

				m_GlyphCount = m_Font.U16(maxp.GetValue().Offset + 4);
				if (m_GlyphCount == 0)
				{
					return Error{"the font has no glyphs"};
				}
				// stbtt__GetGlyfOffset reads 16-bit (0) or 32-bit (1) glyph locations.
				uint16_t const locationFormat = m_Font.U16(head.GetValue().Offset + 50);
				if (locationFormat > 1)
				{
					return MakeError("the font's glyph location format {} is unknown", locationFormat);
				}
				if (Result<void> metrics = CheckMetrics(hhea.GetValue(), hmtx.GetValue()); !metrics)
				{
					return Error{metrics.GetError()};
				}
				if (Result<void> characters = CheckCharacterMap(cmap.GetValue()); !characters)
				{
					return Error{characters.GetError()};
				}
				if (Result<void> outlines = CheckOutlines(loca.GetValue(), glyf.GetValue(), locationFormat == 1); !outlines)
				{
					return Error{outlines.GetError()};
				}
				CheckKerning();
				return m_Repairs;
			}

		private:
			// stbtt_GetFontOffsetForIndex(font, 0): the font itself, or the first font of a collection.
			Result<void> ReadHeader()
			{
				if (!m_Font.Has(0, 4))
				{
					return Error{"the data is too short for a font"};
				}
				// stbtt__isfont
				if (m_Font.U32(0) == 0x31000000 || m_Font.U32(0) == 0x00010000 || m_Font.IsTag(0, "typ1") || m_Font.IsTag(0, "OTTO") ||
				    m_Font.IsTag(0, "true"))
				{
					m_FontStart = 0;
				}
				else if (m_Font.IsTag(0, "ttcf"))
				{
					if (!m_Font.Has(0, 16))
					{
						return Error{"the font collection's header is cut off"};
					}
					if (m_Font.U32(4) != 0x00010000 && m_Font.U32(4) != 0x00020000)
					{
						return Error{"the font collection's version is unknown"};
					}
					if (static_cast<int32_t>(m_Font.U32(8)) < 1)
					{
						return Error{"the font collection is empty"};
					}
					m_FontStart = m_Font.U32(12);
				}
				else
				{
					return Error{"the data is not a TrueType font"};
				}
				// stbtt__find_table
				if (!m_Font.Has(m_FontStart, 12))
				{
					return Error{"the font's header is cut off"};
				}
				m_TableCount = m_Font.U16(m_FontStart + 4);
				if (!m_Font.Has(m_FontStart + 12, 16 * uint64_t(m_TableCount)))
				{
					return Error{"the font's table directory is cut off"};
				}
				return {};
			}

			// stbtt__find_table: the first directory record with the tag. stb_truetype takes a table at offset 0 for a
			// missing one.
			std::optional<Table> Find(std::string_view tag) const
			{
				for (uint64_t index = 0; index < m_TableCount; index++)
				{
					uint64_t const record = m_FontStart + 12 + 16 * index;
					if (m_Font.IsTag(record, tag))
					{
						Table const table{m_Font.U32(record + 8), m_Font.U32(record + 12), record};
						return table.Offset != 0 ? std::optional<Table>(table) : std::nullopt;
					}
				}
				return std::nullopt;
			}

			Result<Table> Require(std::string_view tag, uint64_t minimumLength) const
			{
				std::optional<Table> const table = Find(tag);
				if (!table)
				{
					return MakeError("the font has no '{}' table", tag);
				}
				if (!m_Font.Has(table->Offset, table->Length) || table->Length < minimumLength)
				{
					return MakeError("the font's '{}' table is cut off", tag);
				}
				return *table;
			}

			// stbtt_GetGlyphHMetrics: advances and bearings for the first glyphs, bearings alone for the rest.
			Result<void> CheckMetrics(Table const& hhea, Table const& hmtx) const
			{
				uint64_t const longMetrics = m_Font.U16(hhea.Offset + 34);
				if (longMetrics == 0)
				{
					return Error{"the font has no horizontal metrics"};
				}
				uint64_t const needed = longMetrics >= m_GlyphCount ? 4 * m_GlyphCount : 4 * longMetrics + 2 * (m_GlyphCount - longMetrics);
				if (hmtx.Length < needed)
				{
					return Error{"the font's horizontal metrics are cut off"};
				}
				return {};
			}

			// stbtt_InitFont picks the last Unicode encoding record (Windows Unicode BMP or full, or any on the Unicode
			// platform); stbtt_FindGlyphIndex reads its subtable.
			Result<void> CheckCharacterMap(Table const& cmap)
			{
				uint64_t const recordCount = m_Font.U16(cmap.Offset + 2);
				if (!cmap.Holds(cmap.Offset + 4, 8 * recordCount))
				{
					return Error{"the font's character map is cut off"};
				}
				std::optional<uint64_t> subtable;
				for (uint64_t index = 0; index < recordCount; index++)
				{
					uint64_t const record = cmap.Offset + 4 + 8 * index;
					uint16_t const platform = m_Font.U16(record);
					uint16_t const encoding = m_Font.U16(record + 2);
					if ((platform == 3 && (encoding == 1 || encoding == 10)) || platform == 0)
					{
						subtable = cmap.Offset + m_Font.U32(record + 4);
					}
				}
				if (!subtable)
				{
					return Error{"the font has no Unicode character map"};
				}
				uint64_t const map = *subtable;
				if (!cmap.Holds(map, 2))
				{
					return Error{"the font's character map lies outside its table"};
				}
				uint64_t const available = cmap.GetEnd() - map;
				uint16_t const format = m_Font.U16(map);
				bool fits = false;
				switch (format)
				{
					case 0:
						// The subtable's length field bounds the byte array.
						fits = available >= 4 && m_Font.U16(map + 2) <= available;
						break;
					case 4:
						return CheckSegmentMap(map, available);
					case 6:
						fits = available >= 10 && 10 + 2 * uint64_t(m_Font.U16(map + 8)) <= available;
						break;
					case 12:
					case 13:
						fits = available >= 16 && 16 + 12 * uint64_t(m_Font.U32(map + 12)) <= available;
						break;
					default:
						// stb_truetype asserts on other formats.
						return MakeError("the font's character map format {} is not supported", format);
				}
				return fits ? Result<void>() : Error{"the font's character map is cut off"};
			}

			// Format 4: segments of characters, binary-searched with hints from the subtable.
			Result<void> CheckSegmentMap(uint64_t map, uint64_t available)
			{
				if (available < 14)
				{
					return Error{"the font's character map is cut off"};
				}
				uint64_t const segments = m_Font.U16(map + 6) >> 1;
				if (segments == 0)
				{
					return Error{"the font's character map has no segments"};
				}
				// End codes, a reserved word, start codes, deltas and range offsets.
				if (16 + 8 * segments > available)
				{
					return Error{"the font's character map is cut off"};
				}
				// The search follows the subtable's hints: recompute them from the segment count (as the OpenType specification
				// defines them) so it stays among the segments.
				uint16_t power = 1;
				uint16_t exponent = 0;
				while (2 * uint64_t(power) <= segments)
				{
					power = static_cast<uint16_t>(power * 2);
					exponent++;
				}
				uint16_t const searchRange = static_cast<uint16_t>(2 * power);
				uint16_t const rangeShift = static_cast<uint16_t>(2 * segments - searchRange);
				if (m_Font.U16(map + 8) != searchRange || m_Font.U16(map + 10) != exponent || m_Font.U16(map + 12) != rangeShift)
				{
					m_Font.SetU16(map + 8, searchRange);
					m_Font.SetU16(map + 10, exponent);
					m_Font.SetU16(map + 12, rangeShift);
					m_Repairs.RecomputedCharacterMap = true;
				}
				// Segments with a range offset take their glyphs from an array after it: every character of the segment must
				// find its entry inside the subtable.
				uint64_t const ends = map + 14;
				uint64_t const starts = ends + 2 * segments + 2;
				uint64_t const rangeOffsets = map + 14 + 6 * segments + 2;
				for (uint64_t segment = 0; segment < segments; segment++)
				{
					uint16_t const end = m_Font.U16(ends + 2 * segment);
					uint16_t const start = m_Font.U16(starts + 2 * segment);
					uint16_t const rangeOffset = m_Font.U16(rangeOffsets + 2 * segment);
					if (rangeOffset == 0 || start > end)
					{
						continue;
					}
					uint64_t const last = rangeOffsets + 2 * segment + rangeOffset + 2 * uint64_t(end - start);
					if (!FontBytes::Fits(last, 2, map, map + available))
					{
						return Error{"the font's character map points outside itself"};
					}
				}
				return {};
			}

			// stbtt__GetGlyfOffset reads two locations per glyph; stbtt_GetGlyphShape and stbtt_GetGlyphBox read the glyph
			// between them. Glyphs that do not hold what is read become empty.
			Result<void> CheckOutlines(Table const& loca, Table const& glyf, bool longLocations)
			{
				uint64_t const entrySize = longLocations ? 4 : 2;
				if (loca.Length < (uint64_t(m_GlyphCount) + 1) * entrySize)
				{
					return Error{"the font's glyph locations are cut off"};
				}
				auto const location = [&](uint64_t glyph)
				{
					return longLocations ? uint64_t(m_Font.U32(loca.Offset + 4 * glyph))
					                     : 2 * uint64_t(m_Font.U16(loca.Offset + 2 * glyph));
				};

				std::vector<Glyph> glyphs(m_GlyphCount);
				std::vector<bool> empty(m_GlyphCount, false);
				for (uint64_t index = 0; index < m_GlyphCount; index++)
				{
					uint64_t const begin = location(index);
					uint64_t const end = location(index + 1);
					empty[index] = begin == end;
					if (!empty[index])
					{
						glyphs[index].Valid =
							begin < end && end <= glyf.Length && ReadGlyph(glyf.Offset + begin, glyf.Offset + end, glyphs[index]);
					}
				}
				ResolveComposites(glyphs);

				// A glyph is empty when its location equals the next glyph's. Last first, damaged glyphs and empty ones take the
				// location of the glyph after them: damaged glyphs become empty, and empty ones stay so when that location moved.
				// (Other glyphs are read from their location on, wherever the next one starts.)
				for (uint64_t index = m_GlyphCount; index-- > 0;)
				{
					bool const damaged = !glyphs[index].Valid && !empty[index];
					if (damaged || empty[index])
					{
						uint64_t const next = loca.Offset + (index + 1) * entrySize;
						uint64_t const entry = loca.Offset + index * entrySize;
						m_Font.SetU16(entry, m_Font.U16(next));
						if (longLocations)
						{
							m_Font.SetU16(entry + 2, m_Font.U16(next + 2));
						}
					}
					if (damaged)
					{
						m_Repairs.EmptiedGlyphs++;
					}
				}
				return {};
			}

			// stbtt__GetGlyphShapeTT: the contour count, bounding box and outline of the glyph in [begin, end).
			bool ReadGlyph(uint64_t begin, uint64_t end, Glyph& glyph) const
			{
				auto const fits = [begin, end](uint64_t offset, uint64_t size)
				{
					return FontBytes::Fits(offset, size, begin, end);
				};
				if (!fits(begin, 10))
				{
					return false;
				}
				// stbtt_GetGlyphSDF sizes its image by the bounding box: an inverted one gives a negative size.
				if (m_Font.S16(begin + 2) > m_Font.S16(begin + 6) || m_Font.S16(begin + 4) > m_Font.S16(begin + 8))
				{
					return false;
				}
				int16_t const contours = m_Font.S16(begin);
				if (contours == 0)
				{
					return true;
				}
				if (contours < 0)
				{
					return ReadComposite(begin + 10, end, glyph);
				}

				// Contours end at strictly increasing points, so stb_truetype walks them in order.
				uint64_t const contourCount = static_cast<uint64_t>(contours);
				uint64_t const endPoints = begin + 10;
				if (!fits(endPoints, 2 * contourCount + 2))
				{
					return false;
				}
				std::vector<uint64_t> contourEnds(contourCount);
				for (uint64_t contour = 0; contour < contourCount; contour++)
				{
					contourEnds[contour] = m_Font.U16(endPoints + 2 * contour);
					if (contour > 0 && contourEnds[contour] <= contourEnds[contour - 1])
					{
						return false;
					}
				}
				uint64_t const pointCount = contourEnds.back() + 1;
				uint64_t offset = endPoints + 2 * contourCount;
				offset += 2 + m_Font.U16(offset);

				// Flags (each may repeat for the following points), then x and y coordinates, sized by the flags.
				std::vector<uint8_t> flags(pointCount);
				uint8_t flag = 0;
				uint32_t repeats = 0;
				for (uint64_t point = 0; point < pointCount; point++)
				{
					if (repeats == 0)
					{
						if (!fits(offset, 1))
						{
							return false;
						}
						flag = m_Font.U8(offset++);
						if ((flag & 8) != 0)
						{
							if (!fits(offset, 1))
							{
								return false;
							}
							repeats = m_Font.U8(offset++);
						}
					}
					else
					{
						repeats--;
					}
					flags[point] = flag;
				}
				for (uint8_t const pointFlag : flags)
				{
					offset += (pointFlag & 2) != 0 ? 1 : ((pointFlag & 16) != 0 ? 0 : 2);
				}
				for (uint8_t const pointFlag : flags)
				{
					offset += (pointFlag & 4) != 0 ? 1 : ((pointFlag & 32) != 0 ? 0 : 2);
				}
				if (offset > end)
				{
					return false;
				}

				// A contour of a single off-curve point makes stb_truetype read the point after it, outside the outline for the
				// last contour.
				uint64_t first = 0;
				for (uint64_t const last : contourEnds)
				{
					if (first == last && (flags[first] & 1) == 0)
					{
						return false;
					}
					first = last + 1;
				}
				glyph.Vertices = pointCount + 2 * contourCount;
				return true;
			}

			// Components until one without MORE_COMPONENTS.
			bool ReadComposite(uint64_t offset, uint64_t end, Glyph& glyph) const
			{
				while (true)
				{
					if (!FontBytes::Fits(offset, 4, 0, end))
					{
						return false;
					}
					uint16_t const flags = m_Font.U16(offset);
					glyph.Components.push_back(m_Font.U16(offset + 2));
					offset += 4;
					// stb_truetype positions components by offsets only: it asserts on matched points.
					if ((flags & 2) == 0)
					{
						return false;
					}
					offset += (flags & 1) != 0 ? 4 : 2;
					if ((flags & 8) != 0)
					{
						offset += 2;
					}
					else if ((flags & 64) != 0)
					{
						offset += 4;
					}
					else if ((flags & 128) != 0)
					{
						offset += 8;
					}
					if (offset > end)
					{
						return false;
					}
					if ((flags & 32) == 0)
					{
						return true;
					}
				}
			}

			// stbtt_GetGlyphShape recurses into the components of composites and appends their vertices: composites in a
			// cycle, nested too deep or expanding too far become empty. A depth-first walk with its own stack, as chains of
			// composites can be long.
			static void ResolveComposites(std::vector<Glyph>& glyphs)
			{
				enum class Visit : uint8_t
				{
					New,
					Open,
					Done
				};
				std::vector<Visit> visits(glyphs.size(), Visit::New);
				std::vector<uint32_t> heights(glyphs.size(), 0);
				std::vector<std::pair<size_t, size_t>> stack;
				for (size_t root = 0; root < glyphs.size(); root++)
				{
					if (visits[root] != Visit::New)
					{
						continue;
					}
					visits[root] = Visit::Open;
					stack.emplace_back(root, 0);
					while (!stack.empty())
					{
						size_t const index = stack.back().first;
						Glyph& glyph = glyphs[index];
						if (glyph.Valid && stack.back().second < glyph.Components.size())
						{
							uint16_t const component = glyph.Components[stack.back().second++];
							if (component >= glyphs.size())
							{
								// stb_truetype draws nothing for it.
								continue;
							}
							if (visits[component] == Visit::Open)
							{
								glyph.Valid = false;
							}
							else if (visits[component] == Visit::New)
							{
								visits[component] = Visit::Open;
								stack.emplace_back(component, 0);
							}
							continue;
						}
						if (glyph.Valid && !glyph.Components.empty())
						{
							uint64_t vertices = 0;
							uint32_t height = 1;
							for (uint16_t const component : glyph.Components)
							{
								if (component < glyphs.size() && glyphs[component].Valid)
								{
									vertices += glyphs[component].Vertices;
									height = std::max(height, heights[component] + 1);
								}
							}
							glyph.Vertices = vertices;
							heights[index] = height;
							glyph.Valid = height <= MaxCompositeDepth && vertices <= MaxGlyphVertices;
						}
						visits[index] = Visit::Done;
						stack.pop_back();
					}
				}
			}

			static bool Overlap(Table const& first, Table const& second)
			{
				return first.Offset < second.GetEnd() && second.Offset < first.GetEnd();
			}

			bool OverlapsRegions(Table const& table) const
			{
				return std::ranges::any_of(m_Regions,
				                           [&table](Table const& region)
				                           {
											   return Overlap(table, region);
										   });
			}

			// stbtt_GetGlyphKernAdvance reads GPOS when the font has it, otherwise kern. Damaged tables, and ones overlapping
			// the others (dropping one writes the directory), are dropped.
			void CheckKerning()
			{
				for (std::string_view const tag : {"GPOS", "kern"})
				{
					while (std::optional<Table> const table = Find(tag))
					{
						bool const valid = m_Font.Has(table->Offset, table->Length) && !OverlapsRegions(*table) &&
						                   (tag == "GPOS" ? IsValidPositioning(*table) : IsValidKerning(*table));
						if (valid)
						{
							m_Regions.push_back(*table);
							break;
						}
						Drop(*table);
					}
				}
			}

			void Drop(Table const& table)
			{
				// No lookup of stb_truetype asks for this tag.
				m_Font.SetTag(table.Record, "    ");
				m_Repairs.DroppedTables++;
			}

			// stbtt__GetGlyphKernInfoAdvance: the first subtable, when horizontal (format 0), is a sorted array of pairs.
			bool IsValidKerning(Table const& kern) const
			{
				if (kern.Length < 4)
				{
					return false;
				}
				if (m_Font.U16(kern.Offset + 2) < 1)
				{
					return true;
				}
				if (kern.Length < 10)
				{
					return false;
				}
				if (m_Font.U16(kern.Offset + 8) != 1)
				{
					return true;
				}
				return kern.Length >= 12 && 18 + 6 * uint64_t(m_Font.U16(kern.Offset + 10)) <= kern.Length;
			}

			// stbtt__GetGlyphGPOSInfoAdvance: pair adjustments (lookup type 2) of GPOS 1.0.
			bool IsValidPositioning(Table const& gpos) const
			{
				if (!gpos.Holds(gpos.Offset, 4))
				{
					return false;
				}
				if (m_Font.U16(gpos.Offset) != 1 || m_Font.U16(gpos.Offset + 2) != 0)
				{
					return true;
				}
				if (!gpos.Holds(gpos.Offset, 10))
				{
					return false;
				}
				uint64_t const lookups = gpos.Offset + m_Font.U16(gpos.Offset + 8);
				if (!gpos.Holds(lookups, 2))
				{
					return false;
				}
				uint64_t const lookupCount = m_Font.U16(lookups);
				if (!gpos.Holds(lookups + 2, 2 * lookupCount))
				{
					return false;
				}
				for (uint64_t index = 0; index < lookupCount; index++)
				{
					uint64_t const lookup = lookups + m_Font.U16(lookups + 2 + 2 * index);
					if (!gpos.Holds(lookup, 6))
					{
						return false;
					}
					if (m_Font.U16(lookup) != 2)
					{
						continue;
					}
					uint64_t const subtableCount = m_Font.U16(lookup + 4);
					if (!gpos.Holds(lookup + 6, 2 * subtableCount))
					{
						return false;
					}
					for (uint64_t subtable = 0; subtable < subtableCount; subtable++)
					{
						if (!IsValidPairAdjustment(gpos, lookup + m_Font.U16(lookup + 6 + 2 * subtable)))
						{
							return false;
						}
					}
				}
				return true;
			}

			// Formats 1 (pair sets) and 2 (class pairs), with x advances only.
			bool IsValidPairAdjustment(Table const& gpos, uint64_t subtable) const
			{
				if (!gpos.Holds(subtable, 4))
				{
					return false;
				}
				std::optional<int64_t> const lastCoverage = GetLastCoverageIndex(gpos, subtable + m_Font.U16(subtable + 2));
				if (!lastCoverage)
				{
					return false;
				}
				if (*lastCoverage < 0)
				{
					// No glyph is covered, so nothing more is read.
					return true;
				}
				uint16_t const format = m_Font.U16(subtable);
				if (format != 1 && format != 2)
				{
					return true;
				}
				if (!gpos.Holds(subtable, 8))
				{
					return false;
				}
				if (m_Font.U16(subtable + 4) != 4 || m_Font.U16(subtable + 6) != 0)
				{
					return true;
				}
				if (format == 1)
				{
					if (!gpos.Holds(subtable, 10))
					{
						return false;
					}
					// A pair set is read for any covered glyph before its index is compared with the pair set count.
					uint64_t const pairSets = m_Font.U16(subtable + 8);
					uint64_t const offsets = std::max(pairSets, static_cast<uint64_t>(*lastCoverage) + 1);
					if (!gpos.Holds(subtable + 10, 2 * offsets))
					{
						return false;
					}
					for (uint64_t index = 0; index < offsets; index++)
					{
						uint64_t const pairSet = subtable + m_Font.U16(subtable + 10 + 2 * index);
						if (!gpos.Holds(pairSet, 2) || (index < pairSets && !gpos.Holds(pairSet + 2, 4 * uint64_t(m_Font.U16(pairSet)))))
						{
							return false;
						}
					}
					return true;
				}
				if (!gpos.Holds(subtable, 16) || !IsValidClassDefinition(gpos, subtable + m_Font.U16(subtable + 8)) ||
				    !IsValidClassDefinition(gpos, subtable + m_Font.U16(subtable + 10)))
				{
					return false;
				}
				uint64_t const firstClasses = m_Font.U16(subtable + 12);
				uint64_t const secondClasses = m_Font.U16(subtable + 14);
				return gpos.Holds(subtable + 16, 2 * firstClasses * secondClasses);
			}

			// stbtt__GetCoverageIndex: the largest index the coverage table returns, -1 when it returns none; nothing when it
			// lies outside GPOS.
			std::optional<int64_t> GetLastCoverageIndex(Table const& gpos, uint64_t coverage) const
			{
				if (!gpos.Holds(coverage, 2))
				{
					return std::nullopt;
				}
				uint16_t const format = m_Font.U16(coverage);
				if (format != 1 && format != 2)
				{
					return -1;
				}
				if (!gpos.Holds(coverage, 4))
				{
					return std::nullopt;
				}
				uint64_t const count = m_Font.U16(coverage + 2);
				if (format == 1)
				{
					return gpos.Holds(coverage + 4, 2 * count) ? std::optional<int64_t>(static_cast<int64_t>(count) - 1) : std::nullopt;
				}
				if (!gpos.Holds(coverage + 4, 6 * count))
				{
					return std::nullopt;
				}
				int64_t last = -1;
				for (uint64_t range = 0; range < count; range++)
				{
					uint64_t const record = coverage + 4 + 6 * range;
					uint16_t const start = m_Font.U16(record);
					uint16_t const end = m_Font.U16(record + 2);
					if (start <= end)
					{
						last = std::max(last, static_cast<int64_t>(m_Font.U16(record + 4)) + (end - start));
					}
				}
				return last;
			}

			// stbtt__GetGlyphClass: format 1 (a class for each glyph of a range) or 2 (ranges of glyphs in a class).
			bool IsValidClassDefinition(Table const& gpos, uint64_t definition) const
			{
				if (!gpos.Holds(definition, 2))
				{
					return false;
				}
				uint16_t const format = m_Font.U16(definition);
				if (format == 1)
				{
					return gpos.Holds(definition, 6) && gpos.Holds(definition + 6, 2 * uint64_t(m_Font.U16(definition + 4)));
				}
				if (format == 2)
				{
					return gpos.Holds(definition, 4) && gpos.Holds(definition + 4, 6 * uint64_t(m_Font.U16(definition + 2)));
				}
				return true;
			}

			FontBytes m_Font;
			// The directory and the tables in use, which must not overlap.
			std::vector<Table> m_Regions;
			uint64_t m_FontStart = 0;
			uint16_t m_TableCount = 0;
			uint16_t m_GlyphCount = 0;
			TrueTypeRepairs m_Repairs;
		};
	}

	Result<TrueTypeRepairs> SanitizeTrueTypeFont(std::span<uint8_t> font)
	{
		return Sanitizer(font).Run();
	}
}
