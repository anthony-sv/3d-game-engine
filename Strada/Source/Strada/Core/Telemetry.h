#pragma once

#include "Strada/Core/Result.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The engine's telemetry recorder (Docs/Architecture.md §4.5): named channels of one float per fixed step, and marks
// (laps are the spans between marks whose label begins with "Lap"), recorded by any thread into per-thread buffers and
// collected by the main thread at the end of each fixed step (Scene::RunFixedStep's last call, Scene::SampleTelemetry)
// into bounded sessions, which the editor's Telemetry panel, the telemetry.* commands and CSV exports
// (Serialization/TelemetryCsv.h) read. It records only while a recording runs (the editor starts one while its panel is
// open or a telemetry.start recording runs; the player starts none); an idle recorder costs its callers one branch.
//
// The scripting boundary: Strada.Telemetry.Record(string, float), Mark(string) and IsRecording (Strada-ScriptCore's
// Telemetry.cs) reach Record, Mark and IsRecording below through the bindings Telemetry_Record(char const* channel,
// int32_t length, float value), Telemetry_Mark(char const* label, int32_t length) and uint8_t Telemetry_IsRecording()
// (ScriptBindingsRuntime.cpp and InternalCalls.cs); the managed side asks IsRecording before it encodes a name, so an
// idle Record costs scripts one native call. Scripts may call them from any thread.
namespace Strada
{
	// Limits of a recording (Telemetry::Start). The bounds of each are Telemetry's Min... and Max... constants.
	struct TelemetrySettings
	{
		// Channels one session holds at most; values of further channels are dropped and counted.
		uint32_t MaxChannels = 256;
		// Rows (fixed steps) one session holds at most (216,000: an hour at 60 steps a second); the values and marks of
		// further steps are dropped, and the steps counted.
		uint32_t MaxSteps = 216000;
		// Sessions kept over recordings, oldest dropped first (never the live one).
		uint32_t HistorySessions = 8;
		// Samples (cells of the channels' columns, gaps included: 4 bytes each) kept over every session, oldest sessions
		// dropped first; the live session alone past it drops its further values and counts them.
		uint64_t HistorySamples = 16ull * 1024 * 1024;
		// Capacity of each thread's buffer, in records (values and marks) between two collections; a thread that records
		// more drops the rest, and the session counts them.
		uint32_t RecordsPerThread = 4096;
	};

	// A fixed step as the scene that ran it hands it to the recorder (Telemetry::EndStep).
	struct TelemetryStep
	{
		// The scene that ran the step (Scene::GetSerial, never 0): a session holds the steps of one scene.
		uint64_t Source = 0;
		// The fixed steps the scene has run, this one counted (Scene::GetFixedStepCount after the step): from 1.
		uint64_t Step = 0;
		// The fixed step's length in seconds (Scene::GetFixedTimestep), positive.
		float StepSeconds = 0.0f;
	};

	// A channel of a session: one value per row at most, in a column that begins at the row of its first value.
	struct TelemetryChannel
	{
		// 1 to Telemetry::MaxNameBytes bytes of UTF-8 without control characters (IsTelemetryName); unique in its session.
		std::string Name;
		// The row of Values[0].
		uint64_t FirstRow = 0;
		// One cell per row from FirstRow to the row of the channel's last value; a NaN cell is a row without a value (a
		// recorded value is always finite).
		std::vector<float> Values;
		// Values recorded (cells that are not NaN), and the smallest, largest and last of them.
		uint64_t Count = 0;
		float Minimum = 0.0f;
		float Maximum = 0.0f;
		float Last = 0.0f;
	};

	// A label on a row (Telemetry::Mark).
	struct TelemetryMark
	{
		uint64_t Row = 0;
		// 1 to Telemetry::MaxNameBytes bytes of UTF-8 without control characters (IsTelemetryName).
		std::string Label;
	};

	// The rows one scene's consecutive fixed steps recorded. Row r is step FirstStep + r, at GetTelemetryTime(session, r).
	struct TelemetrySession
	{
		// Counted from 1 from Telemetry::Init on: a number names one session until Shutdown.
		uint32_t Id = 0;
		// The TelemetryStep::Source of its steps.
		uint64_t Source = 0;
		// The step of row 0, from 1.
		uint64_t FirstStep = 0;
		uint64_t Rows = 0;
		float StepSeconds = 0.0f;
		// In the order their first values were collected.
		std::vector<TelemetryChannel> Channels;
		// In row order; a row's marks in the order they were collected.
		std::vector<TelemetryMark> Marks;
		// Cells of Channels' columns.
		uint64_t Samples = 0;
		// What the session lost: values (a full buffer, a name refused, a value not finite, a channel past MaxChannels, a
		// step past MaxSteps or the samples past HistorySamples), marks (the same, or past MaxMarks) and steps (past
		// MaxSteps).
		uint64_t DroppedValues = 0;
		uint64_t DroppedMarks = 0;
		uint64_t DroppedSteps = 0;
		// Whether further steps of its scene can still extend it: the newest session while its recording runs and no
		// EndSession, Stop or step that began another ended it.
		bool Live = false;
	};

