#include "stpch.h"
#include "Strada/Core/Telemetry.h"

#include "Strada/Core/Assert.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Profiler.h"
#include "Strada/Core/Utf8.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace Strada
{
	constinit std::atomic<bool> Telemetry::s_Recording{false};

	namespace
	{
		constexpr uint32_t NoSlot = std::numeric_limits<uint32_t>::max();
		constexpr uint32_t NoChannel = std::numeric_limits<uint32_t>::max();
		// Free buffers kept ready for threads that have not recorded yet in the session; each collection tops them up.
		constexpr uint32_t SpareSlots = 4;
		constexpr size_t CacheLineBytes = 64;
		constexpr float NoValue = std::numeric_limits<float>::quiet_NaN();
		constexpr uint64_t BlockBytes = uint64_t{TelemetryChannel::BlockCells} * sizeof(float);
		// RecordMark's step for MarkStep's step 0 (Telemetry::InvalidMarkStep).
		constexpr uint64_t RefusedMarkStep = std::numeric_limits<uint64_t>::max();

		// The life of a buffer slot within one generation (a session, or the wait for a recording's first one). A slot is
		// free once the main thread has given it a ring, owned once a thread has claimed it, and ending once its thread
		// has called EndThread, until the main thread collects it and frees it again.
		enum class SlotState : uint32_t
		{
			Unprovisioned = 0,
			Free,
			Owned,
			Ending
		};

		// A slot's owner word: the generation the state belongs to (high half) and the state, so that a thread holding a
		// slot of an ended session can never take or release a slot of a later one.
		constexpr uint64_t PackOwner(uint32_t generation, SlotState state)
		{
			return (static_cast<uint64_t>(generation) << 32) | static_cast<uint32_t>(state);
		}

		constexpr uint32_t GetOwnerGeneration(uint64_t owner)
		{
			return static_cast<uint32_t>(owner >> 32);
		}

		constexpr SlotState GetOwnerState(uint64_t owner)
		{
			return static_cast<SlotState>(static_cast<uint32_t>(owner));
		}

		enum class RecordKind : uint8_t
		{
			Value = 0,
			Mark
		};

		// RecordedName::Length of a name longer than Telemetry::MaxNameBytes, which is refused.
		constexpr uint8_t TooLongName = std::numeric_limits<uint8_t>::max();
		static_assert(Telemetry::MaxNameBytes < TooLongName);

		// A value or a mark as a thread other than the main one writes it into its ring.
		struct TelemetryRecord
		{
			// A mark's step: 0 for the step that collects it, RefusedMarkStep, or the step MarkStep named.
			uint64_t Step = 0;
			float Value = 0.0f;
			RecordKind Kind = RecordKind::Value;
			uint8_t Length = 0;
			std::array<char, Telemetry::MaxNameBytes> Name{};
		};

		// A buffer slot: the header of one thread's single-producer, single-consumer ring, as the profiler's (Profiler.cpp).
		// The headers have static storage and are never freed, so a thread that races the end of a session, Stop or
		// Shutdown only ever touches memory that lives on. A producer counts itself in Writers before it reads or writes
		// anything else of a slot, then checks the generation; the main thread changes the generation and then waits for
		// every slot's Writers to reach zero (a Dekker pair: both sides are sequentially consistent), so once it has
		// waited no producer reads a ring or writes a slot of the ended generation.
		struct TelemetrySlotFields
		{
			std::atomic<uint64_t> Owner{0};
			std::atomic<uint32_t> Writers{0};
			// Records written by the producer (release), read by the consumer (acquire).
			std::atomic<uint32_t> Head{0};
			// Records the producer dropped because the ring was full; the consumer takes them each collection.
			std::atomic<uint32_t> DroppedValues{0};
			std::atomic<uint32_t> DroppedMarks{0};
			// Written by the main thread before it publishes the slot as free, read by the thread that claims it.
			TelemetryRecord* Records = nullptr;
			uint32_t Mask = 0;
			uint32_t Capacity = 0;
		};

		constexpr size_t GetPaddingToCacheLine(size_t bytes)
		{
			return (bytes + CacheLineBytes - 1) / CacheLineBytes * CacheLineBytes - bytes;
		}

		// The fields padded to whole cache lines, then the consumer's index on a cache line of its own.
		struct TelemetrySlot : TelemetrySlotFields
		{
			std::array<std::byte, GetPaddingToCacheLine(sizeof(TelemetrySlotFields))> FieldsPadding{};
			std::atomic<uint32_t> Tail{0};
			std::array<std::byte, CacheLineBytes - sizeof(std::atomic<uint32_t>)> TailPadding{};
		};

		static_assert(sizeof(TelemetrySlot) % CacheLineBytes == 0);

		// What a thread remembers between its records: trivially destructible and constant-initialized, so that reaching
		// it never registers a destructor or runs an initializer. MainInit is compared with s_InitNumber.
		struct TelemetryThreadState
		{
			uint32_t Slot = NoSlot;
			uint32_t SlotGeneration = 0;
			uint32_t MainInit = 0;
		};

		static_assert(std::is_trivially_destructible_v<TelemetryThreadState>);

		alignas(CacheLineBytes) constinit TelemetrySlot s_Slots[Telemetry::MaxThreads];
		// The generation in progress: Start, Stop and every session's beginning and end advance it.
		constinit std::atomic<uint32_t> s_Generation{0};
		constinit std::atomic<uint32_t> s_InitNumber{0};
		// Records dropped by threads that found no free slot, counted in the next collected row.
		constinit std::atomic<uint32_t> s_UnattributedValues{0};
		constinit std::atomic<uint32_t> s_UnattributedMarks{0};
		constinit thread_local TelemetryThreadState s_ThreadState;

		struct StringHash
		{
			using is_transparent = void;

			size_t operator()(std::string_view text) const { return std::hash<std::string_view>{}(text); }
		};

		using NameIndexMap = std::unordered_map<std::string, uint32_t, StringHash, std::equal_to<>>;

		// Why something recorded was dropped; the first drop of each kind in a session is logged.
		enum class DropKind : uint8_t
		{
			Name = 0,
			NotFinite,
			Channels,
			Steps,
			Bytes,
			Marks,
			MarkStep,
			FullBuffer,
			NoBuffer
		};

		constexpr size_t DropKinds = static_cast<size_t>(DropKind::NoBuffer) + 1;

		// A channel's value of the pending row, and where the live session keeps the channel.
		struct PendingCell
		{
			std::string Name;
			float Value = 0.0f;
			// The pending row's stamp when the cell holds a value of it.
			uint64_t Stamp = 0;
			// The live session's channel of this name, valid while Session is that session's Id.
			uint32_t Session = 0;
			uint32_t Channel = NoChannel;
		};

		struct PendingMark
		{
			std::string Label;
			// 0: the step of the row that collects it; else the step MarkStep named.
			uint64_t Step = 0;
		};

		// What the pending row lost, counted into the session that collects it.
		struct PendingDrops
		{
			uint64_t Values = 0;
			uint64_t Marks = 0;
			std::array<bool, DropKinds> Seen{};
			// What the first drop of each kind concerned, for its warning.
			std::array<std::string, DropKinds> Examples;
		};

		// The live session's state beside its public data.
		struct LiveState
		{
			NameIndexMap ChannelIndices;
			// Per channel: the block made ahead of its column's need, empty when none.
			std::vector<std::vector<float>> Spares;
			// Marks for steps the session has not reached yet, in the order collected.
			std::vector<PendingMark> FutureMarks;
			// The step the session's next row must have.
			uint64_t NextStep = 0;
			std::array<bool, DropKinds> Warned{};
		};

		// The main thread's state: the recording, the sessions, the pending row and the threads' rings.
		struct TelemetryData
		{
			TelemetrySettings Settings;
			bool Recording = false;
			uint32_t Generation = 0;
			uint32_t NextSessionId = 1;
			std::deque<TelemetrySession> Sessions;
			uint64_t BytesHeld = 0;
			// Over the recording in progress or the last one.
			uint64_t DroppedValues = 0;
			uint64_t DroppedMarks = 0;
			uint64_t DroppedSteps = 0;

			LiveState Live;

			// The row the next step collects: the main thread's records go straight into it, the rings are folded into
			// it, and EndStep commits it.
			NameIndexMap PendingIndices;
			std::vector<PendingCell> PendingCells;
			std::vector<uint32_t> PendingTouched;
			uint64_t PendingStamp = 1;
			std::vector<PendingMark> PendingMarks;
			PendingDrops Drops;

			// The rings of provisioned slots, which are slots 0 to ProvisionedSlots - 1.
			std::array<Scope<TelemetryRecord[]>, Telemetry::MaxThreads> Rings;
			uint32_t ProvisionedSlots = 0;
		};

		Scope<TelemetryData> s_Data;

		bool IsMainThread()
		{
			uint32_t const init = s_ThreadState.MainInit;
			return init != 0 && init == s_InitNumber.load(std::memory_order_relaxed);
		}

		// A name as a warning shows it: quoted when it is a valid name, else described.
		std::string DescribeName(std::string_view name, bool tooLong)
		{
			if (tooLong || name.size() > Telemetry::MaxNameBytes)
			{
				return fmt::format("a name longer than {} bytes", Telemetry::MaxNameBytes);
			}
			if (name.empty())
			{
				return "an empty name";
			}
			if (!IsTelemetryName(name))
			{
				return fmt::format("a name of {} bytes that is not valid UTF-8 or holds control characters", name.size());
			}
			return fmt::format("'{}'", name);
		}

		void NoteDrop(PendingDrops& drops, DropKind kind, bool mark, std::string_view name, bool tooLong = false, uint64_t count = 1)
		{
			(mark ? drops.Marks : drops.Values) += count;
			size_t const index = static_cast<size_t>(kind);
			if (!drops.Seen[index])
			{
				drops.Seen[index] = true;
				drops.Examples[index] = DescribeName(name, tooLong);
			}
		}

		void Warn(TelemetryData const& data, DropKind kind, std::string const& example)
		{
			TelemetrySettings const& settings = data.Settings;
			switch (kind)
			{
				case DropKind::Name:
					ST_CORE_WARN("Telemetry: dropped what was recorded under {}: a channel or a mark is named by 1 to {} bytes of UTF-8 "
					             "without control characters",
					             example, Telemetry::MaxNameBytes);
					break;
				case DropKind::NotFinite:
					ST_CORE_WARN("Telemetry: dropped a value of channel {} that is not a finite number", example);
					break;
				case DropKind::Channels:
					ST_CORE_WARN("Telemetry: dropped the values of channel {}: a session holds at most {} channels", example,
					             settings.MaxChannels);
					break;
				case DropKind::Steps:
					ST_CORE_WARN("Telemetry: a session holds at most {} steps; the values and marks of its further steps are dropped",
					             settings.MaxSteps);
					break;
				case DropKind::Bytes:
					ST_CORE_WARN("Telemetry: dropped what {} recorded past the {} bytes the sessions hold at most", example,
					             settings.HistoryBytes);
					break;
				case DropKind::Marks:
					ST_CORE_WARN("Telemetry: dropped mark {}: a session holds at most {} marks", example, Telemetry::MaxMarks);
					break;
				case DropKind::MarkStep:
					ST_CORE_WARN("Telemetry: dropped mark {}: it names step 0, a step before its session began or one the session "
					             "never reached",
					             example);
					break;
				case DropKind::FullBuffer:
					ST_CORE_WARN("Telemetry: a thread recorded more than its buffer's {} records between two steps; the rest were dropped",
					             settings.RecordsPerThread);
					break;
				case DropKind::NoBuffer:
					ST_CORE_WARN("Telemetry: a thread found no free buffer ({} threads record at most at once); its records were dropped",
					             Telemetry::MaxThreads);
					break;
			}
		}

		TelemetrySession* GetLiveSession(TelemetryData& data)
		{
			return !data.Sessions.empty() && data.Sessions.back().Live ? &data.Sessions.back() : nullptr;
		}

		void PopOldestSession(TelemetryData& data)
		{
			data.BytesHeld -= data.Sessions.front().Bytes;
			data.Sessions.pop_front();
		}

		// Makes room for `bytes` more, dropping the oldest sessions that are not live, and takes them into the session.
		// False when even the live session alone would pass HistoryBytes.
		bool TakeBytes(TelemetryData& data, TelemetrySession& session, uint64_t bytes)
		{
			while (data.BytesHeld + bytes > data.Settings.HistoryBytes && data.Sessions.size() > 1 && !data.Sessions.front().Live)
			{
				PopOldestSession(data);
			}
			if (data.BytesHeld + bytes > data.Settings.HistoryBytes)
			{
				return false;
			}
			data.BytesHeld += bytes;
			session.Bytes += bytes;
			return true;
		}

		uint64_t GetMarkBytes(std::string_view label)
		{
			return sizeof(TelemetryMark) + label.size();
		}

		// --- The threads' buffers. ---

		uint32_t GetRingSize(uint32_t capacity)
		{
			return std::bit_ceil(capacity);
		}

		// Gives a slot a ring and publishes it as free in the generation. Main thread; no producer reaches the slot's
		// fields, since none can claim it before the publication.
		void ProvisionSlot(TelemetryData& data, uint32_t slotIndex)
		{
			uint32_t const ringSize = GetRingSize(data.Settings.RecordsPerThread);
			data.Rings[slotIndex] = std::make_unique<TelemetryRecord[]>(ringSize);
			TelemetrySlot& slot = s_Slots[slotIndex];
			slot.Records = data.Rings[slotIndex].get();
			slot.Mask = ringSize - 1;
			slot.Capacity = data.Settings.RecordsPerThread;
			slot.Head.store(0, std::memory_order_relaxed);
			slot.Tail.store(0, std::memory_order_relaxed);
			slot.DroppedValues.store(0, std::memory_order_relaxed);
			slot.DroppedMarks.store(0, std::memory_order_relaxed);
			slot.Owner.store(PackOwner(data.Generation, SlotState::Free), std::memory_order_release);
		}

		// Keeps SpareSlots free slots ready, up to Telemetry::MaxThreads provisioned ones.
		void ProvisionSpareSlots(TelemetryData& data)
		{
			uint64_t const free = PackOwner(data.Generation, SlotState::Free);
			uint32_t freeSlots = 0;
			for (uint32_t slotIndex = 0; slotIndex < data.ProvisionedSlots; ++slotIndex)
			{
				if (s_Slots[slotIndex].Owner.load(std::memory_order_relaxed) == free)
				{
					++freeSlots;
				}
			}
			while (freeSlots < SpareSlots && data.ProvisionedSlots < Telemetry::MaxThreads)
			{
				ProvisionSlot(data, data.ProvisionedSlots++);
				++freeSlots;
			}
		}

		// Claims a free slot of the generation for the calling thread. Any thread; neither allocates nor locks. A claim
		// that races the generation's end leaves a slot the main thread frees again, and the writer that follows finds
		// the generation changed.
		bool ClaimSlot(TelemetryThreadState& thread, uint32_t generation)
		{
			uint64_t const free = PackOwner(generation, SlotState::Free);
			for (uint32_t slotIndex = 0; slotIndex < Telemetry::MaxThreads; ++slotIndex)
			{
				TelemetrySlot& slot = s_Slots[slotIndex];
				uint64_t expected = free;
				if (slot.Owner.load(std::memory_order_relaxed) != free ||
				    !slot.Owner.compare_exchange_strong(expected, PackOwner(generation, SlotState::Owned), std::memory_order_acquire,
				                                        std::memory_order_relaxed))
				{
					continue;
				}
				thread.Slot = slotIndex;
				thread.SlotGeneration = generation;
				return true;
			}
			return false;
		}

		// Waits until no producer is inside a slot. Called after the generation changed, so producers arriving later
		// leave without touching anything.
		void WaitForWriters()
		{
			for (TelemetrySlot const& slot : s_Slots)
			{
				while (slot.Writers.load(std::memory_order_seq_cst) != 0)
				{
					std::this_thread::yield();
				}
			}
		}

		// Ends the generation in progress and waits for its writers; returns the ended one. Main thread.
		uint32_t AdvanceGeneration(TelemetryData& data)
		{
			uint32_t const ended = data.Generation;
			data.Generation = s_Generation.fetch_add(1, std::memory_order_seq_cst) + 1;
			WaitForWriters();
			return ended;
		}

		// Every provisioned slot free again in the generation in progress, its records discarded. Main thread, after
		// AdvanceGeneration: threads holding a slot of the ended generation claim one again.
		void ResetSlots(TelemetryData& data)
		{
			for (uint32_t slotIndex = 0; slotIndex < data.ProvisionedSlots; ++slotIndex)
			{
				TelemetrySlot& slot = s_Slots[slotIndex];
				slot.Head.store(0, std::memory_order_relaxed);
				slot.Tail.store(0, std::memory_order_relaxed);
				slot.DroppedValues.store(0, std::memory_order_relaxed);
				slot.DroppedMarks.store(0, std::memory_order_relaxed);
				slot.Owner.store(PackOwner(data.Generation, SlotState::Free), std::memory_order_release);
			}
		}

		// Frees every ring and slot. Main thread, after AdvanceGeneration.
		void ReleaseSlots(TelemetryData& data)
		{
			for (uint32_t slotIndex = 0; slotIndex < Telemetry::MaxThreads; ++slotIndex)
			{
				TelemetrySlot& slot = s_Slots[slotIndex];
				slot.Owner.store(PackOwner(0, SlotState::Unprovisioned), std::memory_order_relaxed);
				slot.Records = nullptr;
				slot.Mask = 0;
				slot.Capacity = 0;
				slot.Head.store(0, std::memory_order_relaxed);
				slot.Tail.store(0, std::memory_order_relaxed);
				slot.DroppedValues.store(0, std::memory_order_relaxed);
				slot.DroppedMarks.store(0, std::memory_order_relaxed);
				data.Rings[slotIndex].reset();
			}
			data.ProvisionedSlots = 0;
			s_UnattributedValues.store(0, std::memory_order_relaxed);
			s_UnattributedMarks.store(0, std::memory_order_relaxed);
		}

		// A thread's record, written into its ring. Any thread but the main one.
		void WriteRecord(RecordKind kind, std::string_view text, float value, uint64_t step)
		{
			TelemetryThreadState& thread = s_ThreadState;
			// A second attempt when the generation changed under the first: the record then belongs to the next session.
			for (int attempt = 0; attempt < 2 && Telemetry::IsRecording(); ++attempt)
			{
				uint32_t const generation = s_Generation.load(std::memory_order_acquire);
				if (thread.Slot == NoSlot || thread.SlotGeneration != generation)
				{
					thread.Slot = NoSlot;
					if (!ClaimSlot(thread, generation))
					{
						if (s_Generation.load(std::memory_order_relaxed) == generation)
						{
							(kind == RecordKind::Value ? s_UnattributedValues : s_UnattributedMarks)
								.fetch_add(1, std::memory_order_relaxed);
							return;
						}
						continue;
					}
				}

				TelemetrySlot& slot = s_Slots[thread.Slot];
				slot.Writers.fetch_add(1, std::memory_order_seq_cst);
				bool const current = s_Generation.load(std::memory_order_seq_cst) == generation;
				if (current)
				{
					uint32_t const head = slot.Head.load(std::memory_order_relaxed);
					if (head - slot.Tail.load(std::memory_order_acquire) < slot.Capacity)
					{
						TelemetryRecord& record = slot.Records[head & slot.Mask];
						record.Step = step;
						record.Value = value;
						record.Kind = kind;
						size_t const kept = std::min<size_t>(text.size(), Telemetry::MaxNameBytes);
						record.Length = text.size() > Telemetry::MaxNameBytes ? TooLongName : static_cast<uint8_t>(kept);
						std::memcpy(record.Name.data(), text.data(), kept);
						slot.Head.store(head + 1, std::memory_order_release);
					}
					else
					{
						(kind == RecordKind::Value ? slot.DroppedValues : slot.DroppedMarks).fetch_add(1, std::memory_order_relaxed);
					}
				}
				slot.Writers.fetch_sub(1, std::memory_order_release);
				if (current)
				{
					return;
				}
			}
		}

		// --- The pending row. ---

		void SetPendingValue(TelemetryData& data, std::string_view name, float value, bool tooLong = false)
		{
			if (tooLong)
			{
				NoteDrop(data.Drops, DropKind::Name, false, name, true);
				return;
			}
			if (!std::isfinite(value))
			{
				NoteDrop(data.Drops, DropKind::NotFinite, false, name);
				return;
			}
			uint32_t index = 0;
			auto const found = data.PendingIndices.find(name);
			if (found != data.PendingIndices.end())
			{
				index = found->second;
			}
			else
			{
				if (!IsTelemetryName(name))
				{
					NoteDrop(data.Drops, DropKind::Name, false, name);
					return;
				}
				if (data.PendingCells.size() >= data.Settings.MaxChannels)
				{
					NoteDrop(data.Drops, DropKind::Channels, false, name);
					return;
				}
				index = static_cast<uint32_t>(data.PendingCells.size());
				data.PendingCells.push_back({std::string(name)});
				data.PendingIndices.emplace(std::string(name), index);
			}
			PendingCell& cell = data.PendingCells[index];
			cell.Value = value;
			if (cell.Stamp != data.PendingStamp)
			{
				cell.Stamp = data.PendingStamp;
				data.PendingTouched.push_back(index);
			}
		}

		// Puts a mark on a held row of the session, after the marks the row already has.
		void InsertMark(TelemetryData& data, TelemetrySession& session, uint64_t row, std::string_view label)
		{
			if (session.Marks.size() + data.Live.FutureMarks.size() >= Telemetry::MaxMarks)
			{
				NoteDrop(data.Drops, DropKind::Marks, true, label);
				return;
			}
			if (!TakeBytes(data, session, GetMarkBytes(label)))
			{
				NoteDrop(data.Drops, DropKind::Bytes, true, label);
				return;
			}
			auto const position = std::upper_bound(session.Marks.begin(), session.Marks.end(), row,
			                                       [](uint64_t markRow, TelemetryMark const& mark)
			                                       {
													   return markRow < mark.Row;
												   });
			session.Marks.insert(position, TelemetryMark{row, std::string(label)});
		}

		void AddPendingMark(TelemetryData& data, std::string_view label, uint64_t step, bool tooLong = false)
		{
			if (tooLong || !IsTelemetryName(label))
			{
				NoteDrop(data.Drops, DropKind::Name, true, label, tooLong);
				return;
			}
			if (step == RefusedMarkStep)
			{
				NoteDrop(data.Drops, DropKind::MarkStep, true, label);
				return;
			}
			if (data.PendingMarks.size() >= Telemetry::MaxMarks)
			{
				NoteDrop(data.Drops, DropKind::Marks, true, label);
				return;
			}
			data.PendingMarks.push_back({std::string(label), step});
		}

		// A main thread's mark: a step the live session holds is marked at once, anything else waits for the next row.
		void AddMainMark(TelemetryData& data, std::string_view label, uint64_t step)
		{
			TelemetrySession* const live = GetLiveSession(data);
			if (live != nullptr && step != 0 && step != RefusedMarkStep && step >= live->FirstStep && step - live->FirstStep < live->Rows &&
			    IsTelemetryName(label))
			{
				InsertMark(data, *live, step - live->FirstStep, label);
				// Its drops (marks or bytes past the bounds) are counted and logged when the next row is collected.
				return;
			}
			AddPendingMark(data, label, step);
		}

		// Every record of the rings of a generation folded into the pending row, in each ring's order and the slots' order,
		// and the slots of ended threads freed. Main thread.
		void DrainSlots(TelemetryData& data, uint32_t generation)
		{
			for (uint32_t slotIndex = 0; slotIndex < data.ProvisionedSlots; ++slotIndex)
			{
				TelemetrySlot& slot = s_Slots[slotIndex];
				uint64_t const owner = slot.Owner.load(std::memory_order_acquire);
				SlotState const state = GetOwnerState(owner);
				if (GetOwnerGeneration(owner) != generation || (state != SlotState::Owned && state != SlotState::Ending))
				{
					continue;
				}
				uint32_t const tail = slot.Tail.load(std::memory_order_relaxed);
				uint32_t const head = slot.Head.load(std::memory_order_acquire);
				for (uint32_t position = tail; position != head; ++position)
				{
					TelemetryRecord const& record = slot.Records[position & slot.Mask];
					bool const tooLong = record.Length == TooLongName;
					std::string_view const name(record.Name.data(), tooLong ? Telemetry::MaxNameBytes : record.Length);
					if (record.Kind == RecordKind::Value)
					{
						SetPendingValue(data, name, record.Value, tooLong);
					}
					else
					{
						AddPendingMark(data, name, record.Step, tooLong);
					}
				}
				slot.Tail.store(head, std::memory_order_release);
				uint32_t const droppedValues = slot.DroppedValues.exchange(0, std::memory_order_relaxed);
				uint32_t const droppedMarks = slot.DroppedMarks.exchange(0, std::memory_order_relaxed);
				if (droppedValues > 0)
				{
					NoteDrop(data.Drops, DropKind::FullBuffer, false, {}, false, droppedValues);
				}
				if (droppedMarks > 0)
				{
					NoteDrop(data.Drops, DropKind::FullBuffer, true, {}, false, droppedMarks);
				}
				if (state == SlotState::Ending)
				{
					// The thread called EndThread after its last record: nothing writes the slot until it is claimed again.
					slot.Owner.store(PackOwner(generation, SlotState::Free), std::memory_order_release);
				}
			}
			uint32_t const values = s_UnattributedValues.exchange(0, std::memory_order_relaxed);
			uint32_t const marks = s_UnattributedMarks.exchange(0, std::memory_order_relaxed);
			if (values > 0)
			{
				NoteDrop(data.Drops, DropKind::NoBuffer, false, {}, false, values);
			}
			if (marks > 0)
			{
				NoteDrop(data.Drops, DropKind::NoBuffer, true, {}, false, marks);
			}
		}

		// The pending row emptied of values, marks and drops; the channel names stay (keepNames) or go.
		void ClearPending(TelemetryData& data, bool keepNames)
		{
			if (!keepNames)
			{
				data.PendingIndices.clear();
				data.PendingCells.clear();
			}
			data.PendingTouched.clear();
			data.PendingMarks.clear();
			data.Drops = {};
			++data.PendingStamp;
		}

		// At a session's beginning in EndStep: the pending row's cells stay, the names of the ended session go, so that
		// the names held never outgrow what one session may hold.
		void KeepTouchedCells(TelemetryData& data)
		{
			std::vector<PendingCell> kept;
			kept.reserve(data.PendingTouched.size());
			data.PendingIndices.clear();
			for (uint32_t const index : data.PendingTouched)
			{
				kept.push_back(std::move(data.PendingCells[index]));
				data.PendingIndices.emplace(kept.back().Name, static_cast<uint32_t>(kept.size() - 1));
			}
			data.PendingCells = std::move(kept);
			data.PendingTouched.clear();
			for (uint32_t index = 0; index < data.PendingCells.size(); ++index)
			{
				data.PendingTouched.push_back(index);
			}
		}

		// At a session's end: marks of the pending row that name a step are dropped and counted (the session never reaches
		// their steps, or they were refused); what else the row holds is discarded.
		void DropPendingStepMarks(TelemetryData& data)
		{
			for (PendingMark const& mark : data.PendingMarks)
			{
				if (mark.Step != 0)
				{
					NoteDrop(data.Drops, DropKind::MarkStep, true, mark.Label);
				}
			}
		}

		// Counts and logs what the pending row lost in the session that collects it.
		void FlushDrops(TelemetryData& data, TelemetrySession& session)
		{
			PendingDrops& drops = data.Drops;
			session.DroppedValues += drops.Values;
			session.DroppedMarks += drops.Marks;
			data.DroppedValues += drops.Values;
			data.DroppedMarks += drops.Marks;
			for (size_t kind = 0; kind < DropKinds; ++kind)
			{
				if (drops.Seen[kind] && !data.Live.Warned[kind])
				{
					data.Live.Warned[kind] = true;
					Warn(data, static_cast<DropKind>(kind), drops.Examples[kind]);
				}
			}
			drops = {};
		}

		// --- Sessions. ---

		void EndLiveSession(TelemetryData& data)
		{
			TelemetrySession* const session = GetLiveSession(data);
			if (session == nullptr)
			{
				return;
			}
			LiveState& live = data.Live;
			if (!live.FutureMarks.empty())
			{
				session->DroppedMarks += live.FutureMarks.size();
				data.DroppedMarks += live.FutureMarks.size();
				if (!live.Warned[static_cast<size_t>(DropKind::MarkStep)])
				{
					Warn(data, DropKind::MarkStep, DescribeName(live.FutureMarks.front().Label, false));
				}
			}
			for (std::vector<float> const& spare : live.Spares)
			{
				if (spare.capacity() != 0)
				{
					session->Bytes -= BlockBytes;
					data.BytesHeld -= BlockBytes;
				}
			}
			session->Live = false;
			data.Live = {};
		}

		void BeginSession(TelemetryData& data, TelemetryStep const& step)
		{
			while (!data.Sessions.empty() && data.Sessions.size() >= data.Settings.HistorySessions)
			{
				PopOldestSession(data);
			}
			TelemetrySession session;
			session.Id = data.NextSessionId++;
			session.Source = step.Source;
			session.FirstStep = step.Step;
			session.StepSeconds = step.StepSeconds;
			session.Live = true;
			data.Sessions.push_back(std::move(session));
			data.Live = {};
			data.Live.NextStep = step.Step;
		}

		// The cell of the pending row in the live session's channel of its name, making the channel when it is new. False
		// when the session holds MaxChannels channels already.
		bool FindChannel(TelemetryData& data, TelemetrySession& session, PendingCell& cell)
		{
			if (cell.Session == session.Id && cell.Channel != NoChannel)
			{
				return true;
			}
			uint32_t channelIndex = 0;
			auto const found = data.Live.ChannelIndices.find(cell.Name);
			if (found != data.Live.ChannelIndices.end())
			{
				channelIndex = found->second;
			}
			else
			{
				if (session.Channels.size() >= data.Settings.MaxChannels)
				{
					return false;
				}
				channelIndex = static_cast<uint32_t>(session.Channels.size());
				TelemetryChannel channel;
				channel.Name = cell.Name;
				session.Channels.push_back(std::move(channel));
				data.Live.Spares.emplace_back();
				data.Live.ChannelIndices.emplace(cell.Name, channelIndex);
			}
			cell.Session = session.Id;
			cell.Channel = channelIndex;
			return true;
		}

		// The cell of the block a channel of index channelIndex makes ahead of need: from half its last block on, at a row
		// that differs between channels, so that channels recorded every step make their blocks in different steps.
		uint64_t GetSpareCell(uint32_t channelIndex)
		{
			constexpr uint32_t Spread = 32;
			return TelemetryChannel::BlockCells / 2 + (channelIndex % Spread) * (TelemetryChannel::BlockCells / (2 * Spread));
		}

		void WriteValue(TelemetryData& data, TelemetrySession& session, PendingCell& cell, uint64_t row)
		{
			if (!FindChannel(data, session, cell))
			{
				NoteDrop(data.Drops, DropKind::Channels, false, cell.Name);
				return;
			}
			TelemetryChannel& channel = session.Channels[cell.Channel];
			std::vector<float>& spare = data.Live.Spares[cell.Channel];
			if (channel.Blocks.empty())
			{
				channel.FirstRow = row;
			}
			uint64_t const index = row - channel.FirstRow;
			uint64_t const block = index / TelemetryChannel::BlockCells;
			while (channel.Blocks.size() <= block)
			{
				// A block is made with room for all its cells and filled as rows come, so it never moves and costs no fill
				// when it is made; one left behind is filled to its end, its last rows having no value.
				if (!channel.Blocks.empty())
				{
					channel.Blocks.back().resize(TelemetryChannel::BlockCells, NoValue);
				}
				if (spare.capacity() != 0)
				{
					channel.Blocks.push_back(std::move(spare));
					spare = std::vector<float>();
					continue;
				}
				if (!TakeBytes(data, session, BlockBytes))
				{
					NoteDrop(data.Drops, DropKind::Bytes, false, cell.Name);
					return;
				}
				channel.Blocks.emplace_back().reserve(TelemetryChannel::BlockCells);
			}
			uint64_t const cellInBlock = index % TelemetryChannel::BlockCells;
			std::vector<float>& cells = channel.Blocks[block];
			ST_CORE_ASSERT(cells.size() <= cellInBlock, "A column's rows only grow");
			// Rows between the column's last value and this one have none.
			cells.resize(static_cast<size_t>(cellInBlock), NoValue);
			cells.push_back(cell.Value);
			channel.Cells = index + 1;
			if (channel.Count == 0)
			{
				channel.Minimum = cell.Value;
				channel.Maximum = cell.Value;
			}
			else
			{
				channel.Minimum = std::min(channel.Minimum, cell.Value);
				channel.Maximum = std::max(channel.Maximum, cell.Value);
			}
			channel.Last = cell.Value;
			++channel.Count;
			if (spare.capacity() == 0 && block + 1 == channel.Blocks.size() && cellInBlock >= GetSpareCell(cell.Channel) &&
			    TakeBytes(data, session, BlockBytes))
			{
				spare.reserve(TelemetryChannel::BlockCells);
			}
		}

		// A mark of the pending row placed by its step: on the row being committed (`row`, of step `step`), on an earlier
		// row the session holds, held for a later step, or dropped.
		void PlaceMark(TelemetryData& data, TelemetrySession& session, PendingMark& mark, uint64_t step, uint64_t row)
		{
			if (mark.Step == 0 || mark.Step == step)
			{
				InsertMark(data, session, row, mark.Label);
			}
			else if (mark.Step < session.FirstStep)
			{
				NoteDrop(data.Drops, DropKind::MarkStep, true, mark.Label);
			}
			else if (mark.Step < step)
			{
				InsertMark(data, session, mark.Step - session.FirstStep, mark.Label);
			}
			else if (session.Marks.size() + data.Live.FutureMarks.size() >= Telemetry::MaxMarks)
			{
				NoteDrop(data.Drops, DropKind::Marks, true, mark.Label);
			}
			else
			{
				data.Live.FutureMarks.push_back(std::move(mark));
			}
		}

		void CommitRow(TelemetryData& data, TelemetryStep const& step)
		{
			TelemetrySession& session = data.Sessions.back();
			LiveState& live = data.Live;
			if (session.Rows >= data.Settings.MaxSteps)
			{
				++session.DroppedSteps;
				++data.DroppedSteps;
				NoteDrop(data.Drops, DropKind::Steps, false, {}, false, data.PendingTouched.size());
				data.Drops.Marks += data.PendingMarks.size();
			}
			else
			{
				uint64_t const row = session.Rows++;
				// Marks waiting for this step were collected before this row's own.
				if (!live.FutureMarks.empty())
				{
					std::vector<PendingMark> later;
					for (PendingMark& mark : live.FutureMarks)
					{
						if (mark.Step == step.Step)
						{
							InsertMark(data, session, row, mark.Label);
						}
						else if (mark.Step > step.Step)
						{
							later.push_back(std::move(mark));
						}
						else
						{
							// Its step passed while the session held no more rows (MaxSteps).
							NoteDrop(data.Drops, DropKind::MarkStep, true, mark.Label);
						}
					}
					live.FutureMarks = std::move(later);
				}
				for (PendingMark& mark : data.PendingMarks)
				{
					PlaceMark(data, session, mark, step.Step, row);
				}
				for (uint32_t const index : data.PendingTouched)
				{
					WriteValue(data, session, data.PendingCells[index], row);
				}
			}
			FlushDrops(data, session);
			data.PendingTouched.clear();
			data.PendingMarks.clear();
			++data.PendingStamp;
		}

		Result<void> CheckSettings(TelemetrySettings const& settings)
		{
			if (settings.MaxChannels < Telemetry::MinChannels || settings.MaxChannels > Telemetry::MaxChannelsLimit)
			{
				return MakeError("MaxChannels must be between {} and {}, not {}", Telemetry::MinChannels, Telemetry::MaxChannelsLimit,
				                 settings.MaxChannels);
			}
			if (settings.MaxSteps < Telemetry::MinSteps || settings.MaxSteps > Telemetry::MaxStepsLimit)
			{
				return MakeError("MaxSteps must be between {} and {}, not {}", Telemetry::MinSteps, Telemetry::MaxStepsLimit,
				                 settings.MaxSteps);
			}
			if (settings.HistorySessions < Telemetry::MinHistorySessions || settings.HistorySessions > Telemetry::MaxHistorySessions)
			{
				return MakeError("HistorySessions must be between {} and {}, not {}", Telemetry::MinHistorySessions,
				                 Telemetry::MaxHistorySessions, settings.HistorySessions);
			}
			if (settings.HistoryBytes < Telemetry::MinHistoryBytes || settings.HistoryBytes > Telemetry::MaxHistoryBytes)
			{
				return MakeError("HistoryBytes must be between {} and {}, not {}", Telemetry::MinHistoryBytes, Telemetry::MaxHistoryBytes,
				                 settings.HistoryBytes);
			}
			if (settings.RecordsPerThread < Telemetry::MinRecordsPerThread || settings.RecordsPerThread > Telemetry::MaxRecordsPerThread)
			{
				return MakeError("RecordsPerThread must be between {} and {}, not {}", Telemetry::MinRecordsPerThread,
				                 Telemetry::MaxRecordsPerThread, settings.RecordsPerThread);
			}
			return {};
		}
	}

	double GetTelemetryTime(TelemetrySession const& session, uint64_t row)
	{
		return static_cast<double>(session.FirstStep + row) * static_cast<double>(session.StepSeconds);
	}

	TelemetryChannel const* FindTelemetryChannel(TelemetrySession const& session, std::string_view name)
	{
		auto const found = std::find_if(session.Channels.begin(), session.Channels.end(),
		                                [name](TelemetryChannel const& channel)
		                                {
											return channel.Name == name;
										});
		return found != session.Channels.end() ? &*found : nullptr;
	}

	std::span<float const> GetTelemetryCells(TelemetryChannel const& channel, uint64_t row)
	{
		if (row < channel.FirstRow || row - channel.FirstRow >= channel.Cells)
		{
			return {};
		}
		uint64_t const index = row - channel.FirstRow;
		uint64_t const block = index / TelemetryChannel::BlockCells;
		uint64_t const cellInBlock = index % TelemetryChannel::BlockCells;
		if (block >= channel.Blocks.size())
		{
			return {};
		}
		std::vector<float> const& cells = channel.Blocks[block];
		uint64_t const blockStart = block * TelemetryChannel::BlockCells;
		uint64_t const end = std::min<uint64_t>(cells.size(), channel.Cells - blockStart);
		if (cellInBlock >= end)
		{
			return {};
		}
		return std::span<float const>(cells.data() + cellInBlock, static_cast<size_t>(end - cellInBlock));
	}

	std::optional<float> GetTelemetryValue(TelemetryChannel const& channel, uint64_t row)
	{
		std::span<float const> const cells = GetTelemetryCells(channel, row);
		if (cells.empty() || std::isnan(cells.front()))
		{
			return std::nullopt;
		}
		return cells.front();
	}

	std::optional<uint32_t> ParseTelemetryLapNumber(std::string_view label)
	{
		constexpr std::string_view Prefix = "Lap ";
		if (!label.starts_with(Prefix))
		{
			return std::nullopt;
		}
		std::string_view const digits = label.substr(Prefix.size());
		if (digits.empty() || digits.front() == '0' ||
		    !std::all_of(digits.begin(), digits.end(),
		                 [](char character)
		                 {
							 return character >= '0' && character <= '9';
						 }))
		{
			return std::nullopt;
		}
		uint32_t number = 0;
		auto const [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), number);
		if (error != std::errc() || end != digits.data() + digits.size())
		{
			return std::nullopt;
		}
		return number;
	}

	namespace
	{
		// Walks a session's lap boundaries: calls onLap(begin, end, number) for each lap, and returns the mark that begins
		// the lap in progress, if any.
		template<typename TOnLap>
		TelemetryMark const* WalkLaps(TelemetrySession const& session, TOnLap&& onLap)
		{
			TelemetryMark const* open = nullptr;
			uint32_t openNumber = 0;
			for (TelemetryMark const& mark : session.Marks)
			{
				std::optional<uint32_t> const number = ParseTelemetryLapNumber(mark.Label);
				if (!number && mark.Label != TelemetryLapEndLabel)
				{
					continue;
				}
				if (open != nullptr && number && uint64_t{*number} == uint64_t{openNumber} + 1 && mark.Row > open->Row)
				{
					onLap(*open, mark.Row, openNumber);
				}
				open = number ? &mark : nullptr;
				openNumber = number.value_or(0);
			}
			return open;
		}

		TelemetryLap MakeLap(TelemetrySession const& session, TelemetryMark const& begin, uint64_t endRow, uint32_t number)
		{
			TelemetryLap lap;
			lap.Label = begin.Label;
			lap.Number = number;
			lap.StartRow = begin.Row;
			lap.EndRow = endRow;
			lap.Seconds = static_cast<double>(endRow - begin.Row) * static_cast<double>(session.StepSeconds);
			return lap;
		}
	}

	std::vector<TelemetryLap> FindTelemetryLaps(TelemetrySession const& session)
	{
		std::vector<TelemetryLap> laps;
		WalkLaps(session,
		         [&laps, &session](TelemetryMark const& begin, uint64_t endRow, uint32_t number)
		         {
					 laps.push_back(MakeLap(session, begin, endRow, number));
				 });
		return laps;
	}

	std::optional<TelemetryLap> FindTelemetryLapInProgress(TelemetrySession const& session)
	{
		TelemetryMark const* const open = WalkLaps(session, [](TelemetryMark const&, uint64_t, uint32_t) {});
		if (open == nullptr || session.Rows == 0)
		{
			return std::nullopt;
		}
		return MakeLap(session, *open, std::max(open->Row, session.Rows - 1), ParseTelemetryLapNumber(open->Label).value_or(0));
	}

	std::optional<size_t> FindBestTelemetryLap(std::span<TelemetryLap const> laps)
	{
		std::optional<size_t> best;
		for (size_t index = 0; index < laps.size(); ++index)
		{
			uint64_t const rows = laps[index].EndRow - laps[index].StartRow;
			if (!best || rows < laps[*best].EndRow - laps[*best].StartRow)
			{
				best = index;
			}
		}
		return best;
	}

	bool IsTelemetryName(std::string_view text)
	{
		if (text.empty() || text.size() > Telemetry::MaxNameBytes)
		{
			return false;
		}
		bool const hasControl = std::any_of(text.begin(), text.end(),
		                                    [](char character)
		                                    {
												auto const byte = static_cast<unsigned char>(character);
												return byte < 0x20 || byte == 0x7F;
											});
		return !hasControl && Utf8::IsValid(text);
	}

	void Telemetry::Init()
	{
		ST_CORE_ASSERT(!s_Data, "Telemetry is already initialized");
		s_Data = CreateScope<TelemetryData>();
		s_ThreadState.MainInit = s_InitNumber.fetch_add(1, std::memory_order_relaxed) + 1;
	}

	void Telemetry::Shutdown()
	{
		if (!s_Data)
		{
			return;
		}
		Stop();
		s_Data.reset();
	}

	bool Telemetry::IsInitialized()
	{
		return static_cast<bool>(s_Data);
	}

	Result<void> Telemetry::Start(TelemetrySettings const& settings)
	{
		if (!s_Data)
		{
			return Error{"Telemetry is not initialized"};
		}
		TelemetryData& data = *s_Data;
		if (data.Recording)
		{
			return Error{"A telemetry recording is already in progress"};
		}
		if (Result<void> checked = CheckSettings(settings); !checked)
		{
			return checked;
		}

		data.Settings = settings;
		while (!data.Sessions.empty() && (data.Sessions.size() > settings.HistorySessions || data.BytesHeld > settings.HistoryBytes))
		{
			PopOldestSession(data);
		}
		data.DroppedValues = 0;
		data.DroppedMarks = 0;
		data.DroppedSteps = 0;
		data.Live = {};
		ClearPending(data, false);

		// Only the main thread advances the generation, so it can read and publish it in two steps.
		data.Generation = s_Generation.load(std::memory_order_relaxed) + 1;
		for (uint32_t slotIndex = 0; slotIndex < SpareSlots; ++slotIndex)
		{
			ProvisionSlot(data, slotIndex);
		}
		data.ProvisionedSlots = SpareSlots;
		s_UnattributedValues.store(0, std::memory_order_relaxed);
		s_UnattributedMarks.store(0, std::memory_order_relaxed);
		s_Generation.store(data.Generation, std::memory_order_seq_cst);
		s_Recording.store(true, std::memory_order_seq_cst);
		data.Recording = true;
		return {};
	}

	void Telemetry::Stop()
	{
		if (!s_Data || !s_Data->Recording)
		{
			return;
		}
		TelemetryData& data = *s_Data;
		s_Recording.store(false, std::memory_order_seq_cst);
		AdvanceGeneration(data);
		ReleaseSlots(data);
		// What was dropped since the last row is counted; what was recorded and not collected is discarded.
		if (TelemetrySession* const live = GetLiveSession(data); live != nullptr)
		{
			DropPendingStepMarks(data);
			FlushDrops(data, *live);
		}
		ClearPending(data, false);
		EndLiveSession(data);
		data.Recording = false;
	}

	void Telemetry::EndStep(TelemetryStep const& step)
	{
		if (!s_Data || !s_Data->Recording)
		{
			return;
		}
		bool const valid = step.Source != 0 && step.Step != 0 && std::isfinite(step.StepSeconds) && step.StepSeconds > 0.0f;
		ST_CORE_ASSERT(valid, "A fixed step has a source, a number from 1 and a positive length");
		if (!valid)
		{
			return;
		}
		ST_PROFILE_SCOPE("Telemetry.Collect");
		TelemetryData& data = *s_Data;
		TelemetrySession const* const live = GetLiveSession(data);
		bool const begins =
			live == nullptr || live->Source != step.Source || data.Live.NextStep != step.Step || live->StepSeconds != step.StepSeconds;
		if (begins)
		{
			// What the threads wrote so far belongs to this row, the new session's first; then every buffer is freed for the
			// new session's threads, so that buffers of threads that ended without EndThread serve again.
			uint32_t const ended = AdvanceGeneration(data);
			DrainSlots(data, ended);
			ResetSlots(data);
			EndLiveSession(data);
			BeginSession(data, step);
			KeepTouchedCells(data);
		}
		else
		{
			DrainSlots(data, data.Generation);
		}
		CommitRow(data, step);
		data.Live.NextStep = step.Step + 1;
		ProvisionSpareSlots(data);
	}

	void Telemetry::EndSession()
	{
		if (!s_Data || !s_Data->Recording || GetLiveSession(*s_Data) == nullptr)
		{
			return;
		}
		TelemetryData& data = *s_Data;
		AdvanceGeneration(data);
		ResetSlots(data);
		s_UnattributedValues.store(0, std::memory_order_relaxed);
		s_UnattributedMarks.store(0, std::memory_order_relaxed);
		DropPendingStepMarks(data);
		FlushDrops(data, *GetLiveSession(data));
		ClearPending(data, false);
		EndLiveSession(data);
	}

	std::deque<TelemetrySession> const& Telemetry::GetSessions()
	{
		static std::deque<TelemetrySession> const s_Empty;
		return s_Data ? s_Data->Sessions : s_Empty;
	}

	TelemetrySession const* Telemetry::FindSession(uint32_t id)
	{
		if (!s_Data)
		{
			return nullptr;
		}
		auto const found = std::find_if(s_Data->Sessions.begin(), s_Data->Sessions.end(),
		                                [id](TelemetrySession const& session)
		                                {
											return session.Id == id;
										});
		return found != s_Data->Sessions.end() ? &*found : nullptr;
	}

	TelemetryStatus Telemetry::GetStatus()
	{
		TelemetryStatus status;
		if (!s_Data)
		{
			return status;
		}
		TelemetryData const& data = *s_Data;
		status.Initialized = true;
		status.Recording = data.Recording;
		status.Settings = data.Settings;
		status.SessionsHeld = static_cast<uint32_t>(data.Sessions.size());
		status.BytesHeld = data.BytesHeld;
		status.LiveSession = !data.Sessions.empty() && data.Sessions.back().Live ? data.Sessions.back().Id : 0;
		status.DroppedValues = data.DroppedValues;
		status.DroppedMarks = data.DroppedMarks;
		status.DroppedSteps = data.DroppedSteps;
		status.Buffers = data.ProvisionedSlots;
		return status;
	}

	void Telemetry::RecordValue(std::string_view channel, float value)
	{
		if (IsMainThread())
		{
			if (s_Data && s_Data->Recording)
			{
				SetPendingValue(*s_Data, channel, value, channel.size() > MaxNameBytes);
			}
			return;
		}
		WriteRecord(RecordKind::Value, channel, value, 0);
	}

	void Telemetry::RecordMark(std::string_view label, uint64_t step)
	{
		static_assert(InvalidMarkStep == RefusedMarkStep);
		if (IsMainThread())
		{
			if (s_Data && s_Data->Recording)
			{
				AddMainMark(*s_Data, label, step);
			}
			return;
		}
		WriteRecord(RecordKind::Mark, label, 0.0f, step);
	}

	void Telemetry::EndThread()
	{
		TelemetryThreadState& thread = s_ThreadState;
		if (thread.Slot != NoSlot)
		{
			// Fails harmlessly when the session that gave the slot has ended.
			uint64_t expected = PackOwner(thread.SlotGeneration, SlotState::Owned);
			s_Slots[thread.Slot].Owner.compare_exchange_strong(expected, PackOwner(thread.SlotGeneration, SlotState::Ending),
			                                                   std::memory_order_release, std::memory_order_relaxed);
		}
		thread.Slot = NoSlot;
		thread.SlotGeneration = 0;
	}
}
