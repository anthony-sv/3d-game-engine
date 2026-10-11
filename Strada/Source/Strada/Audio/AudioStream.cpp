#include "stpch.h"
#include "Strada/Audio/AudioStream.h"

#include "Strada/Asset/AudioClipAsset.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Core/FileSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <system_error>

namespace Strada
{
	namespace
	{
		// Decoded samples a stream keeps at most (4 MB): files with many channels or a high sample rate buffer less time.
		constexpr uint64_t MaxBufferedSamples = uint64_t(1) << 20;
		constexpr uint64_t MinBufferedFrames = 1024;
		// One visit of the streaming thread decodes at most this share of a stream's buffer, then moves on to the others.
		constexpr uint64_t VisitShare = 4;
		// How often the streaming thread looks at full buffers again (they last about a second).
		constexpr std::chrono::milliseconds PollInterval(20);
		// Gaps of a stream are reported at most this often.
		constexpr std::chrono::seconds GapReportInterval(5);
		// How long the mixer tries for the lock (a few microseconds) before it plays silence: the streaming thread holds it
		// only to move a few positions.
		constexpr uint32_t MixerLockAttempts = 4096;
		// Decoders such as libvorbis report damaged data as a failed read and continue after it on the next read: the file
		// counts as ended after this many failed reads in a row.
		constexpr uint32_t MaxFailedReads = 8;
	}

	Result<Ref<AudioStream>> AudioStream::Open(std::filesystem::path const& path, AudioStreamFileOpener const& opener)
	{
		std::string name = FileSystem::PathToUtf8(path);
		Result<Scope<AudioStreamFile>> opened = opener ? opener(path) : AudioStreamFile::Open(path);
		if (!opened)
		{
			return Error{opened.GetError()};
		}
		Scope<AudioStreamFile> file = opened.TakeValue();
		if (!file)
		{
			return MakeError("{} cannot be opened", name);
		}

		// The format from the first bytes, as for clips: files in other formats are refused before a decoder reads them.
		std::array<uint8_t, 16> header{};
		size_t const headerSize = std::min(file->Read(header), header.size());
		AudioFormat const format = DetectAudioFormat(std::span<uint8_t const>(header.data(), headerSize));
		if (format == AudioFormat::Unknown)
		{
			return MakeError("{} is not in a supported audio format (WAV, FLAC, MP3 or Ogg Vorbis)", name);
		}
		if (!file->Seek(0))
		{
			return MakeError("{} cannot be read", name);
		}

		Ref<AudioStream> stream = CreateRef<AudioStream>(PrivateTag{}, std::move(file), std::move(name));
		if (Result<void> initialized = stream->Initialize(format); !initialized)
		{
			return Error{initialized.GetError()};
		}
		return stream;
	}

	AudioStream::AudioStream(PrivateTag, Scope<AudioStreamFile> file, std::string name)
		: m_Name(std::move(name)),
		  m_File(std::move(file))
	{
	}

	AudioStream::~AudioStream()
	{
		// No sound reads the data source any more (sounds are uninitialized before they release their stream).
		if (m_HasDataSource)
		{
			ma_data_source_uninit(&m_DataSource.Base);
		}
	}

