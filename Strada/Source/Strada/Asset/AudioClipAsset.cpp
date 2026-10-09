#include "stpch.h"
#include "Strada/Asset/AudioClipAsset.h"

#include <cstring>

namespace Strada
{
	namespace
	{
		bool StartsWith(std::span<uint8_t const> data, size_t offset, char const* signature)
		{
			size_t const length = std::strlen(signature);
			return data.size() >= offset + length && std::memcmp(data.data() + offset, signature, length) == 0;
		}
	}

	char const* AudioFormatToString(AudioFormat format)
	{
		switch (format)
		{
			case AudioFormat::Wav:
				return "WAV";
			case AudioFormat::Flac:
				return "FLAC";
			case AudioFormat::Mp3:
				return "MP3";
			case AudioFormat::Ogg:
				return "Ogg Vorbis";
			case AudioFormat::Unknown:
				break;
		}
		return "Unknown";
	}

	AudioFormat DetectAudioFormat(std::span<uint8_t const> data)
	{
		if (StartsWith(data, 0, "RIFF") && StartsWith(data, 8, "WAVE"))
		{
			return AudioFormat::Wav;
		}
		if (StartsWith(data, 0, "fLaC"))
		{
			return AudioFormat::Flac;
		}
		if (StartsWith(data, 0, "OggS"))
		{
			return AudioFormat::Ogg;
		}
		// MP3: an ID3v2 tag, or an MPEG audio frame header (11-bit sync, layer bits not "reserved").
		if (StartsWith(data, 0, "ID3"))
		{
			return AudioFormat::Mp3;
		}
		if (data.size() >= 2 && data[0] == 0xFF && (data[1] & 0xE0) == 0xE0 && (data[1] & 0x06) != 0)
		{
			return AudioFormat::Mp3;
		}
		return AudioFormat::Unknown;
	}

	Result<Ref<AudioClipAsset>> AudioClipAsset::Create(Buffer data)
	{
		AudioFormat const format = DetectAudioFormat(data.GetSpan());
		if (format == AudioFormat::Unknown)
		{
			return Error{"unsupported audio format (expected WAV, FLAC, MP3 or Ogg Vorbis)"};
		}
		return CreateRef<AudioClipAsset>(PrivateTag{}, std::move(data), format);
	}

	AudioClipAsset::AudioClipAsset(PrivateTag, Buffer data, AudioFormat format)
		: m_Data(std::move(data)),
		  m_Format(format)
	{
	}
}
