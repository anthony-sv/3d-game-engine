#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// The engine's CPU and GPU profiler (Docs/Architecture.md §4.4). Zones are recorded by any thread into per-thread
// buffers and collected by the main thread once per frame into a bounded history, which the Profiler panel, the
// profiler.* commands and captures (Serialization/ProfilerCaptureSerializer.h, the Chrome trace event format) read.
//
// Zones are compiled in Debug and Release builds. Dist builds compile the macros below to nothing (their arguments are
// referenced but never evaluated, so a local used only as a zone name raises no unused-variable warning); the Profiler
// class itself is compiled in every configuration, and Start refuses in Dist builds. A translation unit may define
// ST_DISABLE_PROFILING before its first include to compile the Dist form of the macros in any configuration (not in a
// target whose sources include a precompiled header first, which the define cannot precede).
#if !defined(ST_DIST) && !defined(ST_DISABLE_PROFILING)
#define ST_ENABLE_PROFILING
#endif

namespace Strada
{
	// Limits of a recording (Profiler::Start).
	struct ProfilerSettings
	{
		// Collected frames kept in the history, oldest dropped first (Profiler::MinHistoryFrames to MaxHistoryFrames).
		uint32_t HistoryFrames = 600;
		// Zones kept in the history over all its frames, oldest frames dropped first (Profiler::MinHistoryZones to
		// MaxHistoryZones). A frame that alone collects more keeps the first ones in its order and counts the rest dropped.
		uint32_t HistoryZones = 2000000;
		// Capacity of each thread's buffer, in zones that end between two collections (Profiler::MinZonesPerThread to
		// MaxZonesPerThread). A thread that ends more zones in one frame drops the rest, and the frame counts them.
		uint32_t ZonesPerThread = 4096;
	};

	// A GPU pass measured by a renderer: a duration, since GPU timer queries give no start time.
	struct ProfilerGpuPassTiming
	{
		// A name with static storage duration (a string literal).
		char const* Name = nullptr;
		uint64_t DurationNanoseconds = 0;
	};

	// The handle of a zone in progress, returned by Profiler::BeginZone and passed to Profiler::EndZone. Trivially
	// copyable and at most 64 bytes, so that callers whose scopes are not C++ scopes (Jolt's external profile
	// measurement, whose user data is 64 bytes) can keep it. A token whose Name is null records nothing.
	struct ProfilerZoneToken
	{
		char const* Name = nullptr;
		// The label of Value (static storage duration), or null when the zone carries no value.
		char const* ValueLabel = nullptr;
		uint64_t Value = 0;
		int64_t StartNanoseconds = 0;
		// The recording the zone began in: a zone that ends in another recording is dropped.
		uint32_t Recording = 0;
		// Zones of the same recording open on the thread when this one began.
		uint32_t Depth = 0;
	};

	static_assert(sizeof(ProfilerZoneToken) <= 64, "Jolt's external profile measurement keeps the token in 64 bytes");
	static_assert(std::is_trivially_copyable_v<ProfilerZoneToken>, "callers keep the token in raw storage");

	// --- Recorded data: what the history holds and what a capture file stores. Plain data, read on the main thread. ---

	// Names (zones, value labels, views, GPU passes) are indices into ProfilerCapture::Names; this one means none.
	inline constexpr uint32_t ProfilerNoName = std::numeric_limits<uint32_t>::max();

	// A zone that ended on a thread. Times are in nanoseconds from the start of the recording, never negative: nothing
	// that began before the recording is recorded. A zone of a thread whose work runs independently of frames (the audio
	// and streaming threads) can begin before the frame that collected it.
	struct ProfilerZone
	{
		uint32_t Name = ProfilerNoName;
		// Index into ProfilerCapture::Threads.
		uint32_t Thread = 0;
		// Zones that were open on the thread when this one began. A zone whose parent was dropped (a full buffer) or
		// collected by an earlier frame has no zone of a smaller depth before it in its frame: consumers that build trees
		// place each zone under the nearest zone before it on its thread with a smaller depth, or at the thread's root.
		uint32_t Depth = 0;
		uint32_t ValueLabel = ProfilerNoName;
		uint64_t Value = 0;
		int64_t StartNanoseconds = 0;
		int64_t DurationNanoseconds = 0;
	};