	Result<void> AudioStream::Initialize(AudioFormat format)
	{
		Result<Scope<AudioDecoder>> decoder = AudioDecoder::Create(format, *m_File, MaxAudioStreamHeaderBytes);
		if (!decoder)
		{
			return MakeError("the {} file {} cannot be decoded: {}", AudioFormatToString(format), m_Name, decoder.GetError());
		}
		m_Decoder = decoder.TakeValue();
		m_Channels = m_Decoder->GetChannelCount();

		auto const frames = static_cast<uint64_t>(std::ceil(static_cast<double>(AudioStreamBufferSeconds) * m_Decoder->GetSampleRate()));
		m_Capacity = std::clamp(frames, MinBufferedFrames, std::max(MaxBufferedSamples / m_Channels, MinBufferedFrames));
		m_Samples.assign(m_Capacity * m_Channels, 0.0f);

		// Ranges and loop points are not supported: miniaudio leaves the reads to the stream, which loops by itself.
		static constexpr ma_data_source_vtable DataSourceVTable = {
			.onRead = OnRead,
			.onSeek = OnSeek,
			.onGetDataFormat = OnGetDataFormat,
			.onGetCursor = nullptr,
			.onGetLength = nullptr,
			.onSetLooping = nullptr,
			.flags = MA_DATA_SOURCE_SELF_MANAGED_RANGE_AND_LOOP_POINT,
		};
		ma_data_source_config dataSourceConfig = ma_data_source_config_init();
		dataSourceConfig.vtable = &DataSourceVTable;
		if (ma_result const result = ma_data_source_init(&dataSourceConfig, &m_DataSource.Base); result != MA_SUCCESS)
		{
			return MakeError("the stream of {} cannot be created: {}", m_Name, ma_result_description(result));
		}
		m_DataSource.Stream = this;
		m_HasDataSource = true;
		return {};
	}

	ma_data_source* AudioStream::GetDataSource()
	{
		return &m_DataSource.Base;
	}

	void AudioStream::Seek(uint64_t frame)
	{
		m_SeekTarget.store(frame, std::memory_order_relaxed);
		m_SeekRequests.fetch_add(1, std::memory_order_release);
	}

	bool AudioStream::TryLockBriefly()
	{
		for (uint32_t attempt = 0; attempt < MixerLockAttempts; attempt++)
		{
			if (!m_Locked.load(std::memory_order_relaxed) && !m_Locked.exchange(true, std::memory_order_acquire))
			{
				return true;
			}
		}
		return false;
	}

	void AudioStream::Lock()
	{
		// The mixer holds it while it copies samples.
		while (m_Locked.exchange(true, std::memory_order_acquire))
		{
			std::this_thread::yield();
		}
	}

	void AudioStream::Unlock()
	{
		m_Locked.store(false, std::memory_order_release);
	}

	uint64_t AudioStream::Read(float* output, uint64_t frameCount, bool& atEnd)
	{
		atEnd = false;
		auto const silence = [this, output, frameCount](uint64_t from)
		{
			if (output != nullptr)
			{
				std::fill(output + from * m_Channels, output + frameCount * m_Channels, 0.0f);
			}
			return frameCount;
		};
		if (!TryLockBriefly())
		{
			// The streaming thread was interrupted while it held the lock.
			return silence(0);
		}
		if (uint32_t const requests = m_SeekRequests.load(std::memory_order_acquire); requests != m_SeeksHandled)
		{
			if (!SeekWithinBuffer(m_SeekTarget.load(std::memory_order_relaxed)))
			{
				// The streaming thread empties the buffer and decodes from there.
				Unlock();
				return silence(0);
			}
			m_SeeksHandled = requests;
		}

		bool const looping = ma_data_source_is_looping(&m_DataSource.Base) == MA_TRUE;
		uint64_t position = m_ReadPosition.load(std::memory_order_relaxed);
		uint64_t frames = 0;
		while (frames < frameCount)
		{
			// The write position first: the file ends it covers are then visible too.
			uint64_t const written = m_WritePosition.load(std::memory_order_acquire);
			uint32_t const endsRead = m_FileEndsRead.load(std::memory_order_relaxed);
			uint32_t const endsWritten = m_FileEndsWritten.load(std::memory_order_acquire);
			uint64_t available = written - position;
			if (endsRead != endsWritten)
			{
				FileEnd const& end = m_FileEnds[endsRead % MaxFileEnds];
				if (end.Position == position)
				{
					if (end.Final || !looping)
					{
						atEnd = true;
						break;
					}
					// Looping: the start of the file follows.
					m_FileEndsRead.store(endsRead + 1, std::memory_order_release);
					m_FileCursor = 0;
					continue;
				}
				available = std::min(available, end.Position - position);
			}
			if (available == 0)
			{
				// Not decoded yet: silence now, and the sound continues from here once it is.
				if (m_Filled.load(std::memory_order_acquire))
				{
					m_Gaps.fetch_add(1, std::memory_order_relaxed);
				}
				frames = silence(frames);
				break;
			}

			uint64_t const count = std::min(available, frameCount - frames);
			if (output != nullptr)
			{
				uint64_t const start = position % m_Capacity;
				uint64_t const first = std::min(count, m_Capacity - start);
				float* const destination = output + frames * m_Channels;
				std::memcpy(destination, m_Samples.data() + start * m_Channels, first * m_Channels * sizeof(float));
				std::memcpy(destination + first * m_Channels, m_Samples.data(), (count - first) * m_Channels * sizeof(float));
			}
			position += count;
			frames += count;
			m_FileCursor += count;
		}
		m_ReadPosition.store(position, std::memory_order_release);
		Unlock();
		return frames;
	}