	// A lap of a session: from a mark whose label begins with "Lap" to the next such mark, which ends it (and begins the
	// next lap). Two such marks on one row make no lap; the last such mark begins the lap in progress, which is not
	// listed.
	struct TelemetryLap
	{
		// The label of the mark that begins it ("Lap 2").
		std::string Label;
		// The rows of the marks that begin and end it.
		uint64_t StartRow = 0;
		uint64_t EndRow = 0;
		// (EndRow - StartRow) steps of the session's StepSeconds: a whole number of steps, since marks sit on rows.
		double Seconds = 0.0;
	};

	struct TelemetryStatus
	{
		bool Initialized = false;
		bool Recording = false;
		// The settings of the recording in progress or the last one.
		TelemetrySettings Settings;
		uint32_t SessionsHeld = 0;
		uint64_t SamplesHeld = 0;
		// The live session's Id, 0 when none is.
		uint32_t LiveSession = 0;
		// Over the recording in progress or the last one, every session of it counted (held or not).
		uint64_t DroppedValues = 0;
		uint64_t DroppedMarks = 0;
		uint64_t DroppedSteps = 0;
	};

	// The seconds of a row: its step's number times the session's StepSeconds (the simulation's time at the end of that
	// step, which no frame rate changes). Pure; any thread.
	[[nodiscard]] double GetTelemetryTime(TelemetrySession const& session, uint64_t row);
	// The channel of that name, or null. Pure; any thread.
	[[nodiscard]] TelemetryChannel const* FindTelemetryChannel(TelemetrySession const& session, std::string_view name);
	// The channel's value at a row, if it recorded one there. Pure; any thread.
	[[nodiscard]] std::optional<float> GetTelemetryValue(TelemetryChannel const& channel, uint64_t row);
	// The session's laps in row order (TelemetryLap). Pure; any thread.
	[[nodiscard]] std::vector<TelemetryLap> FindTelemetryLaps(TelemetrySession const& session);
	// The index of the shortest lap, the first of equal ones; none for no lap. Pure; any thread.
	[[nodiscard]] std::optional<size_t> FindBestTelemetryLap(std::span<TelemetryLap const> laps);
	// Whether a text may name a channel or a mark: 1 to Telemetry::MaxNameBytes bytes of valid UTF-8 without control
	// characters (U+0000 to U+001F and U+007F). Pure; any thread.
	[[nodiscard]] bool IsTelemetryName(std::string_view text);

	// The recorder. A static facade (Init/Shutdown); Application initializes it right after the profiler and shuts it
	// down right before it. Every function is safe to call before Init and after Shutdown: records are then dropped
	// uncounted, Start refuses and the main-thread functions do nothing.
	//
	// Threads keep a little thread-local state (the buffer they took and the recording it belongs to), trivially
	// destructible and constant-initialized, so on Windows and Linux reaching it never allocates; on Apple platforms a
	// thread's first access to thread-local storage allocates once (Profiler's class comment). Telemetry is never
	// recorded on the audio device's callback.
	class Telemetry
	{
	public:
		// Buffers for threads that record during one recording; a thread finding none free drops its records, counted.
		// A thread keeps its buffer until the recording stops or it calls EndThread.
		static constexpr uint32_t MaxThreads = 64;
		static constexpr uint32_t MinChannels = 64;
		static constexpr uint32_t MaxChannelsLimit = 4096;
		static constexpr uint32_t MinSteps = 1;
		static constexpr uint32_t MaxStepsLimit = 8640000;
		static constexpr uint32_t MinHistorySessions = 1;
		static constexpr uint32_t MaxHistorySessions = 64;
		static constexpr uint64_t MinHistorySamples = 65536;
		static constexpr uint64_t MaxHistorySamples = 256ull * 1024 * 1024;
		static constexpr uint32_t MinRecordsPerThread = 256;
		static constexpr uint32_t MaxRecordsPerThread = 65536;
		// Longest channel name and mark label in bytes. A longer one is refused, never cut, so that two names never
		// become one channel.
		static constexpr uint32_t MaxNameBytes = 64;
		// Marks one session holds at most; further marks are dropped and counted.
		static constexpr uint32_t MaxMarks = 65536;

