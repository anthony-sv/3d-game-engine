#include "stpch.h"
#include "Strada/Audio/AudioWaveform.h"

#include "Strada/Asset/AudioClipAsset.h"

#include <extras/decoders/libvorbis/miniaudio_libvorbis.h>
#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace Strada
{
	namespace
	{
		// Frames whose peak is kept separately at first. Long clips merge neighboring blocks as they are decoded (the length of
		// compressed streams is not always known up front), so memory stays bounded however long the clip is.
		constexpr uint64_t InitialBlockFrames = 64;
		// Blocks kept per part of the waveform before neighbors merge: part boundaries are exact to this fraction.
		constexpr size_t BlocksPerPart = 16;
		constexpr ma_uint64 ReadFrames = 4096;

		class Decoder
		{
		public:
			Decoder() = default;
			~Decoder()
			{
				if (m_Initialized)
				{
					ma_decoder_uninit(&m_Decoder);
				}
			}

			Decoder(Decoder const&) = delete;
			Decoder& operator=(Decoder const&) = delete;

			Result<void> Init(AudioClipAsset const& clip)
			{
				ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
				// Ogg Vorbis is decoded by the libvorbis backend, which miniaudio only tries for data of an unknown encoding.
				switch (clip.GetFormat())
				{
					case AudioFormat::Wav:
						config.encodingFormat = ma_encoding_format_wav;
						break;
					case AudioFormat::Flac:
						config.encodingFormat = ma_encoding_format_flac;
						break;
					case AudioFormat::Mp3:
						config.encodingFormat = ma_encoding_format_mp3;
						break;
					case AudioFormat::Ogg:
					case AudioFormat::Unknown:
						break;
				}
				// Read only while the decoder initializes.
				std::array<ma_decoding_backend_vtable*, 1> customBackends = {ma_decoding_backend_libvorbis};
				config.ppCustomBackendVTables = customBackends.data();
				config.customBackendCount = static_cast<ma_uint32>(customBackends.size());
				std::span<uint8_t const> const data = clip.GetData();
				if (ma_result const result = ma_decoder_init_memory(data.data(), data.size(), &config, &m_Decoder); result != MA_SUCCESS)
				{
					return MakeError("the {} clip cannot be decoded: {}", AudioFormatToString(clip.GetFormat()),
					                 ma_result_description(result));
				}
				m_Initialized = true;
				return {};
			}

			ma_decoder& Get() { return m_Decoder; }

		private:
			ma_decoder m_Decoder{};
			bool m_Initialized = false;
		};
	}

	Result<AudioWaveform> ComputeAudioWaveform(AudioClipAsset const& clip, uint32_t partCount, std::atomic<bool> const* cancel)
	{
		if (partCount == 0)
		{
			return Error{"a waveform needs at least one part"};
		}
		Decoder decoder;
		if (Result<void> initialized = decoder.Init(clip); !initialized)
		{
			return Error{initialized.GetError()};
		}
		uint32_t const channels = decoder.Get().outputChannels;
		uint32_t const sampleRate = decoder.Get().outputSampleRate;
		if (channels == 0 || sampleRate == 0)
		{
			return Error{"the clip has no channels"};
		}

		size_t const maxBlocks = static_cast<size_t>(partCount) * BlocksPerPart;
		uint64_t blockFrames = InitialBlockFrames;
		std::vector<float> blocks;
		blocks.reserve(maxBlocks * 2);
		float blockPeak = 0.0f;
		uint64_t framesInBlock = 0;
		uint64_t totalFrames = 0;
		std::vector<float> samples(static_cast<size_t>(ReadFrames) * channels);
		while (true)
		{
			if (cancel != nullptr && cancel->load(std::memory_order_relaxed))
			{
				return Error{"cancelled"};
			}
			ma_uint64 framesRead = 0;
			ma_result const result = ma_decoder_read_pcm_frames(&decoder.Get(), samples.data(), ReadFrames, &framesRead);
			for (size_t frame = 0; frame < framesRead; frame++)
			{
				for (size_t channel = 0; channel < channels; channel++)
				{
					float const sample = std::abs(samples[frame * channels + channel]);
					// NaN never wins the comparison.
					blockPeak = sample > blockPeak ? sample : blockPeak;
				}
				if (++framesInBlock < blockFrames)
				{
					continue;
				}
				blocks.push_back(blockPeak);
				blockPeak = 0.0f;
				framesInBlock = 0;
				// Merging exactly at an even count keeps the blocks on one grid: the next block starts on a merged boundary.
				if (blocks.size() == maxBlocks * 2)
				{
					for (size_t index = 0; index < maxBlocks; index++)
					{
						blocks[index] = std::max(blocks[index * 2], blocks[index * 2 + 1]);
					}
					blocks.resize(maxBlocks);
					blockFrames *= 2;
				}
			}
			totalFrames += framesRead;
			if (result != MA_SUCCESS || framesRead < ReadFrames)
			{
				break;
			}
		}
		if (framesInBlock > 0)
		{
			blocks.push_back(blockPeak);
		}
		if (totalFrames == 0)
		{
			return Error{"the clip holds no audio"};
		}

		AudioWaveform waveform;
		waveform.Duration = static_cast<double>(totalFrames) / static_cast<double>(sampleRate);
		waveform.SampleRate = sampleRate;
		waveform.ChannelCount = channels;
		size_t const parts = std::min<size_t>(partCount, blocks.size());
		waveform.Peaks.resize(parts);
		for (size_t part = 0; part < parts; part++)
		{
			size_t const begin = part * blocks.size() / parts;
			size_t const end = (part + 1) * blocks.size() / parts;
			float const peak =
				*std::max_element(blocks.begin() + static_cast<std::ptrdiff_t>(begin), blocks.begin() + static_cast<std::ptrdiff_t>(end));
			waveform.Peaks[part] = std::min(peak, 1.0f);
		}
		return waveform;
	}
}
