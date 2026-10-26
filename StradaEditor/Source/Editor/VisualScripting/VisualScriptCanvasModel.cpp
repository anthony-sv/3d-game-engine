#include "Editor/VisualScripting/VisualScriptCanvasModel.h"

#include "Editor/VisualScripting/VisualScriptGeometry.h"
#include "Editor/VisualScripting/VisualScriptNodeTitles.h"

#include "Strada/Core/Hash.h"
#include "Strada/Math/Math.h"
#include "Strada/Script/ScriptField.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>

namespace Strada
{
	namespace
	{
		namespace Metrics = VisualScriptCanvasMetrics;

		constexpr uint32_t NoIndex = std::numeric_limits<uint32_t>::max();
		constexpr float Unlimited = std::numeric_limits<float>::infinity();
		// Texts longer than a text reference holds are cut, at a character boundary.
		constexpr size_t MaxTextBytes = std::numeric_limits<uint16_t>::max();
		constexpr std::string_view NoneText = "None";
		constexpr std::string_view SelfText = "Self";
		// printf's %g: six significant digits. Chips too narrow for them show fewer.
		constexpr int DefaultPrecision = 6;

		// The symbols of the binary operators' compact shape, in UTF-8 ("−", "×", "÷", "≠", "≤", "≥" are escaped).
		struct OperatorSymbol
		{
			std::string_view Type;
			std::string_view Symbol;
		};

		constexpr std::array<OperatorSymbol, 14> OperatorSymbols = {{
			{"Operator.Add", "+"},
			{"Operator.Subtract", "\xE2\x88\x92"},
			{"Operator.Multiply", "\xC3\x97"},
			{"Operator.Divide", "\xC3\xB7"},
			{"Operator.Modulo", "%"},
			{"Operator.Equal", "="},
			{"Operator.NotEqual", "\xE2\x89\xA0"},
			{"Operator.Less", "<"},
			{"Operator.LessOrEqual", "\xE2\x89\xA4"},
			{"Operator.Greater", ">"},
			{"Operator.GreaterOrEqual", "\xE2\x89\xA5"},
			{"Operator.And", "AND"},
			{"Operator.Or", "OR"},
			{"Operator.Xor", "XOR"},
		}};

		uint64_t HashBits(float value)
		{
			return std::bit_cast<uint32_t>(value);
		}

		uint64_t HashText(uint64_t seed, std::string_view text)
		{
			return Hash::Combine(seed, Hash::FNV1a(text));
		}

		uint64_t HashVector(uint64_t seed, glm::vec2 value)
		{
			return Hash::Combine(Hash::Combine(seed, HashBits(value.x)), HashBits(value.y));
		}

		uint64_t HashRect(uint64_t seed, VisualScriptRect const& rect)
		{
			return HashVector(HashVector(seed, rect.Min), rect.Max);
		}

		// Of a JSON value as written: kinds, keys, order, and numbers bit for bit.
		uint64_t HashJson(uint64_t seed, Json const& value)
		{
			switch (value.type())
			{
				case Json::value_t::null:
					return Hash::Combine(seed, 1);
				case Json::value_t::boolean:
					return Hash::Combine(seed, value.get<bool>() ? 3 : 2);
				case Json::value_t::number_integer:
					return Hash::Combine(Hash::Combine(seed, 4), static_cast<uint64_t>(value.get<int64_t>()));
				case Json::value_t::number_unsigned:
					return Hash::Combine(Hash::Combine(seed, 5), value.get<uint64_t>());
				case Json::value_t::number_float:
					return Hash::Combine(Hash::Combine(seed, 6), std::bit_cast<uint64_t>(value.get<double>()));
				case Json::value_t::string:
					return HashText(Hash::Combine(seed, 7), value.get_ref<std::string const&>());
				case Json::value_t::array:
				{
					uint64_t hash = Hash::Combine(Hash::Combine(seed, 8), value.size());
					for (Json const& element : value)
					{
						hash = HashJson(hash, element);
					}
					return hash;
				}
				case Json::value_t::object:
				{
					uint64_t hash = Hash::Combine(Hash::Combine(seed, 9), value.size());
					for (auto const& member : value.items())
					{
						hash = HashJson(HashText(hash, member.key()), member.value());
					}
					return hash;
				}
				case Json::value_t::binary:
				case Json::value_t::discarded:
					return Hash::Combine(seed, 10);
			}
			return Hash::Combine(seed, 11);
		}

		uint64_t HashPin(uint64_t seed, VisualScriptPin const& pin)
		{
			uint64_t hash = HashText(HashText(HashText(seed, pin.Name), pin.Type), pin.Label);
			hash = Hash::Combine(hash, static_cast<uint64_t>(pin.Direction) | (pin.DefaultsToSelf ? 2u : 0u));
			return HashJson(hash, pin.Default);
		}

		// Of everything a node's drawing takes from its analysis entry.
		uint64_t HashEntry(uint64_t seed, VisualScriptAnalyzedNode const& entry)
		{
			uint64_t hash = HashText(HashText(HashText(seed, entry.Title), entry.Subtitle), entry.PassThrough);
			hash = HashText(HashText(hash, entry.Type), entry.CanonicalType);
			hash = Hash::Combine(hash, static_cast<uint64_t>(entry.Kind) | (entry.Pure ? 0x100u : 0u) | (entry.Latent ? 0x200u : 0u));
			for (VisualScriptPinRename const& rename : entry.PinRenames)
			{
				hash = HashText(HashText(Hash::Combine(hash, static_cast<uint64_t>(rename.Direction)), rename.From), rename.To);
			}
			for (VisualScriptPin const& pin : entry.Pins)
			{
				hash = HashPin(hash, pin);
			}
			return hash;
		}

		// The end of the character that starts at offset.
		size_t NextCodePoint(std::string_view text, size_t offset)
		{
			offset++;
			while (offset < text.size() && (static_cast<uint8_t>(text[offset]) & 0xC0u) == 0x80u)
			{
				offset++;
			}
			return offset;
		}

		// The longest prefix of at most maxBytes that ends at a character boundary.
		std::string_view CutAtBoundary(std::string_view text, size_t maxBytes)
		{
			if (text.size() <= maxBytes)
			{
				return text;
			}
			size_t end = maxBytes;
			while (end > 0 && (static_cast<uint8_t>(text[end]) & 0xC0u) == 0x80u)
			{
				end--;
			}
			return text.substr(0, end);
		}

		std::string_view GetFirstLine(std::string_view text)
		{
			std::string_view line = text.substr(0, std::min(text.find('\n'), text.size()));
			if (!line.empty() && line.back() == '\r')
			{
				line.remove_suffix(1);
			}
			return line;
		}

		Json const* FindMember(Json const& object, std::string_view key)
		{
			if (!object.is_object())
			{
				return nullptr;
			}
			auto const found = object.find(key);
			return found != object.end() ? &*found : nullptr;
		}

		// An ID setting: an integer within uint32.
		std::optional<uint32_t> ReadID(Json const& settings, std::string_view key)
		{
			Json const* value = FindMember(settings, key);
			if (value == nullptr || !value->is_number_integer())
			{
				return std::nullopt;
			}
			if (value->is_number_unsigned())
			{
				uint64_t const id = value->get<uint64_t>();
				return id <= std::numeric_limits<uint32_t>::max() ? std::optional<uint32_t>(static_cast<uint32_t>(id)) : std::nullopt;
			}
			int64_t const id = value->get<int64_t>();
			bool const valid = id >= 0 && id <= static_cast<int64_t>(std::numeric_limits<uint32_t>::max());
			return valid ? std::optional<uint32_t>(static_cast<uint32_t>(id)) : std::nullopt;
		}

		VisualScriptGraph const* FindGraph(VisualScriptDocument const& document, uint32_t function)
		{
			if (function == 0)
			{
				return &document.Graph;
			}
			for (VisualScriptFunction const& candidate : document.Functions)
			{
				if (candidate.ID == function)
				{
					return &candidate.Graph;
				}
			}
			return nullptr;
		}

		using IdIndex = std::pair<uint32_t, uint32_t>;

		bool CompareIDs(IdIndex const& a, IdIndex const& b)
		{
			return a.first < b.first;
		}

		std::optional<uint32_t> FindSorted(std::vector<IdIndex> const& sorted, uint32_t id)
		{
			auto const found = std::lower_bound(sorted.begin(), sorted.end(), IdIndex(id, 0), CompareIDs);
			if (found == sorted.end() || found->first != id)
			{
				return std::nullopt;
			}
			return found->second;
		}

		// Stable, so the first of repeated IDs (a document built in memory) comes first.
		void SortByID(std::vector<IdIndex>& pairs)
		{
			std::stable_sort(pairs.begin(), pairs.end(), CompareIDs);
		}

		bool HaveSameKeys(Json const& a, Json const& b)
		{
			if (!a.is_object() || !b.is_object())
			{
				return a.is_object() == b.is_object();
			}
			if (a.size() != b.size())
			{
				return false;
			}
			for (auto const& member : a.items())
			{
				if (!b.contains(member.key()))
				{
					return false;
				}
			}
			return true;
		}

		uint32_t ToSrgbColor(glm::vec4 linear)
		{
			auto const toByte = [](float value)
			{
				float const clamped = value >= 0.0f ? std::min(value, 1.0f) : 0.0f;
				return static_cast<uint32_t>(std::lround(clamped * 255.0f));
			};
			auto const encode = [&toByte](float channel)
			{
				return toByte(Math::LinearToSrgb(channel >= 0.0f ? std::min(channel, 1.0f) : 0.0f));
			};
			return VisualScriptRgba(encode(linear.r), encode(linear.g), encode(linear.b), toByte(linear.a));
		}

		// %g with the precision; zero is "0" whatever its sign (an identity rotation's Euler angles can be -0).
		std::string FormatFloat(double value, int precision)
		{
			return fmt::format("{:.{}g}", value == 0.0 ? 0.0 : value, precision);
		}

		template<size_t Count>
		std::string JoinCells(std::array<float, Count> const& values, int precision)
		{
			std::string text;
			for (size_t index = 0; index < Count; index++)
			{
				if (index > 0)
				{
					text += '\n';
				}
				text += FormatFloat(values[index], precision);
			}
			return text;
		}

		std::string DumpCompact(Json const& value)
		{
			return value.dump(-1, ' ', false, Json::error_handler_t::replace);
		}

		VisualScriptChipValue MakeInvalid(Json const& literal)
		{
			return {VisualScriptChipKind::Raw, DumpCompact(literal), 0, true};
		}

		std::string GetIntegerText(ScriptFieldValue const& value)
		{
			switch (value.GetType())
			{
				case ScriptFieldType::Int32:
					return std::to_string(value.GetInt32());
				case ScriptFieldType::UInt32:
					return std::to_string(value.GetUInt32());
				case ScriptFieldType::Int64:
					return std::to_string(value.GetInt64());
				case ScriptFieldType::UInt64:
					return std::to_string(value.GetUInt64());
				default:
					return "0";
			}
		}

		// An enum value by name: the first enumerator of its value, for [Flags] enums the enumerators that make it up joined by
		// " | " ("None" for 0 without an enumerator of its own); a value no enumerator names by its number.
		std::string GetEnumText(VisualScriptEnumInfo const& info, ScriptFieldValue const& value)
		{
			std::optional<Json> const canonical = EncodeVisualScriptEnumLiteral(value, info);
			if (canonical && canonical->is_string())
			{
				return canonical->get<std::string>();
			}
			if (canonical && canonical->is_array())
			{
				std::string text;
				for (Json const& name : *canonical)
				{
					if (name.is_string())
					{
						text += (text.empty() ? "" : " | ") + name.get<std::string>();
					}
				}
				return text.empty() ? std::string(NoneText) : text;
			}
			return GetIntegerText(value);
		}

		VisualScriptChipValue FormatEnum(Json const& literal, std::string_view typeId, VisualScriptEnumLookup const& enumLookup)
		{
			std::optional<VisualScriptEnumInfo> const info = enumLookup ? enumLookup(typeId) : std::nullopt;
			if (!info || !info->IsUsable())
			{
				// Without the enumerators only the default can be shown.
				return literal.is_null() ? VisualScriptChipValue{VisualScriptChipKind::Choice, "0", 0, false}
				                         : VisualScriptChipValue{VisualScriptChipKind::Raw, DumpCompact(literal), 0, false};
			}
			if (literal.is_null())
			{
				// The type default: the value 0 of the underlying type.
				return {VisualScriptChipKind::Choice, GetEnumText(*info, ScriptFieldValue::CreateDefault(info->UnderlyingType)), 0, false};
			}
			std::optional<ScriptFieldValue> const value = DecodeVisualScriptEnumLiteral(literal, *info);
			if (!value)
			{
				return MakeInvalid(literal);
			}
			return {VisualScriptChipKind::Choice, GetEnumText(*info, *value), 0, false};
		}

