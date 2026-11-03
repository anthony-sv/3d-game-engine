#include "stpch.h"
#include "Strada/Script/VisualScriptDebugger.h"

#include "Strada/Core/Utf8.h"
#include "Strada/Script/ScriptEngineInternal.h"

#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace Strada
{
	namespace
	{
		using ScriptEngineInternal::GetData;
		using ScriptEngineInternal::ReceiveText;
		using ScriptEngineInternal::ScriptEngineData;
		using ScriptEngineInternal::ScriptStatus;
		using ScriptEngineInternal::ToLength;

		// Reads the members of a reply, keeping the first error, which names the member's JSON path: reads after an error
		// answer defaults, so a reader reads on and checks once at the end.
		class ReplyReader
		{
		public:
			bool IsOk() const { return m_Error.empty(); }
			std::string const& GetError() const { return m_Error; }

			void Fail(std::string_view path, std::string_view problem)
			{
				if (m_Error.empty())
				{
					m_Error = fmt::format("{}: {}", path.empty() ? std::string_view("the reply") : path, problem);
				}
			}

			static std::string Member(std::string_view path, std::string_view name)
			{
				return path.empty() ? std::string(name) : fmt::format("{}.{}", path, name);
			}

			static std::string Element(std::string_view path, size_t index) { return fmt::format("{}[{}]", path, index); }

			// Whether the value is an object, failing at the path when it is not.
			bool ExpectObject(Json const& value, std::string_view path)
			{
				if (!value.is_object())
				{
					Fail(path, "not an object");
					return false;
				}
				return true;
			}

			// The member that must be there; null (failing) when it is missing or the value is no object.
			Json const* Require(Json const& object, std::string_view name, std::string_view path)
			{
				if (!object.is_object())
				{
					Fail(path, "not an object");
					return nullptr;
				}
				auto const member = object.find(name);
				if (member == object.end())
				{
					Fail(Member(path, name), "missing");
					return nullptr;
				}
				return &*member;
			}

			// The member when it is there and not null.
			static Json const* Find(Json const& object, std::string_view name)
			{
				if (!object.is_object())
				{
					return nullptr;
				}
				auto const member = object.find(name);
				return member != object.end() && !member->is_null() ? &*member : nullptr;
			}

			bool Bool(Json const& object, std::string_view name, std::string_view path)
			{
				Json const* const value = Require(object, name, path);
				if (value == nullptr)
				{
					return false;
				}
				if (!value->is_boolean())
				{
					Fail(Member(path, name), "not a boolean");
					return false;
				}
				return value->get<bool>();
			}

			// An optional boolean: false when absent.
			bool OptionalBool(Json const& object, std::string_view name, std::string_view path)
			{
				return object.is_object() && object.contains(name) ? Bool(object, name, path) : false;
			}

			uint64_t UInt(Json const& value, std::string_view path, uint64_t maximum = std::numeric_limits<uint64_t>::max())
			{
				if (value.is_number_unsigned())
				{
					uint64_t const number = value.get<uint64_t>();
					if (number <= maximum)
					{
						return number;
					}
				}
				else if (value.is_number_integer() && value.get<int64_t>() >= 0 && static_cast<uint64_t>(value.get<int64_t>()) <= maximum)
				{
					return static_cast<uint64_t>(value.get<int64_t>());
				}
				Fail(path, maximum == std::numeric_limits<uint32_t>::max() ? "not an unsigned 32-bit integer" : "not an unsigned integer");
				return 0;
			}

			uint64_t UInt(Json const& object, std::string_view name, std::string_view path,
			              uint64_t maximum = std::numeric_limits<uint64_t>::max())
			{
				Json const* const value = Require(object, name, path);
				return value != nullptr ? UInt(*value, Member(path, name), maximum) : 0;
			}

			uint32_t UInt32(Json const& object, std::string_view name, std::string_view path)
			{
				return static_cast<uint32_t>(UInt(object, name, path, std::numeric_limits<uint32_t>::max()));
			}

			// An unsigned 32-bit member that may be absent (0 then).
			uint32_t OptionalUInt32(Json const& object, std::string_view name, std::string_view path)
			{
				return object.is_object() && object.contains(name) ? UInt32(object, name, path) : 0;
			}

			// An unsigned integer or null.
			std::optional<uint64_t> NullableUInt(Json const& object, std::string_view name, std::string_view path,
			                                     uint64_t maximum = std::numeric_limits<uint64_t>::max())
			{
				Json const* const value = Require(object, name, path);
				if (value == nullptr || value->is_null())
				{
					return std::nullopt;
				}
				return UInt(*value, Member(path, name), maximum);
			}

			int64_t Int(Json const& value, std::string_view path)
			{
				if (value.is_number_integer() && !value.is_number_unsigned())
				{
					return value.get<int64_t>();
				}
				if (value.is_number_unsigned() && value.get<uint64_t>() <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
				{
					return static_cast<int64_t>(value.get<uint64_t>());
				}
				Fail(path, "not a 64-bit integer");
				return 0;
			}

			int64_t Int(Json const& object, std::string_view name, std::string_view path)
			{
				Json const* const value = Require(object, name, path);
				return value != nullptr ? Int(*value, Member(path, name)) : 0;
			}

			double Number(Json const& value, std::string_view path)
			{
				if (!value.is_number())
				{
					Fail(path, "not a number");
					return 0.0;
				}
				double const number = value.get<double>();
				if (!std::isfinite(number))
				{
					Fail(path, "not a finite number");
					return 0.0;
				}
				return number;
			}

			double Number(Json const& object, std::string_view name, std::string_view path)
			{
				Json const* const value = Require(object, name, path);
				return value != nullptr ? Number(*value, Member(path, name)) : 0.0;
			}

			std::string String(Json const& value, std::string_view path)
			{
				if (!value.is_string())
				{
					Fail(path, "not a string");
					return {};
				}
				return value.get<std::string>();
			}

			std::string String(Json const& object, std::string_view name, std::string_view path)
			{
				Json const* const value = Require(object, name, path);
				return value != nullptr ? String(*value, Member(path, name)) : std::string();
			}

			// A string or null.
			std::optional<std::string> NullableString(Json const& object, std::string_view name, std::string_view path)
			{
				Json const* const value = Require(object, name, path);
				if (value == nullptr || value->is_null())
				{
					return std::nullopt;
				}
				return String(*value, Member(path, name));
			}

			// A handle or an entity: a decimal string of an unsigned 64-bit integer.
			uint64_t Id(Json const& value, std::string_view path)
			{
				if (value.is_string())
				{
					if (std::optional<UUID> const id = UUID::FromString(value.get_ref<std::string const&>()))
					{
						return id->GetValue();
					}
				}
				Fail(path, "not a decimal string of an unsigned 64-bit integer");
				return 0;
			}

			uint64_t Id(Json const& object, std::string_view name, std::string_view path)
			{
				Json const* const value = Require(object, name, path);
				return value != nullptr ? Id(*value, Member(path, name)) : 0;
			}

			AssetHandle Handle(Json const& object, std::string_view name, std::string_view path)
			{
				return AssetHandle(UUID(Id(object, name, path)));
			}

			UUID Entity(Json const& object, std::string_view name, std::string_view path) { return UUID(Id(object, name, path)); }

			// The array member; null (failing) when it is missing or no array.
			Json const* Array(Json const& object, std::string_view name, std::string_view path)
			{
				Json const* const value = Require(object, name, path);
				if (value != nullptr && !value->is_array())
				{
					Fail(Member(path, name), "not an array");
					return nullptr;
				}
				return value;
			}

			// An enumerator by its name among the names, in the order of the enum's values.
			template<typename TEnum, size_t Count>
			TEnum Enum(Json const& object, std::string_view name, std::string_view path, std::array<std::string_view, Count> const& names)
			{
				std::string const text = String(object, name, path);
				for (size_t index = 0; index < Count; index++)
				{
					if (names[index] == text)
					{
						return static_cast<TEnum>(index);
					}
				}
				if (IsOk())
				{
					Fail(Member(path, name), fmt::format("unknown value \"{}\"", text));
				}
				return static_cast<TEnum>(0);
			}

		private:
			std::string m_Error;
		};

		constexpr std::array<std::string_view, 6> StopReasonNames = {"Breakpoint", "Step", "Break", "RunTo", "Error", "Passed"};
		constexpr std::array<std::string_view, 3> StopPlaceNames = {"Enter", "Continue", "Error"};
		constexpr std::array<std::string_view, 4> FrameKindNames = {"Graph", "Call", "Delivery", "Native"};
		constexpr std::array<std::string_view, 6> StartKindNames = {"Event", "Tick", "Timeline", "Resume", "SendEvent", "Delivery"};
		constexpr std::array<std::string_view, 4> ActionNames = {"Continue", "StepInto", "StepOver", "StepOut"};
		// The runtime's outcomes only: Armed and Resumed are the editor's.
		constexpr std::array<std::string_view, 4> OutcomeNames = {"Stopped", "Ended", "Waiting", "Dismissed"};
		constexpr std::array<std::string_view, 4> CurrencyNames = {"Current", "Earlier", "NotComputed", "Unknown"};
		// The runtime's reasons only ("" for none): TargetGone is the editor's.
		constexpr std::array<std::string_view, 3> InactivityNames = {"", "NotInProgram", "NotRunning"};
		constexpr std::array<std::string_view, 3> SourceNames = {"Frame", "LastCall", "None"};
		constexpr std::array<std::string_view, 3> InstanceStateNames = {"Idle", "Waiting", "Stopped"};
		constexpr std::array<std::string_view, 4> LoopKindNames = {"ForLoop", "ForEach", "While", "Sequence"};
		constexpr std::array<std::string_view, 2> DirectionNames = {"Out", "In"};

		// The members of a value (spec-slice4 section 5.4): Type unless bare, exactly one of Value, Entity and Text (Text with
		// Entity for components and script instances), and their extras.
		void ReadValueMembers(ReplyReader& reader, Json const& json, std::string_view path, bool bare, VisualScriptDebugValue& value)
		{
			if (!reader.ExpectObject(json, path))
			{
				return;
			}
			if (!bare)
			{
				value.Type = reader.String(json, "Type", path);
			}
			bool const hasValue = json.contains("Value");
			bool const hasEntity = json.contains("Entity");
			bool const hasText = json.contains("Text");
			if (hasValue ? hasEntity || hasText : !hasEntity && !hasText)
			{
				reader.Fail(path, "not one of Value, Entity and Text");
				return;
			}
			if (hasValue)
			{
				value.Value = json.at("Value");
			}
			if (hasEntity)
			{
				value.Entity = reader.Entity(json, "Entity", path);
				if (json.contains("Valid"))
				{
					value.EntityValid = reader.Bool(json, "Valid", path);
				}
			}
			if (hasText)
			{
				value.Text = reader.String(json, "Text", path);
			}
			if (json.contains("Length"))
			{
				value.Length = reader.UInt32(json, "Length", path);
			}
			value.Truncated = reader.OptionalBool(json, "Truncated", path);
		}

		// A value of a reply's section: its Name, its encoding, and what Describe and Values add to it.
		VisualScriptDebugValue ReadNamedValue(ReplyReader& reader, Json const& json, std::string_view path)
		{
			VisualScriptDebugValue value;
			if (!reader.ExpectObject(json, path))
			{
				return value;
			}
			value.Name = reader.String(json, "Name", path);
			ReadValueMembers(reader, json, path, false, value);
			if (json.contains("Currency"))
			{
				value.Currency = reader.Enum<VisualScriptValueCurrency>(json, "Currency", path, CurrencyNames);
			}
			if (json.contains("ID"))
			{
				value.ID = reader.UInt32(json, "ID", path);
			}
			value.Public = reader.OptionalBool(json, "Public", path);
			value.Changed = reader.OptionalBool(json, "Changed", path);
			value.Edited = reader.OptionalBool(json, "Edited", path);
			if (value.Edited)
			{
				// Before is the snapshot's value without Type: checked as one, kept as it came.
				if (Json const* const before = reader.Require(json, "Before", path))
				{
					VisualScriptDebugValue checkedBefore;
					ReadValueMembers(reader, *before, ReplyReader::Member(path, "Before"), true, checkedBefore);
					value.Before = *before;
				}
			}
			return value;
		}

		// A value of a summary (First, Last, Min, Max): without Type, which is the summary's.
		VisualScriptDebugValue ReadBareValue(ReplyReader& reader, Json const& json, std::string_view path, std::string const& type)
		{
			VisualScriptDebugValue value;
			ReadValueMembers(reader, json, path, true, value);
			value.Type = type;
			return value;
		}

		std::vector<VisualScriptDebugValue> ReadNamedValues(ReplyReader& reader, Json const& object, std::string_view name,
		                                                    std::string_view path)
		{
			std::vector<VisualScriptDebugValue> values;
			if (Json const* const array = reader.Array(object, name, path))
			{
				std::string const member = ReplyReader::Member(path, name);
				values.reserve(array->size());
				for (size_t index = 0; index < array->size() && reader.IsOk(); index++)
				{
					values.push_back(ReadNamedValue(reader, (*array)[index], ReplyReader::Element(member, index)));
				}
			}
			return values;
		}

		VisualScriptNodeValues ReadNodeValues(ReplyReader& reader, Json const& json, std::string_view path, std::string_view idName,
		                                      bool withInputs)
		{
			VisualScriptNodeValues node;
			if (!reader.ExpectObject(json, path))
			{
				return node;
			}
			node.Node = reader.UInt32(json, idName, path);
			if (withInputs)
			{
				node.Inputs = ReadNamedValues(reader, json, "Inputs", path);
			}
			node.Outputs = ReadNamedValues(reader, json, "Outputs", path);
			return node;
		}

		std::vector<VisualScriptNodeValues> ReadNodeValuesArray(ReplyReader& reader, Json const& object, std::string_view name,
		                                                        std::string_view path, std::string_view idName)
		{
			std::vector<VisualScriptNodeValues> nodes;
			if (Json const* const array = reader.Array(object, name, path))
			{
				std::string const member = ReplyReader::Member(path, name);
				for (size_t index = 0; index < array->size() && reader.IsOk(); index++)
				{
					nodes.push_back(ReadNodeValues(reader, (*array)[index], ReplyReader::Element(member, index), idName, false));
				}
			}
			return nodes;
		}

		VisualScriptStopFrame ReadFrame(ReplyReader& reader, Json const& json, std::string_view path)
		{
			VisualScriptStopFrame frame;
			if (!reader.ExpectObject(json, path))
			{
				return frame;
			}
			frame.Kind = reader.Enum<VisualScriptStopFrameKind>(json, "Kind", path, FrameKindNames);
			frame.Title = reader.String(json, "Title", path);
			if (frame.Kind == VisualScriptStopFrameKind::Native)
			{
				return frame;
			}
			frame.Entity = reader.Entity(json, "Entity", path);
			frame.Graph = reader.Handle(json, "Graph", path);
			frame.Document = reader.Handle(json, "Document", path);
			frame.Function = reader.UInt32(json, "Function", path);
			frame.FunctionName = reader.String(json, "FunctionName", path);
			frame.Node = reader.UInt32(json, "Node", path);
			frame.Pin = reader.String(json, "Pin", path);
			frame.Note = reader.String(json, "Note", path);
			return frame;
		}

		VisualScriptStopStart ReadStart(ReplyReader& reader, Json const& json, std::string_view path)
		{
			VisualScriptStopStart start;
			if (!reader.ExpectObject(json, path))
			{
				return start;
			}
			start.Kind = reader.Enum<VisualScriptStartKind>(json, "Kind", path, StartKindNames);
			start.Graph = reader.Handle(json, "Graph", path);
			start.Document = reader.Handle(json, "Document", path);
			start.Node = reader.UInt32(json, "Node", path);
			start.Title = reader.String(json, "Title", path);
			start.Pin = reader.String(json, "Pin", path);
			if (Json const* const origin = reader.Require(json, "Origin", path); origin != nullptr && !origin->is_null())
			{
				std::string const at = ReplyReader::Member(path, "Origin");
				VisualScriptStopOrigin read;
				read.Document = reader.Handle(*origin, "Document", at);
				read.Node = reader.UInt32(*origin, "Node", at);
				read.Title = reader.String(*origin, "Title", at);
				start.Origin = std::move(read);
			}
			return start;
		}

		VisualScriptStop ReadStop(ReplyReader& reader, Json const& json, std::string_view path)
		{
			VisualScriptStop stop;
			if (!reader.ExpectObject(json, path))
			{
				return stop;
			}
			stop.ID = reader.UInt(json, "ID", path);
			stop.Reason = reader.Enum<VisualScriptStopReason>(json, "Reason", path, StopReasonNames);
			stop.Resumable = reader.Bool(json, "Resumable", path);
			stop.Entity = reader.Entity(json, "Entity", path);
			stop.Graph = reader.Handle(json, "Graph", path);
			stop.Place = reader.Enum<VisualScriptStopPlace>(json, "Place", path, StopPlaceNames);
			stop.Message = reader.String(json, "Message", path);
			stop.Migrated = reader.Bool(json, "Migrated", path);
			if (Json const* const changed = reader.Array(json, "Changed", path))
			{
				std::string const member = ReplyReader::Member(path, "Changed");
				for (size_t index = 0; index < changed->size() && reader.IsOk(); index++)
				{
					std::string const at = ReplyReader::Element(member, index);
					stop.Changed.push_back({reader.UInt32((*changed)[index], "Node", at), reader.String((*changed)[index], "Name", at)});
				}
			}
			stop.ChangedMore = reader.UInt32(json, "ChangedMore", path);
			if (Json const* const breakpoint = reader.Require(json, "Breakpoint", path); breakpoint != nullptr && !breakpoint->is_null())
			{
				std::string const at = ReplyReader::Member(path, "Breakpoint");
				stop.Breakpoint = std::make_pair(reader.Handle(*breakpoint, "Document", at), reader.UInt32(*breakpoint, "Node", at));
			}
			if (Json const* const frames = reader.Array(json, "Frames", path))
			{
				std::string const member = ReplyReader::Member(path, "Frames");
				for (size_t index = 0; index < frames->size() && reader.IsOk(); index++)
				{
					stop.Frames.push_back(ReadFrame(reader, (*frames)[index], ReplyReader::Element(member, index)));
				}
			}
			if (Json const* const start = reader.Require(json, "Start", path))
			{
				stop.Start = ReadStart(reader, *start, ReplyReader::Member(path, "Start"));
			}
			return stop;
		}

		// The reply as JSON: refused when it is not, when it is no object, and when it is an error reply.
		Result<Json> ParseReply(std::string_view reply)
		{
			Result<Json> parsed = ParseJson(reply);
			if (!parsed)
			{
				return MakeError("the reply is not JSON: {}", parsed.GetError());
			}
			Json const& root = parsed.GetValue();
			if (!root.is_object())
			{
				return Error{"the reply: not an object"};
			}
			if (auto const code = root.find("Code"); code != root.end())
			{
				auto const error = root.find("Error");
				if (!code->is_string() || error == root.end() || !error->is_string())
				{
					return Error{"the reply: an error reply without a string Code and Error"};
				}
				return MakeError("{}: {}", code->get_ref<std::string const&>(), error->get_ref<std::string const&>());
			}
			return parsed;
		}

		template<typename T>
		Result<T> Finish(ReplyReader const& reader, T value)
		{
			if (!reader.IsOk())
			{
				return Error{reader.GetError()};
			}
			return value;
		}

		// A little-endian unsigned integer of the block at the offset (which the caller checked).
		template<typename T>
		T ReadLittleEndian(std::span<std::byte const> block, size_t offset)
		{
			T value = 0;
			for (size_t index = 0; index < sizeof(T); index++)
			{
				value |= static_cast<T>(std::to_integer<uint8_t>(block[offset + index])) << (8 * index);
			}
			return value;
		}

		// Sends a request to VisualScriptDebug: its reply, or why there is none.
		Result<std::string> Send(Json const& request)
		{
			ScriptEngineData* const data = GetData();
			if (data == nullptr)
			{
				return Error{"the scripting runtime is not initialized"};
			}
			// Texts users wrote (conditions, formulas) may hold invalid UTF-8: it is replaced, never thrown.
			std::string const text = request.dump(-1, ' ', false, Json::error_handler_t::replace);
			std::string reply;
			auto const status =
				static_cast<ScriptStatus>(data->Host.VisualScriptDebug(text.data(), ToLength(text.size()), &reply, &ReceiveText));
			if (status != ScriptStatus::Success)
			{
				return Error{"the visual-script debugger failed (see the log)"};
			}
			return reply;
		}

		Json Command(std::string_view name)
		{
			return Json::object({{"Command", name}});
		}

		// A request whose reply is an empty object.
		Result<void> SendCommand(Json const& request)
		{
			Result<std::string> const reply = Send(request);
			if (!reply)
			{
				return Error{reply.GetError()};
			}
			Result<Json> const parsed = ParseReply(reply.GetValue());
			if (!parsed)
			{
				return Error{parsed.GetError()};
			}
			return {};
		}

		template<typename T>
		Result<T> SendAndRead(Json const& request, Result<T> (*read)(std::string_view reply))
		{
			Result<std::string> const reply = Send(request);
			if (!reply)
			{
				return Error{reply.GetError()};
			}
			return read(reply.GetValue());
		}
	}

	namespace ScriptEngineInternal
	{
		Result<VisualScriptFrameTrace> ReadVisualScriptFrameTrace(std::span<std::byte const> block)
		{
			constexpr size_t HeaderSize = 32;
			constexpr size_t GraphSize = 16;
			constexpr size_t StepSize = 16;
			constexpr uint32_t Version = 1;
			constexpr uint32_t NoExit = 0xFFFFFFFF;
			constexpr std::array<char, 4> Magic = {'V', 'S', 'D', 'T'};

			if (block.size() < HeaderSize)
			{
				return MakeError("the trace block has {} bytes, fewer than its header's {}", block.size(), HeaderSize);
			}
			for (size_t index = 0; index < Magic.size(); index++)
			{
				if (std::to_integer<char>(block[index]) != Magic[index])
				{
					return Error{"the trace block does not start with VSDT"};
				}
			}
			if (uint32_t const version = ReadLittleEndian<uint32_t>(block, 4); version != Version)
			{
				return MakeError("the trace block has version {}, not {}", version, Version);
			}
			if (ReadLittleEndian<uint32_t>(block, 28) != 0)
			{
				return Error{"the trace block's reserved header bytes are not 0"};
			}
			VisualScriptFrameTrace trace;
			trace.StopRevision = ReadLittleEndian<uint64_t>(block, 8);
			uint32_t const graphCount = ReadLittleEndian<uint32_t>(block, 16);
			uint32_t const stringCount = ReadLittleEndian<uint32_t>(block, 20);
			trace.Dropped = ReadLittleEndian<uint32_t>(block, 24);

			// Every count is checked against the bytes left before anything is reserved for it.
			size_t offset = HeaderSize;
			if (graphCount > (block.size() - offset) / GraphSize)
			{
				return MakeError("the trace block's {} graphs do not fit in its {} bytes", graphCount, block.size());
			}
			std::vector<std::vector<uint32_t>> exits(graphCount);
			trace.Graphs.resize(graphCount);
			for (uint32_t graph = 0; graph < graphCount; graph++)
			{
				if (block.size() - offset < GraphSize)
				{
					return MakeError("the trace block ends inside graph {}", graph);
				}
				VisualScriptGraphTrace& read = trace.Graphs[graph];
				read.Document = AssetHandle(UUID(ReadLittleEndian<uint64_t>(block, offset)));
				read.Function = ReadLittleEndian<uint32_t>(block, offset + 8);
				uint32_t const stepCount = ReadLittleEndian<uint32_t>(block, offset + 12);
				offset += GraphSize;
				if (stepCount > (block.size() - offset) / StepSize)
				{
					return MakeError("the trace block's graph {} has {} steps, more than its bytes hold", graph, stepCount);
				}
				read.Steps.resize(stepCount);
				exits[graph].resize(stepCount);
				for (uint32_t step = 0; step < stepCount; step++)
				{
					VisualScriptTraceStep& traced = read.Steps[step];
					traced.Node = ReadLittleEndian<uint32_t>(block, offset);
					exits[graph][step] = ReadLittleEndian<uint32_t>(block, offset + 4);
					traced.Count = ReadLittleEndian<uint32_t>(block, offset + 8);
					traced.FirstOrder = ReadLittleEndian<uint32_t>(block, offset + 12);
					offset += StepSize;
				}
			}
			// Each string takes at least its length's four bytes.
			if (stringCount > (block.size() - offset) / 4)
			{
				return MakeError("the trace block's {} exit names do not fit in its {} bytes", stringCount, block.size());
			}
			std::vector<std::string> strings(stringCount);
			for (uint32_t index = 0; index < stringCount; index++)
			{
				if (block.size() - offset < 4)
				{
					return MakeError("the trace block ends inside exit name {}", index);
				}
				uint32_t const length = ReadLittleEndian<uint32_t>(block, offset);
				offset += 4;
				if (length > block.size() - offset)
				{
					return MakeError("the trace block's exit name {} has {} bytes, more than are left", index, length);
				}
				strings[index].assign(reinterpret_cast<char const*>(block.data() + offset), length);
				offset += length;
				if (!Utf8::IsValid(strings[index]))
				{
					return MakeError("the trace block's exit name {} is not UTF-8", index);
				}
			}
			if (offset != block.size())
			{
				return MakeError("the trace block has {} bytes after its exit names", block.size() - offset);
			}
			for (uint32_t graph = 0; graph < graphCount; graph++)
			{
				for (size_t step = 0; step < exits[graph].size(); step++)
				{
					uint32_t const exit = exits[graph][step];
					if (exit == NoExit)
					{
						continue;
					}
					if (exit >= stringCount)
					{
						return MakeError("the trace block's graph {} step {} names exit name {} of {}", graph, step, exit, stringCount);
					}
					trace.Graphs[graph].Steps[step].Exit = strings[exit];
				}
			}
			return trace;
		}

		Result<VisualScriptDebugState> ReadVisualScriptDebugState(std::string_view reply)
		{
			Result<Json> const parsed = ParseReply(reply);
			if (!parsed)
			{
				return Error{parsed.GetError()};
			}
			Json const& root = parsed.GetValue();
			ReplyReader reader;
			VisualScriptDebugState state;
			state.Revision = reader.UInt32(root, "Revision", "");
			state.StopRevision = reader.UInt(root, "StopRevision", "");
			state.Current = reader.NullableUInt(root, "Current", "");
			state.Passed = reader.UInt32(root, "Passed", "");
			if (Json const* const stepping = reader.Require(root, "Stepping", ""); stepping != nullptr && !stepping->is_null())
			{
				VisualScriptStepping step;
				step.Stop = reader.UInt(*stepping, "Stop", "Stepping");
				step.Action = reader.Enum<VisualScriptDebugAction>(*stepping, "Action", "Stepping", ActionNames);
				if (Json const* const waits = reader.Require(*stepping, "WaitsIn", "Stepping"); waits != nullptr && !waits->is_null())
				{
					constexpr std::string_view At = "Stepping.WaitsIn";
					VisualScriptStepWait wait;
					wait.Document = reader.Handle(*waits, "Document", At);
					wait.Function = reader.UInt32(*waits, "Function", At);
					wait.Node = reader.UInt32(*waits, "Node", At);
					wait.Title = reader.String(*waits, "Title", At);
					wait.Entity = reader.Entity(*waits, "Entity", At);
					step.WaitsIn = std::move(wait);
				}
				state.Stepping = std::move(step);
			}
			if (Json const* const stops = reader.Array(root, "Stops", ""))
			{
				for (size_t index = 0; index < stops->size() && reader.IsOk(); index++)
				{
					state.Stops.push_back(ReadStop(reader, (*stops)[index], ReplyReader::Element("Stops", index)));
				}
			}
			if (Json const* const breakpoints = reader.Array(root, "Breakpoints", ""))
			{
				for (size_t index = 0; index < breakpoints->size() && reader.IsOk(); index++)
				{
					Json const& json = (*breakpoints)[index];
					std::string const at = ReplyReader::Element("Breakpoints", index);
					VisualScriptBreakpointState breakpoint;
					breakpoint.Document = reader.Handle(json, "Document", at);
					breakpoint.Node = reader.UInt32(json, "Node", at);
					breakpoint.Hits = reader.UInt32(json, "Hits", at);
					breakpoint.Reached = reader.UInt32(json, "Reached", at);
					breakpoint.Active = reader.Bool(json, "Active", at);
					breakpoint.Reason = reader.Enum<VisualScriptBreakpointInactivity>(json, "Reason", at, InactivityNames);
					breakpoint.Error = reader.NullableString(json, "Error", at).value_or(std::string());
					if (breakpoint.Active != (breakpoint.Reason == VisualScriptBreakpointInactivity::None))
					{
						reader.Fail(ReplyReader::Member(at, "Reason"), "does not agree with Active");
					}
					state.Breakpoints.push_back(std::move(breakpoint));
				}
			}
			return Finish(reader, std::move(state));
		}

		Result<VisualScriptResumeOutcome> ReadVisualScriptResumeOutcome(std::string_view reply)
		{
			Result<Json> const parsed = ParseReply(reply);
			if (!parsed)
			{
				return Error{parsed.GetError()};
			}
			Json const& root = parsed.GetValue();
			ReplyReader reader;
			VisualScriptResumeOutcome outcome;
			outcome.Result = reader.Enum<VisualScriptResumeResult>(root, "Outcome", "", OutcomeNames);
			outcome.Failed = reader.Bool(root, "Failed", "");
			outcome.Stop = reader.NullableUInt(root, "Stop", "");
			outcome.StopRevision = reader.UInt(root, "StopRevision", "");
			if (reader.IsOk() && (outcome.Result == VisualScriptResumeResult::Stopped) != outcome.Stop.has_value())
			{
				reader.Fail("Stop", outcome.Stop ? "a stop for an outcome that is not Stopped" : "missing for the outcome Stopped");
			}
			return Finish(reader, outcome);
		}

		Result<VisualScriptStopValues> ReadVisualScriptStopValues(std::string_view reply)
		{
			Result<Json> const parsed = ParseReply(reply);
			if (!parsed)
			{
				return Error{parsed.GetError()};
			}
			Json const& root = parsed.GetValue();
			ReplyReader reader;
			VisualScriptStopValues values;
			values.Stop = reader.UInt(root, "Stop", "");
			values.Frame = reader.UInt32(root, "Frame", "");
			values.Entity = reader.Entity(root, "Entity", "");
			values.Graph = reader.Handle(root, "Graph", "");
			values.Document = reader.Handle(root, "Document", "");
			values.Function = reader.UInt32(root, "Function", "");
			if (Json const* const node = reader.Require(root, "Node", ""))
			{
				values.Node = ReadNodeValues(reader, *node, "Node", "ID", true);
			}
			values.Event = ReadNamedValues(reader, root, "Event", "");
			values.Parameters = ReadNamedValues(reader, root, "Parameters", "");
			values.Locals = ReadNamedValues(reader, root, "Locals", "");
			values.Variables = ReadNamedValues(reader, root, "Variables", "");
			if (Json const* const loops = reader.Array(root, "Loops", ""))
			{
				for (size_t index = 0; index < loops->size() && reader.IsOk(); index++)
				{
					Json const& json = (*loops)[index];
					std::string const at = ReplyReader::Element("Loops", index);
					VisualScriptLoopState loop;
					loop.Node = reader.UInt32(json, "Node", at);
					size_t const kind = static_cast<size_t>(reader.Enum<uint8_t>(json, "Kind", at, LoopKindNames));
					loop.Kind = std::string(LoopKindNames[kind]);
					if (loop.Kind == "ForLoop")
					{
						loop.Index = reader.Int(json, "Index", at);
						loop.First = reader.Int(json, "First", at);
						loop.Last = reader.Int(json, "Last", at);
					}
					else if (loop.Kind == "ForEach")
					{
						loop.Index = reader.Int(json, "Index", at);
						loop.Length = reader.UInt32(json, "Length", at);
						if (json.contains("Element"))
						{
							loop.Element = ReadNamedValue(reader, json.at("Element"), ReplyReader::Member(at, "Element"));
						}
					}
					else if (loop.Kind == "Sequence")
					{
						loop.Next = reader.String(json, "Next", at);
					}
					values.Loops.push_back(std::move(loop));
				}
			}
			values.Computed = ReadNodeValuesArray(reader, root, "Computed", "", "Node");
			if (Json const* const evaluated = reader.Array(root, "Evaluated", ""))
			{
				for (size_t index = 0; index < evaluated->size() && reader.IsOk(); index++)
				{
					Json const& json = (*evaluated)[index];
					std::string const at = ReplyReader::Element("Evaluated", index);
					VisualScriptEvaluated formula;
					formula.Formula = reader.String(json, "Formula", at);
					bool const hasValue = json.is_object() && json.contains("Value");
					bool const hasError = json.is_object() && json.contains("Error");
					if (hasValue == hasError)
					{
						reader.Fail(at, "not one of Value and Error");
					}
					else if (hasValue)
					{
						VisualScriptDebugValue value;
						ReadValueMembers(reader, json.at("Value"), ReplyReader::Member(at, "Value"), false, value);
						formula.Value = std::move(value);
					}
					else
					{
						formula.Error = reader.String(json, "Error", at);
					}
					values.Evaluated.push_back(std::move(formula));
				}
			}
			values.Truncated = reader.UInt32(root, "Truncated", "");
			return Finish(reader, std::move(values));
		}

		Result<VisualScriptLiveValues> ReadVisualScriptLiveValues(std::string_view reply)
		{
			Result<Json> const parsed = ParseReply(reply);
			if (!parsed)
			{
				return Error{parsed.GetError()};
			}
			Json const& root = parsed.GetValue();
			ReplyReader reader;
			VisualScriptLiveValues values;
			values.Entity = reader.Entity(root, "Entity", "");
			values.Graph = reader.Handle(root, "Graph", "");
			values.Source = reader.Enum<VisualScriptValueSource>(root, "Source", "", SourceNames);
			values.Variables = ReadNamedValues(reader, root, "Variables", "");
			if (Json const* const timelines = reader.Array(root, "Timelines", ""))
			{
				for (size_t index = 0; index < timelines->size() && reader.IsOk(); index++)
				{
					Json const& json = (*timelines)[index];
					std::string const at = ReplyReader::Element("Timelines", index);
					VisualScriptTimelineState timeline;
					timeline.Node = reader.UInt32(json, "Node", at);
					timeline.Time = reader.Number(json, "Time", at);
					timeline.Length = reader.Number(json, "Length", at);
					timeline.Playing = reader.Bool(json, "Playing", at);
					timeline.Reverse = reader.Bool(json, "Reverse", at);
					values.Timelines.push_back(timeline);
				}
			}
			values.Nodes = ReadNodeValuesArray(reader, root, "Nodes", "", "ID");
			values.Truncated = reader.UInt32(root, "Truncated", "");
			return Finish(reader, std::move(values));
		}

		Result<VisualScriptInstanceList> ReadVisualScriptInstanceList(std::string_view reply)
		{
			Result<Json> const parsed = ParseReply(reply);
			if (!parsed)
			{
				return Error{parsed.GetError()};
			}
			Json const& root = parsed.GetValue();
			ReplyReader reader;
			VisualScriptInstanceList list;
			list.Total = reader.UInt32(root, "Total", "");
			if (Json const* const instances = reader.Array(root, "Instances", ""))
			{
				for (size_t index = 0; index < instances->size() && reader.IsOk(); index++)
				{
					Json const& json = (*instances)[index];
					std::string const at = ReplyReader::Element("Instances", index);
					VisualScriptInstanceInfo instance;
					instance.Entity = reader.Entity(json, "Entity", at);
					instance.Graph = reader.Handle(json, "Graph", at);
					instance.State = reader.Enum<VisualScriptInstanceState>(json, "State", at, InstanceStateNames);
					if (Json const* const waits = reader.Array(json, "Waits", at))
					{
						std::string const member = ReplyReader::Member(at, "Waits");
						for (size_t position = 0; position < waits->size() && reader.IsOk(); position++)
						{
							Json const& entry = (*waits)[position];
							std::string const where = ReplyReader::Element(member, position);
							VisualScriptInstanceWait wait;
							wait.Document = reader.Handle(entry, "Document", where);
							wait.Function = reader.UInt32(entry, "Function", where);
							wait.Node = reader.UInt32(entry, "Node", where);
							wait.Title = reader.String(entry, "Title", where);
							wait.Kind = reader.String(entry, "Kind", where);
							if (Json const* const remaining = reader.Require(entry, "Remaining", where);
							    remaining != nullptr && !remaining->is_null())
							{
								wait.Remaining = reader.Number(*remaining, ReplyReader::Member(where, "Remaining"));
							}
							if (std::optional<uint64_t> const frames =
							        reader.NullableUInt(entry, "Frames", where, std::numeric_limits<uint32_t>::max()))
							{
								wait.Frames = static_cast<uint32_t>(*frames);
							}
							if (Json const* const next = reader.Require(entry, "Index", where); next != nullptr && !next->is_null())
							{
								wait.Index = reader.Int(*next, ReplyReader::Member(where, "Index"));
							}
							instance.Waits.push_back(std::move(wait));
						}
					}
					if (Json const* const stops = reader.Array(json, "Stops", at))
					{
						std::string const member = ReplyReader::Member(at, "Stops");
						for (size_t position = 0; position < stops->size() && reader.IsOk(); position++)
						{
							instance.Stops.push_back(reader.UInt((*stops)[position], ReplyReader::Element(member, position)));
						}
					}
					list.Instances.push_back(std::move(instance));
				}
			}
			if (reader.IsOk() && list.Instances.size() > list.Total)
			{
				reader.Fail("Total", "fewer than the instances listed");
			}
			return Finish(reader, std::move(list));
		}

		Result<VisualScriptConditionCheck> ReadVisualScriptConditionCheck(std::string_view reply)
		{
			Result<Json> const parsed = ParseReply(reply);
			if (!parsed)
			{
				return Error{parsed.GetError()};
			}
			Json const& root = parsed.GetValue();
			ReplyReader reader;
			VisualScriptConditionCheck check;
			check.Checked = reader.Bool(root, "Checked", "");
			check.Error = reader.NullableString(root, "Error", "");
			if (Json const* const reads = reader.Array(root, "Reads", ""))
			{
				for (size_t index = 0; index < reads->size() && reader.IsOk(); index++)
				{
					check.Reads.push_back(reader.String((*reads)[index], ReplyReader::Element("Reads", index)));
				}
			}
			if (Json const* const scope = reader.Array(root, "Scope", ""))
			{
				for (size_t index = 0; index < scope->size() && reader.IsOk(); index++)
				{
					Json const& json = (*scope)[index];
					std::string const at = ReplyReader::Element("Scope", index);
					VisualScriptConditionName name;
					name.Name = reader.String(json, "Name", at);
					name.Form = reader.String(json, "Form", at);
					name.Type = reader.String(json, "Type", at);
					name.Source = reader.String(json, "Source", at);
					name.Node = reader.UInt32(json, "Node", at);
					check.Scope.push_back(std::move(name));
				}
			}
			return Finish(reader, std::move(check));
		}

		Result<VisualScriptRecording> ReadVisualScriptRecording(std::string_view reply)
		{
			Result<Json> const parsed = ParseReply(reply);
			if (!parsed)
			{
				return Error{parsed.GetError()};
			}
			Json const& root = parsed.GetValue();
			ReplyReader reader;
			VisualScriptRecording recording;
			if (Json const* const summary = ReplyReader::Find(root, "Summary"))
			{
				std::vector<VisualScriptGraphSummary> graphs;
				if (Json const* const array = reader.Array(*summary, "Graphs", "Summary"))
				{
					for (size_t index = 0; index < array->size() && reader.IsOk(); index++)
					{
						Json const& json = (*array)[index];
						std::string const at = ReplyReader::Element("Summary.Graphs", index);
						VisualScriptGraphSummary graph;
						graph.Graph = reader.Handle(json, "Graph", at);
						graph.Document = reader.Handle(json, "Document", at);
						graph.Function = reader.UInt32(json, "Function", at);
						if (Json const* const steps = reader.Array(json, "Steps", at))
						{
							std::string const member = ReplyReader::Member(at, "Steps");
							for (size_t position = 0; position < steps->size() && reader.IsOk(); position++)
							{
								Json const& entry = (*steps)[position];
								std::string const where = ReplyReader::Element(member, position);
								VisualScriptStepSummary step;
								step.Node = reader.UInt32(entry, "Node", where);
								step.Title = reader.String(entry, "Title", where);
								step.Exit = reader.NullableString(entry, "Exit", where).value_or(std::string());
								step.Count = reader.UInt32(entry, "Count", where);
								step.FirstFrame = reader.UInt32(entry, "FirstFrame", where);
								step.LastFrame = reader.UInt32(entry, "LastFrame", where);
								graph.Steps.push_back(std::move(step));
							}
						}
						if (Json const* const values = reader.Array(json, "Values", at))
						{
							std::string const member = ReplyReader::Member(at, "Values");
							for (size_t position = 0; position < values->size() && reader.IsOk(); position++)
							{
								Json const& entry = (*values)[position];
								std::string const where = ReplyReader::Element(member, position);
								VisualScriptValueSummary value;
								value.Node = reader.UInt32(entry, "Node", where);
								value.Pin = reader.String(entry, "Pin", where);
								value.Input = reader.Enum<uint8_t>(entry, "Direction", where, DirectionNames) == 1;
								value.Type = reader.String(entry, "Type", where);
								value.Count = reader.UInt32(entry, "Count", where);
								if (Json const* const first = reader.Require(entry, "First", where))
								{
									value.First = ReadBareValue(reader, *first, ReplyReader::Member(where, "First"), value.Type);
								}
								if (Json const* const last = reader.Require(entry, "Last", where))
								{
									value.Last = ReadBareValue(reader, *last, ReplyReader::Member(where, "Last"), value.Type);
								}
								bool const hasMin = entry.is_object() && entry.contains("Min");
								bool const hasMax = entry.is_object() && entry.contains("Max");
								if (hasMin != hasMax)
								{
									reader.Fail(where, "only one of Min and Max");
								}
								else if (hasMin)
								{
									value.Min = ReadBareValue(reader, entry.at("Min"), ReplyReader::Member(where, "Min"), value.Type);
									value.Max = ReadBareValue(reader, entry.at("Max"), ReplyReader::Member(where, "Max"), value.Type);
								}
								graph.Values.push_back(std::move(value));
							}
						}
						graphs.push_back(std::move(graph));
					}
				}
				recording.Summary = std::move(graphs);
			}
			else
			{
				if (Json const* const steps = reader.Array(root, "Steps", ""))
				{
					for (size_t index = 0; index < steps->size() && reader.IsOk(); index++)
					{
						Json const& json = (*steps)[index];
						std::string const at = ReplyReader::Element("Steps", index);
						VisualScriptRecordedStep step;
						step.Index = reader.UInt32(json, "I", at);
						step.Frame = reader.UInt32(json, "Frame", at);
						step.Execution = reader.UInt(json, "Execution", at);
						step.Entity = reader.Entity(json, "Entity", at);
						step.Graph = reader.Handle(json, "Graph", at);
						step.Document = reader.Handle(json, "Document", at);
						step.Function = reader.UInt32(json, "Function", at);
						step.Node = reader.UInt32(json, "Node", at);
						step.Title = reader.String(json, "Title", at);
						step.Exit = reader.NullableString(json, "Exit", at).value_or(std::string());
						recording.Steps.push_back(std::move(step));
					}
				}
				if (Json const* const values = reader.Array(root, "Values", ""))
				{
					for (size_t index = 0; index < values->size() && reader.IsOk(); index++)
					{
						Json const& json = (*values)[index];
						std::string const at = ReplyReader::Element("Values", index);
						VisualScriptRecordedValue value;
						value.Step = reader.UInt32(json, "Step", at);
						value.Node = reader.UInt32(json, "Node", at);
						value.Pin = reader.String(json, "Pin", at);
						value.Input = reader.Enum<uint8_t>(json, "Direction", at, DirectionNames) == 1;
						ReadValueMembers(reader, json, at, false, value.Value);
						recording.Values.push_back(std::move(value));
					}
				}
			}
			if (Json const* const truncated = reader.Require(root, "Truncated", ""))
			{
				recording.TruncatedSteps = reader.UInt32(*truncated, "Steps", "Truncated");
				recording.TruncatedValues = reader.UInt32(*truncated, "Values", "Truncated");
			}
			return Finish(reader, std::move(recording));
		}
	}

	Result<void> VisualScriptDebugger::Attach()
	{
		Result<void> attached = SendCommand(Command("Attach"));
		if (attached)
		{
			GetData()->VisualScriptDebuggerAttached = true;
		}
		return attached;
	}

	uint32_t VisualScriptDebugger::Detach()
	{
		ScriptEngineData* const data = GetData();
		if (data == nullptr)
		{
			return 0;
		}
		Result<std::string> const reply = Send(Command("Detach"));
		Result<Json> const parsed = reply ? ParseReply(reply.GetValue()) : Result<Json>(Error{reply.GetError()});
		ReplyReader reader;
		uint32_t const released = parsed ? reader.UInt32(parsed.GetValue(), "Released", "") : 0;
		if (!parsed || !reader.IsOk())
		{
			ST_CORE_ERROR("The visual-script debugger cannot be detached: {}", parsed ? reader.GetError() : parsed.GetError());
			return 0;
		}
		data->VisualScriptDebuggerAttached = false;
		return released;
	}

	bool VisualScriptDebugger::IsAttached()
	{
		ScriptEngineData const* const data = GetData();
		return data != nullptr && data->VisualScriptDebuggerAttached;
	}

	Result<void> VisualScriptDebugger::Configure(VisualScriptDebugConfiguration const& configuration)
	{
		Json breakpoints = Json::array();
		for (VisualScriptBreakpointSetting const& breakpoint : configuration.Breakpoints)
		{
			breakpoints.push_back(Json::object({{"Document", breakpoint.Document.ToString()},
			                                    {"Node", breakpoint.Node},
			                                    {"Type", breakpoint.Type},
			                                    {"Enabled", breakpoint.Enabled},
			                                    {"Condition", breakpoint.Condition},
			                                    {"HitCount", breakpoint.HitCount}}));
		}
		Json targets = Json::array();
		for (VisualScriptDebugTarget const& target : configuration.Targets)
		{
			targets.push_back(Json::object({{"Graph", target.Graph.ToString()}, {"Entity", target.Entity.ToString()}}));
		}
		Json watches = Json::array();
		for (VisualScriptDebugWatch const& watch : configuration.Watches)
		{
			watches.push_back(Json::object({{"Document", watch.Document.ToString()}, {"Function", watch.Function}}));
		}
		Json request = Command("Configure");
		request["Revision"] = configuration.Revision;
		request["Breakpoints"] = std::move(breakpoints);
		request["Targets"] = std::move(targets);
		request["Watches"] = std::move(watches);
		request["BreakOnErrors"] = configuration.BreakOnErrors;
		return SendCommand(request);
	}

	Result<VisualScriptFrameTrace> VisualScriptDebugger::TakeTrace()
	{
		ScriptEngineData* const data = GetData();
		if (data == nullptr)
		{
			return Error{"the scripting runtime is not initialized"};
		}
		std::string block;
		auto const status = static_cast<ScriptStatus>(data->Host.VisualScriptTakeTrace(&block, &ReceiveText));
		if (status == ScriptStatus::NotFound)
		{
			return Error{"NotAttached: no debugger is attached"};
		}
		if (status != ScriptStatus::Success)
		{
			return Error{"the visual-script debugger failed (see the log)"};
		}
		return ScriptEngineInternal::ReadVisualScriptFrameTrace(std::as_bytes(std::span<char const>(block.data(), block.size())));
	}

	Result<VisualScriptDebugState> VisualScriptDebugger::GetState()
	{
		return SendAndRead(Command("State"), &ScriptEngineInternal::ReadVisualScriptDebugState);
	}

	Result<VisualScriptResumeOutcome> VisualScriptDebugger::Resume(std::optional<uint64_t> stop, VisualScriptDebugAction action, bool all)
	{
		Json request = Command("Resume");
		request["Action"] = ActionNames[static_cast<size_t>(action)];
		if (stop)
		{
			request["Stop"] = *stop;
		}
		if (all)
		{
			request["All"] = true;
		}
		Result<VisualScriptResumeOutcome> outcome = SendAndRead(request, &ScriptEngineInternal::ReadVisualScriptResumeOutcome);
		if (outcome && outcome.GetValue().Failed)
		{
			// The runtime logged the error; the application learns of it once (test runs fail on it).
			ScriptEngineInternal::ReportScriptException();
		}
		return outcome;
	}

	Result<void> VisualScriptDebugger::Break(bool cancel, std::optional<AssetHandle> document)
	{
		Json request = Command("Break");
		request["Cancel"] = cancel;
		if (document)
		{
			request["Document"] = document->ToString();
		}
		return SendCommand(request);
	}

	Result<void> VisualScriptDebugger::RunTo(AssetHandle document, uint32_t node)
	{
		Json request = Command("RunTo");
		request["Document"] = document.ToString();
		request["Node"] = node;
		return SendCommand(request);
	}

	Result<VisualScriptStopValues> VisualScriptDebugger::DescribeStop(uint64_t stop, uint32_t frame, std::span<std::string const> evaluate)
	{
		Json request = Command("Describe");
		request["Stop"] = stop;
		request["Frame"] = frame;
		if (!evaluate.empty())
		{
			Json formulas = Json::array();
			for (std::string const& formula : evaluate)
			{
				formulas.push_back(formula);
			}
			request["Evaluate"] = std::move(formulas);
		}
		return SendAndRead(request, &ScriptEngineInternal::ReadVisualScriptStopValues);
	}

	Result<VisualScriptLiveValues> VisualScriptDebugger::ReadValues(VisualScriptValueRequest const& request)
	{
		Json values = Command("Values");
		values["Entity"] = request.Entity.ToString();
		values["Function"] = request.Function;
		if (!request.Nodes.empty())
		{
			values["Nodes"] = request.Nodes;
		}
		values["Variables"] = request.Variables;
		values["Timelines"] = request.Timelines;
		return SendAndRead(values, &ScriptEngineInternal::ReadVisualScriptLiveValues);
	}

	Result<VisualScriptInstanceList> VisualScriptDebugger::DescribeInstances(VisualScriptInstanceQuery const& query)
	{
		Json request = Command("Instances");
		if (query.Graph)
		{
			request["Graph"] = query.Graph->ToString();
		}
		if (query.Entity)
		{
			request["Entity"] = query.Entity->ToString();
		}
		request["Offset"] = query.Offset;
		request["Limit"] = query.Limit;
		return SendAndRead(request, &ScriptEngineInternal::ReadVisualScriptInstanceList);
	}

	Result<VisualScriptConditionCheck> VisualScriptDebugger::CheckCondition(AssetHandle document, uint32_t node, std::string_view condition)
	{
		Json request = Command("CheckCondition");
		request["Document"] = document.ToString();
		request["Node"] = node;
		request["Condition"] = std::string(condition);
		Result<VisualScriptConditionCheck> check = SendAndRead(request, &ScriptEngineInternal::ReadVisualScriptConditionCheck);
		if (check && !check.GetValue().Checked)
		{
			return Error{"no running program compiles the graph: the condition is checked when the graph runs"};
		}
		return check;
	}

	Result<void> VisualScriptDebugger::StartRecording(VisualScriptRecordingOptions const& options)
	{
		Json start = Json::object();
		start["Entity"] = options.Entity ? Json(options.Entity->ToString()) : Json();
		if (options.Documents.empty())
		{
			start["Documents"] = Json();
		}
		else
		{
			Json documents = Json::array();
			for (AssetHandle const document : options.Documents)
			{
				documents.push_back(document.ToString());
			}
			start["Documents"] = std::move(documents);
		}
		start["Nodes"] = options.Nodes.empty() ? Json() : Json(options.Nodes);
		start["Values"] = options.Values;
		start["Summary"] = options.Summary;
		start["MaxSteps"] = options.MaxSteps;
		start["MaxValues"] = options.MaxValues;
		Json request = Command("Record");
		request["Start"] = std::move(start);
		return SendCommand(request);
	}

	Result<void> VisualScriptDebugger::MarkRecordingFrame(uint32_t frame)
	{
		Json request = Command("Record");
		request["Frame"] = frame;
		return SendCommand(request);
	}

	Result<VisualScriptRecording> VisualScriptDebugger::StopRecording()
	{
		Json request = Command("Record");
		request["Stop"] = true;
		return SendAndRead(request, &ScriptEngineInternal::ReadVisualScriptRecording);
	}
}