	struct ProfilerGpuPass
	{
		// The renderer that measured the pass (one per view: the editor's viewport, its game view, the player).
		uint32_t View = ProfilerNoName;
		uint32_t Name = ProfilerNoName;
		int64_t DurationNanoseconds = 0;
	};

	// One main-loop iteration, from Profiler::BeginFrame to Profiler::EndFrame.
	struct ProfilerFrame
	{
		// Counted by BeginFrame since Profiler::Init, from 1. The frames of one recording have consecutive indices.
		uint64_t Index = 0;
		int64_t StartNanoseconds = 0;
		int64_t EndNanoseconds = 0;
		// The zones collected at this frame's end: every zone that ended since the previous collection, whichever thread
		// recorded it. Grouped by thread in the order of ProfilerCapture::Threads; each thread's zones are in pre-order
		// (a zone before the zones nested in it, siblings in the order they began), rebuilt from the order in which they
		// ended and their depths, never from their times, which can be equal.
		std::vector<ProfilerZone> Zones;
		// In the order the renderers reported them; a view that rendered twice in the frame reports its passes twice. A
		// frame's passes arrive frames later; frames whose GPU work was not measured have none.
		std::vector<ProfilerGpuPass> GpuPasses;
		// Zones lost because a thread's buffer was full, a thread found no free buffer or no thread entry left
		// (Profiler::MaxRecordedThreads), a name could not be stored (Profiler::MaxInternedNames, MaxCaptureNames), or the
		// frame alone collected more zones than the history holds (ProfilerSettings::HistoryZones).
		uint32_t DroppedZones = 0;
	};

	struct ProfilerThread
	{
		// The name the thread gave itself (Profiler::SetThreadName), or "Thread <n>".
		std::string Name;
	};

	// Frames with the tables their zones index: the recorder's history (Profiler::GetRecording) or a capture opened again.
	// The recorder adds table entries as a recording meets new names and threads, and keeps them while it records (frames
	// leave the history, entries stay); a capture's writer writes only the entries its frames use, renumbered, so a
	// capture opened again holds no unused entry. Both stay within the counts a capture's reader accepts
	// (Profiler::MaxCaptureNames, MaxRecordedThreads, MaxGpuViews). The bytes a capture takes depend on its names, so
	// its writer refuses a capture larger than Profiler::MaxCaptureBytes rather than write what its reader refuses.
	struct ProfilerCapture
	{
		std::vector<std::string> Names;
		// The recording's threads, in the order they first took a buffer. A thread that ends (Profiler::EndThread) frees
		// its entry for a later thread of the same name, so a pool's tasks (threads that come and go under one name)
		// share as many entries as ran at once; an entry never holds zones of two threads in one frame. Entry 0 is always
		// the main thread (the one that called Profiler::Init), whether or not it recorded a zone: frames belong to it.
		std::vector<ProfilerThread> Threads;
		// Oldest first.
		std::deque<ProfilerFrame> Frames;
	};

	struct ProfilerStatus
	{
		bool Initialized = false;
		bool Recording = false;
		// False in Dist builds, where Start refuses.
		bool Available = false;
		// The history's capacity in frames, and the frames it holds now.
		uint32_t HistoryFrames = 0;
		uint32_t FramesHeld = 0;
		// The history's capacity in zones, and the zones it holds now.
		uint32_t HistoryZones = 0;
		uint32_t ZonesHeld = 0;
		// Thread entries of the recording (ProfilerCapture::Threads), the main thread's included.
		uint32_t Threads = 0;
		// Zones dropped during the recording, in every frame (held or not).
		uint64_t DroppedZones = 0;
		// The frame in progress (Profiler::GetFrameIndex).
		uint64_t FrameIndex = 0;
	};

	// A median over a capture's frames, in milliseconds.
	struct ProfilerMedian
	{
		std::string Name;
		double Milliseconds = 0.0;
	};

