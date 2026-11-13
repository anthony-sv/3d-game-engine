#include "stpch.h"
#include "Strada/Serialization/ProfilerCaptureSerializer.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Utf8.h"
#include "Strada/Serialization/JsonScanner.h"
#include "Strada/Serialization/JsonSerialization.h"
#include "Strada/Serialization/JsonTextWriter.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		// The trace's processes: the recorded threads, and the GPU views (one track per view).
		constexpr int64_t ThreadsProcess = 1;
		constexpr int64_t GpuProcess = 2;
		// Times are microseconds in the file; below 2^51 nanoseconds (26 days) a time reads back to the nanosecond.
		constexpr double MaxTimeNanoseconds = 2251799813685248.0;

		// The longest prefix of text at most maxBytes long that ends at a UTF-8 character boundary.
		std::string_view CutAtCharacter(std::string_view text, size_t maxBytes)
		{
			if (text.size() <= maxBytes)
			{
				return text;
			}
			size_t end = maxBytes;
			while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
			{
				end--;
			}
			return text.substr(0, end);
		}

		// A name as captures store it: valid UTF-8, cut at a character boundary.
		std::string MakeStoredName(std::string_view name, size_t maxBytes)
		{
			std::string const valid = Utf8::Sanitize(name);
			return std::string(CutAtCharacter(valid, maxBytes));
		}

		double ToMicroseconds(int64_t nanoseconds)
		{
			return static_cast<double>(nanoseconds) / 1000.0;
		}

		// Whether a time (or a time plus a duration) can be written to read back exactly.
		bool IsWritableTime(int64_t nanoseconds)
		{
			return nanoseconds >= 0 && static_cast<double>(nanoseconds) < MaxTimeNanoseconds;
		}

		// A frame's GPU passes are laid end to end on their view's track from the frame's start, since only their durations
		// are measured: the end of the track after the pass, past the times a capture holds when it would overflow.
		int64_t AddGpuPass(int64_t trackEnd, int64_t duration)
		{
			int64_t const length = std::max<int64_t>(duration, 0);
			return trackEnd > std::numeric_limits<int64_t>::max() - length ? std::numeric_limits<int64_t>::max() : trackEnd + length;
		}

		// --- Writing ---

		// The tables of what a capture's frames use, renumbered in the order of the capture's own tables.
		struct WrittenTables
		{
			// Per thread entry of the capture: its tid in the file (1 for the main thread), or 0 when no frame uses it.
			std::vector<uint32_t> ThreadIds;
			std::vector<uint32_t> Threads;
			// Per name of the capture: its view's tid in the GPU process, or 0 when it names no view a frame uses.
			std::vector<uint32_t> ViewIds;
			std::vector<uint32_t> Views;
			// Per name of the capture: the name as written.
			std::vector<std::string> Names;
		};

		Result<WrittenTables> MakeWrittenTables(ProfilerCapture const& capture)
		{
			if (capture.Threads.empty())
			{
				return Error{"the capture has no main thread"};
			}
			if (capture.Frames.size() > Profiler::MaxHistoryFrames)
			{
				return MakeError("the capture holds {} frames, more than the {} a capture may hold", capture.Frames.size(),
				                 Profiler::MaxHistoryFrames);
			}
			WrittenTables tables;
			tables.ThreadIds.assign(capture.Threads.size(), 0);
			tables.ViewIds.assign(capture.Names.size(), 0);
			std::vector<bool> usedNames(capture.Names.size(), false);
			tables.ThreadIds[0] = 1;
			uint64_t zones = 0;
			uint64_t passes = 0;
			Error const timeError = Error{"a time of the capture lies beyond 26 days from its start, past what a capture holds"};
			for (ProfilerFrame const& frame : capture.Frames)
			{
				zones += frame.Zones.size();
				passes += frame.GpuPasses.size();
				if (!IsWritableTime(frame.StartNanoseconds) || !IsWritableTime(frame.EndNanoseconds) ||
				    !IsWritableTime(frame.EndNanoseconds - frame.StartNanoseconds))
				{
					return timeError;
				}
				std::unordered_map<uint32_t, int64_t> trackEnds;
				for (ProfilerGpuPass const& pass : frame.GpuPasses)
				{
					auto const [track, added] = trackEnds.try_emplace(pass.View, frame.StartNanoseconds);
					track->second = AddGpuPass(track->second, pass.DurationNanoseconds);
					if (!IsWritableTime(track->second))
					{
						return timeError;
					}
				}
				for (ProfilerZone const& zone : frame.Zones)
				{
					if (zone.Thread >= capture.Threads.size() || zone.Name >= capture.Names.size() ||
					    (zone.ValueLabel != ProfilerNoName && zone.ValueLabel >= capture.Names.size()))
					{
						return Error{"a zone refers to a thread or name the capture does not hold"};
					}
					if (!IsWritableTime(zone.StartNanoseconds) || !IsWritableTime(zone.DurationNanoseconds))
					{
						return timeError;
					}
					tables.ThreadIds[zone.Thread] = 1;
					usedNames[zone.Name] = true;
					if (zone.ValueLabel != ProfilerNoName)
					{
						usedNames[zone.ValueLabel] = true;
					}
				}
				for (ProfilerGpuPass const& pass : frame.GpuPasses)
				{
					if (pass.View >= capture.Names.size() || pass.Name >= capture.Names.size())
					{
						return Error{"a GPU pass refers to a name the capture does not hold"};
					}
					tables.ViewIds[pass.View] = 1;
					usedNames[pass.View] = true;
					usedNames[pass.Name] = true;
				}
			}
			if (zones > Profiler::MaxHistoryZones)
			{
				return MakeError("the capture holds {} zones, more than the {} a capture may hold", zones, Profiler::MaxHistoryZones);
			}
			if (passes > ProfilerCaptureSerializer::MaxGpuPasses)
			{
				return MakeError("the capture holds {} GPU passes, more than the {} a capture may hold", passes,
				                 ProfilerCaptureSerializer::MaxGpuPasses);
			}

			for (size_t thread = 0; thread < capture.Threads.size(); thread++)
			{
				if (tables.ThreadIds[thread] != 0)
				{
					tables.Threads.push_back(static_cast<uint32_t>(thread));
					tables.ThreadIds[thread] = static_cast<uint32_t>(tables.Threads.size());
				}
			}
			for (size_t name = 0; name < capture.Names.size(); name++)
			{
				if (tables.ViewIds[name] != 0)
				{
					tables.Views.push_back(static_cast<uint32_t>(name));
					tables.ViewIds[name] = static_cast<uint32_t>(tables.Views.size());
				}
			}
			if (tables.Threads.size() > Profiler::MaxRecordedThreads || tables.Views.size() > Profiler::MaxGpuViews)
			{
				return MakeError("the capture's frames use {} threads and {} GPU views, more than the {} and {} a capture may hold",
				                 tables.Threads.size(), tables.Views.size(), Profiler::MaxRecordedThreads, Profiler::MaxGpuViews);
			}

			tables.Names.resize(capture.Names.size());
			std::unordered_map<std::string, uint32_t> distinct;
			for (size_t name = 0; name < capture.Names.size(); name++)
			{
				if (usedNames[name])
				{
					tables.Names[name] = MakeStoredName(capture.Names[name], Profiler::MaxNameBytes);
					distinct.emplace(tables.Names[name], 0);
				}
			}
			if (distinct.size() > Profiler::MaxCaptureNames)
			{
				return MakeError("the capture's frames use {} names, more than the {} a capture may hold", distinct.size(),
				                 Profiler::MaxCaptureNames);
			}
			return tables;
		}

		// The capture's category of a zone: its subsystem (GetProfilerSubsystem, inherited from the nearest enclosing zone
		// on its thread that has one), "Other" when it has none.
		class SubsystemTracker
		{
		public:
			explicit SubsystemTracker(size_t threadCount)
				: m_Stacks(threadCount)
			{
			}

			void BeginFrame()
			{
				for (std::vector<std::pair<uint32_t, std::string_view>>& stack : m_Stacks)
				{
					stack.clear();
				}
			}

			std::string_view Take(ProfilerZone const& zone, std::string_view name)
			{
				std::vector<std::pair<uint32_t, std::string_view>>& stack = m_Stacks[zone.Thread];
				while (!stack.empty() && stack.back().first >= zone.Depth)
				{
					stack.pop_back();
				}
				std::string_view const own = GetProfilerSubsystem(name);
				std::string_view const subsystem = !own.empty() ? own : !stack.empty() ? stack.back().second : std::string_view("Other");
				stack.emplace_back(zone.Depth, subsystem);
				return subsystem;
			}

		private:
			std::vector<std::vector<std::pair<uint32_t, std::string_view>>> m_Stacks;
		};

		void WriteMetadata(JsonTextWriter& writer, int64_t process, std::optional<uint32_t> thread, std::string_view kind,
		                   std::string_view name)
		{
			writer.BeginObject();
			writer.Key("ph");
			writer.String("M");
			writer.Key("pid");
			writer.Integer(process);
			if (thread)
			{
				writer.Key("tid");
				writer.Unsigned(*thread);
			}
			writer.Key("name");
			writer.String(kind);
			writer.Key("args");
			writer.BeginObject();
			writer.Key("name");
			writer.String(name);
			writer.EndObject();
			writer.EndObject();
		}

		// The members every complete event begins with.
		void BeginCompleteEvent(JsonTextWriter& writer, int64_t process, uint32_t thread, std::string_view name, std::string_view category,
		                        int64_t start, int64_t duration)
		{
			writer.BeginObject();
			writer.Key("ph");
			writer.String("X");
			writer.Key("pid");
			writer.Integer(process);
			writer.Key("tid");
			writer.Unsigned(thread);
			writer.Key("name");
			writer.String(name);
			writer.Key("cat");
			writer.String(category);
			writer.Key("ts");
			writer.Double(ToMicroseconds(start));
			writer.Key("dur");
			writer.Double(ToMicroseconds(duration));
			writer.Key("args");
			writer.BeginObject();
		}

		Error MakeSizeError()
		{
			return MakeError("the capture would take more than {} MiB, the most a capture may take: record fewer frames or zones "
			                 "(HistoryFrames, HistoryZones)",
			                 Profiler::MaxCaptureBytes / (1024 * 1024));
		}

		// --- Reading ---

		Result<uint64_t> ReadUnsigned(Json const& object, char const* key, uint64_t maximum)
		{
			auto const member = object.find(key);
			if (member == object.end() || !member->is_number_integer() || (!member->is_number_unsigned() && member->get<int64_t>() < 0))
			{
				return MakeError("\"{}\" is not an unsigned integer", key);
			}
			uint64_t const value = member->get<uint64_t>();
			if (value > maximum)
			{
				return MakeError("\"{}\" is {}, more than {}", key, value, maximum);
			}
			return value;
		}

		// A time in microseconds, as nanoseconds.
		Result<int64_t> ReadTime(Json const& object, char const* key)
		{
			auto const member = object.find(key);
			if (member == object.end() || !member->is_number())
			{
				return MakeError("\"{}\" is not a number", key);
			}
			double const nanoseconds = member->get<double>() * 1000.0;
			if (!std::isfinite(nanoseconds) || nanoseconds < 0.0 || nanoseconds >= MaxTimeNanoseconds)
			{
				return MakeError("\"{}\" is not a time from 0 to 26 days", key);
			}
			return static_cast<int64_t>(std::llround(nanoseconds));
		}

		Result<std::string_view> ReadName(Json const& object, char const* key, size_t maxBytes)
		{
			auto const member = object.find(key);
			if (member == object.end() || !member->is_string())
			{
				return MakeError("\"{}\" is not a string", key);
			}
			std::string const& text = member->get_ref<std::string const&>();
			if (text.size() > maxBytes)
			{
				return MakeError("\"{}\" is longer than {} bytes", key, maxBytes);
			}
			if (!Utf8::IsValid(text))
			{
				return MakeError("\"{}\" is not UTF-8", key);
			}
			return std::string_view(text);
		}

		Result<Json const*> ReadObject(Json const& object, char const* key)
		{
			auto const member = object.find(key);
			if (member == object.end() || !member->is_object())
			{
				return MakeError("\"{}\" is not an object", key);
			}
			return &*member;
		}

		// Builds a capture from its events in file order.
		class CaptureReader
		{
		public:
			Result<void> ReadEvent(Json const& event)
			{
				if (!event.is_object())
				{
					return Error{"it is not an object"};
				}
				Result<std::string_view> const phase = ReadName(event, "ph", Profiler::MaxNameBytes);
				if (!phase)
				{
					return Error{phase.GetError()};
				}
				if (phase.GetValue() == "M")
				{
					return ReadMetadata(event);
				}
				if (phase.GetValue() == "X")
				{
					return ReadCompleteEvent(event);
				}
				return MakeError("its phase \"{}\" is not one a capture holds (\"M\" or \"X\")", phase.GetValue());
			}

			Result<ProfilerCapture> TakeCapture()
			{
				if (m_Capture.Threads.empty())
				{
					return Error{"the capture names no main thread (a \"thread_name\" event with \"tid\": 1)"};
				}
				return std::move(m_Capture);
			}

		private:
			Result<void> ReadMetadata(Json const& event)
			{
				Result<std::string_view> const kind = ReadName(event, "name", Profiler::MaxNameBytes);
				if (!kind)
				{
					return Error{kind.GetError()};
				}
				// Other metadata (process names, sort orders) says nothing the capture holds.
				if (kind.GetValue() != "thread_name")
				{
					return {};
				}
				Result<uint64_t> const process = ReadUnsigned(event, "pid", GpuProcess);
				Result<uint64_t> const thread = ReadUnsigned(event, "tid", std::numeric_limits<uint32_t>::max());
				Result<Json const*> const arguments = ReadObject(event, "args");
				if (!process || !thread || !arguments)
				{
					return Error{!process ? process.GetError() : !thread ? thread.GetError() : arguments.GetError()};
				}
				if (process.GetValue() == static_cast<uint64_t>(ThreadsProcess))
				{
					Result<std::string_view> const name = ReadName(*arguments.GetValue(), "name", Profiler::MaxThreadNameBytes);
					if (!name)
					{
						return Error{name.GetError()};
					}
					if (thread.GetValue() != m_Capture.Threads.size() + 1)
					{
						return MakeError("thread {} is named out of order (threads are named from tid 1, each once)", thread.GetValue());
					}
					if (m_Capture.Threads.size() >= Profiler::MaxRecordedThreads)
					{
						return MakeError("it names more than {} threads", Profiler::MaxRecordedThreads);
					}
					m_Capture.Threads.push_back({std::string(name.GetValue())});
					return {};
				}
				if (process.GetValue() != static_cast<uint64_t>(GpuProcess))
				{
					return MakeError("its process {} is neither the threads' (1) nor the GPU's (2)", process.GetValue());
				}
				Result<std::string_view> const view = ReadName(*arguments.GetValue(), "name", Profiler::MaxNameBytes);
				if (!view)
				{
					return Error{view.GetError()};
				}
				if (thread.GetValue() != m_Views.size() + 1)
				{
					return MakeError("GPU view {} is named out of order (views are named from tid 1, each once)", thread.GetValue());
				}
				if (m_Views.size() >= Profiler::MaxGpuViews)
				{
					return MakeError("it names more than {} GPU views", Profiler::MaxGpuViews);
				}
				Result<uint32_t> const name = AddName(view.GetValue());
				if (!name)
				{
					return Error{name.GetError()};
				}
				m_Views.push_back(name.GetValue());
				return {};
			}

			Result<void> ReadCompleteEvent(Json const& event)
			{
				Result<uint64_t> const process = ReadUnsigned(event, "pid", GpuProcess);
				Result<uint64_t> const thread = ReadUnsigned(event, "tid", std::numeric_limits<uint32_t>::max());
				Result<int64_t> const start = ReadTime(event, "ts");
				Result<int64_t> const duration = ReadTime(event, "dur");
				Result<Json const*> const arguments = ReadObject(event, "args");
				if (!process || !thread || !start || !duration || !arguments)
				{
					return Error{!process    ? process.GetError()
					             : !thread   ? thread.GetError()
					             : !start    ? start.GetError()
					             : !duration ? duration.GetError()
					                         : arguments.GetError()};
				}
				Json const& args = *arguments.GetValue();
				Result<uint64_t> const frameIndex = ReadUnsigned(args, "Frame", std::numeric_limits<uint64_t>::max());
				if (!frameIndex)
				{
					return Error{frameIndex.GetError()};
				}
				if (process.GetValue() == static_cast<uint64_t>(ThreadsProcess) && !args.contains("Depth"))
				{
					return ReadFrame(thread.GetValue(), frameIndex.GetValue(), start.GetValue(), duration.GetValue(), args);
				}

				ProfilerFrame* frame = FindFrame(frameIndex.GetValue());
				if (frame == nullptr)
				{
					return MakeError("its frame {} is not one a \"Frame\" event before it declares", frameIndex.GetValue());
				}
				Result<std::string_view> const name = ReadName(event, "name", Profiler::MaxNameBytes);
				if (!name)
				{
					return Error{name.GetError()};
				}
				Result<uint32_t> const nameIndex = AddName(name.GetValue());
				if (!nameIndex)
				{
					return Error{nameIndex.GetError()};
				}

				if (process.GetValue() == static_cast<uint64_t>(GpuProcess))
				{
					if (thread.GetValue() == 0 || thread.GetValue() > m_Views.size())
					{
						return MakeError("its GPU view {} is not one a \"thread_name\" event before it names", thread.GetValue());
					}
					if (m_GpuPasses >= ProfilerCaptureSerializer::MaxGpuPasses)
					{
						return MakeError("the capture holds more than {} GPU passes", ProfilerCaptureSerializer::MaxGpuPasses);
					}
					// Written again, the passes are laid end to end on their view's track: they must end within a capture's times.
					std::vector<int64_t>& trackEnds =
						m_TrackEnds[static_cast<size_t>(frameIndex.GetValue() - m_Capture.Frames.front().Index)];
					if (trackEnds.empty())
					{
						trackEnds.assign(Profiler::MaxGpuViews, frame->StartNanoseconds);
					}
					int64_t& trackEnd = trackEnds[thread.GetValue() - 1];
					trackEnd = AddGpuPass(trackEnd, duration.GetValue());
					if (!IsWritableTime(trackEnd))
					{
						return Error{"its frame's passes of its view end beyond 26 days from the capture's start"};
					}
					frame->GpuPasses.push_back({m_Views[thread.GetValue() - 1], nameIndex.GetValue(), duration.GetValue()});
					m_GpuPasses++;
					return {};
				}

				if (thread.GetValue() == 0 || thread.GetValue() > m_Capture.Threads.size())
				{
					return MakeError("its thread {} is not one a \"thread_name\" event before it names", thread.GetValue());
				}
				Result<uint64_t> const depth = ReadUnsigned(args, "Depth", std::numeric_limits<uint32_t>::max());
				if (!depth)
				{
					return Error{depth.GetError()};
				}
				if (m_Zones >= Profiler::MaxHistoryZones)
				{
					return MakeError("the capture holds more than {} zones", Profiler::MaxHistoryZones);
				}
				ProfilerZone zone;
				zone.Name = nameIndex.GetValue();
				zone.Thread = static_cast<uint32_t>(thread.GetValue() - 1);
				zone.Depth = static_cast<uint32_t>(depth.GetValue());
				zone.StartNanoseconds = start.GetValue();
				zone.DurationNanoseconds = duration.GetValue();
				if (auto const value = args.find("Value"); value != args.end())
				{
					if (!value->is_object() || value->size() != 1)
					{
						return Error{"\"Value\" is not an object of one count"};
					}
					std::string const label = value->begin().key();
					if (label.size() > Profiler::MaxNameBytes || !Utf8::IsValid(label))
					{
						return MakeError("the label of \"Value\" is not UTF-8 of at most {} bytes", Profiler::MaxNameBytes);
					}
					Result<uint64_t> const count = ReadUnsigned(*value, label.c_str(), std::numeric_limits<uint64_t>::max());
					if (!count)
					{
						return Error{count.GetError()};
					}
					Result<uint32_t> const labelIndex = AddName(label);
					if (!labelIndex)
					{
						return Error{labelIndex.GetError()};
					}
					zone.ValueLabel = labelIndex.GetValue();
					zone.Value = count.GetValue();
				}
				m_Zones++;
				frame->Zones.push_back(zone);
				return {};
			}

			Result<void> ReadFrame(uint64_t thread, uint64_t index, int64_t start, int64_t duration, Json const& args)
			{
				if (thread != 1)
				{
					return MakeError("its frame is on thread {}, not on the main thread (1)", thread);
				}
				Result<uint64_t> const dropped = ReadUnsigned(args, "DroppedZones", std::numeric_limits<uint32_t>::max());
				if (!dropped)
				{
					return Error{dropped.GetError()};
				}
				if (!m_Capture.Frames.empty() && index != m_Capture.Frames.back().Index + 1)
				{
					return MakeError("frame {} does not follow frame {} (a capture's frames are consecutive)", index,
					                 m_Capture.Frames.back().Index);
				}
				if (m_Capture.Frames.size() >= Profiler::MaxHistoryFrames)
				{
					return MakeError("the capture holds more than {} frames", Profiler::MaxHistoryFrames);
				}
				if (!IsWritableTime(start + duration))
				{
					return Error{"the frame ends beyond 26 days from the capture's start"};
				}
				ProfilerFrame frame;
				frame.Index = index;
				frame.StartNanoseconds = start;
				frame.EndNanoseconds = start + duration;
				frame.DroppedZones = static_cast<uint32_t>(dropped.GetValue());
				m_Capture.Frames.push_back(std::move(frame));
				m_TrackEnds.emplace_back();
				return {};
			}

			ProfilerFrame* FindFrame(uint64_t index)
			{
				if (m_Capture.Frames.empty() || index < m_Capture.Frames.front().Index || index > m_Capture.Frames.back().Index)
				{
					return nullptr;
				}
				return &m_Capture.Frames[static_cast<size_t>(index - m_Capture.Frames.front().Index)];
			}

			Result<uint32_t> AddName(std::string_view name)
			{
				if (auto const found = m_NameIndices.find(std::string(name)); found != m_NameIndices.end())
				{
					return found->second;
				}
				if (m_Capture.Names.size() >= Profiler::MaxCaptureNames)
				{
					return MakeError("the capture holds more than {} names", Profiler::MaxCaptureNames);
				}
				uint32_t const index = static_cast<uint32_t>(m_Capture.Names.size());
				m_Capture.Names.emplace_back(name);
				m_NameIndices.emplace(std::string(name), index);
				return index;
			}

			ProfilerCapture m_Capture;
			std::unordered_map<std::string, uint32_t> m_NameIndices;
			// The names of the GPU views, by tid - 1.
			std::vector<uint32_t> m_Views;
			// Per frame, where each view's passes end when laid end to end (empty until the frame has a pass).
			std::vector<std::vector<int64_t>> m_TrackEnds;
			uint64_t m_Zones = 0;
			uint64_t m_GpuPasses = 0;
		};
	}

	Result<std::string> ProfilerCaptureSerializer::Write(ProfilerCapture const& capture)
	{
		Result<WrittenTables> prepared = MakeWrittenTables(capture);
		if (!prepared)
		{
			return Error{prepared.GetError()};
		}
		WrittenTables const& tables = prepared.GetValue();

		JsonTextWriter writer(JsonTextWriter::Style{.Pretty = false});
		writer.BeginObject();
		writer.Key("Strada");
		writer.BeginObject();
		writer.Key("Version");
		writer.Integer(Version);
		writer.Key("Type");
		writer.String(FileType);
		writer.EndObject();
		writer.Key("displayTimeUnit");
		writer.String("ms");
		writer.Key("traceEvents");
		writer.BeginArray();

		WriteMetadata(writer, ThreadsProcess, std::nullopt, "process_name", "Strada");
		for (uint32_t const thread : tables.Threads)
		{
			WriteMetadata(writer, ThreadsProcess, tables.ThreadIds[thread], "thread_name",
			              MakeStoredName(capture.Threads[thread].Name, Profiler::MaxThreadNameBytes));
		}
		if (!tables.Views.empty())
		{
			WriteMetadata(writer, GpuProcess, std::nullopt, "process_name", "GPU");
			for (uint32_t const view : tables.Views)
			{
				WriteMetadata(writer, GpuProcess, tables.ViewIds[view], "thread_name", tables.Names[view]);
			}
		}

		SubsystemTracker subsystems(capture.Threads.size());
		std::vector<int64_t> viewEnds(tables.Views.size() + 1, 0);
		for (ProfilerFrame const& frame : capture.Frames)
		{
			BeginCompleteEvent(writer, ThreadsProcess, 1, "Frame", "Frame", frame.StartNanoseconds,
			                   std::max<int64_t>(frame.EndNanoseconds - frame.StartNanoseconds, 0));
			writer.Key("Frame");
			writer.Unsigned(frame.Index);
			writer.Key("DroppedZones");
			writer.Unsigned(frame.DroppedZones);
			writer.EndObject();
			writer.EndObject();

			subsystems.BeginFrame();
			for (ProfilerZone const& zone : frame.Zones)
			{
				std::string const& name = tables.Names[zone.Name];
				BeginCompleteEvent(writer, ThreadsProcess, tables.ThreadIds[zone.Thread], name, subsystems.Take(zone, name),
				                   zone.StartNanoseconds, zone.DurationNanoseconds);
				writer.Key("Frame");
				writer.Unsigned(frame.Index);
				writer.Key("Depth");
				writer.Unsigned(zone.Depth);
				if (zone.ValueLabel != ProfilerNoName)
				{
					writer.Key("Value");
					writer.BeginObject();
					writer.Key(tables.Names[zone.ValueLabel]);
					writer.Unsigned(zone.Value);
					writer.EndObject();
				}
				writer.EndObject();
				writer.EndObject();
			}

			// Only durations are measured: each view's passes are laid end to end from the frame's start.
			// The layout MakeWrittenTables checked.
			std::fill(viewEnds.begin(), viewEnds.end(), frame.StartNanoseconds);
			for (ProfilerGpuPass const& pass : frame.GpuPasses)
			{
				uint32_t const view = tables.ViewIds[pass.View];
				int64_t const start = viewEnds[view];
				int64_t const duration = std::max<int64_t>(pass.DurationNanoseconds, 0);
				viewEnds[view] = AddGpuPass(start, duration);
				BeginCompleteEvent(writer, GpuProcess, view, tables.Names[pass.Name], "GPU", start, duration);
				writer.Key("Frame");
				writer.Unsigned(frame.Index);
				writer.EndObject();
				writer.EndObject();
			}

			if (writer.GetSize() > Profiler::MaxCaptureBytes)
			{
				return MakeSizeError();
			}
		}

		writer.EndArray();
		writer.EndObject();
		if (writer.GetSize() > Profiler::MaxCaptureBytes)
		{
			return MakeSizeError();
		}
		return writer.TakeText();
	}

	Result<ProfilerCapture> ProfilerCaptureSerializer::Read(std::string_view text)
	{
		if (text.size() > Profiler::MaxCaptureBytes)
		{
			return MakeError("the capture is larger than {} MiB, the most a capture may take", Profiler::MaxCaptureBytes / (1024 * 1024));
		}
		std::optional<std::vector<JsonScanMember>> const members = ScanJsonTopLevel(text);
		if (!members)
		{
			// Parsing gives the position of the problem.
			Result<Json> const parsed = ParseJson(text);
			return MakeError("the capture is not a JSON object: {}", parsed ? std::string("it is another value") : parsed.GetError());
		}
		JsonScanMember const* header = nullptr;
		JsonScanMember const* events = nullptr;
		for (JsonScanMember const& member : *members)
		{
			JsonScanMember const** found = member.Key == "Strada" ? &header : member.Key == "traceEvents" ? &events : nullptr;
			if (found == nullptr)
			{
				continue;
			}
			if (*found != nullptr)
			{
				return MakeError("the capture has \"{}\" twice", member.Key);
			}
			*found = &member;
		}
		if (header == nullptr)
		{
			return Error{"missing \"Strada\" file header"};
		}
		Result<Json> headerValue = ParseJson(text.substr(header->Value.first, header->Value.second));
		if (!headerValue)
		{
			return MakeError("the file header: {}", headerValue.GetError());
		}
		Json document = Json::object();
		document["Strada"] = headerValue.TakeValue();
		if (Result<int> const version = ReadFileHeader(document, FileType, Version); !version)
		{
			return Error{version.GetError()};
		}
		if (events == nullptr || text[events->Value.first] != '[')
		{
			return Error{"\"traceEvents\" is not an array"};
		}

		CaptureReader reader;
		for (size_t index = 0; index < events->Elements.size(); index++)
		{
			std::pair<uint32_t, uint32_t> const element = events->Elements[index];
			Result<Json> const event = ParseJson(text.substr(element.first, element.second));
			Result<void> const read = event ? reader.ReadEvent(event.GetValue()) : Result<void>(Error{event.GetError()});
			if (!read)
			{
				return MakeError("event {}: {}", index, read.GetError());
			}
		}
		return reader.TakeCapture();
	}

	Result<void> ProfilerCaptureSerializer::Save(ProfilerCapture const& capture, std::filesystem::path const& path)
	{
		Result<std::string> const text = Write(capture);
		if (!text)
		{
			return Error{text.GetError()};
		}
		return FileSystem::WriteTextFile(path, text.GetValue());
	}

	Result<ProfilerCapture> ProfilerCaptureSerializer::Load(std::filesystem::path const& path)
	{
		std::optional<uint64_t> const size = FileSystem::GetFileSize(path);
		if (!size)
		{
			return MakeError("'{}' is not a file that can be read", FileSystem::PathToUtf8(path));
		}
		if (*size > Profiler::MaxCaptureBytes)
		{
			return MakeError("'{}' is larger than {} MiB, the most a capture may take", FileSystem::PathToUtf8(path),
			                 Profiler::MaxCaptureBytes / (1024 * 1024));
		}
		Result<std::string> const text = FileSystem::ReadTextFile(path);
		if (!text)
		{
			return Error{text.GetError()};
		}
		return Read(text.GetValue());
	}
}
