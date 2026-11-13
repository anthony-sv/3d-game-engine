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
#include <vector>

// The engine's CPU and GPU profiler (Docs/Architecture.md §4.4). Zones are recorded by any thread into per-thread
// buffers and collected by the main thread once per frame into a bounded history, which the Profiler panel, the
// profiler.* commands and captures (Serialization/ProfilerCaptureSerializer.h, the Chrome trace event format) read.
//
// Zones are compiled in Debug and Release builds. Dist builds compile the macros below to nothing (their arguments are
// referenced but never evaluated, so a local used only as a zone name raises no unused-variable warning); the Profiler
// class itself is compiled in every configuration, and Start refuses in Dist builds. A translation unit may define
// ST_DISABLE_PROFILING before its first include to compile the Dist form of the macros in any configuration.
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
		// Zones open on the thread when this one began.
		uint32_t Depth = 0;
	};

	// --- Recorded data: what the history holds and what a capture file stores. Plain data, read on the main thread. ---

	// Names (zones, value labels, views, GPU passes) are indices into ProfilerCapture::Names; this one means none.
	inline constexpr uint32_t ProfilerNoName = std::numeric_limits<uint32_t>::max();

	// A zone that ended on a thread. Times are in nanoseconds from the start of the recording (a zone that began before
	// its frame, or before the recording, starts earlier).
	struct ProfilerZone
	{
		uint32_t Name = ProfilerNoName;
		// Index into ProfilerCapture::Threads.
		uint32_t Thread = 0;
		// Zones that were open on the thread when this one began. A zone whose parent was dropped (a full buffer) has no
		// zone of a smaller depth around it: consumers that build trees place it at their thread's root.
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
		// Counted by BeginFrame since Profiler::Init, from 1.
		uint64_t Index = 0;
		int64_t StartNanoseconds = 0;
		int64_t EndNanoseconds = 0;
		// The zones collected at this frame's end: every zone that ended since the previous collection, whichever thread
		// recorded it. Ordered by thread, then by start time, then by depth, so each thread's zones are in pre-order.
		std::vector<ProfilerZone> Zones;
		// In the order the renderers measured them. A frame's passes arrive frames later; frames whose GPU work was not
		// measured have none.
		std::vector<ProfilerGpuPass> GpuPasses;
		// Zones lost because a thread's buffer was full, a thread found no free buffer, or a name could not be stored.
		uint32_t DroppedZones = 0;
	};

	struct ProfilerThread
	{
		// The name the thread gave itself (Profiler::SetThreadName), or "Thread <n>".
		std::string Name;
	};

	// Frames with the tables their zones index: the recorder's history (Profiler::GetRecording) or a capture opened again.
	struct ProfilerCapture
	{
		std::vector<std::string> Names;
		// One entry per buffer a thread took during the recording, in the order threads took them.
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
		// Threads that recorded zones during the recording.
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

	struct ProfilerMedians
	{
		// Frame durations (EndNanoseconds - StartNanoseconds).
		double FrameMilliseconds = 0.0;
		// Per subsystem (GetProfilerSubsystem), sorted by name: per frame, the summed durations of the subsystem's outermost
		// zones on every thread. Subsystems overlap where zones of one nest in another's.
		std::vector<ProfilerMedian> Subsystems;
		// Per view and pass ("<view>/<pass>"), sorted by name, over the frames that measured the pass.
		std::vector<ProfilerMedian> GpuPasses;
	};

	// The subsystem of a zone name written "<Subsystem>.<Phase>" ("Physics.Step" is in "Physics"): the text before the
	// first '.', or empty when the name has none (a zone named by ST_PROFILE_FUNCTION, a script class, a graph), in which
	// case the zone belongs to the subsystem of its nearest enclosing zone that has one. Pure; any thread.
	[[nodiscard]] std::string_view GetProfilerSubsystem(std::string_view zoneName);

	// Medians of a capture's frames (the figures StradaRuntime --profile logs at exit). Zones without a subsystem and
	// without an enclosing zone that has one count as "Other". Pure; any thread.
	[[nodiscard]] ProfilerMedians ComputeProfilerMedians(ProfilerCapture const& capture);

	// The recorder. A static facade (Init/Shutdown); Application initializes it before every other subsystem and shuts
	// it down after them. Every function is safe to call before Init and after Shutdown: zones then record nothing,
	// Start refuses and the main-thread functions do nothing.
	class Profiler
	{
	public:
		// Buffers for threads that record during one recording; a thread finding none free drops its zones.
		static constexpr uint32_t MaxThreads = 64;
		static constexpr uint32_t MinHistoryFrames = 1;
		static constexpr uint32_t MaxHistoryFrames = 36000;
		static constexpr uint32_t MinZonesPerThread = 256;
		static constexpr uint32_t MaxZonesPerThread = 65536;
		// Zones the history holds at most, over all its frames: past it, the oldest frames are dropped as when the
		// history is full. Captures hold at most as many.
		static constexpr uint64_t MaxHeldZones = 4000000;
		// Run-time names stored at most between Init and Shutdown (InternName); past it, their zones are dropped.
		static constexpr uint32_t MaxInternedNames = 65536;
		// Longest run-time name in bytes; longer names are cut at a UTF-8 character boundary.
		static constexpr uint32_t MaxNameBytes = 256;
		// Longest thread name in bytes; longer names are cut at a UTF-8 character boundary.
		static constexpr uint32_t MaxThreadNameBytes = 63;

		// --- Lifetime and recording. Main thread only. ---

		// Names the calling thread "Main" and makes it the thread the main-thread functions belong to.
		static void Init();
		// Stops a recording in progress (Stop), then frees the history and the stored names. Other threads may still be
		// running and recording: Shutdown waits only for zone ends already writing into a buffer (a few instructions each)
		// and frees nothing those writers can still reach. Init may be called again afterwards.
		static void Shutdown();
		static bool IsInitialized();

		// Starts a recording: clears the history and prepares the threads' buffers. Refuses when the profiler is not
		// initialized, a recording is in progress, the settings are out of bounds, or in Dist builds.
		[[nodiscard]] static Result<void> Start(ProfilerSettings const& settings = {});
		// Ends the recording. The history is kept (GetRecording) until the next Start or Shutdown; zones that ended since
		// the last EndFrame, and the frame in progress, are discarded. Does nothing when not recording.
		static void Stop();

		// Begins a frame: gives it the next index. Called by the main loop at the top of each iteration, and by tests and
		// tools that run frames of their own. Counts frames even when not recording.
		static void BeginFrame();
		// Ends the frame begun last: while recording, collects every thread's buffer into it and appends it to the
		// history; then attaches GPU passes that waited for it. Does nothing when no frame is in progress.
		static void EndFrame();
		// The index BeginFrame gave the frame in progress (or the last one begun); 0 before the first.
		static uint64_t GetFrameIndex();

		// Attaches GPU pass durations to the frame whose GPU work they measured (its GetFrameIndex when the work was
		// recorded). Passes of a frame still in progress wait for its end; passes of a frame the history no longer holds,
		// or that was not recorded, are dropped. view names the renderer (copied).
		static void RecordGpuPasses(uint64_t frameIndex, std::string_view view, std::span<ProfilerGpuPassTiming const> passes);

		// The history of the current or last recording. The reference stays valid until the next call of Start, Stop,
		// EndFrame, RecordGpuPasses or Shutdown.
		static ProfilerCapture const& GetRecording();
		static ProfilerStatus GetStatus();

		// --- Any thread. ---

		// Whether a recording is in progress: one relaxed atomic load. Renderers measure GPU passes only while it is true.
		static bool IsRecording();
		// Nanoseconds of the monotonic clock zones are timed with.
		static int64_t Now();

		// Begins a zone: while recording, reads the clock and the thread's depth; otherwise returns an empty token after
		// one predictable branch. name must have static storage duration (a string literal, __func__, or InternName's
		// result). Neither allocates nor locks: safe on the audio callback.
		static ProfilerZoneToken BeginZone(char const* name);
		// Ends a zone begun by BeginZone on the same thread; zones of a thread end in the reverse order they began. An
		// empty token costs one branch. Otherwise the zone is written into the thread's buffer, taking a free buffer on the
		// thread's first zone of a recording. Neither allocates nor locks: safe on the audio callback.
		static void EndZone(ProfilerZoneToken const& token);

		// Stores a name known only at run time (a script class, a graph) and returns a copy that lives until Shutdown;
		// equal names return the same pointer. Returns null when the profiler is not initialized or the store is full.
		// Locks and may allocate: never on the audio callback. ST_PROFILE_SCOPE_DYNAMIC calls it only while recording.
		static char const* InternName(std::string_view name);

		// Names the calling thread in recordings. Takes effect when the thread next takes a buffer (its first zone of a
		// recording), so threads name themselves before their first zone. Copies into storage of the thread's own;
		// neither allocates nor locks.
		static void SetThreadName(std::string_view name);
		// Called by a thread that is about to end: its buffer returns to the free buffers once the main thread has collected
		// it. A thread that ends without calling it (one the engine does not own, such as miniaudio's device thread) keeps
		// its buffer until the recording stops. Neither allocates nor locks.
		static void EndThread();

	private:
		// Read by IsRecording and the zone functions on every thread; constant-initialized and never destroyed, so threads
		// that outlive Shutdown read it safely.
		static std::atomic<bool> s_Recording;
	};

	// Records the zone of its own scope: BeginZone on construction, EndZone on destruction. Any thread.
	class ProfileZone
	{
	public:
		// name: as for Profiler::BeginZone. A null name records nothing.
		explicit ProfileZone(char const* name);
		~ProfileZone();

		ProfileZone(ProfileZone const&) = delete;
		ProfileZone& operator=(ProfileZone const&) = delete;

		// Attaches a count to the zone (the steps a graph's VM ran), written with it when it ends. label has static
		// storage duration. Does nothing when the zone records nothing.
		void SetValue(char const* label, uint64_t value);

	private:
		ProfilerZoneToken m_Token;
	};

	// Names the calling thread for its whole body and ends it in the profiler when the body returns
	// (Profiler::SetThreadName, Profiler::EndThread). Declared first in a thread's function.
	class ProfilerThreadScope
	{
	public:
		explicit ProfilerThreadScope(std::string_view name);
		~ProfilerThreadScope();

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
		::Strada::Profiler::IsRecording() ? ::Strada::Profiler::InternName(name) : nullptr)
// A zone held by a named variable, so that ST_PROFILE_ZONE_VALUE can attach a count to it.
#define ST_PROFILE_ZONE(variable, name) ::Strada::ProfileZone variable(name)
#define ST_PROFILE_ZONE_DYNAMIC(variable, name) \
	::Strada::ProfileZone variable(::Strada::Profiler::IsRecording() ? ::Strada::Profiler::InternName(name) : nullptr)
// Attaches a count to a zone declared by ST_PROFILE_ZONE: label is a string literal, value an unsigned integer.
#define ST_PROFILE_ZONE_VALUE(variable, label, value) (variable).SetValue(label, static_cast<uint64_t>(value))
// Names the calling thread for the rest of its function and ends it in the profiler when the function returns.
#define ST_PROFILE_THREAD(name) ::Strada::ProfilerThreadScope const ST_PROFILE_CONCAT(stProfileThread, __LINE__)(name)

#else

#define ST_PROFILE_SCOPE(name) static_cast<void>(sizeof(name))
#define ST_PROFILE_FUNCTION() static_cast<void>(0)
#define ST_PROFILE_SCOPE_DYNAMIC(name) static_cast<void>(sizeof(name))
#define ST_PROFILE_ZONE(variable, name) static_cast<void>(sizeof(name))
#define ST_PROFILE_ZONE_DYNAMIC(variable, name) static_cast<void>(sizeof(name))
#define ST_PROFILE_ZONE_VALUE(variable, label, value) static_cast<void>(sizeof(label) + sizeof(value))
#define ST_PROFILE_THREAD(name) static_cast<void>(sizeof(name))

#endif