	// Figures of one zone name over every thread: per frame, the summed durations of its zones (a zone nested in a zone
	// of the same name is not added again), the median per call, and the mean of its calls per frame. Per-step costs
	// (a vehicle constraint per physics step) are read from the per-call figure.
	struct ProfilerZoneMedian
	{
		std::string Name;
		double MillisecondsPerFrame = 0.0;
		double MillisecondsPerCall = 0.0;
		double CallsPerFrame = 0.0;
	};

	// Every per-frame sum counts a zone whole in the frame that collected it, never clipped to the frame's interval: a
	// capture's frames partition its zones, so the sums over all frames are the recording's totals.
	struct ProfilerMedians
	{
		// Frame durations (EndNanoseconds - StartNanoseconds): the median and the 99th percentile.
		double FrameMilliseconds = 0.0;
		double FramePercentile99Milliseconds = 0.0;
		// Per subsystem (GetProfilerSubsystem), sorted by name: per frame, the summed durations of the subsystem's outermost
		// zones on the main thread, the frame's wall time spent in it. Subsystems overlap where zones of one nest in
		// another's (scripts inside the scene's update). Work other threads do in parallel is in Threads, never added here.
		std::vector<ProfilerMedian> Subsystems;
		// Per thread other than the main thread, by thread name (threads of one name, such as Jolt's workers, merged), sorted
		// by name: per frame, the summed durations of the thread's outermost zones, its busy time.
		std::vector<ProfilerMedian> Threads;
		// Per zone name over every thread, sorted by name.
		std::vector<ProfilerZoneMedian> Zones;
		// Per view and pass ("<view>/<pass>"), sorted by name, over the frames that measured the pass: per frame, the summed
		// durations of the pass (a view that rendered twice adds both).
		std::vector<ProfilerMedian> GpuPasses;
	};

	// The subsystem of a zone name written "<Subsystem>.<Phase>" ("Physics.Step" is in "Physics"): the text before the
	// first '.', or empty when the name has none (a zone named by ST_PROFILE_FUNCTION, a script class, a graph, one of
	// Jolt's own zones), in which case the zone belongs to the subsystem of its nearest enclosing zone on its thread that
	// has one, and to "Other" when none has. Subsystem medians read the main thread only; the work of other threads is
	// measured per thread (ProfilerMedians::Threads), so their unprefixed zones need no subsystem. Pure; any thread.
	[[nodiscard]] std::string_view GetProfilerSubsystem(std::string_view zoneName);

	// Medians of a capture's frames (the figures StradaRuntime --profile logs at exit). Pure; any thread.
	[[nodiscard]] ProfilerMedians ComputeProfilerMedians(ProfilerCapture const& capture);

	// The recorder. A static facade (Init/Shutdown); Application initializes it before every other subsystem and shuts
	// it down after them. Every function is safe to call before Init and after Shutdown: zones then record nothing,
	// Start refuses and the main-thread functions do nothing.
	//
	// Threads keep a little thread-local state (their buffer, their depth and their name): trivially destructible and
	// constant-initialized, so on Windows and Linux reaching it never allocates. On Apple platforms the first access of a
	// thread to thread-local storage allocates its block once (dyld's thread-local variables), so the first zone or
	// SetThreadName of a thread there allocates once; every later one does not.
	class Profiler
	{
	public:
		// Buffers for threads that record during one recording; a thread finding none free drops its zones. A buffer a
		// thread frees (EndThread) is free again after the next collection, so at most this many threads record between
		// two collections.
		static constexpr uint32_t MaxThreads = 64;
		static constexpr uint32_t MinHistoryFrames = 1;
		static constexpr uint32_t MaxHistoryFrames = 36000;
		static constexpr uint32_t MinHistoryZones = 256;
		static constexpr uint32_t MaxHistoryZones = 3000000;
		static constexpr uint32_t MinZonesPerThread = 256;
		static constexpr uint32_t MaxZonesPerThread = 65536;
		// Run-time names stored at most between Init and Shutdown (InternName); past it, their zones are dropped.
		static constexpr uint32_t MaxInternedNames = 65536;
		// Entries a recording's or a capture's name table holds at most: every run-time name and as many names with
		// static storage. Zones whose name finds the table full are dropped.
		static constexpr uint32_t MaxCaptureNames = 2 * MaxInternedNames;
		// Thread entries one recording makes at most (threads that took a buffer while no free entry of their name was
		// left); zones of threads past it are dropped. Thread churn this large is a defect of the program profiled.
		static constexpr uint32_t MaxRecordedThreads = 1024;
		// Renderers whose GPU passes one recording keeps at most (RecordGpuPasses); passes of further views are dropped.
		static constexpr uint32_t MaxGpuViews = 16;
		// Longest name in bytes (a run-time name, a view, any name a recording stores); longer names are cut at a UTF-8
		// character boundary.
		static constexpr uint32_t MaxNameBytes = 256;
		// Longest thread name in bytes; longer names are cut at a UTF-8 character boundary.
		static constexpr uint32_t MaxThreadNameBytes = 63;
		// Largest capture file a capture's reader accepts and its writer writes.
		static constexpr uint64_t MaxCaptureBytes = 512ull * 1024 * 1024;