		// --- Lifetime and recording. Main thread only. ---

		static void Init();
		// Stops a recording in progress, then frees the sessions. Other threads may still be recording: Shutdown waits
		// only for records already being written into a buffer and frees nothing those writers can still reach.
		static void Shutdown();
		static bool IsInitialized();

		// Starts a recording and prepares the threads' buffers. The sessions held are kept, the oldest dropped first past
		// the new settings' HistorySessions and HistorySamples. Refuses when not initialized, when a recording is in
		// progress, or on settings out of bounds. The recording's first session begins at the next fixed step (EndStep):
		// what any thread records from now until then lands in its first row.
		[[nodiscard]] static Result<void> Start(TelemetrySettings const& settings = {});
		// Ends the recording and its live session. The sessions are kept (GetSessions) until Start drops them past its
		// bounds or Shutdown; records not yet collected are discarded. Does nothing when not recording.
		static void Stop();

		// Ends the fixed step `step` describes: while recording, collects every thread's buffer into the step's row of
		// the live session, after beginning a new session when none is live or the step is not the next one of the live
		// session's scene (another scene, a scene started again, a snapshot restored, another step length): what was
		// recorded since the last collection, a channel recorded several times keeping the last value a buffer holds,
		// and every mark. Within a session, rows are consecutive steps. Called by Scene::SampleTelemetry once per fixed
		// step, after the scene's own channels are recorded; does nothing when not recording.
		static void EndStep(TelemetryStep const& step);
		// Ends the live session (the editor's play mode, when the game stops): the next step begins a new one. Records
		// not yet collected are discarded. Does nothing when no session is live.
		static void EndSession();

		// The sessions held, oldest first. The reference stays valid until the next call of Start, Stop, EndStep,
		// EndSession or Shutdown.
		static std::deque<TelemetrySession> const& GetSessions();
		// The session of that Id, or null; valid as GetSessions' reference is.
		static TelemetrySession const* FindSession(uint32_t id);
		static TelemetryStatus GetStatus();

		// --- Any thread. ---

		// Whether a recording is in progress: one relaxed atomic load.
		static bool IsRecording() { return s_Recording.load(std::memory_order_relaxed); }

		// Records a value of a channel for the current fixed step: one recorded during a step (scripts' OnFixedUpdate,
		// the scene's own channels at the step's end) is that step's, one recorded between steps is the next step's, and
		// the last value of a channel collected for a row is its value. A record another thread writes while a step runs
		// lands in that step's row or the next one's. Not recording, it returns after one predictable branch. Otherwise
		// it copies the name (at most MaxNameBytes bytes are kept, and a longer name is marked refused) and the value
		// into the thread's buffer, taking a free buffer on the thread's first record of a recording. Names and values
		// are checked when the main thread collects them: a name that is not IsTelemetryName, a value that is not finite,
		// and a channel past MaxChannels are dropped and counted, and the first drop of each kind in a session is logged
		// as a warning naming it. Neither locks nor logs, and allocates only on Apple platforms, once per thread. Values
		// of one channel recorded by two threads for one row keep one of them, unspecified: a channel has one writer.
		static void Record(std::string_view channel, float value)
		{
			if (!IsRecording()) [[likely]]
			{
				return;
			}
			RecordValue(channel, value);
		}
		// Marks the current fixed step with a label, as Record places its values (a mark between steps marks the next
		// step); a row keeps all its marks, in the order they were collected. Checked and costed as Record.
		static void Mark(std::string_view label)
		{
			if (!IsRecording()) [[likely]]
			{
				return;
			}
			RecordMark(label);
		}
		// Gives the calling thread's buffer back: what it recorded is collected at the next EndStep, which then frees the
		// buffer, and the thread's next record takes a free one. A thread that records and ends before the recording does
		// calls it, so that its buffer serves another thread. Does nothing for a thread that holds no buffer.
		static void EndThread();

	private:
		static void RecordValue(std::string_view channel, float value);
		static void RecordMark(std::string_view label);

		// Read by IsRecording on every thread; constant-initialized and never destroyed, so threads that outlive Shutdown
		// read it safely.
		static std::atomic<bool> s_Recording;
	};
}