		VisualScriptChipValue FormatValue(ScriptFieldValue const& value, VisualScriptAssetNameLookup const& assetNames, int precision)
		{
			switch (value.GetType())
			{
				case ScriptFieldType::Bool:
					return {VisualScriptChipKind::Check, value.GetBool() ? "true" : "false", 0, false};
				case ScriptFieldType::Int32:
				case ScriptFieldType::UInt32:
				case ScriptFieldType::Int64:
				case ScriptFieldType::UInt64:
					return {VisualScriptChipKind::Number, GetIntegerText(value), 0, false};
				case ScriptFieldType::Float:
					return {VisualScriptChipKind::Number, FormatFloat(value.GetFloat(), precision), 0, false};
				case ScriptFieldType::Double:
					return {VisualScriptChipKind::Number, FormatFloat(value.GetDouble(), precision), 0, false};
				case ScriptFieldType::String:
					return {VisualScriptChipKind::Text, value.GetString(), 0, false};
				case ScriptFieldType::Vector2:
				{
					glm::vec2 const vector = value.GetVector2();
					return {VisualScriptChipKind::Cells, JoinCells(std::array<float, 2>{vector.x, vector.y}, precision), 0, false};
				}
				case ScriptFieldType::Vector3:
				{
					glm::vec3 const vector = value.GetVector3();
					return {VisualScriptChipKind::Cells, JoinCells(std::array<float, 3>{vector.x, vector.y, vector.z}, precision), 0,
					        false};
				}
				case ScriptFieldType::Vector4:
				{
					glm::vec4 const vector = value.GetVector4();
					return {VisualScriptChipKind::Cells, JoinCells(std::array<float, 4>{vector.x, vector.y, vector.z, vector.w}, precision),
					        0, false};
				}
				case ScriptFieldType::Quaternion:
				{
					glm::vec3 const degrees = Math::QuaternionToEulerDegrees(Math::NormalizeRotation(value.GetQuaternion()));
					return {VisualScriptChipKind::Cells, JoinCells(std::array<float, 3>{degrees.x, degrees.y, degrees.z}, precision), 0,
					        false};
				}
				case ScriptFieldType::Color:
					return {VisualScriptChipKind::Color, std::string(), ToSrgbColor(value.GetVector4()), false};
				case ScriptFieldType::Entity:
					return {VisualScriptChipKind::Placeholder, std::string(NoneText), 0, false};
				case ScriptFieldType::Prefab:
				case ScriptFieldType::Asset:
				{
					AssetHandle const handle = value.GetAsset();
					if (!handle.IsValid())
					{
						return {VisualScriptChipKind::Asset, std::string(NoneText), 0, false};
					}
					return {VisualScriptChipKind::Asset, assetNames ? assetNames(handle) : handle.ToString(), 0, false};
				}
				case ScriptFieldType::None:
					break;
			}
			return {};
		}

		bool IsMatrixLiteral(Json const& literal)
		{
			return literal.is_array() && literal.size() == 16 &&
			       std::all_of(literal.begin(), literal.end(),
			                   [](Json const& element)
			                   {
								   return element.is_number();
							   });
		}

		// Whether a type's values are floating-point numbers or made of them: their chips can show fewer digits.
		bool HasFloats(VisualScriptTypeInfo const* type)
		{
			if (type == nullptr)
			{
				return false;
			}
			switch (type->FieldType)
			{
				case ScriptFieldType::Float:
				case ScriptFieldType::Double:
				case ScriptFieldType::Vector2:
				case ScriptFieldType::Vector3:
				case ScriptFieldType::Vector4:
				case ScriptFieldType::Quaternion:
					return true;
				default:
					return false;
			}
		}

		VisualScriptChipValue FormatLiteral(Json const& literal, std::string_view typeId, VisualScriptTypeInfo const* type,
		                                    VisualScriptEnumLookup const& enumLookup, VisualScriptAssetNameLookup const& assetNames,
		                                    int precision)
		{
			if (typeId == VisualScriptExecType)
			{
				return {};
			}
			if (type == nullptr)
			{
				// A wildcard not yet decided, or a type the library does not have: nothing reads the literal yet.
				return literal.is_null() ? VisualScriptChipValue{}
				                         : VisualScriptChipValue{VisualScriptChipKind::Raw, DumpCompact(literal), 0, false};
			}
			switch (type->Kind)
			{
				case VisualScriptTypeKind::Entity:
					// Graphs are shared assets: an entity literal can only be none ("0").
					if (literal.is_null() || (literal.is_string() && literal.get_ref<std::string const&>() == "0"))
					{
						return {VisualScriptChipKind::Placeholder, std::string(NoneText), 0, false};
					}
					return MakeInvalid(literal);
				case VisualScriptTypeKind::Matrix:
					if (literal.is_null() || IsMatrixLiteral(literal))
					{
						return {VisualScriptChipKind::Matrix, "Matrix", 0, false};
					}
					return MakeInvalid(literal);
				case VisualScriptTypeKind::Enum:
					return FormatEnum(literal, typeId, enumLookup);
				default:
					break;
			}
			if (!type->HasLiteral)
			{
				return literal.is_null() ? VisualScriptChipValue{VisualScriptChipKind::Placeholder, std::string(NoneText), 0, false}
				                         : MakeInvalid(literal);
			}
			if (type->FieldType == ScriptFieldType::None)
			{
				// A literal form the editor does not read: shown as stored.
				return literal.is_null() ? VisualScriptChipValue{}
				                         : VisualScriptChipValue{VisualScriptChipKind::Raw, DumpCompact(literal), 0, false};
			}
			if (literal.is_null())
			{
				return FormatValue(ScriptFieldValue::CreateDefault(type->FieldType), assetNames, precision);
			}
			std::optional<ScriptFieldValue> const value = DecodeVisualScriptLiteral(literal, typeId, enumLookup);
			if (!value)
			{
				return MakeInvalid(literal);
			}
			return FormatValue(*value, assetNames, precision);
		}

		template<typename T>
		size_t GetCapacity(std::vector<T> const& vector)
		{
			return vector.capacity() * sizeof(T);
		}

		size_t GetCapacity(std::string const& text)
		{
			return text.capacity();
		}
	}

	VisualScriptChipValue FormatVisualScriptLiteral(Json const& literal, std::string_view typeId, VisualScriptTypeInfo const* type,
	                                                VisualScriptEnumLookup const& enumLookup, VisualScriptAssetNameLookup const& assetNames)
	{
		return FormatLiteral(literal, typeId, type, enumLookup, assetNames, DefaultPrecision);
	}

	std::string_view GetVisualScriptOperatorSymbol(std::string_view nodeType)
	{
		for (OperatorSymbol const& symbol : OperatorSymbols)
		{
			if (symbol.Type == nodeType)
			{
				return symbol.Symbol;
			}
		}
		return {};
	}

	bool MatchesVisualScriptAnalysis(VisualScriptNode const& node, VisualScriptNode const& counterpart,
	                                 VisualScriptAnalyzedNode const& entry)
	{
		bool const type = node.Type == counterpart.Type || (!entry.CanonicalType.empty() && node.Type == entry.CanonicalType);
		return type && node.Settings == counterpart.Settings && HaveSameKeys(node.Inputs, counterpart.Inputs) &&
		       node.Split == counterpart.Split;
	}

	std::string_view MapVisualScriptPinName(VisualScriptAnalyzedNode const* entry, std::string_view stored,
	                                        VisualScriptPinDirection direction, std::string& buffer)
	{
		if (entry == nullptr || entry->PinRenames.empty())
		{
			return stored;
		}
		VisualScriptPinRename const* best = nullptr;
		for (VisualScriptPinRename const& rename : entry->PinRenames)
		{
			if (rename.Direction != direction)
			{
				continue;
			}
			if (rename.From == stored)
			{
				return rename.To;
			}
			bool const field = stored.size() > rename.From.size() && stored.starts_with(rename.From) && stored[rename.From.size()] == '.';
			if (field && (best == nullptr || rename.From.size() > best->From.size()))
			{
				best = &rename;
			}
		}
		if (best == nullptr)
		{
			return stored;
		}
		buffer = best->To;
		buffer += stored.substr(best->From.size());
		return buffer;
	}

	// The buffers a Sync works in, kept from one Sync to the next so an unchanged document allocates nothing.
	struct VisualScriptCanvasModel::Scratch
	{
		enum class Source : uint8_t
		{
			Analyzed = 0,
			Template,
			Inferred
		};

		// A pin of the node being laid out, before it is styled.
		struct PinDraft
		{
			std::string_view Name;
			std::string_view Label;
			std::string_view Type;
			// The pin's default literal; null when none.
			Json const* Default = nullptr;
			VisualScriptPinDirection Direction = VisualScriptPinDirection::In;
			bool DefaultsToSelf = false;
			bool Orphan = false;
		};

		// Per visual node: its index in the shown graph's Nodes; per node of the graph: its visual index (NoIndex: left out).
		std::vector<uint32_t> VisualNodes;
		std::vector<uint32_t> DocumentToVisual;
		// Per link of the shown graph: the visual indices of its end nodes (NoIndex when one is not there), an error on it, and
		// per visual node the links that end at it (CSR).
		std::vector<uint32_t> LinkFrom;
		std::vector<uint32_t> LinkTo;
		std::vector<uint8_t> LinkErrors;
		std::vector<uint32_t> NodeLinkStarts;
		std::vector<uint32_t> NodeLinks;
		std::vector<uint32_t> Fill;
		// By ID: the analysis' nodes of the shown graph, the analyzed document's, and the problems located on nodes.
		std::vector<IdIndex> AnalysisNodes;
		std::vector<IdIndex> AnalyzedNodes;
		std::vector<IdIndex> NodeDiagnostics;
		// The node being laid out.
		std::vector<VisualScriptDiagnostic const*> Diagnostics;
		std::vector<PinDraft> Pins;
		std::vector<uint32_t> Boundaries;
		std::vector<std::string_view> FieldNames;
		std::vector<uint32_t> CommentOrder;
		std::vector<VisualScriptCommentVisual> SortedComments;
		std::vector<LayoutRecord> SortedCommentRecords;
		std::string NameBuffer;
		std::string OtherNameBuffer;
		std::string LabelBuffer;
		std::string ChipBuffer;

		size_t GetCapacityBytes() const
		{
			return GetCapacity(VisualNodes) + GetCapacity(DocumentToVisual) + GetCapacity(LinkFrom) + GetCapacity(LinkTo) +
			       GetCapacity(LinkErrors) + GetCapacity(NodeLinkStarts) + GetCapacity(NodeLinks) + GetCapacity(Fill) +
			       GetCapacity(AnalysisNodes) + GetCapacity(AnalyzedNodes) + GetCapacity(NodeDiagnostics) + GetCapacity(Diagnostics) +
			       GetCapacity(Pins) + GetCapacity(Boundaries) + GetCapacity(FieldNames) + GetCapacity(CommentOrder) +
			       GetCapacity(SortedComments) + GetCapacity(SortedCommentRecords) + GetCapacity(NameBuffer) +
			       GetCapacity(OtherNameBuffer) + GetCapacity(LabelBuffer) + GetCapacity(ChipBuffer);
		}
	};

	// One Sync: lays out what changed into the model's current buffers and copies the rest from the spare ones, which hold the
	// previous Sync's results.
	class VisualScriptCanvasModel::Builder
	{
	public:
		Builder(VisualScriptCanvasModel& model, VisualScriptCanvasSource const& source, VisualScriptTextMeasurer const& measure,
		        VisualScriptThemeAnchors const& theme)
			: m_Model(model),
			  m_Source(source),
			  m_Measure(measure),
			  m_Theme(theme),
			  m_Scratch(*model.m_Scratch)
		{
		}

		void Run()
		{
			m_Graph = m_Source.Display != nullptr ? FindGraph(*m_Source.Display, m_Source.Function) : nullptr;
			if (m_Source.Analysis != nullptr && m_Source.AnalyzedDocument != nullptr)
			{
				m_AnalyzedGraph = FindGraph(*m_Source.AnalyzedDocument, m_Source.Function);
			}
			IndexNodes();
			IndexLinks();
			IndexAnalysis();
			uint32_t const nodeCount = static_cast<uint32_t>(m_Scratch.VisualNodes.size());
			m_Model.m_Nodes.reserve(nodeCount);
			m_Model.m_NodeRecords.reserve(nodeCount);
			for (uint32_t visual = 0; visual < nodeCount; visual++)
			{
				BuildNode(visual);
			}
			BuildLinks();
			BuildComments();
		}

	private:
		using Source = Scratch::Source;
		using PinDraft = Scratch::PinDraft;

		// What a node is drawn from.
		struct NodeSource
		{
			Source From = Source::Inferred;
			VisualScriptAnalyzedNode const* Entry = nullptr;
			VisualScriptNodeTemplate const* Template = nullptr;
			std::string Title;
			std::string_view Subtitle;
			VisualScriptNodeKind Kind = VisualScriptNodeKind::Unknown;
			bool Pure = false;
			bool Latent = false;
			// A Variable.Get's variable or local, which its pill shows.
			std::string_view VariableName;
		};

		// What a chip is drawn as: its width, and the room its text (or each of its cells' texts) has.
		struct ChipLayout
		{
			float Width = 0.0f;
			float TextRoom = 0.0f;
		};

		// The nodes of the shown graph get visual indices in document order; m_NodeIndices maps their IDs to them. Of repeated
		// IDs (a document built in memory), the first node is drawn.
		void IndexNodes()
		{
			Scratch& scratch = m_Scratch;
			std::vector<IdIndex>& indices = m_Model.m_NodeIndices;
			indices.clear();
			scratch.VisualNodes.clear();
			scratch.DocumentToVisual.clear();
			if (m_Graph == nullptr)
			{
				return;
			}
			for (uint32_t index = 0; index < m_Graph->Nodes.size(); index++)
			{
				indices.emplace_back(m_Graph->Nodes[index].ID, index);
			}
			SortByID(indices);
			indices.erase(std::unique(indices.begin(), indices.end(),
			                          [](IdIndex const& a, IdIndex const& b)
			                          {
										  return a.first == b.first;
									  }),
			              indices.end());
			scratch.DocumentToVisual.assign(m_Graph->Nodes.size(), NoIndex);
			for (IdIndex const& entry : indices)
			{
				scratch.DocumentToVisual[entry.second] = 0;
			}
			for (uint32_t index = 0; index < m_Graph->Nodes.size(); index++)
			{
				if (scratch.DocumentToVisual[index] != NoIndex)
				{
					scratch.DocumentToVisual[index] = static_cast<uint32_t>(scratch.VisualNodes.size());
					scratch.VisualNodes.push_back(index);
				}
			}
			for (IdIndex& entry : indices)
			{
				entry.second = scratch.DocumentToVisual[entry.second];
			}
		}