		// --- Lifetime and recording. Main thread only. ---

		// Names the calling thread "Main" and makes it the thread the main-thread functions belong to.
		static void Init();
		// Stops a recording in progress (Stop), then frees the history and the stored names. Other threads may still be
		// running and recording: Shutdown waits only for zone ends already writing into a buffer (a few instructions each)
		// and frees nothing those writers can still reach. Init may be called again afterwards.
		static void Shutdown();
		static bool IsInitialized();

		// Starts a recording: clears the history and prepares the threads' buffers. Refuses when the profiler is not
		// initialized, a recording is in progress, the settings are out of bounds, or in Dist builds. The recording's
		// first frame is the next one BeginFrame begins: a frame in progress when Start is called is not recorded, and
		// zones that end before the first frame begins are discarded when it begins.
		[[nodiscard]] static Result<void> Start(ProfilerSettings const& settings = {});
		// Ends the recording. The history is kept (GetRecording) until the next Start or Shutdown; zones that ended since
		// the last EndFrame, and the frame in progress, are discarded. Does nothing when not recording.
		static void Stop();

		// Begins a frame: gives it the next index. Called by the main loop at the top of each iteration, and by tests and
		// tools that run frames of their own. Counts frames even when not recording. Frames do not nest: a BeginFrame while
		// a frame is in progress ends that frame first, as EndFrame would.
		static void BeginFrame();
		// Ends the frame begun last: when the frame is recorded, collects every thread's buffer into it, appends it to the
		// history and attaches the GPU passes that waited for it. Does nothing when no frame is in progress.
		static void EndFrame();
		// The index BeginFrame gave the frame in progress (or the last one begun); 0 before the first.
		static uint64_t GetFrameIndex();

		// Attaches GPU pass durations to the frame whose GPU work they measured (its GetFrameIndex when the work was
		// recorded). Passes of a recorded frame still in progress wait for its end; passes of a frame the history no
		// longer holds, or that was not recorded, are dropped. view names the renderer (copied).
		static void RecordGpuPasses(uint64_t frameIndex, std::string_view view, std::span<ProfilerGpuPassTiming const> passes);

		// The history of the current or last recording. The reference stays valid until the next call of Start, Stop,
		// EndFrame, RecordGpuPasses or Shutdown.
		static ProfilerCapture const& GetRecording();
		static ProfilerStatus GetStatus();

		// --- Any thread. ---

		// Whether a recording is in progress: one relaxed atomic load. Renderers measure GPU passes only while it is true.
		static bool IsRecording() { return s_Recording.load(std::memory_order_relaxed); }
		// Nanoseconds of the monotonic clock zones are timed with.
		static int64_t Now();