	bool AudioStream::SeekWithinBuffer(uint64_t frame)
	{
		if (frame == m_FileCursor)
		{
			return true;
		}
		if (frame != 0)
		{
			return false;
		}
		uint32_t const endsRead = m_FileEndsRead.load(std::memory_order_relaxed);
		if (endsRead == m_FileEndsWritten.load(std::memory_order_acquire))
		{
			return false;
		}
		FileEnd const& end = m_FileEnds[endsRead % MaxFileEnds];
		if (end.Final || end.Position != m_ReadPosition.load(std::memory_order_relaxed))
		{
			return false;
		}
		// At the end of the file, whose start the buffer holds next.
		m_FileEndsRead.store(endsRead + 1, std::memory_order_release);
		m_FileCursor = 0;
		return true;
	}

	void AudioStream::EmptyBuffer(uint64_t frame)
	{
		// The mixer finds the buffer empty, without file ends: it plays silence until the decoder has moved and filled it.
		m_WritePosition.store(0, std::memory_order_relaxed);
		m_ReadPosition.store(0, std::memory_order_relaxed);
		m_FileEndsWritten.store(0, std::memory_order_relaxed);
		m_FileEndsRead.store(0, std::memory_order_relaxed);
		m_Filled.store(false, std::memory_order_relaxed);
		m_FileCursor = frame;
	}

	void AudioStream::MoveDecoder(uint64_t frame)
	{
		m_FramesSinceMove = 0;
		m_MovedToStart = frame == 0;
		m_FailedReads = 0;
		m_Exhausted = false;
		if (ma_data_source_seek_to_pcm_frame(m_Decoder->GetDataSource(), frame) != MA_SUCCESS)
		{
			// Nothing can be played from there (the buffer was just emptied, so there is room for the mark).
			MarkFileEnd(m_WritePosition.load(std::memory_order_relaxed), true);
			m_Exhausted = true;
		}
	}

	void AudioStream::MarkFileEnd(uint64_t position, bool final)
	{
		uint32_t const endsWritten = m_FileEndsWritten.load(std::memory_order_relaxed);
		m_FileEnds[endsWritten % MaxFileEnds] = {position, final};
		m_FileEndsWritten.store(endsWritten + 1, std::memory_order_release);
	}

	bool AudioStream::Service()
	{
		bool worked = false;
		if (uint32_t const requests = m_SeekRequests.load(std::memory_order_acquire); requests != m_SeeksSeen)
		{
			std::optional<uint64_t> emptiedFor;
			Lock();
			uint32_t const pending = m_SeekRequests.load(std::memory_order_acquire);
			if (pending != m_SeeksHandled)
			{
				uint64_t const target = m_SeekTarget.load(std::memory_order_relaxed);
				if (!SeekWithinBuffer(target))
				{
					EmptyBuffer(target);
					emptiedFor = target;
				}
				m_SeeksHandled = pending;
			}
			Unlock();
			m_SeeksSeen = pending;
			if (emptiedFor)
			{
				MoveDecoder(*emptiedFor);
			}
			worked = true;
		}
		worked |= Fill();
		ReportGaps();
		return worked;
	}

