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
// (laps run between the marks "Lap <n>" and "Lap <n + 1>"), recorded by any thread and collected by the main thread at
// the end of each fixed step (Scene::RunFixedStep's last call, Scene::SampleTelemetry) into bounded sessions, which the
// editor's Telemetry panel, the telemetry.* commands and CSV exports (Serialization/TelemetryCsv.h) read. It records
// only while a recording runs (the editor starts one while its panel is open or a telemetry.start recording runs; the
// player starts none); an idle recorder costs its callers one branch.
//
// The scripting boundary: Strada.Telemetry.Record(string, float), Mark(string), MarkStep(string, ulong) and IsRecording
// (Strada-ScriptCore's Telemetry.cs) reach Record, Mark, MarkStep and IsRecording below through the bindings
// Telemetry_Record(char const* channel, int32_t length, float value), Telemetry_Mark(char const* label, int32_t length),
// Telemetry_MarkStep(char const* label, int32_t length, uint64_t step) and uint8_t Telemetry_IsRecording()
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
		// Bytes the sessions' columns and marks hold over every session (TelemetrySession::Bytes), oldest sessions dropped
		// first; the live session alone past it drops its further values and marks and counts them.
		uint64_t HistoryBytes = 64ull * 1024 * 1024;
		// Capacity of the buffer of each thread but the main one, in records (values and marks) between two collections;
		// a thread that records more drops the rest, and the session counts them.
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

	// A channel of a session: one value per row at most, in a column that begins at the row of its first value and ends
	// at the row of its last. The column is stored in blocks of BlockCells cells that never move once made, so that a
	// growing column never copies what it holds; read it through GetTelemetryValue and GetTelemetryCells.
	struct TelemetryChannel
	{
		static constexpr uint32_t BlockCells = 4096;

		// 1 to Telemetry::MaxNameBytes bytes of UTF-8 without control characters (IsTelemetryName); unique in its session.
		std::string Name;
		// The row of the column's first cell.
		uint64_t FirstRow = 0;
		// Rows the column covers from FirstRow: to the row of the channel's last value.
		uint64_t Cells = 0;
		// The column: block b holds rows FirstRow + b * BlockCells on. Each block is made with room for BlockCells cells and
		// never moves; every block but the last holds BlockCells cells, the last those up to the column's end. A NaN cell
		// is a row without a value (a recorded value is always finite).
		std::vector<std::vector<float>> Blocks;
		// Values recorded (cells that are not NaN), and the smallest, largest and last of them.
		uint64_t Count = 0;
		float Minimum = 0.0f;
		float Maximum = 0.0f;
		float Last = 0.0f;
	};

	// A label on a row (Telemetry::Mark, Telemetry::MarkStep).
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
		// What it holds against TelemetrySettings::HistoryBytes: every block its columns made (4 bytes a cell, whole
		// blocks, a block made ahead of a column's need included while the session is live), and per mark its label's
		// bytes and sizeof(TelemetryMark).
		uint64_t Bytes = 0;
		// What the session lost: values (a full buffer, a thread without a buffer, a name refused, a value not finite, a
		// channel past MaxChannels, a step past MaxSteps or the bytes past HistoryBytes), marks (the same, a step named
		// before the session or never reached, or past MaxMarks) and steps (past MaxSteps).
		uint64_t DroppedValues = 0;
		uint64_t DroppedMarks = 0;
		uint64_t DroppedSteps = 0;
		// Whether further steps of its scene can still extend it: the newest session while its recording runs and no
		// EndSession, Stop or step that began another ended it.
		bool Live = false;
	};

	// A lap of a session (FindTelemetryLaps): from a mark "Lap <n>" to the next lap boundary on a later row when that
	// boundary is "Lap <n + 1>". A lap boundary is a mark labelled exactly "Lap <n>" (n a decimal number from 1, without
	// sign or leading zeros: ParseTelemetryLapNumber) or "Lap end" (TelemetryLapEndLabel). Any other boundary after
	// "Lap <n>" ends the lap in progress without making a lap: "Lap end" (a race finished, abandoned or restarted), a
	// number that is not the next one, or "Lap <n + 1>" on the same row. Other marks ("Sector 1.2", "Lapse") are not
	// boundaries.
	struct TelemetryLap
	{
		// The label of the mark that begins it ("Lap 2"), and its number.
		std::string Label;
		uint32_t Number = 0;
		// The rows of the marks that begin and end it (for the lap in progress, FindTelemetryLapInProgress, the session's
		// last row).
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
		// The sessions' Bytes summed.
		uint64_t BytesHeld = 0;
		// The live session's Id, 0 when none is.
		uint32_t LiveSession = 0;
		// Over the recording in progress or the last one, every session of it counted (held or not).
		uint64_t DroppedValues = 0;
		uint64_t DroppedMarks = 0;
		uint64_t DroppedSteps = 0;
		// Threads' buffers allocated now: none while not recording.
		uint32_t Buffers = 0;
	};

	// The label that ends a lap in progress without making a lap (TelemetryLap).
	inline constexpr std::string_view TelemetryLapEndLabel = "Lap end";

	// The seconds of a row: its step's number times the session's StepSeconds (the simulation's time at the end of that
	// step, which no frame rate changes). Pure; any thread.
	[[nodiscard]] double GetTelemetryTime(TelemetrySession const& session, uint64_t row);
	// The channel of that name, or null. Pure; any thread.
	[[nodiscard]] TelemetryChannel const* FindTelemetryChannel(TelemetrySession const& session, std::string_view name);
	// The channel's value at a row, if it recorded one there. Pure; any thread.
	[[nodiscard]] std::optional<float> GetTelemetryValue(TelemetryChannel const& channel, uint64_t row);
	// The channel's cells from a row to the end of that row's block or of the column, whichever comes first (NaN where a
	// row has no value); empty for a row outside the column. A reader walks a range by spans. Pure; any thread.
	[[nodiscard]] std::span<float const> GetTelemetryCells(TelemetryChannel const& channel, uint64_t row);
	// n of a label "Lap <n>" (TelemetryLap), or none. Pure; any thread.
	[[nodiscard]] std::optional<uint32_t> ParseTelemetryLapNumber(std::string_view label);
	// The session's laps in row order (TelemetryLap). Pure; any thread.
	[[nodiscard]] std::vector<TelemetryLap> FindTelemetryLaps(TelemetrySession const& session);
	// The lap in progress: the last "Lap <n>" mark when no boundary follows it, up to the session's last row; none
	// otherwise. Pure; any thread.
	[[nodiscard]] std::optional<TelemetryLap> FindTelemetryLapInProgress(TelemetrySession const& session);
	// The index of the shortest lap, the first of equal ones; none for no lap. Pure; any thread.
	[[nodiscard]] std::optional<size_t> FindBestTelemetryLap(std::span<TelemetryLap const> laps);
	// Whether a text may name a channel or a mark: 1 to Telemetry::MaxNameBytes bytes of valid UTF-8 without control
	// characters (U+0000 to U+001F and U+007F). Pure; any thread.
	[[nodiscard]] bool IsTelemetryName(std::string_view text);

	// The recorder. A static facade (Init/Shutdown); Application initializes it right after the profiler and shuts it
	// down right before it. Every function is safe to call before Init and after Shutdown: records are then dropped
	// uncounted, Start refuses and the main-thread functions do nothing.
	//
	// The main thread (the one that called Init) records straight into the row the next step collects; every other
	// thread records into a buffer of its own. Threads keep a little thread-local state (the buffer they took and the
	// session it belongs to), trivially destructible and constant-initialized, so on Windows and Linux reaching it never
	// allocates; on Apple platforms a thread's first access to thread-local storage allocates once (Profiler's class
	// comment). Telemetry is never recorded on the audio device's callback.
	class Telemetry
	{
	public:
		// Buffers for the threads but the main one that record during one session; a thread finding none free drops its
		// records, counted. A thread keeps its buffer until its session ends (EndSession, Stop, a step that begins
		// another session: every buffer is then freed for the next session's threads) or it calls EndThread.
		static constexpr uint32_t MaxThreads = 64;
		static constexpr uint32_t MinChannels = 64;
		static constexpr uint32_t MaxChannelsLimit = 4096;
		static constexpr uint32_t MinSteps = 1;
		static constexpr uint32_t MaxStepsLimit = 8640000;
		static constexpr uint32_t MinHistorySessions = 1;
		static constexpr uint32_t MaxHistorySessions = 64;
		static constexpr uint64_t MinHistoryBytes = 1ull * 1024 * 1024;
		static constexpr uint64_t MaxHistoryBytes = 1024ull * 1024 * 1024;
		static constexpr uint32_t MinRecordsPerThread = 256;
		static constexpr uint32_t MaxRecordsPerThread = 65536;
		// Longest channel name and mark label in bytes. A longer one is refused, never cut, so that two names never
		// become one channel.
		static constexpr uint32_t MaxNameBytes = 64;
		// Marks one session holds at most, those waiting for a step still to come included; further marks are dropped
		// and counted.
		static constexpr uint32_t MaxMarks = 65536;

		// --- Lifetime and recording. Main thread only. ---

		// Makes the calling thread the recorder's main thread.
		static void Init();
		// Stops a recording in progress, then frees the sessions. Other threads may still be recording: Shutdown waits
		// only for records already being written into a buffer and frees nothing those writers can still reach.
		static void Shutdown();
		static bool IsInitialized();

		// Starts a recording and prepares the threads' buffers. The sessions held are kept, the oldest dropped first past
		// the new settings' HistorySessions and HistoryBytes. Refuses when not initialized, when a recording is in
		// progress, or on settings out of bounds. The recording's first session begins at the next fixed step (EndStep):
		// what any thread records from now until then lands in its first row.
		[[nodiscard]] static Result<void> Start(TelemetrySettings const& settings = {});
		// Ends the recording and its live session. The sessions are kept (GetSessions) until Start drops them past its
		// bounds or Shutdown; records not yet collected are discarded. Does nothing when not recording.
		static void Stop();

		// Ends the fixed step `step` describes: while recording, collects what every thread recorded since the last
		// collection into the step's row of the live session, after beginning a new session when none is live or the
		// step is not the next one of the live session's scene (another scene, a scene started again, a snapshot
		// restored, another step length): a channel recorded several times keeps its last value (per thread; between
		// threads unspecified), and every mark. Within a session, rows are consecutive steps. Called by
		// Scene::SampleTelemetry once per fixed step, after the scene's own channels are recorded; does nothing when not
		// recording.
		static void EndStep(TelemetryStep const& step);
		// Ends the live session (the editor's play mode, when the game stops): the next step begins a new one. Records
		// not yet collected are discarded, and marks waiting for a step still to come are dropped and counted. Does
		// nothing when no session is live.
		static void EndSession();

		// The sessions held, oldest first. The reference stays valid until the next call of Start, Stop, EndStep,
		// EndSession or Shutdown, or of MarkStep on the main thread (which marks a held row at once).
		static std::deque<TelemetrySession> const& GetSessions();
		// The session of that Id, or null; valid as GetSessions' reference is.
		static TelemetrySession const* FindSession(uint32_t id);
		static TelemetryStatus GetStatus();

		// --- Any thread. ---

		// Whether a recording is in progress: one relaxed atomic load.
		static bool IsRecording() { return s_Recording.load(std::memory_order_relaxed); }

		// Records a value of a channel for the current fixed step: one recorded during a step (scripts' OnFixedUpdate,
		// the scene's own channels at the step's end) is that step's, one recorded between steps (OnUpdate, events, a
		// pause at time scale 0, however long) is the next step's, and the last value of a channel recorded for a row is
		// its value. A record another thread writes while a step runs lands in that step's row or the next one's. Not
		// recording, it returns after one predictable branch. On the main thread it then writes the value into the
		// pending row (a lookup by name; it allocates only for a channel new to the session); on any other thread it
		// copies the name (at most MaxNameBytes bytes are kept, and a longer name is marked refused) and the value into
		// the thread's buffer, taking a free buffer on the thread's first record of a session, without locking,
		// logging or allocating (Apple platforms allocate once per thread). A name that is not IsTelemetryName, a value
		// that is not finite and a channel past MaxChannels are dropped and counted, and the first drop of each kind in
		// a session is logged by the collection as a warning naming it. Values of one channel recorded by two threads
		// for one row keep one of them, unspecified: a channel has one writer.
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
			RecordMark(label, 0);
		}
		// Marks the row of a given fixed step (a scene's count of fixed steps from 1, as TelemetryStep::Step and
		// ContactEvent::Step count them): what happened at a step a handler learns of only after the frame's steps (a
		// trigger crossed, Strada.Collision.Step) is marked where it happened, whatever the frame rate. The live session's
		// row of that step gets the mark, before the marks collected later on that row; a step still to come gets it when
		// its row is collected; a step before the session began, one the session never reaches, and step 0 drop the mark,
		// counted. On the main thread a step the live session already holds is marked at once (a mark made just before
		// EndSession stays). Checked and costed as Record.
		static void MarkStep(std::string_view label, uint64_t step)
		{
			if (!IsRecording()) [[likely]]
			{
				return;
			}
			RecordMark(label, step == 0 ? InvalidMarkStep : step);
		}
		// Gives the calling thread's buffer back before its session ends: what it recorded is collected at the next
		// EndStep, which then frees the buffer, and the thread's next record takes a free one. Does nothing for a thread
		// that holds no buffer (the main thread holds none).
		static void EndThread();

	private:
		// RecordMark's step for a mark of the current step (Mark) and for MarkStep's refused step 0.
		static constexpr uint64_t InvalidMarkStep = ~0ull;

		static void RecordValue(std::string_view channel, float value);
		// step: 0 for the current step (Mark), InvalidMarkStep for MarkStep's step 0, else MarkStep's step.
		static void RecordMark(std::string_view label, uint64_t step);

		// Read by IsRecording on every thread; constant-initialized and never destroyed, so threads that outlive Shutdown
		// read it safely.
		static std::atomic<bool> s_Recording;
	};
}