		void IndexLinks()
		{
			Scratch& scratch = m_Scratch;
			size_t const linkCount = m_Graph != nullptr ? m_Graph->Links.size() : 0;
			size_t const nodeCount = scratch.VisualNodes.size();
			scratch.LinkFrom.assign(linkCount, NoIndex);
			scratch.LinkTo.assign(linkCount, NoIndex);
			scratch.NodeLinkStarts.assign(nodeCount + 1, 0);
			for (size_t index = 0; index < linkCount; index++)
			{
				VisualScriptLink const& link = m_Graph->Links[index];
				std::optional<uint32_t> const from = FindSorted(m_Model.m_NodeIndices, link.From.Node);
				std::optional<uint32_t> const to = FindSorted(m_Model.m_NodeIndices, link.To.Node);
				if (!from || !to)
				{
					continue;
				}
				scratch.LinkFrom[index] = *from;
				scratch.LinkTo[index] = *to;
				scratch.NodeLinkStarts[*from + 1]++;
				if (*to != *from)
				{
					scratch.NodeLinkStarts[*to + 1]++;
				}
			}
			for (size_t node = 0; node < nodeCount; node++)
			{
				scratch.NodeLinkStarts[node + 1] += scratch.NodeLinkStarts[node];
			}
			scratch.NodeLinks.assign(scratch.NodeLinkStarts.back(), 0);
			scratch.Fill.assign(scratch.NodeLinkStarts.begin(), scratch.NodeLinkStarts.end() - 1);
			for (uint32_t index = 0; index < linkCount; index++)
			{
				uint32_t const from = scratch.LinkFrom[index];
				uint32_t const to = scratch.LinkTo[index];
				if (from == NoIndex)
				{
					continue;
				}
				scratch.NodeLinks[scratch.Fill[from]++] = index;
				if (to != from)
				{
					scratch.NodeLinks[scratch.Fill[to]++] = index;
				}
			}
		}

		void IndexAnalysis()
		{
			Scratch& scratch = m_Scratch;
			scratch.AnalysisNodes.clear();
			scratch.AnalyzedNodes.clear();
			scratch.NodeDiagnostics.clear();
			scratch.LinkErrors.assign(m_Graph != nullptr ? m_Graph->Links.size() : 0, 0);
			VisualScriptAnalysis const* analysis = m_Source.Analysis;
			if (analysis == nullptr || m_AnalyzedGraph == nullptr)
			{
				return;
			}
			for (uint32_t index = 0; index < analysis->Nodes.size(); index++)
			{
				if (analysis->Nodes[index].Function == m_Source.Function)
				{
					scratch.AnalysisNodes.emplace_back(analysis->Nodes[index].ID, index);
				}
			}
			SortByID(scratch.AnalysisNodes);
			for (uint32_t index = 0; index < m_AnalyzedGraph->Nodes.size(); index++)
			{
				scratch.AnalyzedNodes.emplace_back(m_AnalyzedGraph->Nodes[index].ID, index);
			}
			SortByID(scratch.AnalyzedNodes);
			if (!m_Source.ShowDiagnostics || m_Graph == nullptr)
			{
				return;
			}
			for (uint32_t index = 0; index < analysis->Diagnostics.size(); index++)
			{
				VisualScriptDiagnostic const& diagnostic = analysis->Diagnostics[index];
				if (diagnostic.Function != m_Source.Function)
				{
					continue;
				}
				if (diagnostic.Node)
				{
					scratch.NodeDiagnostics.emplace_back(*diagnostic.Node, index);
				}
				if (diagnostic.Link && diagnostic.Severity == VisualScriptSeverity::Error)
				{
					MarkLinkError(*diagnostic.Link);
				}
			}
			SortByID(scratch.NodeDiagnostics);
		}

		void MarkLinkError(VisualScriptLink const& link)
		{
			std::optional<uint32_t> const from = FindSorted(m_Model.m_NodeIndices, link.From.Node);
			if (!from)
			{
				return;
			}
			Scratch& scratch = m_Scratch;
			for (uint32_t entry = scratch.NodeLinkStarts[*from]; entry < scratch.NodeLinkStarts[*from + 1]; entry++)
			{
				uint32_t const index = scratch.NodeLinks[entry];
				if (m_Graph->Links[index] == link)
				{
					scratch.LinkErrors[index] = 1;
				}
			}
		}

		// The problems located on a node, in the analysis' order: valid until the next call.
		std::span<VisualScriptDiagnostic const* const> GetNodeDiagnostics(uint32_t id)
		{
			std::vector<VisualScriptDiagnostic const*>& found = m_Scratch.Diagnostics;
			found.clear();
			std::vector<IdIndex> const& sorted = m_Scratch.NodeDiagnostics;
			for (auto entry = std::lower_bound(sorted.begin(), sorted.end(), IdIndex(id, 0), CompareIDs);
			     entry != sorted.end() && entry->first == id; ++entry)
			{
				found.push_back(&m_Source.Analysis->Diagnostics[entry->second]);
			}
			return found;
		}

		VisualScriptNode const& GetDocumentNode(uint32_t visual) const { return m_Graph->Nodes[m_Scratch.VisualNodes[visual]]; }

		// A Variable.Get's variable: a local of the shown function, else a variable of the document.
		std::string_view FindVariableName(VisualScriptNode const& node) const
		{
			std::optional<uint32_t> const id = ReadID(node.Settings, "Variable");
			if (!id)
			{
				return {};
			}
			VisualScriptDocument const& document = *m_Source.Display;
			for (VisualScriptFunction const& function : document.Functions)
			{
				if (function.ID != m_Source.Function)
				{
					continue;
				}
				for (VisualScriptLocal const& local : function.Locals)
				{
					if (local.ID == *id)
					{
						return local.Name;
					}
				}
			}
			for (VisualScriptVariable const& variable : document.Variables)
			{
				if (variable.ID == *id)
				{
					return variable.Name;
				}
			}
			return {};
		}

		NodeSource FindSource(VisualScriptNode const& node) const
		{
			NodeSource source;
			if (m_AnalyzedGraph != nullptr)
			{
				std::optional<uint32_t> const entry = FindSorted(m_Scratch.AnalysisNodes, node.ID);
				std::optional<uint32_t> const counterpart = FindSorted(m_Scratch.AnalyzedNodes, node.ID);
				if (entry && counterpart &&
				    MatchesVisualScriptAnalysis(node, m_AnalyzedGraph->Nodes[*counterpart], m_Source.Analysis->Nodes[*entry]))
				{
					VisualScriptAnalyzedNode const& analyzed = m_Source.Analysis->Nodes[*entry];
					source.From = Source::Analyzed;
					source.Entry = &analyzed;
					source.Title = analyzed.Title;
					source.Subtitle = analyzed.Subtitle;
					source.Kind = analyzed.Kind;
					source.Pure = analyzed.Pure;
					source.Latent = analyzed.Latent;
				}
			}
			if (source.From != Source::Analyzed)
			{
				VisualScriptNodeTemplate const* kind = m_Source.Library != nullptr ? m_Source.Library->FindNode(node.Type) : nullptr;
				if (kind != nullptr)
				{
					source.From = Source::Template;
					source.Template = kind;
					source.Title = kind->Title;
					source.Subtitle = kind->Subtitle;
					source.Kind = kind->Kind;
					source.Pure = kind->Pure;
					source.Latent = kind->Latent;
				}
				else
				{
					source.Title = DeriveVisualScriptNodeTitle(node, *m_Source.Display);
				}
			}
			if (node.Type == "Variable.Get")
			{
				source.VariableName = FindVariableName(node);
			}
			return source;
		}

		// Of everything the node's layout is made from: the node, what its pins and title come from, the pin names its links
		// use, and its problems.
		uint64_t HashNode(VisualScriptNode const& node, NodeSource const& source, uint32_t visual)
		{
			uint64_t hash = HashText(Hash::Combine(0, static_cast<uint64_t>(source.From)), node.Type);
			hash = HashJson(hash, node.Settings);
			hash = HashJson(hash, node.Inputs);
			for (std::string const& entry : node.Split.Inputs)
			{
				hash = HashText(Hash::Combine(hash, 1), entry);
			}
			for (std::string const& entry : node.Split.Outputs)
			{
				hash = HashText(Hash::Combine(hash, 2), entry);
			}
			hash = HashText(hash, node.Comment);
			hash = Hash::Combine(hash, (node.Collapsed ? 1u : 0u) | (static_cast<uint32_t>(node.Enabled) << 1));
			hash = HashText(HashText(HashText(hash, source.Title), source.Subtitle), source.VariableName);
			if (source.Entry != nullptr)
			{
				hash = HashEntry(hash, *source.Entry);
			}
			Scratch const& scratch = m_Scratch;
			for (uint32_t entry = scratch.NodeLinkStarts[visual]; entry < scratch.NodeLinkStarts[visual + 1]; entry++)
			{
				uint32_t const index = scratch.NodeLinks[entry];
				VisualScriptLink const& link = m_Graph->Links[index];
				if (scratch.LinkFrom[index] == visual)
				{
					hash = HashText(Hash::Combine(hash, 3), link.From.Pin);
				}
				if (scratch.LinkTo[index] == visual)
				{
					hash = HashText(Hash::Combine(hash, 4), link.To.Pin);
				}
			}
			for (VisualScriptDiagnostic const* diagnostic : GetNodeDiagnostics(node.ID))
			{
				hash = HashText(Hash::Combine(hash, static_cast<uint64_t>(diagnostic->Severity)), diagnostic->Pin);
				hash = Hash::Combine(hash, diagnostic->Direction ? static_cast<uint64_t>(*diagnostic->Direction) + 1 : 0);
			}
			return hash;
		}

		void BuildNode(uint32_t visual)
		{
			VisualScriptNode const& node = GetDocumentNode(visual);
			NodeSource const source = FindSource(node);
			uint64_t const hash = HashNode(node, source, visual);
			glm::vec2 const position = node.Position.value_or(glm::vec2(0.0f));
			std::optional<uint32_t> const previous = FindSorted(m_Model.m_SpareNodeIndices, node.ID);
			if (previous)
			{
				LayoutRecord const& record = m_Model.m_SpareNodeRecords[*previous];
				bool const namesChanged = record.ShowsAssetNames && m_Source.AssetVersion != m_Model.m_AssetVersion;
				if (record.Hash == hash && !namesChanged)
				{
					KeepNode(*previous, position, hash);
					return;
				}
			}
			m_Model.m_MeasuredNodeCount++;
			MeasureNode(node, source, visual, position, hash);
		}

		static void Rebase(VisualScriptTextRef& ref, int64_t shift)
		{
			if (ref.Length > 0)
			{
				ref.Offset = static_cast<uint32_t>(static_cast<int64_t>(ref.Offset) + shift);
			}
		}

		// Copies a node the previous Sync laid out, at its new position.
		void KeepNode(uint32_t previous, glm::vec2 position, uint64_t hash)
		{
			VisualScriptNodeVisual visual = m_Model.m_SpareNodes[previous];
			LayoutRecord const& record = m_Model.m_SpareNodeRecords[previous];
			uint32_t const textOffset = static_cast<uint32_t>(m_Model.m_Text.size());
			m_Model.m_Text.append(m_Model.m_SpareText, record.TextOffset, record.TextLength);
			int64_t const shift = static_cast<int64_t>(textOffset) - static_cast<int64_t>(record.TextOffset);
			Rebase(visual.Title, shift);
			Rebase(visual.Subtitle, shift);
			Rebase(visual.Comment, shift);
			uint32_t const firstPin = static_cast<uint32_t>(m_Model.m_Pins.size());
			for (uint32_t pin = 0; pin < visual.PinCount; pin++)
			{
				VisualScriptPinVisual& moved = m_Model.m_Pins.emplace_back(std::move(m_Model.m_SparePins[visual.FirstPin + pin]));
				Rebase(moved.Name, shift);
				Rebase(moved.Label, shift);
				Rebase(moved.ChipText, shift);
			}
			visual.FirstPin = firstPin;
			glm::vec2 const delta = position - visual.Position;
			visual.Position = position;
			visual.VisualBounds = VisualScriptGeometry::Translate(visual.VisualBounds, delta);
			m_Model.m_Nodes.push_back(visual);
			m_Model.m_NodeRecords.push_back({hash, textOffset, record.TextLength, record.AnchorHash, record.ShowsAssetNames});
		}

		// Appends a text to the arena; with a size, measures it and ellipsizes it to maxWidth. Empty text gives an empty
		// reference.
		VisualScriptTextRef AddText(std::string_view text, float sizeCu = 0.0f, float maxWidth = Unlimited)
		{
			std::string_view const kept = CutAtBoundary(text, MaxTextBytes);
			if (kept.empty())
			{
				return {};
			}
			VisualScriptTextRef ref;
			ref.Offset = static_cast<uint32_t>(m_Model.m_Text.size());
			ref.Length = static_cast<uint16_t>(kept.size());
			ref.VisibleLength = ref.Length;
			m_Model.m_Text.append(kept);
			if (sizeCu > 0.0f)
			{
				Fit(kept, sizeCu, maxWidth, ref);
			}
			return ref;
		}