	bool AudioStream::Fill()
	{
		if (m_Exhausted)
		{
			return false;
		}
		bool worked = false;
		ma_data_source* const source = m_Decoder->GetDataSource();
		uint64_t written = m_WritePosition.load(std::memory_order_relaxed);
		uint64_t const free = m_Capacity - (written - m_ReadPosition.load(std::memory_order_acquire));
		uint64_t budget = std::min(free, std::max<uint64_t>(m_Capacity / VisitShare, 1));
		while (budget > 0)
		{
			uint64_t const start = written % m_Capacity;
			uint64_t const count = std::min(budget, m_Capacity - start);
			ma_uint64 decoded = 0;
			// What was decoded counts even when the rest of the read failed.
			ma_result const result = ma_data_source_read_pcm_frames(source, m_Samples.data() + start * m_Channels, count, &decoded);
			decoded = std::min<ma_uint64>(decoded, count);
			if (decoded > 0)
			{
				written += decoded;
				budget -= decoded;
				m_FramesSinceMove += decoded;
				m_FailedReads = 0;
				m_WritePosition.store(written, std::memory_order_release);
				worked = true;
				continue;
			}
			if (result != MA_SUCCESS && result != MA_AT_END && ++m_FailedReads < MaxFailedReads)
			{
				continue;
			}
			m_FailedReads = 0;

			// The end of the file, or of what can be decoded of it: marked, then decoding goes on from the start.
			if (m_FileEndsWritten.load(std::memory_order_relaxed) - m_FileEndsRead.load(std::memory_order_acquire) >= MaxFileEnds)
			{
				// The mixer has yet to reach the ends marked so far (a very short file).
				break;
			}
			bool const empty = m_MovedToStart && m_FramesSinceMove == 0;
			bool const rewound = !empty && ma_data_source_seek_to_pcm_frame(source, 0) == MA_SUCCESS;
			MarkFileEnd(written, !rewound);
			worked = true;
			if (!rewound)
			{
				m_Exhausted = true;
				break;
			}
			m_FramesSinceMove = 0;
			m_MovedToStart = true;
		}
		if (!m_Filled.load(std::memory_order_relaxed) &&
		    (m_Exhausted || written - m_ReadPosition.load(std::memory_order_acquire) == m_Capacity))
		{
			m_Filled.store(true, std::memory_order_release);
		}
		return worked;
	}

	void AudioStream::ReportGaps()
	{
		m_UnreportedGaps += m_Gaps.exchange(0, std::memory_order_relaxed);
		if (m_UnreportedGaps == 0)
		{
			return;
		}
		auto const now = std::chrono::steady_clock::now();
		if (!m_LastGapReport || now - *m_LastGapReport >= GapReportInterval)
		{
			ST_CORE_WARN("The streamed sound {} ran out of decoded audio {} times: its file is read too slowly", m_Name, m_UnreportedGaps);
			m_UnreportedGaps = 0;
			m_LastGapReport = now;
		}
	}

	ma_result AudioStream::OnRead(ma_data_source* dataSource, void* output, ma_uint64 frameCount, ma_uint64* framesRead)
	{
		bool atEnd = false;
		uint64_t const frames = static_cast<DataSource*>(dataSource)->Stream->Read(static_cast<float*>(output), frameCount, atEnd);
		if (framesRead != nullptr)
		{
			*framesRead = frames;
		}
		return atEnd ? MA_AT_END : MA_SUCCESS;
	}

	ma_result AudioStream::OnSeek(ma_data_source* dataSource, ma_uint64 frame)
	{
		static_cast<DataSource*>(dataSource)->Stream->Seek(frame);
		return MA_SUCCESS;
	}

