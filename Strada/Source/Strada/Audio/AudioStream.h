#pragma once

// Internal to the Audio module.

#include "Strada/Audio/AudioDecoder.h"
#include "Strada/Audio/AudioStreamFile.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"

#include <miniaudio.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Strada
{
	// A sound file played from a ring buffer that the audio streaming thread (AudioStreamer) keeps decoded about a second
	// ahead of the playback (AudioStreamBufferSeconds). The thread that mixes the sound reads its data source, which only
	// copies decoded samples and never waits: when a slow read leaves the buffer empty, this sound is silent for a moment and
	// continues where it was, while every other sound plays on.
	//
	// The streaming thread decodes on past the end of the file from its start, marking where the file ended, so the mixer
	// decides there whether the sound loops: looping is seamless, and a sound that ended starts over from samples already
	// buffered. Other seeks empty the buffer, which the streaming thread refills while the mixer plays silence. The two
	// threads share a lock only to move positions, never while the file is read.
	class AudioStream
	{
		struct PrivateTag
		{
		};

	public:
		// Opens the file and reads its format and headers, at most MaxAudioStreamHeaderBytes of it (main thread).
		[[nodiscard]] static Result<Ref<AudioStream>> Open(std::filesystem::path const& path, AudioStreamFileOpener const& opener);

		AudioStream(PrivateTag, Scope<AudioStreamFile> file, std::string name);
		~AudioStream();

		AudioStream(AudioStream const&) = delete;
		AudioStream& operator=(AudioStream const&) = delete;

		// What the sound plays (a miniaudio data source, read by the thread that mixes it).
		ma_data_source* GetDataSource();
		// Plays from this frame of the file next. Any thread.
		void Seek(uint64_t frame);

		// Streaming thread: carries out a seek the mixer could not, decodes more of the file into the buffer and reports
		// gaps. Returns whether there was anything to do.
		bool Service();

	private:
		// The data source miniaudio reads: its base first, as miniaudio requires.
		struct DataSource
		{
			ma_data_source_base Base;
			AudioStream* Stream = nullptr;
		};

		// Where the file ended in the buffer (a position of the ring). Final: nothing follows (the file cannot be decoded
		// from its start again).
		struct FileEnd
		{
			uint64_t Position = 0;
			bool Final = false;
		};

		static constexpr uint32_t MaxFileEnds = 128;

		Result<void> Initialize(AudioFormat format);

		// Mixing thread. Copies up to frameCount decoded frames to output (when not null), silence where there are none yet;
		// atEnd when the sound reached the end of the file without looping.
		uint64_t Read(float* output, uint64_t frameCount, bool& atEnd);
		// Under the lock: a seek that needs no new samples, to where the mixer is or to the start of the file when the mixer
		// is at its end. Returns whether it was one.
		bool SeekWithinBuffer(uint64_t frame);
		// Streaming thread, under the lock: empties the buffer for a seek to the frame.
		void EmptyBuffer(uint64_t frame);
		// Streaming thread: moves the decoder to the frame, or marks a final end where it cannot.
		void MoveDecoder(uint64_t frame);
		// Streaming thread: decodes into the free part of the buffer, at most a share of it.
		bool Fill();
		void MarkFileEnd(uint64_t position, bool final);
		void ReportGaps();
		// The streaming thread waits for the lock; the mixer only tries for a moment.
		void Lock();
		bool TryLockBriefly();
		void Unlock();

		static ma_result OnRead(ma_data_source* dataSource, void* output, ma_uint64 frameCount, ma_uint64* framesRead);
		static ma_result OnSeek(ma_data_source* dataSource, ma_uint64 frame);
		static ma_result OnGetDataFormat(ma_data_source* dataSource, ma_format* format, ma_uint32* channels, ma_uint32* sampleRate,
		                                 ma_channel* channelMap, size_t channelMapCapacity);

		std::string m_Name;
		Scope<AudioStreamFile> m_File;
		Scope<AudioDecoder> m_Decoder;
		DataSource m_DataSource{};
		bool m_HasDataSource = false;
		uint32_t m_Channels = 0;

		// The ring of decoded frames: positions count frames since the buffer was last emptied. The streaming thread
		// writes, the mixer reads.
		std::vector<float> m_Samples;
		uint64_t m_Capacity = 0;
		std::atomic<uint64_t> m_WritePosition{0};
		std::atomic<uint64_t> m_ReadPosition{0};
		std::array<FileEnd, MaxFileEnds> m_FileEnds{};
		std::atomic<uint32_t> m_FileEndsWritten{0};
		std::atomic<uint32_t> m_FileEndsRead{0};

		// Held by the mixer while it reads and by the streaming thread while it handles a seek.
		std::atomic<bool> m_Locked{false};
		// Seeks: the latest target and how many were asked for. Read and handled under the lock.
		std::atomic<uint64_t> m_SeekTarget{0};
		std::atomic<uint32_t> m_SeekRequests{0};
		uint32_t m_SeeksHandled = 0;
		// The mixer's position in the file (under the lock).
		uint64_t m_FileCursor = 0;

		// Streaming thread only: the seeks it looked at, frames decoded since the decoder last moved, whether it then moved
		// to the start, failed reads in a row, and whether the file cannot be decoded further.
		uint32_t m_SeeksSeen = 0;
		uint64_t m_FramesSinceMove = 0;
		bool m_MovedToStart = true;
		uint32_t m_FailedReads = 0;
		bool m_Exhausted = false;

		// Gaps: reads that found nothing decoded once the buffer had been full. The streaming thread reports them.
		std::atomic<bool> m_Filled{false};
		std::atomic<uint32_t> m_Gaps{0};
		uint32_t m_UnreportedGaps = 0;
		std::optional<std::chrono::steady_clock::time_point> m_LastGapReport;
	};

	// The audio streaming thread: keeps the buffers of the registered streams filled, visiting each in turn, and sleeps
	// while they are full. Owned by AudioEngine; its functions are called on the main thread.
	class AudioStreamer
	{
		struct PrivateTag
		{
		};

	public:
		// Fails when the thread cannot be started.
		[[nodiscard]] static Result<Scope<AudioStreamer>> Create();

		explicit AudioStreamer(PrivateTag);
		~AudioStreamer();

		AudioStreamer(AudioStreamer const&) = delete;
		AudioStreamer& operator=(AudioStreamer const&) = delete;

		void Add(Ref<AudioStream> stream);
		// The thread may still be visiting the stream; then it releases it when the visit ends.
		void Remove(AudioStream const& stream);
		// Visits the streams now (a stream has a seek to carry out).
		void Wake();
		// Returns once a visit of every stream that began after the call found nothing to do (which waits for as long as a
		// read of a file is held up).
		void WaitUntilIdle();

	private:
		void Run();

		std::mutex m_Mutex;
		std::condition_variable m_WakeCondition;
		std::condition_variable m_IdleCondition;
		std::vector<Ref<AudioStream>> m_Streams;
		bool m_Running = true;
		bool m_WakeRequested = false;
		uint64_t m_PassesStarted = 0;
		uint64_t m_LastIdlePass = 0;
		std::thread m_Thread;
	};
}