		// Measures the text and, when it is wider than maxWidth, ellipsizes it: the longest prefix of whole characters that
		// fits with the ellipsis after it (at least the ellipsis).
		void Fit(std::string_view text, float sizeCu, float maxWidth, VisualScriptTextRef& ref)
		{
			ref.VisibleLength = ref.Length;
			ref.Width = m_Measure(text, sizeCu);
			if (ref.Width <= maxWidth)
			{
				return;
			}
			float const ellipsis = m_Measure(VisualScriptEllipsis, sizeCu);
			std::vector<uint32_t>& boundaries = m_Scratch.Boundaries;
			boundaries.clear();
			for (size_t offset = 0; offset < text.size(); offset = NextCodePoint(text, offset))
			{
				boundaries.push_back(static_cast<uint32_t>(offset));
			}
			// Prefixes of boundaries[low] bytes fit; the whole text does not.
			size_t low = 0;
			size_t high = boundaries.size();
			while (high - low > 1)
			{
				size_t const middle = (low + high) / 2;
				if (m_Measure(text.substr(0, boundaries[middle]), sizeCu) + ellipsis <= maxWidth)
				{
					low = middle;
				}
				else
				{
					high = middle;
				}
			}
			size_t const visible = boundaries.empty() ? 0 : boundaries[low];
			ref.VisibleLength = static_cast<uint16_t>(visible);
			ref.Width = (visible > 0 ? m_Measure(text.substr(0, visible), sizeCu) : 0.0f) + ellipsis;
		}

		// Wraps text greedily at spaces (inside words longer than a line too) into lines of at most maxWidth, keeping its own
		// line breaks, and at most maxLines lines: when text is left out, the last line kept ends with an ellipsis. Appends the
		// lines joined by '\n'; the reference's width is the widest line's.
		VisualScriptTextRef AddWrappedText(std::string_view text, float sizeCu, float maxWidth, int maxLines)
		{
			std::string& out = m_Model.m_Text;
			size_t const start = out.size();
			float const space = m_Measure(" ", sizeCu);
			int lineCount = 1;
			float lineWidth = 0.0f;
			bool lineEmpty = true;
			float widest = 0.0f;
			bool truncated = false;
			auto const breakLine = [&]() -> bool
			{
				if (lineCount >= maxLines)
				{
					truncated = true;
					return false;
				}
				out += '\n';
				lineCount++;
				lineWidth = 0.0f;
				lineEmpty = true;
				return true;
			};
			auto const place = [&](std::string_view word) -> bool
			{
				while (!word.empty())
				{
					float const width = m_Measure(word, sizeCu);
					float const needed = lineEmpty ? width : lineWidth + space + width;
					if (needed <= maxWidth)
					{
						if (!lineEmpty)
						{
							out += ' ';
						}
						out.append(word);
						lineWidth = needed;
						lineEmpty = false;
						widest = std::max(widest, lineWidth);
						return true;
					}
					if (!lineEmpty)
					{
						if (!breakLine())
						{
							return false;
						}
						continue;
					}
					// Alone on its line and still too wide: as many characters as fit (at least one), the rest on the next line.
					size_t end = NextCodePoint(word, 0);
					while (end < word.size())
					{
						size_t const next = NextCodePoint(word, end);
						if (m_Measure(word.substr(0, next), sizeCu) > maxWidth)
						{
							break;
						}
						end = next;
					}
					std::string_view const part = word.substr(0, end);
					out.append(part);
					lineWidth = m_Measure(part, sizeCu);
					lineEmpty = false;
					widest = std::max(widest, lineWidth);
					word.remove_prefix(end);
					if (!word.empty() && !breakLine())
					{
						return false;
					}
				}
				return true;
			};
			std::string_view rest = CutAtBoundary(text, MaxTextBytes / 2);
			bool firstLine = true;
			while (!truncated)
			{
				size_t const end = std::min(rest.find('\n'), rest.size());
				std::string_view line = rest.substr(0, end);
				if (!line.empty() && line.back() == '\r')
				{
					line.remove_suffix(1);
				}
				if (!firstLine && !breakLine())
				{
					break;
				}
				firstLine = false;
				while (!line.empty() && !truncated)
				{
					size_t const wordEnd = std::min(line.find(' '), line.size());
					if (wordEnd > 0 && !place(line.substr(0, wordEnd)))
					{
						break;
					}
					line.remove_prefix(std::min(wordEnd + 1, line.size()));
				}
				if (end >= rest.size())
				{
					break;
				}
				rest.remove_prefix(end + 1);
			}
			if (truncated)
			{
				// The ellipsis ends the last line kept, within the width: characters before it go when it does not fit.
				float const ellipsis = m_Measure(VisualScriptEllipsis, sizeCu);
				size_t const lastBreak = out.rfind('\n');
				size_t const lineStart = lastBreak == std::string::npos || lastBreak < start ? start : lastBreak + 1;
				while (out.size() > lineStart && m_Measure(std::string_view(out).substr(lineStart), sizeCu) + ellipsis > maxWidth)
				{
					size_t cut = out.size() - 1;
					while (cut > lineStart && (static_cast<uint8_t>(out[cut]) & 0xC0u) == 0x80u)
					{
						cut--;
					}
					out.resize(cut);
				}
				out.append(VisualScriptEllipsis);
				widest = std::max(widest, m_Measure(std::string_view(out).substr(lineStart), sizeCu));
			}
			std::string_view const written = CutAtBoundary(std::string_view(out).substr(start), MaxTextBytes);
			out.resize(start + written.size());
			if (written.empty())
			{
				return {};
			}
			VisualScriptTextRef ref;
			ref.Offset = static_cast<uint32_t>(start);
			ref.Length = static_cast<uint16_t>(written.size());
			ref.VisibleLength = ref.Length;
			ref.Width = widest;
			return ref;
		}

		// The pins of a node as its source gives them, inputs first, then outputs. Names its links and stored literals use that
		// it does not have are added after each side's pins: orphan rows on analyzed nodes, wildcard rows on the others.
		void DraftPins(VisualScriptNode const& node, NodeSource const& source, uint32_t visual)
		{
			std::vector<PinDraft>& pins = m_Scratch.Pins;
			pins.clear();
			std::span<VisualScriptPin const> known;
			if (source.Entry != nullptr)
			{
				known = source.Entry->Pins;
			}
			else if (source.Template != nullptr)
			{
				known = source.Template->Pins;
			}
			for (VisualScriptPinDirection const direction : {VisualScriptPinDirection::In, VisualScriptPinDirection::Out})
			{
				for (VisualScriptPin const& pin : known)
				{
					if (pin.Direction != direction)
					{
						continue;
					}
					PinDraft& draft = pins.emplace_back();
					draft.Name = pin.Name;
					draft.Label = pin.Label;
					draft.Type = pin.Type;
					draft.Default = pin.Default.is_null() ? nullptr : &pin.Default;
					draft.Direction = direction;
					draft.DefaultsToSelf = pin.DefaultsToSelf;
				}
				auto const addMissing = [&](std::string_view stored)
				{
					std::string_view const name = MapVisualScriptPinName(source.Entry, stored, direction, m_Scratch.NameBuffer);
					for (PinDraft const& pin : pins)
					{
						if (pin.Direction == direction && (pin.Name == name || pin.Name == stored))
						{
							return;
						}
					}
					PinDraft& draft = pins.emplace_back();
					draft.Name = stored;
					draft.Type = VisualScriptWildcardType;
					draft.Direction = direction;
					draft.Orphan = source.From == Source::Analyzed;
				};
				if (direction == VisualScriptPinDirection::In && node.Inputs.is_object())
				{
					for (auto const& input : node.Inputs.items())
					{
						addMissing(input.key());
					}
				}
				Scratch const& scratch = m_Scratch;
				for (uint32_t entry = scratch.NodeLinkStarts[visual]; entry < scratch.NodeLinkStarts[visual + 1]; entry++)
				{
					uint32_t const index = scratch.NodeLinks[entry];
					VisualScriptLink const& link = m_Graph->Links[index];
					if (direction == VisualScriptPinDirection::Out && scratch.LinkFrom[index] == visual)
					{
						addMissing(link.From.Pin);
					}
					if (direction == VisualScriptPinDirection::In && scratch.LinkTo[index] == visual)
					{
						addMissing(link.To.Pin);
					}
				}
			}
		}

		bool IsLinked(VisualScriptAnalyzedNode const* entry, uint32_t visual, std::string_view name, VisualScriptPinDirection direction)
		{
			Scratch& scratch = m_Scratch;
			bool const output = direction == VisualScriptPinDirection::Out;
			for (uint32_t item = scratch.NodeLinkStarts[visual]; item < scratch.NodeLinkStarts[visual + 1]; item++)
			{
				uint32_t const index = scratch.NodeLinks[item];
				if ((output ? scratch.LinkFrom[index] : scratch.LinkTo[index]) != visual)
				{
					continue;
				}
				VisualScriptLink const& link = m_Graph->Links[index];
				std::string_view const stored = output ? std::string_view(link.From.Pin) : std::string_view(link.To.Pin);
				if (stored == name || MapVisualScriptPinName(entry, stored, direction, scratch.NameBuffer) == name)
				{
					return true;
				}
			}
			return false;
		}

		// The literal stored for an input, under its name or under the name it had before the analysis renamed it.
		Json const* FindStoredLiteral(VisualScriptNode const& node, VisualScriptAnalyzedNode const* entry, std::string_view name)
		{
			if (!node.Inputs.is_object())
			{
				return nullptr;
			}
			for (auto const& input : node.Inputs.items())
			{
				if (input.key() == name ||
				    MapVisualScriptPinName(entry, input.key(), VisualScriptPinDirection::In, m_Scratch.NameBuffer) == name)
				{
					return &input.value();
				}
			}
			return nullptr;
		}

		// Whether the node splits the pin of this path (as the canvas names it) in a direction: its Split list names the path, or
		// names it as it was before the analysis renamed the pin.
		bool IsSplitParent(VisualScriptNode const& node, VisualScriptAnalyzedNode const* entry, std::string_view path,
		                   VisualScriptPinDirection direction)
		{
			std::vector<std::string> const& split = direction == VisualScriptPinDirection::In ? node.Split.Inputs : node.Split.Outputs;
			for (std::string const& listed : split)
			{
				if (listed == path || MapVisualScriptPinName(entry, listed, direction, m_Scratch.OtherNameBuffer) == path)
				{
					return true;
				}
			}
			return false;
		}

		// The colour of a split pin's type, for its fields' bracket: the type the node's template gives the root pin, else the
		// one struct of the library with exactly the fields shown; the wildcard colour when neither tells.
		uint32_t GetBracketColor(NodeSource const& source, std::string_view root, VisualScriptPinDirection direction, size_t firstPin)
		{
			VisualScriptLibrary const* library = m_Source.Library;
			if (library == nullptr)
			{
				return VisualScriptWildcardColor;
			}
			VisualScriptNodeTemplate const* kind = source.Template;
			if (kind == nullptr && source.Entry != nullptr)
			{
				kind = library->FindNode(source.Entry->CanonicalType.empty() ? source.Entry->Type : source.Entry->CanonicalType);
			}
			if (kind != nullptr)
			{
				VisualScriptPin const* pin = kind->FindPin(root, direction);
				if (pin != nullptr && pin->Type != VisualScriptWildcardType)
				{
					return GetVisualScriptTypeColor(pin->Type, library);
				}
			}
			std::vector<PinDraft> const& pins = m_Scratch.Pins;
			std::vector<std::string_view>& fields = m_Scratch.FieldNames;
			fields.clear();
			for (size_t index = firstPin; index < pins.size(); index++)
			{
				std::string_view const name = pins[index].Name;
				if (pins[index].Direction != direction || name.size() <= root.size() || !name.starts_with(root) || name[root.size()] != '.')
				{
					break;
				}
				std::string_view field = name.substr(root.size() + 1);
				field = field.substr(0, std::min(field.find('.'), field.size()));
				if (fields.empty() || fields.back() != field)
				{
					fields.push_back(field);
				}
			}
			VisualScriptTypeInfo const* match = nullptr;
			for (VisualScriptTypeInfo const& type : library->GetTypes())
			{
				bool const splits = direction == VisualScriptPinDirection::In ? type.Makeable : type.Breakable;
				if (!splits || type.Fields.size() != fields.size())
				{
					continue;
				}
				bool same = true;
				for (size_t index = 0; index < fields.size() && same; index++)
				{
					same = type.Fields[index].Name == fields[index];
				}
				if (!same)
				{
					continue;
				}
				if (match != nullptr)
				{
					// Several structs have these fields.
					return VisualScriptWildcardColor;
				}
				match = &type;
			}
			return match != nullptr ? GetVisualScriptTypeColor(match->ID, library) : VisualScriptWildcardColor;
		}