		// Begins a zone: while recording, reads the clock and the thread's depth; otherwise returns an empty token after
		// one predictable branch. name must have static storage duration (a string literal, __func__, or InternName's
		// result); a null name records nothing. Neither allocates (but see the class comment on Apple platforms), locks
		// nor logs: safe on the audio callback.
		static ProfilerZoneToken BeginZone(char const* name)
		{
			if (!IsRecording()) [[likely]]
			{
				return {};
			}
			return BeginRecordedZone(name);
		}
		// Ends a zone begun by BeginZone on the same thread; zones of a thread end in the reverse order they began. An
		// empty token costs one branch. Otherwise the zone is written into the thread's buffer, taking a free buffer on the
		// thread's first zone of a recording. Neither allocates, locks nor logs: safe on the audio callback.
		static void EndZone(ProfilerZoneToken const& token)
		{
			if (token.Name == nullptr) [[likely]]
			{
				return;
			}
			EndRecordedZone(token);
		}

		// Begins a zone whose name is known only at run time: stores the name (InternName) and begins the zone. The token
		// takes the recording before the name is stored, so that a name a later Shutdown frees can never reach a recording.
		// Returns an empty token when not recording or when the name cannot be stored (counted as a dropped zone). Locks
		// and may allocate: never on the audio callback. ST_PROFILE_SCOPE_DYNAMIC calls it only while recording.
		static ProfilerZoneToken BeginDynamicZone(std::string_view name);

		// Stores a name known only at run time (a script class, a graph) and returns a copy that lives until Shutdown;
		// equal names return the same pointer. Returns null when the profiler is not initialized or the store is full, and
		// then, while recording, counts the zone the name was for as dropped. Locks and may allocate: never on the audio
		// callback.
		static char const* InternName(std::string_view name);

		// Names the calling thread in recordings. Takes effect when the thread next takes a buffer (its first zone of a
		// recording, or its first after EndThread), so threads name themselves before their first zone. Copies into
		// storage of the thread's own; neither allocates (but see the class comment on Apple platforms) nor locks.
		static void SetThreadName(std::string_view name);
		// Ends the calling thread in the profiler: it forgets its name and its buffer, and the buffer is free again once the
		// main thread has collected it. Called by a thread about to end, and at the end of every task a pooled or
		// std::async thread runs (ST_PROFILE_THREAD), so that the thread's next task takes a buffer of its own under its own
		// name. A thread that ends without calling it (one the engine does not own, such as miniaudio's device thread)
		// keeps its buffer until the recording stops. Neither allocates nor locks.
		static void EndThread();

	private:
		static ProfilerZoneToken BeginRecordedZone(char const* name);
		static ProfilerZoneToken BeginZoneOfRecording(char const* name, uint32_t recording);
		static void EndRecordedZone(ProfilerZoneToken const& token);

		// Read by IsRecording and the zone functions on every thread; constant-initialized and never destroyed, so threads
		// that outlive Shutdown read it safely.
		static std::atomic<bool> s_Recording;
	};

	// Records the zone of its own scope: BeginZone on construction, EndZone on destruction. Any thread.
	class ProfileZone
	{
	public:
		// name: as for Profiler::BeginZone. A null name records nothing.
		explicit ProfileZone(char const* name)
			: m_Token(Profiler::BeginZone(name))
		{
		}

		// A zone begun already (Profiler::BeginDynamicZone), ended with the scope.
		explicit ProfileZone(ProfilerZoneToken const& token)
			: m_Token(token)
		{
		}

		~ProfileZone() { Profiler::EndZone(m_Token); }

		ProfileZone(ProfileZone const&) = delete;
		ProfileZone& operator=(ProfileZone const&) = delete;

		// Attaches a count to the zone (the steps a graph's VM ran), written with it when it ends. label has static
		// storage duration. Does nothing when the zone records nothing.
		void SetValue(char const* label, uint64_t value)
		{
			if (m_Token.Name != nullptr)
			{
				m_Token.ValueLabel = label;
				m_Token.Value = value;
			}
		}

	private:
		ProfilerZoneToken m_Token;
	};