	ma_result AudioStream::OnGetDataFormat(ma_data_source* dataSource, ma_format* format, ma_uint32* channels, ma_uint32* sampleRate,
	                                       ma_channel* channelMap, size_t channelMapCapacity)
	{
		AudioDecoder const& decoder = *static_cast<DataSource*>(dataSource)->Stream->m_Decoder;
		if (format != nullptr)
		{
			*format = ma_format_f32;
		}
		if (channels != nullptr)
		{
			*channels = decoder.GetChannelCount();
		}
		if (sampleRate != nullptr)
		{
			*sampleRate = decoder.GetSampleRate();
		}
		if (channelMap != nullptr)
		{
			std::span<ma_channel const> const map = decoder.GetChannelMap();
			ma_channel_map_copy_or_default(channelMap, channelMapCapacity, map.data(), static_cast<ma_uint32>(map.size()));
		}
		return MA_SUCCESS;
	}

	Result<Scope<AudioStreamer>> AudioStreamer::Create()
	{
		Scope<AudioStreamer> streamer = CreateScope<AudioStreamer>(PrivateTag{});
		try
		{
			streamer->m_Thread = std::thread(&AudioStreamer::Run, streamer.get());
		}
		catch (std::system_error const& error)
		{
			return MakeError("the audio streaming thread cannot be started: {}", error.what());
		}
		return streamer;
	}

	AudioStreamer::AudioStreamer(PrivateTag)
	{
	}

	AudioStreamer::~AudioStreamer()
	{
		{
			std::lock_guard const lock(m_Mutex);
			m_Running = false;
		}
		m_WakeCondition.notify_all();
		m_IdleCondition.notify_all();
		if (m_Thread.joinable())
		{
			m_Thread.join();
		}
	}

	void AudioStreamer::Add(Ref<AudioStream> stream)
	{
		{
			std::lock_guard const lock(m_Mutex);
			m_Streams.push_back(std::move(stream));
			m_WakeRequested = true;
		}
		m_WakeCondition.notify_one();
	}

	void AudioStreamer::Remove(AudioStream const& stream)
	{
		// Released after the lock (or by the streaming thread, when it is visiting the stream).
		Ref<AudioStream> removed;
		std::lock_guard const lock(m_Mutex);
		auto const it = std::find_if(m_Streams.begin(), m_Streams.end(),
		                             [&stream](Ref<AudioStream> const& candidate)
		                             {
										 return candidate.get() == &stream;
									 });
		if (it != m_Streams.end())
		{
			removed = std::move(*it);
			m_Streams.erase(it);
		}
	}

	void AudioStreamer::Wake()
	{
		{
			std::lock_guard const lock(m_Mutex);
			m_WakeRequested = true;
		}
		m_WakeCondition.notify_one();
	}

	void AudioStreamer::WaitUntilIdle()
	{
		std::unique_lock lock(m_Mutex);
		uint64_t const after = m_PassesStarted;
		m_WakeRequested = true;
		m_WakeCondition.notify_one();
		m_IdleCondition.wait(lock,
		                     [this, after]
		                     {
								 return m_LastIdlePass > after || !m_Running;
							 });
	}

	void AudioStreamer::Run()
	{
		std::vector<Ref<AudioStream>> streams;
		std::unique_lock lock(m_Mutex);
		while (m_Running)
		{
			streams = m_Streams;
			uint64_t const pass = ++m_PassesStarted;
			m_WakeRequested = false;
			lock.unlock();

			bool worked = false;
			for (Ref<AudioStream> const& stream : streams)
			{
				worked |= stream->Service();
			}
			// Streams removed meanwhile are destroyed here, outside the lock.
			streams.clear();

			lock.lock();
			if (!worked)
			{
				m_LastIdlePass = pass;
				m_IdleCondition.notify_all();
				// The buffers are full: look again once the mixer has played some of them, or when woken.
				auto const woken = [this]
				{
					return m_WakeRequested || !m_Running;
				};
				if (m_Streams.empty())
				{
					m_WakeCondition.wait(lock, woken);
				}
				else
				{
					m_WakeCondition.wait_for(lock, PollInterval, woken);
				}
			}
		}
	}
}