		static ChipLayout GetChipLayout(VisualScriptChipKind kind, VisualScriptTypeInfo const* type, size_t cells)
		{
			float const padding = 2.0f * Metrics::ChipPaddingX;
			switch (kind)
			{
				case VisualScriptChipKind::None:
					return {};
				case VisualScriptChipKind::Check:
				case VisualScriptChipKind::Color:
					return {Metrics::ChipSquareSize, 0.0f};
				case VisualScriptChipKind::Number:
				{
					ScriptFieldType const field = type != nullptr ? type->FieldType : ScriptFieldType::None;
					float width = Metrics::ChipIntegerWidth;
					if (field == ScriptFieldType::Int64 || field == ScriptFieldType::UInt64)
					{
						width = Metrics::ChipLongWidth;
					}
					else if (field == ScriptFieldType::Float || field == ScriptFieldType::Double)
					{
						width = Metrics::ChipFloatWidth;
					}
					return {width, width - padding};
				}
				case VisualScriptChipKind::Text:
				case VisualScriptChipKind::Raw:
					return {Metrics::ChipStringWidth, Metrics::ChipStringWidth - padding};
				case VisualScriptChipKind::Cells:
				{
					float const count = static_cast<float>(std::max<size_t>(cells, 1));
					return {count * Metrics::ChipVectorCellWidth + (count - 1.0f) * Metrics::ChipCellGap,
					        Metrics::ChipVectorCellWidth - padding - Metrics::ChipAxisStripeWidth};
				}
				case VisualScriptChipKind::Choice:
					return {Metrics::ChipEnumWidth, Metrics::ChipEnumWidth - padding - Metrics::ChipArrowBoxWidth};
				case VisualScriptChipKind::Asset:
					return {Metrics::ChipAssetWidth, Metrics::ChipAssetWidth - padding};
				case VisualScriptChipKind::Matrix:
					return {Metrics::ChipMatrixWidth, Metrics::ChipMatrixWidth - padding};
				case VisualScriptChipKind::Placeholder:
					// As wide as its text, which needs no frame.
					return {padding, Unlimited};
			}
			return {};
		}

		// Whether every line of a chip's text fits the room.
		bool FitsCells(std::string_view text, float room)
		{
			while (true)
			{
				size_t const end = std::min(text.find('\n'), text.size());
				if (m_Measure(text.substr(0, end), Metrics::ChipTextSize) > room)
				{
					return false;
				}
				if (end >= text.size())
				{
					return true;
				}
				text.remove_prefix(end + 1);
			}
		}

		// The chip's text: a number or cells with fewer significant digits when theirs do not fit, then ellipsized (cells one
		// by one, each with its own ellipsis in the text).
		VisualScriptTextRef AddChipText(VisualScriptChipValue& value, ChipLayout const& layout, Json const& shown, std::string_view typeId,
		                                VisualScriptTypeInfo const* type)
		{
			bool const numeric = value.Kind == VisualScriptChipKind::Number || value.Kind == VisualScriptChipKind::Cells;
			if (numeric && HasFloats(type) && !FitsCells(value.Text, layout.TextRoom))
			{
				for (int precision = DefaultPrecision - 1; precision >= 1; precision--)
				{
					VisualScriptChipValue shorter = FormatLiteral(shown, typeId, type, m_Source.EnumLookup, m_Source.AssetNames, precision);
					bool const fits = FitsCells(shorter.Text, layout.TextRoom);
					value.Text = std::move(shorter.Text);
					if (fits)
					{
						break;
					}
				}
			}
			switch (value.Kind)
			{
				case VisualScriptChipKind::None:
				case VisualScriptChipKind::Color:
					return {};
				case VisualScriptChipKind::Cells:
				{
					std::string& cells = m_Scratch.ChipBuffer;
					cells.clear();
					std::string_view rest = value.Text;
					float const ellipsis = m_Measure(VisualScriptEllipsis, Metrics::ChipTextSize);
					while (true)
					{
						size_t const end = std::min(rest.find('\n'), rest.size());
						std::string_view cell = rest.substr(0, end);
						if (!cells.empty())
						{
							cells += '\n';
						}
						if (m_Measure(cell, Metrics::ChipTextSize) <= layout.TextRoom)
						{
							cells.append(cell);
						}
						else
						{
							while (!cell.empty() && m_Measure(cell, Metrics::ChipTextSize) + ellipsis > layout.TextRoom)
							{
								size_t cut = cell.size() - 1;
								while (cut > 0 && (static_cast<uint8_t>(cell[cut]) & 0xC0u) == 0x80u)
								{
									cut--;
								}
								cell = cell.substr(0, cut);
							}
							cells.append(cell);
							cells.append(VisualScriptEllipsis);
						}
						if (end >= rest.size())
						{
							break;
						}
						rest.remove_prefix(end + 1);
					}
					VisualScriptTextRef ref = AddText(cells);
					ref.Width = layout.Width;
					return ref;
				}
				case VisualScriptChipKind::Text:
					return AddText(GetFirstLine(value.Text), Metrics::ChipTextSize, layout.TextRoom);
				case VisualScriptChipKind::Check:
				case VisualScriptChipKind::Number:
				case VisualScriptChipKind::Choice:
				case VisualScriptChipKind::Asset:
				case VisualScriptChipKind::Matrix:
				case VisualScriptChipKind::Placeholder:
				case VisualScriptChipKind::Raw:
					return AddText(value.Text, Metrics::ChipTextSize, layout.TextRoom);
			}
			return {};
		}

		VisualScriptTypeInfo const* FindType(std::string_view type) const
		{
			if (m_Source.Library == nullptr || type == VisualScriptExecType || type == VisualScriptWildcardType)
			{
				return nullptr;
			}
			return m_Source.Library->FindType(type);
		}

		void StylePin(VisualScriptPinVisual& pin, PinDraft const& draft) const
		{
			pin.Type = std::string(draft.Type);
			pin.Direction = draft.Direction;
			if (draft.Orphan)
			{
				pin.Shape = VisualScriptPinShape::Wildcard;
				pin.Color = VisualScriptErrorColor;
				pin.Flags |= VisualScriptPinFlags::Orphan;
				return;
			}
			if (draft.Type == VisualScriptExecType)
			{
				pin.Shape = VisualScriptPinShape::Exec;
				pin.TypeKind = VisualScriptTypeKind::Exec;
				pin.Color = m_Theme.Text;
				return;
			}
			if (draft.Type == VisualScriptWildcardType)
			{
				pin.Shape = VisualScriptPinShape::Wildcard;
				pin.Color = VisualScriptWildcardColor;
				return;
			}
			pin.Color = GetVisualScriptTypeColor(draft.Type, m_Source.Library);
			VisualScriptTypeInfo const* type = FindType(draft.Type);
			if (type == nullptr)
			{
				return;
			}
			pin.TypeKind = type->Kind;
			pin.Shape = type->Kind == VisualScriptTypeKind::Array ? VisualScriptPinShape::Array : VisualScriptPinShape::Data;
			bool const splits = draft.Direction == VisualScriptPinDirection::In ? type->Makeable : type->Breakable;
			if (splits && type->Fields.size() >= 2)
			{
				pin.Flags |= VisualScriptPinFlags::Splittable;
			}
		}

		VisualScriptNodeShape ChooseShape(VisualScriptNode const& node, uint32_t visual, VisualScriptAnalyzedNode const* entry)
		{
			std::vector<PinDraft> const& pins = m_Scratch.Pins;
			size_t inputs = 0;
			size_t outputs = 0;
			for (PinDraft const& pin : pins)
			{
				if (pin.Orphan || pin.Name.find('.') != std::string_view::npos)
				{
					// Orphan rows and split fields need the full shape.
					return VisualScriptNodeShape::Standard;
				}
				(pin.Direction == VisualScriptPinDirection::In ? inputs : outputs)++;
			}
			if (node.Type == "Reroute")
			{
				bool const plain = std::all_of(pins.begin(), pins.end(),
				                               [](PinDraft const& pin)
				                               {
												   return pin.Name == (pin.Direction == VisualScriptPinDirection::In ? "In" : "Out");
											   });
				return plain ? VisualScriptNodeShape::Reroute : VisualScriptNodeShape::Standard;
			}
			if ((node.Type == "Variable.Get" || node.Type == "Self") && inputs == 0 && outputs == 1)
			{
				return VisualScriptNodeShape::Pill;
			}
			if (!GetVisualScriptOperatorSymbol(node.Type).empty() && inputs == 2 && outputs == 1)
			{
				// Compact only while no input shows a value of its own: an unlinked input with a stored literal keeps the full
				// shape, so its value stays visible.
				for (PinDraft const& pin : pins)
				{
					if (pin.Direction == VisualScriptPinDirection::In && !IsLinked(entry, visual, pin.Name, pin.Direction) &&
					    FindStoredLiteral(node, entry, pin.Name) != nullptr)
					{
						return VisualScriptNodeShape::Standard;
					}
				}
				return VisualScriptNodeShape::Operator;
			}
			return VisualScriptNodeShape::Standard;
		}

		static uint16_t GetSourceFlag(Source source)
		{
			switch (source)
			{
				case Source::Analyzed:
					return VisualScriptNodeFlags::Analyzed;
				case Source::Template:
					return VisualScriptNodeFlags::Template;
				case Source::Inferred:
					return VisualScriptNodeFlags::Inferred;
			}
			return VisualScriptNodeFlags::Inferred;
		}