	// Names the calling thread for its whole body, or for one task of a pooled thread, and ends it in the profiler when
	// the scope ends (Profiler::SetThreadName, Profiler::EndThread). Declared first in a thread's function or a task's
	// body; every std::async or pooled task body that can reach a zone opens one.
	class ProfilerThreadScope
	{
	public:
		explicit ProfilerThreadScope(std::string_view name) { Profiler::SetThreadName(name); }
		~ProfilerThreadScope() { Profiler::EndThread(); }

		ProfilerThreadScope(ProfilerThreadScope const&) = delete;
		ProfilerThreadScope& operator=(ProfilerThreadScope const&) = delete;
	};
}

#define ST_PROFILE_CONCAT_DETAIL(a, b) a##b
#define ST_PROFILE_CONCAT(a, b) ST_PROFILE_CONCAT_DETAIL(a, b)

#ifdef ST_ENABLE_PROFILING

// A zone around the rest of the enclosing scope. name: a string with static storage duration, written
// "<Subsystem>.<Phase>" for the zones a subsystem's time is read from ("Physics.Step").
#define ST_PROFILE_SCOPE(name) ::Strada::ProfileZone const ST_PROFILE_CONCAT(stProfileZone, __LINE__)(name)
// A zone named after the enclosing function (__func__, unqualified).
#define ST_PROFILE_FUNCTION() ST_PROFILE_SCOPE(__func__)
// A zone whose name is known only at run time (anything convertible to std::string_view). The name is evaluated and
// stored only while recording.
#define ST_PROFILE_SCOPE_DYNAMIC(name)                                      \
	::Strada::ProfileZone const ST_PROFILE_CONCAT(stProfileZone, __LINE__)( \
		::Strada::Profiler::IsRecording() ? ::Strada::Profiler::BeginDynamicZone(name) : ::Strada::ProfilerZoneToken{})
// A zone held by a named variable, so that ST_PROFILE_ZONE_VALUE can attach a count to it.
#define ST_PROFILE_ZONE(variable, name) ::Strada::ProfileZone variable(name)
#define ST_PROFILE_ZONE_DYNAMIC(variable, name)                                                                   \
	::Strada::ProfileZone variable(::Strada::Profiler::IsRecording() ? ::Strada::Profiler::BeginDynamicZone(name) \
	                                                                 : ::Strada::ProfilerZoneToken{})
// Attaches a count to a zone declared by ST_PROFILE_ZONE: label is a string literal, value an unsigned integer.
#define ST_PROFILE_ZONE_VALUE(variable, label, value) (variable).SetValue(label, static_cast<uint64_t>(value))
// Names the calling thread for the rest of its function (or task) and ends it in the profiler when that returns.
#define ST_PROFILE_THREAD(name) ::Strada::ProfilerThreadScope const ST_PROFILE_CONCAT(stProfileThread, __LINE__)(name)

#else

// The arguments are referenced in a lambda that is never called: they are never evaluated, yet a local or a function
// used only by a zone stays used (sizeof would leave Clang to report such a function as never needed).
#define ST_PROFILE_DISCARD_DETAIL(expression) \
	static_cast<void>(                        \
		[&]()                                 \
		{                                     \
			static_cast<void>(expression);    \
		})
#define ST_PROFILE_SCOPE(name) ST_PROFILE_DISCARD_DETAIL(name)
#define ST_PROFILE_FUNCTION() static_cast<void>(0)
#define ST_PROFILE_SCOPE_DYNAMIC(name) ST_PROFILE_DISCARD_DETAIL(name)
#define ST_PROFILE_ZONE(variable, name) ST_PROFILE_DISCARD_DETAIL(name)
#define ST_PROFILE_ZONE_DYNAMIC(variable, name) ST_PROFILE_DISCARD_DETAIL(name)
#define ST_PROFILE_ZONE_VALUE(variable, label, value) \
	static_cast<void>(                                \
		[&]()                                         \
		{                                             \
			static_cast<void>(label);                 \
			static_cast<void>(value);                 \
		})
#define ST_PROFILE_THREAD(name) ST_PROFILE_DISCARD_DETAIL(name)

#endif
