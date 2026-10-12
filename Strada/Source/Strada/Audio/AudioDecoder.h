#pragma once

// Internal to the Audio module.

#include "Strada/Asset/AudioClipAsset.h"
#include "Strada/Audio/AudioStreamFile.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"

#include <miniaudio.h>

#include <extras/decoders/libvorbis/miniaudio_libvorbis.h>

#include <array>
#include <cstdint>
#include <limits>
#include <span>

namespace Strada
{
	// Decodes WAV, FLAC, MP3 or Ogg Vorbis data of a known format into 32-bit float frames in the data's own channels and
	// sample rate; read it as a miniaudio data source. Only the decoder of the format reads the data: miniaudio's for WAV,
	// FLAC and MP3, libvorbis for Ogg Vorbis. (miniaudio's own setup tries its other decoders on data the expected one
	// refuses, its MP3 decoder reading a whole file in search of a frame, and when told the format it refuses data that a
	// custom decoder such as libvorbis opened.) Used by one thread at a time.
	class AudioDecoder
	{
		struct PrivateTag
		{
		};

	public:
		// No limit on the bytes Create reads.
		static constexpr uint64_t NoReadLimit = std::numeric_limits<uint64_t>::max();

		// Reads the headers of the file's data, which the decoder reads from then on (the file must outlive the decoder),
		// reading at most headerReadLimit bytes while it does. The error says why the data cannot be decoded.
		[[nodiscard]] static Result<Scope<AudioDecoder>> Create(AudioFormat format, AudioStreamFile& file,
		                                                        uint64_t headerReadLimit = NoReadLimit);

		explicit AudioDecoder(PrivateTag);
		~AudioDecoder();

		AudioDecoder(AudioDecoder const&) = delete;
		AudioDecoder& operator=(AudioDecoder const&) = delete;

		ma_data_source* GetDataSource();
		uint32_t GetChannelCount() const { return m_ChannelCount; }
		uint32_t GetSampleRate() const { return m_SampleRate; }
		// The speaker of each channel.
		std::span<ma_channel const> GetChannelMap() const { return std::span<ma_channel const>(m_ChannelMap.data(), m_ChannelCount); }

		// How the decoders reach the file: miniaudio's file system callbacks first (miniaudio finds them at the pointer it is
		// given; libvorbis gets the same pointer with its own callbacks), then the file, which every open returns.
		struct FileAccess
		{
			ma_vfs_callbacks Callbacks;
			AudioStreamFile* File = nullptr;
			// Bytes reads may still return; once used up, reads find the end of the file and LimitReached is set.
			uint64_t ReadsLeft = NoReadLimit;
			bool LimitReached = false;
		};

	private:
		FileAccess m_FileAccess{};
		ma_decoder m_Decoder{};
		ma_libvorbis m_Vorbis{};
		bool m_HasDecoder = false;
		bool m_HasVorbis = false;
		uint32_t m_ChannelCount = 0;
		uint32_t m_SampleRate = 0;
		std::array<ma_channel, MA_MAX_CHANNELS> m_ChannelMap{};
	};

	// Audio data in memory read like a file (the data must outlive it).
	class AudioMemoryFile final : public AudioStreamFile
	{
	public:
		explicit AudioMemoryFile(std::span<uint8_t const> data);

		size_t Read(std::span<uint8_t> data) override;
		bool Seek(uint64_t position) override;
		uint64_t GetPosition() const override { return m_Position; }
		uint64_t GetSize() const override { return m_Data.size(); }

	private:
		std::span<uint8_t const> m_Data;
		uint64_t m_Position = 0;
	};
}