		void MeasureNode(VisualScriptNode const& node, NodeSource const& source, uint32_t visual, glm::vec2 position, uint64_t hash)
		{
			DraftPins(node, source, visual);
			std::vector<PinDraft> const& drafts = m_Scratch.Pins;
			uint32_t const textOffset = static_cast<uint32_t>(m_Model.m_Text.size());
			VisualScriptNodeVisual result;
			result.ID = node.ID;
			result.Position = position;
			result.Kind = source.Kind;
			result.Shape = ChooseShape(node, visual, source.Entry);

			uint16_t flags = GetSourceFlag(source.From);
			flags |= source.Pure ? VisualScriptNodeFlags::Pure : 0;
			flags |= source.Latent ? VisualScriptNodeFlags::Latent : 0;
			flags |= node.Collapsed ? VisualScriptNodeFlags::Collapsed : 0;
			flags |= node.Enabled == VisualScriptNodeEnabled::Disabled ? VisualScriptNodeFlags::Disabled : 0;
			flags |= node.Enabled == VisualScriptNodeEnabled::DevelopmentOnly ? VisualScriptNodeFlags::DevelopmentOnly : 0;
			bool const functionNode = node.Type == "Function.Entry" || node.Type == "Function.Return" || node.Type == "Function.Call";
			flags |= functionNode ? VisualScriptNodeFlags::FunctionNode : 0;
			// The library (or the analysis) does not know the type. Without a library nothing is known, which is not that.
			bool const unknownType = (source.From == Source::Analyzed && source.Kind == VisualScriptNodeKind::Unknown) ||
			                         (source.From == Source::Inferred && m_Source.Library != nullptr);
			flags |= unknownType ? VisualScriptNodeFlags::UnknownType : 0;
			result.Flags = flags;

			// Colours: the kind's, with its template's category; a node nothing describes (no library) is drawn neutral.
			std::string_view category;
			if (source.Template != nullptr)
			{
				category = source.Template->Category;
			}
			else if (source.Entry != nullptr && m_Source.Library != nullptr)
			{
				std::string_view const type = source.Entry->CanonicalType.empty() ? source.Entry->Type : source.Entry->CanonicalType;
				VisualScriptNodeTemplate const* kind = m_Source.Library->FindNode(type);
				category = kind != nullptr ? std::string_view(kind->Category) : std::string_view();
			}
			bool const neutral = source.From == Source::Inferred && m_Source.Library == nullptr;
			uint32_t const header = GetVisualScriptHeaderColor(neutral ? VisualScriptNodeKind::Flow : source.Kind, category);
			VisualScriptNodeColors const colors = ComputeVisualScriptNodeColors(header, m_Theme);
			result.HeaderColor = colors.Header;
			result.HeaderEndColor = colors.HeaderEnd;
			result.BoxColor = colors.Box;

			uint32_t errors = 0;
			uint32_t warnings = 0;
			for (VisualScriptDiagnostic const* diagnostic : GetNodeDiagnostics(node.ID))
			{
				(diagnostic->Severity == VisualScriptSeverity::Error ? errors : warnings)++;
			}
			result.ErrorCount = static_cast<uint16_t>(std::min<uint32_t>(errors, std::numeric_limits<uint16_t>::max()));
			result.WarningCount = static_cast<uint16_t>(std::min<uint32_t>(warnings, std::numeric_limits<uint16_t>::max()));

			// The pins: styled, labelled and given their values here, placed by LayOut.
			uint32_t const firstPin = static_cast<uint32_t>(m_Model.m_Pins.size());
			result.FirstPin = firstPin;
			result.PinCount = static_cast<uint16_t>(std::min<size_t>(drafts.size(), std::numeric_limits<uint16_t>::max()));
			int execInputs = 0;
			int execOutputs = 0;
			for (size_t index = 0; index < result.PinCount; index++)
			{
				if (drafts[index].Type == VisualScriptExecType && !drafts[index].Orphan)
				{
					(drafts[index].Direction == VisualScriptPinDirection::In ? execInputs : execOutputs)++;
				}
			}
			std::string_view groupRoot;
			VisualScriptPinDirection groupDirection = VisualScriptPinDirection::In;
			bool inGroup = false;
			for (size_t index = 0; index < result.PinCount; index++)
			{
				PinDraft const& draft = drafts[index];
				bool const input = draft.Direction == VisualScriptPinDirection::In;
				VisualScriptPinVisual& pin = m_Model.m_Pins.emplace_back();
				StylePin(pin, draft);
				if (draft.DefaultsToSelf)
				{
					pin.Flags |= VisualScriptPinFlags::DefaultsToSelf;
				}
				if (IsLinked(source.Entry, visual, draft.Name, draft.Direction))
				{
					pin.Flags |= VisualScriptPinFlags::Linked;
				}
				// A split field: a path whose parent is split in its direction (an exact pin name is never read as a field).
				size_t const dot = draft.Name.rfind('.');
				bool const field = !draft.Orphan && dot != std::string_view::npos &&
				                   IsSplitParent(node, source.Entry, draft.Name.substr(0, dot), draft.Direction);
				if (field)
				{
					pin.Flags |= VisualScriptPinFlags::SplitField;
					std::string_view const root = draft.Name.substr(0, draft.Name.find('.'));
					if (!inGroup || root != groupRoot || draft.Direction != groupDirection)
					{
						pin.Flags |= VisualScriptPinFlags::SplitGroupStart;
						pin.BracketColor = GetBracketColor(source, root, draft.Direction, index);
						groupRoot = root;
						groupDirection = draft.Direction;
					}
				}
				inGroup = field;

				bool const hidden =
					draft.Type == VisualScriptExecType && !draft.Orphan &&
					((input && execInputs == 1 && draft.Name == "Execute") || (!input && execOutputs == 1 && draft.Name == "Then"));
				if (hidden)
				{
					pin.Flags |= VisualScriptPinFlags::LabelHidden;
				}
				pin.Name = AddText(draft.Name);
				if (!hidden && result.Shape == VisualScriptNodeShape::Standard)
				{
					std::string_view label = draft.Label.empty() ? draft.Name : draft.Label;
					if (draft.Label.empty() && field)
					{
						m_Scratch.LabelBuffer.assign(draft.Name);
						std::replace(m_Scratch.LabelBuffer.begin(), m_Scratch.LabelBuffer.end(), '.', ' ');
						label = m_Scratch.LabelBuffer;
					}
					pin.Label = AddText(label, Metrics::PinLabelTextSize);
				}

				// The value of an unlinked data input.
				if (input && !pin.HasFlag(VisualScriptPinFlags::Linked) && !draft.Orphan && draft.Type != VisualScriptExecType)
				{
					AddChip(pin, node, source, draft);
				}
			}

			// Problems located on pins.
			for (VisualScriptDiagnostic const* diagnostic : GetNodeDiagnostics(node.ID))
			{
				if (diagnostic->Pin.empty())
				{
					continue;
				}
				for (uint32_t index = 0; index < result.PinCount; index++)
				{
					VisualScriptPinVisual& pin = m_Model.m_Pins[firstPin + index];
					bool const direction = !diagnostic->Direction || *diagnostic->Direction == pin.Direction;
					std::string_view const name = m_Model.GetText(pin.Name);
					bool const named = name == diagnostic->Pin ||
					                   MapVisualScriptPinName(source.Entry, diagnostic->Pin, pin.Direction, m_Scratch.NameBuffer) == name;
					if (direction && named)
					{
						pin.Flags |= diagnostic->Severity == VisualScriptSeverity::Error ? VisualScriptPinFlags::HasError
						                                                                 : VisualScriptPinFlags::HasWarning;
						break;
					}
				}
			}

			// Where the flow leaves a disabled node, as the analysis says.
			if (source.Entry != nullptr && node.Enabled == VisualScriptNodeEnabled::Disabled && !source.Entry->PassThrough.empty())
			{
				uint32_t const last = std::min<uint32_t>(result.PinCount, std::numeric_limits<uint8_t>::max() + 1u);
				for (uint32_t index = 0; index < last; index++)
				{
					VisualScriptPinVisual const& pin = m_Model.m_Pins[firstPin + index];
					if (pin.Direction == VisualScriptPinDirection::Out && m_Model.GetText(pin.Name) == source.Entry->PassThrough)
					{
						result.Flags |= VisualScriptNodeFlags::PassThroughExit;
						result.PassThroughPin = static_cast<uint8_t>(index);
						break;
					}
				}
			}

			switch (result.Shape)
			{
				case VisualScriptNodeShape::Standard:
					result.Title = AddText(source.Title, Metrics::TitleTextSize);
					result.Subtitle = AddText(source.Subtitle, Metrics::SubtitleTextSize);
					break;
				case VisualScriptNodeShape::Pill:
				{
					std::string_view const name = !source.VariableName.empty() ? source.VariableName : std::string_view(source.Title);
					float const room = Metrics::MaxNodeWidth - Metrics::HorizontalPadding - Metrics::PinGlyphInset -
					                   Metrics::PinGlyphRadius - Metrics::PinLabelGap;
					result.Title = AddText(name, Metrics::TitleTextSize, room);
					break;
				}
				case VisualScriptNodeShape::Operator:
					result.Title = AddText(GetVisualScriptOperatorSymbol(node.Type), Metrics::OperatorSymbolTextSize);
					break;
				case VisualScriptNodeShape::Reroute:
					break;
			}
			if (!node.Comment.empty())
			{
				result.Comment =
					AddWrappedText(node.Comment, Metrics::NodeCommentTextSize, Metrics::NodeCommentWrapWidth, Metrics::NodeCommentMaxLines);
			}

			uint64_t const anchorHash = LayOut(result);
			std::span<VisualScriptPinVisual const> const pins(m_Model.m_Pins.data() + result.FirstPin, result.PinCount);
			bool const showsAssetNames = std::any_of(pins.begin(), pins.end(),
			                                         [](VisualScriptPinVisual const& pin)
			                                         {
														 return pin.ChipKind == VisualScriptChipKind::Asset;
													 });
			m_Model.m_Nodes.push_back(result);
			m_Model.m_NodeRecords.push_back(
				{hash, textOffset, static_cast<uint32_t>(m_Model.m_Text.size()) - textOffset, anchorHash, showsAssetNames});
		}

		// The chip of an unlinked data input: "Self" for inputs that mean the running entity, else the stored literal, the pin's
		// default or the type's, formatted. Its width goes in ChipRect for LayOut to place.
		void AddChip(VisualScriptPinVisual& pin, VisualScriptNode const& node, NodeSource const& source, PinDraft const& draft)
		{
			Json const* stored = FindStoredLiteral(node, source.Entry, draft.Name);
			VisualScriptTypeInfo const* type = FindType(draft.Type);
			Json const& shown = stored != nullptr ? *stored : (draft.Default != nullptr ? *draft.Default : m_Null);
			VisualScriptChipValue value;
			if (draft.DefaultsToSelf)
			{
				// A Self input holds no value: whatever is stored is not read.
				value = stored == nullptr ? VisualScriptChipValue{VisualScriptChipKind::Placeholder, std::string(SelfText), 0, false}
				                          : MakeInvalid(*stored);
			}
			else
			{
				value = FormatVisualScriptLiteral(shown, draft.Type, type, m_Source.EnumLookup, m_Source.AssetNames);
			}
			if (value.Kind == VisualScriptChipKind::None)
			{
				return;
			}
			pin.Flags |= stored == nullptr ? VisualScriptPinFlags::ChipIsDefault : 0;
			pin.Flags |= value.Invalid ? VisualScriptPinFlags::ChipInvalid : 0;
			size_t const cells = static_cast<size_t>(std::count(value.Text.begin(), value.Text.end(), '\n')) + 1;
			ChipLayout layout = GetChipLayout(value.Kind, type, cells);
			pin.ChipKind = value.Kind;
			pin.ChipColor = value.Color;
			pin.ChipText = AddChipText(value, layout, shown, draft.Type, type);
			if (value.Kind == VisualScriptChipKind::Placeholder)
			{
				layout.Width += pin.ChipText.Width;
			}
			pin.ChipRect = {glm::vec2(0.0f), glm::vec2(layout.Width, Metrics::ChipHeight)};
		}

		// Sizes the node and places its pins, chips, bubble and bounds. Returns the hash of its pins' places.
		uint64_t LayOut(VisualScriptNodeVisual& node)
		{
			std::span<VisualScriptPinVisual> const pins(m_Model.m_Pins.data() + node.FirstPin, node.PinCount);
			bool const marked = node.HasFlag(VisualScriptNodeFlags::Disabled) || node.HasFlag(VisualScriptNodeFlags::DevelopmentOnly);
			float const strip = marked ? Metrics::EnabledStripHeight : 0.0f;
			switch (node.Shape)
			{
				case VisualScriptNodeShape::Standard:
					LayOutStandard(node, pins, strip);
					break;
				case VisualScriptNodeShape::Pill:
				{
					float const width = std::clamp(Metrics::HorizontalPadding + node.Title.Width + Metrics::PinLabelGap +
					                                   Metrics::PinGlyphRadius + Metrics::PinGlyphInset,
					                               Metrics::PillMinWidth, Metrics::MaxNodeWidth);
					node.Size = glm::vec2(width, Metrics::PillHeight + strip);
					for (VisualScriptPinVisual& pin : pins)
					{
						pin.Anchor = glm::vec2(width - Metrics::PinGlyphInset, Metrics::PillHeight * 0.5f);
						pin.HitRect = {glm::vec2(width - 2.0f * Metrics::PinGlyphInset, 0.0f), glm::vec2(width, Metrics::PillHeight)};
					}
					break;
				}
				case VisualScriptNodeShape::Operator:
				{
					node.Size = glm::vec2(Metrics::OperatorWidth, Metrics::OperatorHeight + strip);
					float const half = Metrics::OperatorHeight * 0.5f;
					uint32_t input = 0;
					for (VisualScriptPinVisual& pin : pins)
					{
						if (pin.Direction == VisualScriptPinDirection::In)
						{
							float const top = half * static_cast<float>(input++);
							pin.Anchor = glm::vec2(Metrics::PinGlyphInset, top + half * 0.5f);
							pin.HitRect = {glm::vec2(0.0f, top), glm::vec2(2.0f * Metrics::PinGlyphInset, top + half)};
						}
						else
						{
							pin.Anchor = glm::vec2(Metrics::OperatorWidth - Metrics::PinGlyphInset, half);
							pin.HitRect = {glm::vec2(Metrics::OperatorWidth - 2.0f * Metrics::PinGlyphInset, 0.0f),
							               glm::vec2(Metrics::OperatorWidth, Metrics::OperatorHeight)};
						}
						// A compact operator draws no chips: its pins' tooltips show their values.
						pin.ChipRect = {};
					}
					break;
				}
				case VisualScriptNodeShape::Reroute:
				{
					node.Size = glm::vec2(2.0f * Metrics::RerouteRadius);
					for (VisualScriptPinVisual& pin : pins)
					{
						// The dot is the reroute: pressing it moves the reroute, so its pins have no rows of their own.
						pin.Anchor = glm::vec2(Metrics::RerouteRadius);
						pin.HitRect = {};
						pin.ChipRect = {};
					}
					break;
				}
			}
			if (node.Shape != VisualScriptNodeShape::Standard)
			{
				node.ExpandedSize = node.Size;
			}

			// The comment's bubble above the node, left-aligned with it, its tail below it.
			if (node.Comment.Length > 0)
			{
				std::string_view const comment = m_Model.GetText(node.Comment);
				float const lines = static_cast<float>(std::count(comment.begin(), comment.end(), '\n') + 1);
				glm::vec2 const size(node.Comment.Width + 2.0f * Metrics::NodeCommentPadding,
				                     lines * Metrics::NodeCommentTextSize + 2.0f * Metrics::NodeCommentPadding);
				node.BubbleRect = {glm::vec2(0.0f, -(size.y + Metrics::NodeCommentTailHeight)),
				                   glm::vec2(size.x, -Metrics::NodeCommentTailHeight)};
			}

			// Everything drawn: the body with its shadow (a reroute's dot with its rings), the badges at their size at 100 % and
			// the bubble.
			VisualScriptRect const rect = VisualScriptGeometry::GetNodeRect(node);
			VisualScriptRect bounds;
			if (node.Shape == VisualScriptNodeShape::Reroute)
			{
				bounds = VisualScriptGeometry::Expand(rect, Metrics::RerouteRadius);
			}
			else
			{
				bounds = VisualScriptGeometry::Expand(rect, Metrics::ShadowReach);
				bounds.Max.y += Metrics::ShadowOffsetY;
			}
			bounds = VisualScriptGeometry::Unite(bounds, VisualScriptGeometry::GetBadgeRect(node, 1.0f, 1.0f));
			bounds = VisualScriptGeometry::Unite(bounds, VisualScriptGeometry::GetBubbleRect(node));
			node.VisualBounds = bounds;

			uint64_t hash = HashVector(Hash::Combine(0, node.PinCount), node.ExpandedSize);
			for (VisualScriptPinVisual const& pin : pins)
			{
				hash = HashRect(HashVector(hash, pin.Anchor), pin.HitRect);
			}
			return hash;
		}

