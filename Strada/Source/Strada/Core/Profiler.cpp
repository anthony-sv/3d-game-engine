#include "stpch.h"
#include "Strada/Core/Profiler.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Strada
{
	constinit std::atomic<bool> Profiler::s_Recording{false};

	namespace
	{
		constexpr uint32_t NoSlot = std::numeric_limits<uint32_t>::max();
		constexpr uint32_t NoEntry = std::numeric_limits<uint32_t>::max();
		// Free buffers kept ready for threads that have not recorded yet; EndFrame tops them up.
		constexpr uint32_t SpareSlots = 4;
		constexpr size_t CacheLineBytes = 64;

		// The life of a buffer slot within one recording. A slot is free once the main thread has given it a ring, owned
		// once a thread has claimed it, and ending once its thread has called EndThread, until the main thread collects
		// it and frees it again.
		enum class SlotState : uint32_t
		{
			Unprovisioned = 0,
			Free,
			Claiming,
			Owned,
			Ending
		};

		// A slot's owner word: the recording the state belongs to (high half) and the state, so that a thread holding a
		// slot of an ended recording can never take or release a slot of a later one.
		constexpr uint64_t PackOwner(uint32_t recording, SlotState state)
		{
			return (static_cast<uint64_t>(recording) << 32) | static_cast<uint32_t>(state);
		}

		constexpr uint32_t GetOwnerRecording(uint64_t owner)
		{
			return static_cast<uint32_t>(owner >> 32);
		}
		constexpr SlotState GetOwnerState(uint64_t owner)
		{
			return static_cast<SlotState>(static_cast<uint32_t>(owner));
		}

		// A zone as its thread writes it when it ends.
		struct ZoneRecord
		{
			char const* Name = nullptr;
			char const* ValueLabel = nullptr;
			uint64_t Value = 0;
			int64_t StartNanoseconds = 0;
			int64_t EndNanoseconds = 0;
			uint32_t Depth = 0;
		};

		// A buffer slot: the header of one thread's single-producer, single-consumer ring. The headers have static storage
		// and are never freed, so a thread that races Stop or Shutdown only ever touches memory that lives on. A producer
		// counts itself in Writers before it reads or writes anything else of a slot, then checks the recording number;
		// Stop changes the number and then waits for every slot's Writers to reach zero (a Dekker pair: both sides are
		// sequentially consistent), so once Stop returns no producer reads a ring or writes a slot of the ended recording.
		struct ProfilerSlotFields
		{
			std::atomic<uint64_t> Owner{0};
			std::atomic<uint32_t> Writers{0};
			// Records written by the producer (release), read by the consumer (acquire).
			std::atomic<uint32_t> Head{0};
			// Zones the producer dropped because the ring was full; the consumer takes them each collection.
			std::atomic<uint32_t> Dropped{0};
			// Written by the main thread before it publishes the slot as free, read by the thread that claims it.
			ZoneRecord* Records = nullptr;
			uint32_t Mask = 0;
			uint32_t Capacity = 0;
			// Written by the claiming thread before it publishes the slot as owned, read by the main thread after.
			bool IsMain = false;
			uint32_t NameLength = 0;
			std::array<char, Profiler::MaxThreadNameBytes + 1> Name{};
		};

		constexpr size_t GetPaddingToCacheLine(size_t bytes)
		{
			return (bytes + CacheLineBytes - 1) / CacheLineBytes * CacheLineBytes - bytes;
		}

		// The fields padded to whole cache lines, then the consumer's index on a cache line of its own, so that the main
		// thread's writes never share a line with a producer's. The slot table is aligned to a cache line.
		struct ProfilerSlot : ProfilerSlotFields
		{
			std::array<std::byte, GetPaddingToCacheLine(sizeof(ProfilerSlotFields))> FieldsPadding{};
			// Records read by the consumer.
			std::atomic<uint32_t> Tail{0};
			std::array<std::byte, CacheLineBytes - sizeof(std::atomic<uint32_t>)> TailPadding{};
		};

		static_assert(sizeof(ProfilerSlot) % CacheLineBytes == 0);

		// What a thread remembers between its zones: trivially destructible and constant-initialized, so that reaching it
		// never registers a destructor or runs an initializer (see the class comment of Profiler on Apple platforms).
		struct ThreadState
		{
			uint32_t Slot = NoSlot;
			uint32_t SlotRecording = 0;
			// Zones of DepthRecording open on the thread.
			uint32_t Depth = 0;
			uint32_t DepthRecording = 0;
			// The Init that made this thread the main thread (compared with s_InitNumber).
			uint32_t MainInit = 0;
			uint32_t NameLength = 0;
			std::array<char, Profiler::MaxThreadNameBytes + 1> Name{};
		};

		static_assert(std::is_trivially_destructible_v<ThreadState>, "registering a thread-local destructor may allocate");

		alignas(CacheLineBytes) constinit ProfilerSlot s_Slots[Profiler::MaxThreads];
		// The recording in progress, or a number no recording has while none is: Start and Stop each advance it.
		constinit std::atomic<uint32_t> s_RecordingNumber{0};
		constinit std::atomic<uint32_t> s_InitNumber{0};
		// Zones dropped without a buffer to count them in: their thread found no free slot, or their name was not stored.
		constinit std::atomic<uint32_t> s_UnattributedDrops{0};
		constinit thread_local ThreadState s_ThreadState;

		// The longest prefix of text at most maxBytes long that ends at a UTF-8 character boundary.
		std::string_view CutAtCharacter(std::string_view text, size_t maxBytes)
		{
			if (text.size() <= maxBytes)
			{
				return text;
			}
			size_t length = maxBytes;
			while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0u) == 0x80u)
			{
				--length;
			}
			return text.substr(0, length);
		}

		struct StringHash
		{
			using is_transparent = void;

			size_t operator()(std::string_view text) const { return std::hash<std::string_view>{}(text); }
		};

		using InternedNameSet = std::unordered_set<std::string, StringHash, std::equal_to<>>;

		// The run-time names (InternName), which any thread but the audio callback may store. Nodes of an unordered set
		// never move, so the strings' characters stay where they are until Shutdown.
		std::mutex s_InternedNamesMutex;
		Scope<InternedNameSet> s_InternedNames;

		// A run of one thread's zones within a frame's collection.
		struct ZoneGroup
		{
			uint32_t Entry = 0;
			size_t Begin = 0;
			size_t End = 0;
		};

		// The main thread's state: the recording, the frames and the history.
		struct ProfilerData
		{
			ProfilerSettings Settings;
			bool Recording = false;
			uint32_t RecordingNumber = 0;
			// The clock when the recording started: recorded times count from it.
			int64_t Origin = 0;

			uint64_t FrameIndex = 0;
			bool FrameInProgress = false;
			// Whether the frame in progress belongs to the recording.
			bool FrameRecorded = false;
			int64_t FrameStart = 0;
			// Whether a frame of the recording has begun (the first one discards what ended before it).
			bool RecordingHasFrame = false;

			ProfilerCapture History;
			uint64_t ZonesHeld = 0;
			uint64_t DroppedZones = 0;

			// The rings of provisioned slots, and the thread entry bound to each slot's current owner.
			std::array<Scope<ZoneRecord[]>, Profiler::MaxThreads> Rings;
			std::array<uint32_t, Profiler::MaxThreads> SlotEntries{};
			// Whether each thread entry is bound to a slot's owner (entries of ended threads can be bound again).
			std::vector<bool> EntryBound;

			std::unordered_map<char const*, uint32_t> NameIndicesByPointer;
			std::unordered_map<std::string, uint32_t, StringHash, std::equal_to<>> NameIndicesByText;
			// Name indices of the views that reported GPU passes.
			std::vector<uint32_t> Views;
			// GPU passes of the recorded frame in progress, attached at its end.
			std::vector<ProfilerGpuPass> PendingGpuPasses;

			// Scratch storage of the collection, kept to avoid allocating every frame.
			std::vector<ZoneRecord> Drained;
			std::vector<ZoneRecord> Ordered;
			std::vector<uint32_t> SubtreeSizes;
			std::vector<uint32_t> Parents;
			std::vector<uint32_t> Ancestors;
			std::vector<uint32_t> Stack;
			std::vector<ProfilerZone> Collected;
			std::vector<ZoneGroup> Groups;
			std::vector<uint32_t> EntriesToRelease;
		};

		Scope<ProfilerData> s_Data;

		uint32_t GetRingSize(uint32_t capacity)
		{
			return std::bit_ceil(capacity);
		}

		// Gives a slot a ring and publishes it as free in the recording. Main thread; no producer reaches the slot's
		// fields, since none can claim it before the publication.
		void ProvisionSlot(ProfilerData& data, uint32_t slotIndex)
		{
			uint32_t const ringSize = GetRingSize(data.Settings.ZonesPerThread);
			data.Rings[slotIndex] = std::make_unique<ZoneRecord[]>(ringSize);
			ProfilerSlot& slot = s_Slots[slotIndex];
			slot.Records = data.Rings[slotIndex].get();
			slot.Mask = ringSize - 1;
			slot.Capacity = data.Settings.ZonesPerThread;
			slot.IsMain = false;
			slot.NameLength = 0;
			slot.Head.store(0, std::memory_order_relaxed);
			slot.Tail.store(0, std::memory_order_relaxed);
			slot.Dropped.store(0, std::memory_order_relaxed);
			slot.Owner.store(PackOwner(data.RecordingNumber, SlotState::Free), std::memory_order_release);
		}

		// Keeps SpareSlots free slots ready, up to Profiler::MaxThreads provisioned ones.
		void ProvisionSpareSlots(ProfilerData& data)
		{
			uint32_t freeSlots = 0;
			for (ProfilerSlot const& slot : s_Slots)
			{
				if (slot.Owner.load(std::memory_order_relaxed) == PackOwner(data.RecordingNumber, SlotState::Free))
				{
					++freeSlots;
				}
			}
			for (uint32_t slotIndex = 0; slotIndex < Profiler::MaxThreads && freeSlots < SpareSlots; ++slotIndex)
			{
				if (!data.Rings[slotIndex])
				{
					ProvisionSlot(data, slotIndex);
					++freeSlots;
				}
			}
		}

		// Claims a free slot of the recording for the calling thread. Any thread; neither allocates nor locks.
		bool ClaimSlot(ThreadState& thread, uint32_t recording)
		{
			uint64_t const free = PackOwner(recording, SlotState::Free);
			for (uint32_t slotIndex = 0; slotIndex < Profiler::MaxThreads; ++slotIndex)
			{
				ProfilerSlot& slot = s_Slots[slotIndex];
				uint64_t expected = free;
				if (slot.Owner.load(std::memory_order_relaxed) != free ||
				    !slot.Owner.compare_exchange_strong(expected, PackOwner(recording, SlotState::Claiming), std::memory_order_acquire,
				                                        std::memory_order_relaxed))
				{
					continue;
				}
				slot.Writers.fetch_add(1, std::memory_order_seq_cst);
				if (s_RecordingNumber.load(std::memory_order_seq_cst) != recording)
				{
					// The recording ended while the slot was being claimed: Stop has given up on it or will wait for us.
					slot.Writers.fetch_sub(1, std::memory_order_release);
					return false;
				}
				slot.IsMain = thread.MainInit != 0 && thread.MainInit == s_InitNumber.load(std::memory_order_relaxed);
				slot.NameLength = thread.NameLength;
				std::memcpy(slot.Name.data(), thread.Name.data(), thread.NameLength);
				slot.Owner.store(PackOwner(recording, SlotState::Owned), std::memory_order_release);
				slot.Writers.fetch_sub(1, std::memory_order_release);
				thread.Slot = slotIndex;
				thread.SlotRecording = recording;
				return true;
			}
			return false;
		}

		// Waits until no producer is inside a slot. Called after the recording number changed, so producers arriving
		// later leave without touching anything.
		void WaitForWriters()
		{
			for (ProfilerSlot const& slot : s_Slots)
			{
				while (slot.Writers.load(std::memory_order_seq_cst) != 0)
				{
					std::this_thread::yield();
				}
			}
		}

		// The index of a text in the recording's name table (cut to Profiler::MaxNameBytes), adding it when it is new;
		// ProfilerNoName when the table is full.
		uint32_t GetTextIndex(ProfilerData& data, std::string_view name)
		{
			std::string_view const text = CutAtCharacter(name, Profiler::MaxNameBytes);
			auto const found = data.NameIndicesByText.find(text);
			if (found != data.NameIndicesByText.end())
			{
				return found->second;
			}
			if (data.History.Names.size() >= Profiler::MaxCaptureNames)
			{
				return ProfilerNoName;
			}
			uint32_t const index = static_cast<uint32_t>(data.History.Names.size());
			data.History.Names.emplace_back(text);
			data.NameIndicesByText.emplace(std::string(text), index);
			return index;
		}

		// GetTextIndex for a name with static storage duration or interned until Shutdown, remembered by its address so
		// that a name seen before costs one lookup.
		uint32_t GetNameIndex(ProfilerData& data, char const* name)
		{
			auto const found = data.NameIndicesByPointer.find(name);
			if (found != data.NameIndicesByPointer.end())
			{
				return found->second;
			}
			uint32_t const index = GetTextIndex(data, name);
			if (index != ProfilerNoName)
			{
				data.NameIndicesByPointer.emplace(name, index);
			}
			return index;
		}

		// The thread entry of a slot's owner, binding one when the owner has none: the main thread's entry, a free entry
		// of the owner's name, or a new one. NoEntry when the recording has made Profiler::MaxRecordedThreads entries.
		uint32_t BindThreadEntry(ProfilerData& data, uint32_t slotIndex)
		{
			if (data.SlotEntries[slotIndex] != NoEntry)
			{
				return data.SlotEntries[slotIndex];
			}
			ProfilerSlot const& slot = s_Slots[slotIndex];
			std::vector<ProfilerThread>& threads = data.History.Threads;
			uint32_t entry = NoEntry;
			if (slot.IsMain && !data.EntryBound[0])
			{
				entry = 0;
			}
			else
			{
				std::string_view const name(slot.Name.data(), slot.NameLength);
				if (!name.empty())
				{
					for (uint32_t candidate = 1; candidate < threads.size(); ++candidate)
					{
						if (!data.EntryBound[candidate] && threads[candidate].Name == name)
						{
							entry = candidate;
							break;
						}
					}
				}
				if (entry == NoEntry)
				{
					if (threads.size() >= Profiler::MaxRecordedThreads)
					{
						return NoEntry;
					}
					entry = static_cast<uint32_t>(threads.size());
					threads.push_back({name.empty() ? fmt::format("Thread {}", entry) : std::string(name)});
					data.EntryBound.push_back(false);
				}
			}
			data.EntryBound[entry] = true;
			data.SlotEntries[slotIndex] = entry;
			return entry;
		}

		// Reorders one thread's records from the order they ended in (each zone after the zones nested in it) to
		// pre-order (each zone before them, siblings in the order they began), from their depths alone: a zone adopts the
		// zones deeper than it that ended just before it and are not adopted yet. A zone's pre-order position is the first
		// position of its subtree in end order plus its count of ancestors.
		void OrderZonesForFrame(ProfilerData& data)
		{
			std::vector<ZoneRecord> const& records = data.Drained;
			size_t const count = records.size();
			data.SubtreeSizes.assign(count, 1);
			data.Parents.assign(count, NoSlot);
			data.Ancestors.assign(count, 0);
			data.Stack.clear();
			for (size_t index = 0; index < count; ++index)
			{
				while (!data.Stack.empty() && records[data.Stack.back()].Depth > records[index].Depth)
				{
					uint32_t const child = data.Stack.back();
					data.Stack.pop_back();
					data.SubtreeSizes[index] += data.SubtreeSizes[child];
					data.Parents[child] = static_cast<uint32_t>(index);
				}
				data.Stack.push_back(static_cast<uint32_t>(index));
			}
			// Parents end after their children, so walking backwards meets every parent first.
			for (size_t index = count; index-- > 0;)
			{
				if (data.Parents[index] != NoSlot)
				{
					data.Ancestors[index] = data.Ancestors[data.Parents[index]] + 1;
				}
			}
			data.Ordered.resize(count);
			for (size_t index = 0; index < count; ++index)
			{
				size_t const position = index + 1 - data.SubtreeSizes[index] + data.Ancestors[index];
				data.Ordered[position] = records[index];
			}
		}

		// Empties every slot of the recording. With a frame, its zones go into the frame in thread order and its drops
		// are counted there; without one (the first frame of a recording discarding what ended before it), they are
		// discarded. Slots whose thread ended are freed, and their thread entries after the whole collection, so that an
		// entry never holds zones of two threads in one frame.
		void CollectSlots(ProfilerData& data, ProfilerFrame* frame)
		{
			uint64_t dropped = s_UnattributedDrops.exchange(0, std::memory_order_relaxed);
			data.Collected.clear();
			data.Groups.clear();
			data.EntriesToRelease.clear();
			for (uint32_t slotIndex = 0; slotIndex < Profiler::MaxThreads; ++slotIndex)
			{
				ProfilerSlot& slot = s_Slots[slotIndex];
				uint64_t const owner = slot.Owner.load(std::memory_order_acquire);
				SlotState const state = GetOwnerState(owner);
				if (GetOwnerRecording(owner) != data.RecordingNumber || (state != SlotState::Owned && state != SlotState::Ending))
				{
					continue;
				}
				uint32_t const tail = slot.Tail.load(std::memory_order_relaxed);
				uint32_t const head = slot.Head.load(std::memory_order_acquire);
				dropped += slot.Dropped.exchange(0, std::memory_order_relaxed);
				if (frame != nullptr && head != tail)
				{
					uint32_t const entry = BindThreadEntry(data, slotIndex);
					if (entry == NoEntry)
					{
						dropped += head - tail;
					}
					else
					{
						data.Drained.clear();
						for (uint32_t position = tail; position != head; ++position)
						{
							data.Drained.push_back(slot.Records[position & slot.Mask]);
						}
						OrderZonesForFrame(data);
						ZoneGroup group{entry, data.Collected.size(), 0};
						for (ZoneRecord const& record : data.Ordered)
						{
							uint32_t const name = GetNameIndex(data, record.Name);
							uint32_t const valueLabel =
								record.ValueLabel != nullptr ? GetNameIndex(data, record.ValueLabel) : ProfilerNoName;
							if (name == ProfilerNoName || (record.ValueLabel != nullptr && valueLabel == ProfilerNoName))
							{
								++dropped;
								continue;
							}
							ProfilerZone zone;
							zone.Name = name;
							zone.Thread = entry;
							zone.Depth = record.Depth;
							zone.ValueLabel = valueLabel;
							zone.Value = record.Value;
							zone.StartNanoseconds = std::max<int64_t>(record.StartNanoseconds - data.Origin, 0);
							zone.DurationNanoseconds = std::max<int64_t>(record.EndNanoseconds - record.StartNanoseconds, 0);
							data.Collected.push_back(zone);
						}
						group.End = data.Collected.size();
						data.Groups.push_back(group);
					}
				}
				slot.Tail.store(head, std::memory_order_release);
				if (state == SlotState::Ending)
				{
					// The thread called EndThread after its last record: nothing writes the slot until it is claimed again.
					if (data.SlotEntries[slotIndex] != NoEntry)
					{
						data.EntriesToRelease.push_back(data.SlotEntries[slotIndex]);
						data.SlotEntries[slotIndex] = NoEntry;
					}
					slot.Owner.store(PackOwner(data.RecordingNumber, SlotState::Free), std::memory_order_release);
				}
			}
			for (uint32_t const entry : data.EntriesToRelease)
			{
				data.EntryBound[entry] = false;
			}
			if (frame == nullptr)
			{
				return;
			}

			std::sort(data.Groups.begin(), data.Groups.end(),
			          [](ZoneGroup const& left, ZoneGroup const& right)
			          {
						  return left.Entry < right.Entry;
					  });
			size_t const limit = data.Settings.HistoryZones;
			frame->Zones.reserve(std::min(data.Collected.size(), limit));
			for (ZoneGroup const& group : data.Groups)
			{
				size_t const room = limit - frame->Zones.size();
				size_t const taken = std::min(room, group.End - group.Begin);
				frame->Zones.insert(frame->Zones.end(), data.Collected.begin() + static_cast<ptrdiff_t>(group.Begin),
				                    data.Collected.begin() + static_cast<ptrdiff_t>(group.Begin + taken));
				dropped += group.End - group.Begin - taken;
			}
			frame->DroppedZones = static_cast<uint32_t>(std::min<uint64_t>(dropped, std::numeric_limits<uint32_t>::max()));
			data.DroppedZones += dropped;
		}

		// Frees every ring and slot of the ended recording. Main thread, after WaitForWriters.
		void ReleaseSlots(ProfilerData& data)
		{
			for (uint32_t slotIndex = 0; slotIndex < Profiler::MaxThreads; ++slotIndex)
			{
				ProfilerSlot& slot = s_Slots[slotIndex];
				slot.Owner.store(PackOwner(0, SlotState::Unprovisioned), std::memory_order_relaxed);
				slot.Records = nullptr;
				slot.Mask = 0;
				slot.Capacity = 0;
				slot.Head.store(0, std::memory_order_relaxed);
				slot.Tail.store(0, std::memory_order_relaxed);
				slot.Dropped.store(0, std::memory_order_relaxed);
				data.Rings[slotIndex].reset();
				data.SlotEntries[slotIndex] = NoEntry;
			}
		}

		void EndFrameNow(ProfilerData& data)
		{
			data.FrameInProgress = false;
			if (!data.FrameRecorded)
			{
				return;
			}
			data.FrameRecorded = false;

			ProfilerFrame frame;
			frame.Index = data.FrameIndex;
			frame.StartNanoseconds = std::max<int64_t>(data.FrameStart - data.Origin, 0);
			frame.EndNanoseconds = std::max<int64_t>(Profiler::Now() - data.Origin, frame.StartNanoseconds);
			CollectSlots(data, &frame);
			frame.GpuPasses = std::move(data.PendingGpuPasses);
			data.PendingGpuPasses.clear();

			std::deque<ProfilerFrame>& frames = data.History.Frames;
			data.ZonesHeld += frame.Zones.size();
			frames.push_back(std::move(frame));
			while (frames.size() > 1 && (frames.size() > data.Settings.HistoryFrames || data.ZonesHeld > data.Settings.HistoryZones))
			{
				data.ZonesHeld -= frames.front().Zones.size();
				frames.pop_front();
			}
			ProvisionSpareSlots(data);
		}

		Result<void> CheckSettings(ProfilerSettings const& settings)
		{
#ifdef ST_DIST
			// Dist builds compile no zone, so a recording would hold empty frames.
			static_cast<void>(settings);
			return Error{"The profiler is not available in Dist builds"};
#else
			if (settings.HistoryFrames < Profiler::MinHistoryFrames || settings.HistoryFrames > Profiler::MaxHistoryFrames)
			{
				return MakeError("HistoryFrames must be between {} and {}, not {}", Profiler::MinHistoryFrames, Profiler::MaxHistoryFrames,
				                 settings.HistoryFrames);
			}
			if (settings.HistoryZones < Profiler::MinHistoryZones || settings.HistoryZones > Profiler::MaxHistoryZones)
			{
				return MakeError("HistoryZones must be between {} and {}, not {}", Profiler::MinHistoryZones, Profiler::MaxHistoryZones,
				                 settings.HistoryZones);
			}
			if (settings.ZonesPerThread < Profiler::MinZonesPerThread || settings.ZonesPerThread > Profiler::MaxZonesPerThread)
			{
				return MakeError("ZonesPerThread must be between {} and {}, not {}", Profiler::MinZonesPerThread,
				                 Profiler::MaxZonesPerThread, settings.ZonesPerThread);
			}
			return {};
#endif
		}
	}

	void Profiler::Init()
	{
		ST_CORE_ASSERT(!s_Data, "The profiler is already initialized");
		s_Data = CreateScope<ProfilerData>();
		s_Data->SlotEntries.fill(NoEntry);
		{
			std::scoped_lock const lock(s_InternedNamesMutex);
			s_InternedNames = CreateScope<InternedNameSet>();
		}
		s_ThreadState.MainInit = s_InitNumber.fetch_add(1, std::memory_order_relaxed) + 1;
		SetThreadName("Main");
	}

	void Profiler::Shutdown()
	{
		if (!s_Data)
		{
			return;
		}
		Stop();
		{
			std::scoped_lock const lock(s_InternedNamesMutex);
			s_InternedNames.reset();
		}
		s_Data.reset();
	}

	bool Profiler::IsInitialized()
	{
		return static_cast<bool>(s_Data);
	}

	Result<void> Profiler::Start(ProfilerSettings const& settings)
	{
		if (!s_Data)
		{
			return Error{"The profiler is not initialized"};
		}
		ProfilerData& data = *s_Data;
		if (data.Recording)
		{
			return Error{"A recording is already in progress"};
		}
		if (Result<void> checked = CheckSettings(settings); !checked)
		{
			return checked;
		}

		data.Settings = settings;
		data.History = {};
		data.History.Threads.push_back({"Main"});
		data.EntryBound.assign(1, false);
		data.SlotEntries.fill(NoEntry);
		data.NameIndicesByPointer.clear();
		data.NameIndicesByText.clear();
		data.Views.clear();
		data.PendingGpuPasses.clear();
		data.ZonesHeld = 0;
		data.DroppedZones = 0;
		data.RecordingHasFrame = false;
		// A frame in progress began before the recording: it is not recorded.
		data.FrameRecorded = false;

		// Only the main thread advances the number, so it can read and publish it in two steps.
		data.RecordingNumber = s_RecordingNumber.load(std::memory_order_relaxed) + 1;
		uint32_t const initialSlots = std::min(MaxThreads, std::max(std::thread::hardware_concurrency(), 1u) + SpareSlots);
		for (uint32_t slotIndex = 0; slotIndex < initialSlots; ++slotIndex)
		{
			ProvisionSlot(data, slotIndex);
		}
		data.Origin = Now();
		s_UnattributedDrops.store(0, std::memory_order_relaxed);
		s_RecordingNumber.store(data.RecordingNumber, std::memory_order_seq_cst);
		s_Recording.store(true, std::memory_order_seq_cst);
		data.Recording = true;
		return {};
	}

	void Profiler::Stop()
	{
		if (!s_Data || !s_Data->Recording)
		{
			return;
		}
		ProfilerData& data = *s_Data;
		s_Recording.store(false, std::memory_order_seq_cst);
		s_RecordingNumber.fetch_add(1, std::memory_order_seq_cst);
		WaitForWriters();
		ReleaseSlots(data);
		s_UnattributedDrops.store(0, std::memory_order_relaxed);
		data.Recording = false;
		data.FrameRecorded = false;
		data.PendingGpuPasses.clear();
		data.EntryBound.assign(data.History.Threads.size(), false);
	}

	void Profiler::BeginFrame()
	{
		if (!s_Data)
		{
			return;
		}
		ProfilerData& data = *s_Data;
		if (data.FrameInProgress)
		{
			EndFrameNow(data);
		}
		++data.FrameIndex;
		data.FrameInProgress = true;
		data.FrameStart = Now();
		data.FrameRecorded = data.Recording;
		if (data.Recording && !data.RecordingHasFrame)
		{
			// Zones that ended between Start and the first frame belong to no frame.
			CollectSlots(data, nullptr);
			data.RecordingHasFrame = true;
		}
	}

	void Profiler::EndFrame()
	{
		if (!s_Data || !s_Data->FrameInProgress)
		{
			return;
		}
		EndFrameNow(*s_Data);
	}

	uint64_t Profiler::GetFrameIndex()
	{
		return s_Data ? s_Data->FrameIndex : 0;
	}

	void Profiler::RecordGpuPasses(uint64_t frameIndex, std::string_view view, std::span<ProfilerGpuPassTiming const> passes)
	{
		if (!s_Data || !s_Data->Recording || passes.empty())
		{
			return;
		}
		ProfilerData& data = *s_Data;

		std::vector<ProfilerGpuPass>* target = nullptr;
		std::deque<ProfilerFrame>& frames = data.History.Frames;
		if (data.FrameInProgress && data.FrameRecorded && frameIndex == data.FrameIndex)
		{
			target = &data.PendingGpuPasses;
		}
		else if (!frames.empty() && frameIndex >= frames.front().Index && frameIndex <= frames.back().Index)
		{
			// The frames of one recording have consecutive indices.
			ProfilerFrame& frame = frames[static_cast<size_t>(frameIndex - frames.front().Index)];
			if (frame.Index == frameIndex)
			{
				target = &frame.GpuPasses;
			}
		}
		if (target == nullptr)
		{
			return;
		}

		// The view's text is copied (by text, never by address: the caller's string may not outlive the call).
		uint32_t const viewIndex = GetTextIndex(data, view);
		if (viewIndex == ProfilerNoName)
		{
			return;
		}
		if (std::find(data.Views.begin(), data.Views.end(), viewIndex) == data.Views.end())
		{
			if (data.Views.size() >= MaxGpuViews)
			{
				return;
			}
			data.Views.push_back(viewIndex);
		}

		for (ProfilerGpuPassTiming const& pass : passes)
		{
			if (pass.Name == nullptr)
			{
				continue;
			}
			uint32_t const name = GetNameIndex(data, pass.Name);
			if (name == ProfilerNoName)
			{
				continue;
			}
			uint64_t const duration = std::min<uint64_t>(pass.DurationNanoseconds, std::numeric_limits<int64_t>::max());
			target->push_back({viewIndex, name, static_cast<int64_t>(duration)});
		}
	}

	ProfilerCapture const& Profiler::GetRecording()
	{
		static ProfilerCapture const s_Empty;
		return s_Data ? s_Data->History : s_Empty;
	}

	ProfilerStatus Profiler::GetStatus()
	{
		ProfilerStatus status;
#ifndef ST_DIST
		status.Available = true;
#endif
		if (!s_Data)
		{
			return status;
		}
		ProfilerData const& data = *s_Data;
		status.Initialized = true;
		status.Recording = data.Recording;
		status.HistoryFrames = data.Settings.HistoryFrames;
		status.FramesHeld = static_cast<uint32_t>(data.History.Frames.size());
		status.HistoryZones = data.Settings.HistoryZones;
		status.ZonesHeld = static_cast<uint32_t>(data.ZonesHeld);
		status.Threads = static_cast<uint32_t>(data.History.Threads.size());
		status.DroppedZones = data.DroppedZones;
		status.FrameIndex = data.FrameIndex;
		return status;
	}

	int64_t Profiler::Now()
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	ProfilerZoneToken Profiler::BeginRecordedZone(char const* name)
	{
		return BeginZoneOfRecording(name, s_RecordingNumber.load(std::memory_order_acquire));
	}

	ProfilerZoneToken Profiler::BeginDynamicZone(std::string_view name)
	{
		if (!IsRecording())
		{
			return {};
		}
		// Taken before the name is stored: a Shutdown that frees the name ends this recording first, so the zone is then
		// dropped at its end and its name never read.
		uint32_t const recording = s_RecordingNumber.load(std::memory_order_acquire);
		return BeginZoneOfRecording(InternName(name), recording);
	}

	ProfilerZoneToken Profiler::BeginZoneOfRecording(char const* name, uint32_t recording)
	{
		if (name == nullptr)
		{
			return {};
		}
		ThreadState& thread = s_ThreadState;
		if (thread.DepthRecording != recording)
		{
			thread.DepthRecording = recording;
			thread.Depth = 0;
		}
		ProfilerZoneToken token;
		token.Name = name;
		token.Recording = recording;
		token.Depth = thread.Depth++;
		token.StartNanoseconds = Now();
		return token;
	}

	void Profiler::EndRecordedZone(ProfilerZoneToken const& token)
	{
		int64_t const end = Now();
		ThreadState& thread = s_ThreadState;
		if (thread.DepthRecording == token.Recording && thread.Depth > 0)
		{
			--thread.Depth;
		}
		if (thread.Slot == NoSlot || thread.SlotRecording != token.Recording)
		{
			if (!ClaimSlot(thread, token.Recording))
			{
				if (s_RecordingNumber.load(std::memory_order_relaxed) == token.Recording)
				{
					s_UnattributedDrops.fetch_add(1, std::memory_order_relaxed);
				}
				return;
			}
		}

		ProfilerSlot& slot = s_Slots[thread.Slot];
		slot.Writers.fetch_add(1, std::memory_order_seq_cst);
		if (s_RecordingNumber.load(std::memory_order_seq_cst) == token.Recording)
		{
			uint32_t const head = slot.Head.load(std::memory_order_relaxed);
			if (head - slot.Tail.load(std::memory_order_acquire) < slot.Capacity)
			{
				slot.Records[head & slot.Mask] = {token.Name, token.ValueLabel, token.Value, token.StartNanoseconds, end, token.Depth};
				slot.Head.store(head + 1, std::memory_order_release);
			}
			else
			{
				slot.Dropped.fetch_add(1, std::memory_order_relaxed);
			}
		}
		slot.Writers.fetch_sub(1, std::memory_order_release);
	}

	char const* Profiler::InternName(std::string_view name)
	{
		std::string_view const text = CutAtCharacter(name, MaxNameBytes);
		{
			std::scoped_lock const lock(s_InternedNamesMutex);
			if (s_InternedNames)
			{
				auto const found = s_InternedNames->find(text);
				if (found != s_InternedNames->end())
				{
					return found->c_str();
				}
				if (s_InternedNames->size() < MaxInternedNames)
				{
					return s_InternedNames->emplace(text).first->c_str();
				}
			}
		}
		if (IsRecording())
		{
			s_UnattributedDrops.fetch_add(1, std::memory_order_relaxed);
		}
		return nullptr;
	}

	void Profiler::SetThreadName(std::string_view name)
	{
		std::string_view const text = CutAtCharacter(name, MaxThreadNameBytes);
		ThreadState& thread = s_ThreadState;
		std::memcpy(thread.Name.data(), text.data(), text.size());
		thread.Name[text.size()] = '\0';
		thread.NameLength = static_cast<uint32_t>(text.size());
	}

	void Profiler::EndThread()
	{
		ThreadState& thread = s_ThreadState;
		if (thread.Slot != NoSlot)
		{
			// Fails harmlessly when the recording that gave the slot has ended.
			uint64_t expected = PackOwner(thread.SlotRecording, SlotState::Owned);
			s_Slots[thread.Slot].Owner.compare_exchange_strong(expected, PackOwner(thread.SlotRecording, SlotState::Ending),
			                                                   std::memory_order_release, std::memory_order_relaxed);
		}
		thread.Slot = NoSlot;
		thread.SlotRecording = 0;
		thread.Depth = 0;
		thread.DepthRecording = 0;
		thread.NameLength = 0;
		thread.Name[0] = '\0';
	}

	namespace
	{
		// The subsystem of zones that neither name one nor lie in a zone that does.
		constexpr std::string_view OtherSubsystem = "Other";

		double ToMilliseconds(int64_t nanoseconds)
		{
			return static_cast<double>(nanoseconds) / 1.0e6;
		}

		// The median of the values (the mean of the two middle ones for an even count); 0 for none. Reorders them.
		double TakeMedian(std::vector<double>& values)
		{
			if (values.empty())
			{
				return 0.0;
			}
			size_t const middle = values.size() / 2;
			std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle), values.end());
			double const upper = values[middle];
			if (values.size() % 2 != 0)
			{
				return upper;
			}
			double const lower = *std::max_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle));
			return (lower + upper) / 2.0;
		}

		// The nearest-rank percentile (0 to 100) of the values; 0 for none. Reorders them.
		double TakePercentile(std::vector<double>& values, double percentile)
		{
			if (values.empty())
			{
				return 0.0;
			}
			double const rank = std::ceil(percentile / 100.0 * static_cast<double>(values.size()));
			size_t const index = static_cast<size_t>(std::clamp(rank, 1.0, static_cast<double>(values.size()))) - 1;
			std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
			return values[index];
		}

		// A zone open on its thread while the zones after it are read: the nearest earlier zone of a smaller depth is the
		// parent (Docs/Architecture.md §4.4).
		struct OpenZone
		{
			uint32_t Depth = 0;
			std::string_view Name;
			std::string_view Subsystem;
		};

		struct ZoneSeries
		{
			std::vector<double> PerFrame;
			std::vector<double> PerCall;
			uint64_t Calls = 0;
		};

		std::vector<ProfilerMedian> TakeMedians(std::map<std::string, std::vector<double>, std::less<>>& series)
		{
			std::vector<ProfilerMedian> medians;
			medians.reserve(series.size());
			for (auto& [name, values] : series)
			{
				medians.push_back({name, TakeMedian(values)});
			}
			return medians;
		}

		// The per-frame series of a name, made with a value per frame when first met.
		std::vector<double>& GetSeries(std::map<std::string, std::vector<double>, std::less<>>& series, std::string_view name,
		                               size_t frameCount)
		{
			auto found = series.find(name);
			if (found == series.end())
			{
				found = series.emplace(std::string(name), std::vector<double>(frameCount, 0.0)).first;
			}
			return found->second;
		}
	}

	std::string_view GetProfilerSubsystem(std::string_view zoneName)
	{
		size_t const dot = zoneName.find('.');
		return dot == std::string_view::npos ? std::string_view() : zoneName.substr(0, dot);
	}

	ProfilerMedians ComputeProfilerMedians(ProfilerCapture const& capture)
	{
		ProfilerMedians medians;
		size_t const frameCount = capture.Frames.size();
		if (frameCount == 0)
		{
			return medians;
		}

		std::vector<double> frameTimes;
		frameTimes.reserve(frameCount);
		std::map<std::string, std::vector<double>, std::less<>> subsystems;
		std::map<std::string, std::vector<double>, std::less<>> threads;
		std::map<std::string, ZoneSeries, std::less<>> zones;
		std::map<std::string, std::vector<double>, std::less<>> gpuPasses;
		std::map<std::string, double, std::less<>> framePasses;
		std::vector<std::vector<OpenZone>> stacks(capture.Threads.size());
		std::string passName;

		for (size_t frameIndex = 0; frameIndex < frameCount; frameIndex++)
		{
			ProfilerFrame const& frame = capture.Frames[frameIndex];
			frameTimes.push_back(ToMilliseconds(frame.EndNanoseconds - frame.StartNanoseconds));
			for (std::vector<OpenZone>& stack : stacks)
			{
				stack.clear();
			}

			for (ProfilerZone const& zone : frame.Zones)
			{
				if (zone.Thread >= stacks.size() || zone.Name >= capture.Names.size())
				{
					continue;
				}
				std::vector<OpenZone>& stack = stacks[zone.Thread];
				while (!stack.empty() && stack.back().Depth >= zone.Depth)
				{
					stack.pop_back();
				}
				std::string_view const name = capture.Names[zone.Name];
				std::string_view const own = GetProfilerSubsystem(name);
				std::string_view const subsystem = !own.empty() ? own : !stack.empty() ? stack.back().Subsystem : OtherSubsystem;
				double const milliseconds = ToMilliseconds(zone.DurationNanoseconds);

				bool const nestedInSameName = std::any_of(stack.begin(), stack.end(),
				                                          [name](OpenZone const& open)
				                                          {
															  return open.Name == name;
														  });
				if (!nestedInSameName)
				{
					auto found = zones.find(name);
					if (found == zones.end())
					{
						found = zones.emplace(std::string(name), ZoneSeries{std::vector<double>(frameCount, 0.0), {}, 0}).first;
					}
					found->second.PerFrame[frameIndex] += milliseconds;
					found->second.PerCall.push_back(milliseconds);
					found->second.Calls++;
				}

				if (zone.Thread == 0)
				{
					bool const nestedInSubsystem = std::any_of(stack.begin(), stack.end(),
					                                           [subsystem](OpenZone const& open)
					                                           {
																   return open.Subsystem == subsystem;
															   });
					if (!nestedInSubsystem)
					{
						GetSeries(subsystems, subsystem, frameCount)[frameIndex] += milliseconds;
					}
				}
				else if (stack.empty())
				{
					GetSeries(threads, capture.Threads[zone.Thread].Name, frameCount)[frameIndex] += milliseconds;
				}
				stack.push_back({zone.Depth, name, subsystem});
			}

			framePasses.clear();
			for (ProfilerGpuPass const& pass : frame.GpuPasses)
			{
				if (pass.View >= capture.Names.size() || pass.Name >= capture.Names.size())
				{
					continue;
				}
				passName.assign(capture.Names[pass.View]);
				passName += '/';
				passName += capture.Names[pass.Name];
				framePasses[passName] += ToMilliseconds(pass.DurationNanoseconds);
			}
			for (auto const& [name, milliseconds] : framePasses)
			{
				gpuPasses[name].push_back(milliseconds);
			}
		}

		std::vector<double> percentileTimes = frameTimes;
		medians.FrameMilliseconds = TakeMedian(frameTimes);
		medians.FramePercentile99Milliseconds = TakePercentile(percentileTimes, 99.0);
		medians.Subsystems = TakeMedians(subsystems);
		medians.Threads = TakeMedians(threads);
		medians.GpuPasses = TakeMedians(gpuPasses);
		medians.Zones.reserve(zones.size());
		for (auto& [name, series] : zones)
		{
			double const calls = static_cast<double>(series.Calls) / static_cast<double>(frameCount);
			medians.Zones.push_back({name, TakeMedian(series.PerFrame), TakeMedian(series.PerCall), calls});
		}
		return medians;
	}
}
