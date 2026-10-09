#pragma once

#include "Strada/Asset/AudioClipAsset.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Core/Buffer.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace Strada::Testing
{
	inline constexpr uint32_t AudioSampleRate = 48000;

	// Initializes the AudioEngine with manual stereo output for as long as it lives: the mix advances only when a test
	// reads it, so playback is deterministic.
	class AudioEngineScope
	{
	public:
		AudioEngineScope()
		{
			AudioEngineSettings settings;
			settings.Output = AudioOutput::Manual;
			settings.SampleRate = AudioSampleRate;
			settings.ChannelCount = 2;
			AudioEngine::Init(settings);
			REQUIRE(AudioEngine::IsInitialized());
		}

		~AudioEngineScope()
		{
			if (AudioEngine::IsInitialized())
			{
				AudioEngine::Shutdown();
			}
		}

		AudioEngineScope(AudioEngineScope const&) = delete;
		AudioEngineScope& operator=(AudioEngineScope const&) = delete;
	};

	inline uint32_t AudioFrames(float seconds)
	{
		return static_cast<uint32_t>(std::lround(seconds * static_cast<float>(AudioSampleRate)));
	}

	// Mono samples holding one level per segment: {{level, seconds}, ...}.
	struct AudioSegment
	{
		float Level = 0.0f;
		float Seconds = 0.0f;
	};

	inline std::vector<float> MakeSamples(std::initializer_list<AudioSegment> segments)
	{
		std::vector<float> samples;
		for (AudioSegment const& segment : segments)
		{
			samples.insert(samples.end(), AudioFrames(segment.Seconds), segment.Level);
		}
		return samples;
	}

	inline uint16_t ToPcm16(float sample)
	{
		long const value = std::clamp(std::lround(sample * 32768.0f), -32768L, 32767L);
		return static_cast<uint16_t>(static_cast<int16_t>(value));
	}

	// A 16-bit PCM mono WAV file.
	inline Buffer MakeWav(std::span<float const> samples, uint32_t sampleRate = AudioSampleRate)
	{
		std::vector<uint8_t> bytes;
		auto const text = [&bytes](char const(&chars)[5])
		{
			bytes.insert(bytes.end(), chars, chars + 4);
		};
		auto const littleEndian = [&bytes](uint32_t value, uint32_t byteCount)
		{
			for (uint32_t index = 0; index < byteCount; index++)
			{
				bytes.push_back(static_cast<uint8_t>((value >> (8 * index)) & 0xFFu));
			}
		};
		uint32_t const dataSize = static_cast<uint32_t>(samples.size() * 2);
		text("RIFF");
		littleEndian(36 + dataSize, 4);
		text("WAVE");
		text("fmt ");
		littleEndian(16, 4);
		littleEndian(1, 2); // PCM
		littleEndian(1, 2); // Mono
		littleEndian(sampleRate, 4);
		littleEndian(sampleRate * 2, 4);
		littleEndian(2, 2);
		littleEndian(16, 2);
		text("data");
		littleEndian(dataSize, 4);
		for (float const sample : samples)
		{
			littleEndian(ToPcm16(sample), 2);
		}
		return Buffer::Copy(bytes.data(), bytes.size());
	}

	// Writes values most significant bit first, as FLAC does.
	class BitWriter
	{
	public:
		void Write(uint64_t value, uint32_t bitCount)
		{
			for (uint32_t bit = bitCount; bit-- > 0;)
			{
				m_Current = static_cast<uint8_t>((m_Current << 1) | ((value >> bit) & 1u));
				if (++m_BitCount == 8)
				{
					m_Bytes.push_back(m_Current);
					m_Current = 0;
					m_BitCount = 0;
				}
			}
		}

		void PadToByte()
		{
			while (m_BitCount != 0)
			{
				Write(0, 1);
			}
		}

		std::vector<uint8_t> TakeBytes()
		{
			REQUIRE(m_BitCount == 0);
			return std::move(m_Bytes);
		}

	private:
		std::vector<uint8_t> m_Bytes;
		uint8_t m_Current = 0;
		uint32_t m_BitCount = 0;
	};

	// FLAC frame header checksum: CRC-8, polynomial x^8 + x^2 + x + 1.
	inline uint8_t FlacCrc8(std::span<uint8_t const> bytes)
	{
		uint32_t crc = 0;
		for (uint8_t const byte : bytes)
		{
			crc ^= byte;
			for (int bit = 0; bit < 8; bit++)
			{
				crc = ((crc & 0x80u) != 0 ? (crc << 1) ^ 0x07u : crc << 1) & 0xFFu;
			}
		}
		return static_cast<uint8_t>(crc);
	}

	// FLAC frame checksum: CRC-16, polynomial x^16 + x^15 + x^2 + 1.
	inline uint16_t FlacCrc16(std::span<uint8_t const> bytes)
	{
		uint32_t crc = 0;
		for (uint8_t const byte : bytes)
		{
			crc ^= static_cast<uint32_t>(byte) << 8;
			for (int bit = 0; bit < 8; bit++)
			{
				crc = ((crc & 0x8000u) != 0 ? (crc << 1) ^ 0x8005u : crc << 1) & 0xFFFFu;
			}
		}
		return static_cast<uint16_t>(crc);
	}

	// A 16-bit mono FLAC file with uncompressed (verbatim) subframes.
	inline Buffer MakeFlac(std::span<float const> samples, uint32_t sampleRate = AudioSampleRate)
	{
		constexpr uint32_t BlockSize = 4096;
		REQUIRE((samples.size() + BlockSize - 1) / BlockSize < 128); // Frame numbers stay single-byte UTF-8.

		BitWriter header;
		header.Write(0x664C6143, 32); // "fLaC"
		// STREAMINFO, the only (last) metadata block.
		header.Write(1, 1);
		header.Write(0, 7);
		header.Write(34, 24);
		header.Write(BlockSize, 16);
		header.Write(BlockSize, 16);
		header.Write(0, 24); // Frame sizes unknown.
		header.Write(0, 24);
		header.Write(sampleRate, 20);
		header.Write(0, 3);  // One channel.
		header.Write(15, 5); // 16 bits per sample.
		header.Write(samples.size(), 36);
		for (int byte = 0; byte < 16; byte++)
		{
			header.Write(0, 8); // MD5 unknown.
		}
		std::vector<uint8_t> bytes = header.TakeBytes();

		for (size_t start = 0, frame = 0; start < samples.size(); start += BlockSize, frame++)
		{
			size_t const count = std::min<size_t>(BlockSize, samples.size() - start);
			BitWriter frameHeader;
			frameHeader.Write(0x3FFE, 14); // Sync code.
			frameHeader.Write(0, 1);
			frameHeader.Write(0, 1);      // Fixed block size.
			frameHeader.Write(0b0111, 4); // Block size in 16 bits after the frame number.
			frameHeader.Write(0b0000, 4); // Sample rate from STREAMINFO.
			frameHeader.Write(0b0000, 4); // Mono.
			frameHeader.Write(0b100, 3);  // 16 bits per sample.
			frameHeader.Write(0, 1);
			frameHeader.Write(frame, 8);
			frameHeader.Write(count - 1, 16);
			std::vector<uint8_t> frameBytes = frameHeader.TakeBytes();
			frameBytes.push_back(FlacCrc8(frameBytes));

			BitWriter subframe;
			subframe.Write(0, 1);
			subframe.Write(0b000001, 6); // Verbatim.
			subframe.Write(0, 1);        // No wasted bits.
			for (size_t index = 0; index < count; index++)
			{
				subframe.Write(ToPcm16(samples[start + index]), 16);
			}
			subframe.PadToByte();
			std::vector<uint8_t> const subframeBytes = subframe.TakeBytes();
			frameBytes.insert(frameBytes.end(), subframeBytes.begin(), subframeBytes.end());
			uint16_t const crc = FlacCrc16(frameBytes);
			frameBytes.push_back(static_cast<uint8_t>(crc >> 8));
			frameBytes.push_back(static_cast<uint8_t>(crc & 0xFFu));
			bytes.insert(bytes.end(), frameBytes.begin(), frameBytes.end());
		}
		return Buffer::Copy(bytes.data(), bytes.size());
	}

	// MPEG-1 Layer III frames (128 kbit/s, 48 kHz, mono, no CRC) of 1152 samples whose zeroed side information and main
	// data decode to silence.
	inline constexpr uint32_t Mp3FrameSamples = 1152;

	inline Buffer MakeSilentMp3(uint32_t frameCount)
	{
		constexpr size_t FrameSize = 144 * 128000 / 48000;
		std::vector<uint8_t> bytes(FrameSize * frameCount, 0);
		for (size_t frame = 0; frame < frameCount; frame++)
		{
			bytes[frame * FrameSize + 0] = 0xFF;
			bytes[frame * FrameSize + 1] = 0xFB;
			bytes[frame * FrameSize + 2] = 0x94;
			bytes[frame * FrameSize + 3] = 0xC0;
		}
		return Buffer::Copy(bytes.data(), bytes.size());
	}

	inline Ref<AudioClipAsset> MakeClip(Buffer data)
	{
		Result<Ref<AudioClipAsset>> clip = AudioClipAsset::Create(std::move(data));
		REQUIRE(clip.IsOk());
		return clip.TakeValue();
	}

	inline Ref<AudioClipAsset> MakeWavClip(std::initializer_list<AudioSegment> segments)
	{
		std::vector<float> const samples = MakeSamples(segments);
		return MakeClip(MakeWav(samples));
	}

	// Mean absolute sample value per channel of the next part of the manual mix.
	struct AudioLevels
	{
		float Left = 0.0f;
		float Right = 0.0f;
	};

	inline AudioLevels ReadLevels(float seconds)
	{
		uint32_t const frames = AudioFrames(seconds);
		std::vector<float> samples(static_cast<size_t>(frames) * 2);
		REQUIRE(AudioEngine::ReadFrames(samples) == frames);
		double left = 0.0;
		double right = 0.0;
		for (size_t frame = 0; frame < frames; frame++)
		{
			left += std::abs(samples[frame * 2]);
			right += std::abs(samples[frame * 2 + 1]);
		}
		return {static_cast<float>(left / frames), static_cast<float>(right / frames)};
	}

	// Advances the manual mix.
	inline void Skip(float seconds)
	{
		ReadLevels(seconds);
	}

	// Gain miniaudio gives a stereo speaker for a sound in the given normalized listener-space direction. Its stereo
	// speakers face straight left and right: max((cos(angle to the speaker) + 1) / 2, 0.2), so a sound in front reaches
	// each speaker at half gain.
	inline float SpeakerGain(glm::vec3 const& direction, bool right)
	{
		float const cosine = right ? direction.x : -direction.x;
		return std::max((cosine + 1.0f) * 0.5f, 0.2f);
	}
}