		void LayOutStandard(VisualScriptNodeVisual& node, std::span<VisualScriptPinVisual> pins, float strip)
		{
			float const glyphEnd = Metrics::PinGlyphInset + Metrics::PinGlyphRadius;
			// A pin's column: its glyph, its label and an input's chip.
			auto const getColumn = [glyphEnd](VisualScriptPinVisual const& pin)
			{
				float width = glyphEnd;
				if (pin.Label.Length > 0)
				{
					width += Metrics::PinLabelGap + pin.Label.Width;
				}
				if (!VisualScriptGeometry::IsEmpty(pin.ChipRect))
				{
					width += Metrics::PinLabelGap + pin.ChipRect.Max.x;
				}
				return width;
			};
			float inputsColumn = 0.0f;
			float outputsColumn = 0.0f;
			size_t inputs = 0;
			size_t outputs = 0;
			for (VisualScriptPinVisual const& pin : pins)
			{
				bool const input = pin.Direction == VisualScriptPinDirection::In;
				(input ? inputs : outputs)++;
				float& column = input ? inputsColumn : outputsColumn;
				column = std::max(column, getColumn(pin));
			}

			// Labels too long for the widest node are ellipsized; the side that needs less keeps what it needs.
			float const pinsRoom = Metrics::MaxNodeWidth - Metrics::MinPinColumnGap;
			if (inputsColumn + outputsColumn > pinsRoom)
			{
				float const half = pinsRoom * 0.5f;
				float inputsRoom = half;
				float outputsRoom = half;
				if (inputsColumn < half)
				{
					outputsRoom = pinsRoom - inputsColumn;
				}
				else if (outputsColumn < half)
				{
					inputsRoom = pinsRoom - outputsColumn;
				}
				inputsColumn = 0.0f;
				outputsColumn = 0.0f;
				for (VisualScriptPinVisual& pin : pins)
				{
					bool const input = pin.Direction == VisualScriptPinDirection::In;
					float const room = input ? inputsRoom : outputsRoom;
					if (getColumn(pin) > room && pin.Label.Length > 0)
					{
						float const chip = VisualScriptGeometry::IsEmpty(pin.ChipRect) ? 0.0f : Metrics::PinLabelGap + pin.ChipRect.Max.x;
						float const labelRoom = std::max(room - glyphEnd - Metrics::PinLabelGap - chip, 0.0f);
						Fit(m_Model.GetText(pin.Label), Metrics::PinLabelTextSize, labelRoom, pin.Label);
					}
					float& column = input ? inputsColumn : outputsColumn;
					column = std::max(column, getColumn(pin));
				}
			}

			// The header: padding, the kind's glyph, the title, the collapsed marker, the latent clock and the badges.
			float const badges = Metrics::BadgeSlot * static_cast<float>((node.ErrorCount > 0 ? 1 : 0) + (node.WarningCount > 0 ? 1 : 0));
			bool const kindGlyph = node.Kind == VisualScriptNodeKind::Event || node.HasFlag(VisualScriptNodeFlags::FunctionNode);
			float const fixedHeader = 2.0f * Metrics::HorizontalPadding + (kindGlyph ? Metrics::KindGlyphSlot : 0.0f) +
			                          (node.HasFlag(VisualScriptNodeFlags::Collapsed) ? Metrics::KindGlyphSlot : 0.0f) +
			                          (node.HasFlag(VisualScriptNodeFlags::Latent) ? Metrics::KindGlyphSlot : 0.0f) + badges;
			if (node.Title.Length > 0 && fixedHeader + node.Title.Width > Metrics::MaxNodeWidth)
			{
				Fit(m_Model.GetText(node.Title), Metrics::TitleTextSize, std::max(Metrics::MaxNodeWidth - fixedHeader, 0.0f), node.Title);
			}
			float const headerWidth = fixedHeader + node.Title.Width;
			float pinsWidth = 0.0f;
			if (inputs + outputs > 0)
			{
				pinsWidth = (inputs > 0 ? inputsColumn : 0.0f) + Metrics::MinPinColumnGap + (outputs > 0 ? outputsColumn : 0.0f);
			}
			float const width = std::clamp(std::max(headerWidth, pinsWidth), Metrics::MinNodeWidth, Metrics::MaxNodeWidth);
			float const header = node.Subtitle.Length > 0 ? Metrics::HeaderHeightWithSubtitle : Metrics::HeaderHeight;

			// Rows: inputs down the left side, outputs down the right one.
			std::array<size_t, 2> rows = {0, 0};
			for (VisualScriptPinVisual& pin : pins)
			{
				bool const input = pin.Direction == VisualScriptPinDirection::In;
				float const top = header + Metrics::PinRowsTop + Metrics::PinRowHeight * static_cast<float>(rows[input ? 0 : 1]++);
				float const middle = top + Metrics::PinRowHeight * 0.5f;
				float const labelEnd = glyphEnd + (pin.Label.Length > 0 ? Metrics::PinLabelGap + pin.Label.Width : 0.0f);
				if (input)
				{
					pin.Anchor = glm::vec2(Metrics::PinGlyphInset, middle);
					pin.HitRect = {glm::vec2(0.0f, top), glm::vec2(labelEnd, top + Metrics::PinRowHeight)};
					if (!VisualScriptGeometry::IsEmpty(pin.ChipRect))
					{
						glm::vec2 const min(labelEnd + Metrics::PinLabelGap, middle - Metrics::ChipHeight * 0.5f);
						pin.ChipRect = {min, min + glm::vec2(pin.ChipRect.Max.x, Metrics::ChipHeight)};
					}
				}
				else
				{
					pin.Anchor = glm::vec2(width - Metrics::PinGlyphInset, middle);
					pin.HitRect = {glm::vec2(width - labelEnd, top), glm::vec2(width, top + Metrics::PinRowHeight)};
					pin.ChipRect = {};
				}
			}
			size_t const rowCount = std::max(rows[0], rows[1]);
			float const body =
				rowCount > 0 ? header + Metrics::PinRowsTop + Metrics::PinRowHeight * static_cast<float>(rowCount) + Metrics::PinRowsBottom
							 : header;
			node.ExpandedSize = glm::vec2(width, body + strip);
			if (node.HasFlag(VisualScriptNodeFlags::Collapsed))
			{
				node.Size = glm::vec2(std::clamp(headerWidth, Metrics::MinNodeWidth, Metrics::MaxNodeWidth), header + strip);
			}
			else
			{
				node.Size = node.ExpandedSize;
			}
		}

		VisualScriptAnalyzedNode const* FindEntry(VisualScriptNodeVisual const& node) const
		{
			if (!node.HasFlag(VisualScriptNodeFlags::Analyzed) || m_Source.Analysis == nullptr)
			{
				return nullptr;
			}
			std::optional<uint32_t> const entry = FindSorted(m_Scratch.AnalysisNodes, node.ID);
			return entry ? &m_Source.Analysis->Nodes[*entry] : nullptr;
		}

		std::optional<uint32_t> FindLinkPin(uint32_t node, std::string_view stored, VisualScriptPinDirection direction, std::string& buffer)
		{
			VisualScriptAnalyzedNode const* entry = FindEntry(m_Model.m_Nodes[node]);
			std::string_view const name = MapVisualScriptPinName(entry, stored, direction, buffer);
			std::optional<uint32_t> pin = m_Model.FindPin(node, name, direction);
			return pin ? pin : m_Model.FindPin(node, stored, direction);
		}

		void BuildLinks()
		{
			std::vector<VisualScriptLinkVisual>& links = m_Model.m_Links;
			links.clear();
			Scratch& scratch = m_Scratch;
			size_t const nodeCount = m_Model.m_Nodes.size();
			m_Model.m_IncidentStarts.assign(nodeCount + 1, 0);
			m_Model.m_FirstOutgoing.assign(nodeCount, NoIndex);
			size_t const linkCount = m_Graph != nullptr ? m_Graph->Links.size() : 0;
			for (uint32_t index = 0; index < linkCount; index++)
			{
				uint32_t const from = scratch.LinkFrom[index];
				uint32_t const to = scratch.LinkTo[index];
				if (from == NoIndex)
				{
					continue;
				}
				VisualScriptLink const& link = m_Graph->Links[index];
				std::optional<uint32_t> const fromPin = FindLinkPin(from, link.From.Pin, VisualScriptPinDirection::Out, scratch.NameBuffer);
				std::optional<uint32_t> const toPin = FindLinkPin(to, link.To.Pin, VisualScriptPinDirection::In, scratch.OtherNameBuffer);
				if (!fromPin || !toPin)
				{
					// Only pins beyond the most a node holds (65535) are missing.
					continue;
				}
				VisualScriptNodeVisual const& fromNode = m_Model.m_Nodes[from];
				VisualScriptNodeVisual const& toNode = m_Model.m_Nodes[to];
				VisualScriptPinVisual const& output = m_Model.m_Pins[*fromPin];
				VisualScriptPinVisual const& input = m_Model.m_Pins[*toPin];
				VisualScriptLinkVisual visual;
				visual.FromNode = from;
				visual.ToNode = to;
				visual.FromPin = *fromPin;
				visual.ToPin = *toPin;
				visual.Source = index;
				visual.FromName = m_Model.GetText(output.Name) == link.From.Pin ? output.Name : AddText(link.From.Pin);
				visual.ToName = m_Model.GetText(input.Name) == link.To.Pin ? input.Name : AddText(link.To.Pin);
				bool const exec = output.Type == VisualScriptExecType || input.Type == VisualScriptExecType;
				bool const orphan = output.HasFlag(VisualScriptPinFlags::Orphan) || input.HasFlag(VisualScriptPinFlags::Orphan);
				bool const disabled = fromNode.HasFlag(VisualScriptNodeFlags::Disabled) || toNode.HasFlag(VisualScriptNodeFlags::Disabled);
				bool const converted = !exec && !orphan && fromNode.HasFlag(VisualScriptNodeFlags::Analyzed) &&
				                       toNode.HasFlag(VisualScriptNodeFlags::Analyzed) && output.Type != input.Type &&
				                       output.Type != VisualScriptWildcardType && input.Type != VisualScriptWildcardType;
				uint8_t flags = 0;
				flags |= exec ? VisualScriptLinkFlags::Exec : 0;
				flags |= converted ? VisualScriptLinkFlags::Converted : 0;
				flags |= scratch.LinkErrors[index] != 0 ? VisualScriptLinkFlags::Error : 0;
				flags |= orphan ? VisualScriptLinkFlags::Orphan : 0;
				flags |= disabled ? VisualScriptLinkFlags::Disabled : 0;
				visual.Flags = flags;
				visual.FromColor = exec ? m_Theme.Text : output.Color;
				visual.ToColor = converted ? input.Color : visual.FromColor;
				uint32_t const linkIndex = static_cast<uint32_t>(links.size());
				links.push_back(visual);
				if (m_Model.m_FirstOutgoing[from] == NoIndex)
				{
					m_Model.m_FirstOutgoing[from] = linkIndex;
				}
				m_Model.m_IncidentStarts[from + 1]++;
				if (to != from)
				{
					m_Model.m_IncidentStarts[to + 1]++;
				}
			}
			for (size_t node = 0; node < nodeCount; node++)
			{
				m_Model.m_IncidentStarts[node + 1] += m_Model.m_IncidentStarts[node];
			}
			m_Model.m_Incident.assign(m_Model.m_IncidentStarts.back(), 0);
			scratch.Fill.assign(m_Model.m_IncidentStarts.begin(), m_Model.m_IncidentStarts.end() - 1);
			for (uint32_t index = 0; index < links.size(); index++)
			{
				VisualScriptLinkVisual const& link = links[index];
				m_Model.m_Incident[scratch.Fill[link.FromNode]++] = index;
				if (link.ToNode != link.FromNode)
				{
					m_Model.m_Incident[scratch.Fill[link.ToNode]++] = index;
				}
			}
		}

		void BuildComments()
		{
			std::vector<IdIndex>& indices = m_Model.m_CommentIndices;
			indices.clear();
			if (m_Graph == nullptr)
			{
				return;
			}
			m_Model.m_Comments.reserve(m_Graph->Comments.size());
			m_Model.m_CommentRecords.reserve(m_Graph->Comments.size());
			for (VisualScriptComment const& comment : m_Graph->Comments)
			{
				uint64_t hash =
					HashText(HashVector(HashVector(Hash::Combine(0, comment.ID), comment.Position), comment.Size), comment.Text);
				hash = Hash::Combine(Hash::Combine(hash, HashBits(comment.Color.r)), HashBits(comment.Color.g));
				hash = Hash::Combine(Hash::Combine(hash, HashBits(comment.Color.b)), HashBits(comment.Color.a));
				std::optional<uint32_t> const previous = FindSorted(m_Model.m_SpareCommentIndices, comment.ID);
				if (previous && m_Model.m_SpareCommentRecords[*previous].Hash == hash)
				{
					VisualScriptCommentVisual visual = m_Model.m_SpareComments[*previous];
					LayoutRecord const& record = m_Model.m_SpareCommentRecords[*previous];
					uint32_t const textOffset = static_cast<uint32_t>(m_Model.m_Text.size());
					m_Model.m_Text.append(m_Model.m_SpareText, record.TextOffset, record.TextLength);
					int64_t const shift = static_cast<int64_t>(textOffset) - static_cast<int64_t>(record.TextOffset);
					Rebase(visual.Title, shift);
					Rebase(visual.Body, shift);
					m_Model.m_Comments.push_back(visual);
					m_Model.m_CommentRecords.push_back({hash, textOffset, record.TextLength, 0, false});
					continue;
				}
				VisualScriptCommentVisual visual;
				visual.ID = comment.ID;
				visual.Rect = {comment.Position, comment.Position + glm::max(comment.Size, glm::vec2(0.0f))};
				glm::vec2 const size = visual.Rect.GetSize();
				uint32_t const textOffset = static_cast<uint32_t>(m_Model.m_Text.size());
				visual.Title = AddText(GetFirstLine(comment.Text), Metrics::CommentTitleTextSize,
				                       std::max(size.x - 2.0f * Metrics::HorizontalPadding, 0.0f));
				size_t const bodyStart = std::min(comment.Text.find('\n'), comment.Text.size());
				std::string_view const body =
					bodyStart < comment.Text.size() ? std::string_view(comment.Text).substr(bodyStart + 1) : std::string_view();
				// The body's lines fill the box under its title bar at 100 %.
				float const bodyHeight = size.y - Metrics::CommentTitleBarHeight - Metrics::CommentBodyInset;
				int const lines = static_cast<int>(std::floor(std::max(bodyHeight, 0.0f) / Metrics::CommentBodyTextSize));
				if (!body.empty() && lines > 0)
				{
					visual.Body = AddWrappedText(body, Metrics::CommentBodyTextSize,
					                             std::max(size.x - 2.0f * Metrics::CommentBodyInset, 1.0f), lines);
				}
				VisualScriptCommentColors const colors = ComputeVisualScriptCommentColors(comment.Color, m_Theme);
				visual.BodyColor = colors.Body;
				visual.BarColor = colors.Bar;
				visual.BarTextColor = colors.BarText;
				visual.BorderColor = colors.Border;
				m_Model.m_Comments.push_back(visual);
				m_Model.m_CommentRecords.push_back({hash, textOffset, static_cast<uint32_t>(m_Model.m_Text.size()) - textOffset, 0, false});
			}

			// Largest first, so inner comments draw above outer ones; equal areas keep document order.
			Scratch& scratch = m_Scratch;
			std::vector<VisualScriptCommentVisual>& comments = m_Model.m_Comments;
			scratch.CommentOrder.resize(comments.size());
			for (uint32_t index = 0; index < comments.size(); index++)
			{
				scratch.CommentOrder[index] = index;
			}
			std::stable_sort(scratch.CommentOrder.begin(), scratch.CommentOrder.end(),
			                 [&comments](uint32_t a, uint32_t b)
			                 {
								 glm::vec2 const sizeA = comments[a].Rect.GetSize();
								 glm::vec2 const sizeB = comments[b].Rect.GetSize();
								 return sizeA.x * sizeA.y > sizeB.x * sizeB.y;
							 });
			if (!std::is_sorted(scratch.CommentOrder.begin(), scratch.CommentOrder.end()))
			{
				scratch.SortedComments.clear();
				scratch.SortedCommentRecords.clear();
				for (uint32_t const index : scratch.CommentOrder)
				{
					scratch.SortedComments.push_back(comments[index]);
					scratch.SortedCommentRecords.push_back(m_Model.m_CommentRecords[index]);
				}
				std::copy(scratch.SortedComments.begin(), scratch.SortedComments.end(), comments.begin());
				std::copy(scratch.SortedCommentRecords.begin(), scratch.SortedCommentRecords.end(), m_Model.m_CommentRecords.begin());
			}
			for (uint32_t index = 0; index < comments.size(); index++)
			{
				indices.emplace_back(comments[index].ID, index);
			}
			SortByID(indices);
		}

