#pragma once

#include "Strada/Asset/Asset.h"
#include "Strada/Core/Buffer.h"
#include "Strada/Core/Result.h"

#include <span>

namespace Strada
{
	enum class AudioFormat : uint8_t
	{
		Unknown = 0,
		Wav,
		Flac,
		Mp3,
		Ogg
	};

	char const* AudioFormatToString(AudioFormat format);
	// Identifies the container from its signature bytes.
	AudioFormat DetectAudioFormat(std::span<uint8_t const> data);

	// Encoded audio file data (WAV, FLAC, MP3, Ogg Vorbis); decoded by the audio engine.
	class AudioClipAsset final : public Asset
	{
		struct PrivateTag
		{
		};

	public:
		// Fails for empty data and unrecognized formats.
		[[nodiscard]] static Result<Ref<AudioClipAsset>> Create(Buffer data);

		AudioClipAsset(PrivateTag, Buffer data, AudioFormat format);

		static AssetType GetStaticType() { return AssetType::AudioClip; }
		AssetType GetAssetType() const override { return GetStaticType(); }

		std::span<uint8_t const> GetData() const { return m_Data.GetSpan(); }
		AudioFormat GetFormat() const { return m_Format; }

	private:
		Buffer m_Data;
		AudioFormat m_Format = AudioFormat::Unknown;
	};
}
