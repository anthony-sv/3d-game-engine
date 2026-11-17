#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

namespace Strada
{
	// Writes a WAV file of 16-bit PCM as samples arrive (the player's --audio-capture, Docs/Architecture.md §9.8): the header
	// first with sizes of 0, the samples appended, the sizes written into the header by Finish. Memory stays that of one
	// write whatever the length. Any thread, one at a time.
	class WaveFileWriter
	{
		struct PrivateTag
		{
		};

	public:
		// The RIFF header's sizes are 32-bit: a file holds at most this many bytes of samples (about 6.2 hours of 48 kHz stereo).
		static constexpr uint64_t MaxDataBytes = 0xFFFFFFFFull - 36;
		// The bytes before the samples: the RIFF header, the "fmt " chunk of PCM and the "data" chunk's header.
		static constexpr uint32_t HeaderBytes = 44;

		// Creates (or replaces) the file and writes its header. Fails when the file cannot be opened or written, or the format
		// is not one a WAV file can say (no channels, a rate of 0, more than 65535 channels).
		[[nodiscard]] static Result<Scope<WaveFileWriter>> Create(std::filesystem::path const& path, uint32_t sampleRate,
		                                                          uint32_t channelCount);

		WaveFileWriter(PrivateTag, std::ofstream stream, uint32_t sampleRate, uint32_t channelCount);
		WaveFileWriter(WaveFileWriter const&) = delete;
		WaveFileWriter& operator=(WaveFileWriter const&) = delete;

		// Appends interleaved samples, a whole number of frames, each clamped to -1 to 1 and scaled by 32767, rounded to the
		// nearest (NaN is 0), little-endian. Fails when the file cannot be written or would pass MaxDataBytes; nothing is
		// written after a failure.
		[[nodiscard]] Result<void> Write(std::span<float const> samples);
		// Writes the sizes into the header and closes the file; Write and Finish fail after it.
		[[nodiscard]] Result<void> Finish();

		uint64_t GetFrameCount() const { return m_DataBytes / (2ull * m_ChannelCount); }
		bool IsFinished() const { return m_Finished; }

		// The 16-bit value a sample is written as.
		static int16_t ToPcm16(float sample);

	private:
		std::ofstream m_Stream;
		uint32_t m_SampleRate = 0;
		uint32_t m_ChannelCount = 0;
		uint64_t m_DataBytes = 0;
		bool m_Failed = false;
		bool m_Finished = false;
		// Reused by Write, so that writing allocates only when a write is longer than any before.
		std::vector<uint8_t> m_Bytes;
	};
}