		VisualScriptCanvasModel& m_Model;
		VisualScriptCanvasSource const& m_Source;
		VisualScriptTextMeasurer const& m_Measure;
		VisualScriptThemeAnchors const& m_Theme;
		Scratch& m_Scratch;
		VisualScriptGraph const* m_Graph = nullptr;
		VisualScriptGraph const* m_AnalyzedGraph = nullptr;
		Json const m_Null;
	};

	VisualScriptCanvasModel::VisualScriptCanvasModel()
		: m_Scratch(CreateScope<Scratch>())
	{
	}

	VisualScriptCanvasModel::~VisualScriptCanvasModel() = default;
	VisualScriptCanvasModel::VisualScriptCanvasModel(VisualScriptCanvasModel&&) noexcept = default;
	VisualScriptCanvasModel& VisualScriptCanvasModel::operator=(VisualScriptCanvasModel&&) noexcept = default;

	bool VisualScriptCanvasModel::Sync(VisualScriptCanvasSource const& source, VisualScriptTextMeasurer const& measure,
	                                   VisualScriptThemeAnchors const& theme)
	{
		bool const everything =
			!m_HasSynced || source.Library != m_Library || !(theme == m_Theme) || measure.Signature != m_MeasureSignature;
		// The last results become the spare buffers the new ones copy kept layouts from. Vectors swap; the text is copied,
		// because a string's reserve may grow it more than asked (MSVC grows it geometrically), so two strings swapped and
		// reserved to each other's capacity would keep growing.
		std::swap(m_Nodes, m_SpareNodes);
		std::swap(m_NodeRecords, m_SpareNodeRecords);
		std::swap(m_Pins, m_SparePins);
		std::swap(m_Comments, m_SpareComments);
		std::swap(m_CommentRecords, m_SpareCommentRecords);
		std::swap(m_NodeIndices, m_SpareNodeIndices);
		std::swap(m_CommentIndices, m_SpareCommentIndices);
		m_SpareText.assign(m_Text);
		m_Nodes.clear();
		m_NodeRecords.clear();
		m_Pins.clear();
		m_Comments.clear();
		m_CommentRecords.clear();
		m_Text.clear();
		if (everything)
		{
			m_SpareNodeIndices.clear();
			m_SpareCommentIndices.clear();
		}
		m_MeasuredNodeCount = 0;
		m_Function = source.Function;

		Builder(*this, source, measure, theme).Run();

		m_Library = source.Library;
		m_Theme = theme;
		m_MeasureSignature = measure.Signature;
		m_AssetVersion = source.AssetVersion;
		m_ShowsAssetNames = std::any_of(m_NodeRecords.begin(), m_NodeRecords.end(),
		                                [](NodeRecord const& record)
		                                {
											return record.ShowsAssetNames;
										});
		// The spare buffers are done with. The vectors keep the results' capacity (vectors reserve exactly what they are
		// asked), so the next Sync builds into them without growing.
		m_SpareNodes.clear();
		m_SpareNodeRecords.clear();
		m_SparePins.clear();
		m_SpareComments.clear();
		m_SpareCommentRecords.clear();
		m_SpareText.clear();
		m_SpareNodeIndices.clear();
		m_SpareCommentIndices.clear();
		m_SpareNodes.reserve(m_Nodes.capacity());
		m_SpareNodeRecords.reserve(m_NodeRecords.capacity());
		m_SparePins.reserve(m_Pins.capacity());
		m_SpareComments.reserve(m_Comments.capacity());
		m_SpareCommentRecords.reserve(m_CommentRecords.capacity());
		m_SpareNodeIndices.reserve(m_NodeIndices.capacity());
		m_SpareCommentIndices.reserve(m_CommentIndices.capacity());
		// The text is copied into the spare one at the next Sync: room for it now, so that copy does not grow it.
		if (m_SpareText.capacity() < m_Text.size())
		{
			m_SpareText.reserve(m_Text.size());
		}

		uint64_t geometry = Hash::Combine(Hash::Combine(0, m_Nodes.size()), m_Comments.size());
		for (size_t index = 0; index < m_Nodes.size(); index++)
		{
			VisualScriptNodeVisual const& node = m_Nodes[index];
			geometry = HashVector(HashVector(Hash::Combine(geometry, node.ID), node.Position), node.Size);
			geometry = Hash::Combine(HashRect(geometry, node.VisualBounds), m_NodeRecords[index].AnchorHash);
		}
		for (VisualScriptLinkVisual const& link : m_Links)
		{
			geometry = Hash::Combine(Hash::Combine(geometry, link.FromNode), link.ToNode);
			geometry = Hash::Combine(Hash::Combine(geometry, link.FromPin), link.ToPin);
		}
		for (VisualScriptCommentVisual const& comment : m_Comments)
		{
			geometry = HashRect(Hash::Combine(geometry, comment.ID), comment.Rect);
		}
		bool const changed = !m_HasSynced || geometry != m_GeometryHash;
		m_GeometryHash = geometry;
		m_HasSynced = true;
		return changed;
	}

	std::span<VisualScriptPinVisual const> VisualScriptCanvasModel::GetPins(VisualScriptNodeVisual const& node) const
	{
		if (static_cast<size_t>(node.FirstPin) + node.PinCount > m_Pins.size())
		{
			return {};
		}
		return {m_Pins.data() + node.FirstPin, node.PinCount};
	}

	std::span<uint32_t const> VisualScriptCanvasModel::GetIncidentLinks(uint32_t nodeIndex) const
	{
		if (static_cast<size_t>(nodeIndex) + 1 >= m_IncidentStarts.size())
		{
			return {};
		}
		return {m_Incident.data() + m_IncidentStarts[nodeIndex], m_IncidentStarts[nodeIndex + 1] - m_IncidentStarts[nodeIndex]};
	}

	std::optional<uint32_t> VisualScriptCanvasModel::GetFirstOutgoingLink(uint32_t nodeIndex) const
	{
		if (nodeIndex >= m_FirstOutgoing.size() || m_FirstOutgoing[nodeIndex] == NoIndex)
		{
			return std::nullopt;
		}
		return m_FirstOutgoing[nodeIndex];
	}

	std::optional<uint32_t> VisualScriptCanvasModel::FindNode(uint32_t id) const
	{
		return FindSorted(m_NodeIndices, id);
	}

	std::optional<uint32_t> VisualScriptCanvasModel::FindComment(uint32_t id) const
	{
		return FindSorted(m_CommentIndices, id);
	}

	std::optional<uint32_t> VisualScriptCanvasModel::FindPin(uint32_t nodeIndex, std::string_view name,
	                                                         VisualScriptPinDirection direction) const
	{
		if (nodeIndex >= m_Nodes.size())
		{
			return std::nullopt;
		}
		VisualScriptNodeVisual const& node = m_Nodes[nodeIndex];
		for (uint32_t index = node.FirstPin; index < node.FirstPin + node.PinCount && index < m_Pins.size(); index++)
		{
			if (m_Pins[index].Direction == direction && GetText(m_Pins[index].Name) == name)
			{
				return index;
			}
		}
		return std::nullopt;
	}

	std::optional<uint32_t> VisualScriptCanvasModel::FindExecLink(uint32_t nodeIndex, std::string_view outputPin) const
	{
		for (uint32_t const link : GetIncidentLinks(nodeIndex))
		{
			VisualScriptLinkVisual const& visual = m_Links[link];
			if (visual.FromNode == nodeIndex && GetText(m_Pins[visual.FromPin].Name) == outputPin)
			{
				return link;
			}
		}
		return std::nullopt;
	}

	std::string_view VisualScriptCanvasModel::GetText(VisualScriptTextRef ref) const
	{
		if (ref.Length == 0 || static_cast<size_t>(ref.Offset) + ref.Length > m_Text.size())
		{
			return {};
		}
		return std::string_view(m_Text).substr(ref.Offset, ref.Length);
	}

	std::string_view VisualScriptCanvasModel::GetVisibleText(VisualScriptTextRef ref) const
	{
		return GetText(ref).substr(0, ref.VisibleLength);
	}

	VisualScriptLink VisualScriptCanvasModel::GetDocumentLink(uint32_t linkIndex) const
	{
		VisualScriptLinkVisual const& link = m_Links[linkIndex];
		return {{m_Nodes[link.FromNode].ID, std::string(GetText(link.FromName))},
		        {m_Nodes[link.ToNode].ID, std::string(GetText(link.ToName))}};
	}

	glm::vec2 VisualScriptCanvasModel::GetExtents(uint32_t id) const
	{
		std::optional<uint32_t> const node = FindNode(id);
		return node ? m_Nodes[*node].Size : glm::vec2(0.0f);
	}

	std::optional<VisualScriptRect> VisualScriptCanvasModel::GetBounds(std::span<uint32_t const> elements) const
	{
		std::optional<VisualScriptRect> bounds;
		auto const add = [&bounds](VisualScriptRect const& rect)
		{
			bounds = bounds ? VisualScriptRect{glm::min(bounds->Min, rect.Min), glm::max(bounds->Max, rect.Max)} : rect;
		};
		for (uint32_t const id : elements)
		{
			if (std::optional<uint32_t> const node = FindNode(id))
			{
				add(VisualScriptGeometry::GetNodeRect(m_Nodes[*node]));
			}
			else if (std::optional<uint32_t> const comment = FindComment(id))
			{
				add(m_Comments[*comment].Rect);
			}
		}
		return bounds;
	}

	std::optional<VisualScriptRect> VisualScriptCanvasModel::GetBounds() const
	{
		std::optional<VisualScriptRect> bounds;
		auto const add = [&bounds](VisualScriptRect const& rect)
		{
			bounds = bounds ? VisualScriptRect{glm::min(bounds->Min, rect.Min), glm::max(bounds->Max, rect.Max)} : rect;
		};
		for (VisualScriptNodeVisual const& node : m_Nodes)
		{
			add(VisualScriptGeometry::GetNodeRect(node));
		}
		for (VisualScriptCommentVisual const& comment : m_Comments)
		{
			add(comment.Rect);
		}
		return bounds;
	}

	size_t VisualScriptCanvasModel::GetCapacityBytes() const
	{
		size_t bytes = GetCapacity(m_Nodes) + GetCapacity(m_NodeRecords) + GetCapacity(m_Pins) + GetCapacity(m_Links) +
		               GetCapacity(m_Comments) + GetCapacity(m_CommentRecords) + GetCapacity(m_Text) + GetCapacity(m_NodeIndices) +
		               GetCapacity(m_CommentIndices) + GetCapacity(m_IncidentStarts) + GetCapacity(m_Incident) +
		               GetCapacity(m_FirstOutgoing);
		bytes += GetCapacity(m_SpareNodes) + GetCapacity(m_SpareNodeRecords) + GetCapacity(m_SparePins) + GetCapacity(m_SpareComments) +
		         GetCapacity(m_SpareCommentRecords) + GetCapacity(m_SpareText) + GetCapacity(m_SpareNodeIndices) +
		         GetCapacity(m_SpareCommentIndices);
		return bytes + (m_Scratch != nullptr ? m_Scratch->GetCapacityBytes() : 0);
	}
}
