#include "stpch.h"
#include "Strada/Audio/AudioDecoder.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>

namespace Strada
{
	namespace
	{
		using FileAccess = AudioDecoder::FileAccess;

		static_assert(offsetof(FileAccess, Callbacks) == 0, "miniaudio reads the callbacks at the file system's address");

		FileAccess& GetAccess(void* access)
		{
			return *static_cast<FileAccess*>(access);
		}

		// Reads short of the size are no error: the decoders tell the end of the file from the bytes read (libvorbis takes
		// any other result as nothing read, even when some bytes were).
		ma_result ReadBytes(FileAccess& access, void* destination, size_t size, size_t* bytesRead)
		{
			size_t read = 0;
			if (size > 0)
			{
				size_t const allowed = static_cast<size_t>(std::min<uint64_t>(size, access.ReadsLeft));
				if (allowed < size)
				{
					access.LimitReached = true;
				}
				if (allowed > 0)
				{
					read = std::min(access.File->Read(std::span<uint8_t>(static_cast<uint8_t*>(destination), allowed)), allowed);
				}
				if (access.ReadsLeft != AudioDecoder::NoReadLimit)
				{
					access.ReadsLeft -= read;
				}
			}
			if (bytesRead != nullptr)
			{
				*bytesRead = read;
			}
			return read == 0 && size > 0 ? MA_AT_END : MA_SUCCESS;
		}

		ma_result SeekBytes(FileAccess& access, ma_int64 offset, ma_seek_origin origin)
		{
			AudioStreamFile& file = *access.File;
			uint64_t base = 0;
			switch (origin)
			{
				case ma_seek_origin_start:
					break;
				case ma_seek_origin_current:
					base = file.GetPosition();
					break;
				case ma_seek_origin_end:
					base = file.GetSize();
					break;
			}
			uint64_t target = 0;
			if (offset >= 0)
			{
				if (static_cast<uint64_t>(offset) > std::numeric_limits<uint64_t>::max() - base)
				{
					return MA_BAD_SEEK;
				}
				target = base + static_cast<uint64_t>(offset);
			}
			else
			{
				// Negated without overflowing for the most negative offset.
				uint64_t const back = static_cast<uint64_t>(-(offset + 1)) + 1;
				if (back > base)
				{
					return MA_BAD_SEEK;
				}
				target = base - back;
			}
			return file.Seek(target) ? MA_SUCCESS : MA_BAD_SEEK;
		}

		ma_result TellBytes(FileAccess& access, ma_int64* cursor)
		{
			if (cursor == nullptr)
			{
				return MA_INVALID_ARGS;
			}
			uint64_t const position = access.File->GetPosition();
			if (position > static_cast<uint64_t>(std::numeric_limits<ma_int64>::max()))
			{
				return MA_BAD_SEEK;
			}
			*cursor = static_cast<ma_int64>(position);
			return MA_SUCCESS;
		}

		// miniaudio's decoders of WAV, FLAC and MP3 read through a file system whose every open returns the file.
		ma_result OnFileOpen(ma_vfs* fileSystem, char const*, ma_uint32 openMode, ma_vfs_file* file)
		{
			if (file == nullptr)
			{
				return MA_INVALID_ARGS;
			}
			*file = nullptr;
			if ((openMode & MA_OPEN_MODE_WRITE) != 0)
			{
				return MA_ACCESS_DENIED;
			}
			*file = GetAccess(fileSystem).File;
			return MA_SUCCESS;
		}

		ma_result OnFileOpenWide(ma_vfs*, wchar_t const*, ma_uint32, ma_vfs_file* file)
		{
			if (file != nullptr)
			{
				*file = nullptr;
			}
			return MA_NOT_IMPLEMENTED;
		}

		// The file belongs to whoever created the decoder.
		ma_result OnFileClose(ma_vfs*, ma_vfs_file)
		{
			return MA_SUCCESS;
		}

		ma_result OnFileRead(ma_vfs* fileSystem, ma_vfs_file, void* destination, size_t size, size_t* bytesRead)
		{
			return ReadBytes(GetAccess(fileSystem), destination, size, bytesRead);
		}

		ma_result OnFileWrite(ma_vfs*, ma_vfs_file, void const*, size_t, size_t* bytesWritten)
		{
			if (bytesWritten != nullptr)
			{
				*bytesWritten = 0;
			}
			return MA_ACCESS_DENIED;
		}

		ma_result OnFileSeek(ma_vfs* fileSystem, ma_vfs_file, ma_int64 offset, ma_seek_origin origin)
		{
			return SeekBytes(GetAccess(fileSystem), offset, origin);
		}

		ma_result OnFileTell(ma_vfs* fileSystem, ma_vfs_file, ma_int64* cursor)
		{
			return TellBytes(GetAccess(fileSystem), cursor);
		}

		ma_result OnFileInfo(ma_vfs* fileSystem, ma_vfs_file, ma_file_info* info)
		{
			if (info == nullptr)
			{
				return MA_INVALID_ARGS;
			}
			info->sizeInBytes = GetAccess(fileSystem).File->GetSize();
			return MA_SUCCESS;
		}

		// libvorbis reads through these.
		ma_result OnRead(void* access, void* destination, size_t size, size_t* bytesRead)
		{
			return ReadBytes(GetAccess(access), destination, size, bytesRead);
		}

		ma_result OnSeek(void* access, ma_int64 offset, ma_seek_origin origin)
		{
			return SeekBytes(GetAccess(access), offset, origin);
		}

		ma_result OnTell(void* access, ma_int64* cursor)
		{
			return TellBytes(GetAccess(access), cursor);
		}

		ma_encoding_format ToEncodingFormat(AudioFormat format)
		{
			switch (format)
			{
				case AudioFormat::Wav:
					return ma_encoding_format_wav;
				case AudioFormat::Flac:
					return ma_encoding_format_flac;
				case AudioFormat::Mp3:
					return ma_encoding_format_mp3;
				case AudioFormat::Ogg:
					return ma_encoding_format_vorbis;
				case AudioFormat::Unknown:
					break;
			}
			return ma_encoding_format_unknown;
		}

		// miniaudio reports data its decoder refuses as having no decoder, libvorbis as an invalid file.
		std::string DescribeFailure(ma_result result, FileAccess const& access, uint64_t headerReadLimit)
		{
			if (access.LimitReached)
			{
				return fmt::format("its audio does not start within its first {} KB (it may be damaged)", headerReadLimit / 1024);
			}
			switch (result)
			{
				case MA_NO_BACKEND:
				case MA_INVALID_FILE:
				case MA_INVALID_DATA:
				case MA_FORMAT_NOT_SUPPORTED:
					return "it is damaged or not in the format its first bytes announce";
				default:
					break;
			}
			return ma_result_description(result);
		}
	}

	Result<Scope<AudioDecoder>> AudioDecoder::Create(AudioFormat format, AudioStreamFile& file, uint64_t headerReadLimit)
	{
		// miniaudio keeps pointers into the decoder and its file access: they must not move once initialized.
		Scope<AudioDecoder> decoder = CreateScope<AudioDecoder>(PrivateTag{});
		FileAccess& access = decoder->m_FileAccess;
		access.Callbacks = {
			.onOpen = OnFileOpen,
			.onOpenW = OnFileOpenWide,
			.onClose = OnFileClose,
			.onRead = OnFileRead,
			.onWrite = OnFileWrite,
			.onSeek = OnFileSeek,
			.onTell = OnFileTell,
			.onInfo = OnFileInfo,
		};
		access.File = &file;
		access.ReadsLeft = headerReadLimit;

		ma_data_source* source = nullptr;
		switch (format)
		{
			case AudioFormat::Wav:
			case AudioFormat::Flac:
			case AudioFormat::Mp3:
			{
				ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
				config.encodingFormat = ToEncodingFormat(format);
				// The path only names the data: the file system returns the file.
				if (ma_result const result = ma_decoder_init_vfs(&access, "audio", &config, &decoder->m_Decoder); result != MA_SUCCESS)
				{
					return Error{DescribeFailure(result, access, headerReadLimit)};
				}
				decoder->m_HasDecoder = true;
				source = &decoder->m_Decoder;
				break;
			}
			case AudioFormat::Ogg:
			{
				ma_decoding_backend_config const config = ma_decoding_backend_config_init(ma_format_f32, 0);
				if (ma_result const result = ma_libvorbis_init(OnRead, OnSeek, OnTell, &access, &config, nullptr, &decoder->m_Vorbis);
				    result != MA_SUCCESS)
				{
					return Error{DescribeFailure(result, access, headerReadLimit)};
				}
				decoder->m_HasVorbis = true;
				source = &decoder->m_Vorbis;
				break;
			}
			case AudioFormat::Unknown:
				return Error{"it is not in a supported audio format (WAV, FLAC, MP3 or Ogg Vorbis)"};
		}
		access.ReadsLeft = NoReadLimit;

		ma_format decodedFormat = ma_format_unknown;
		ma_uint32 channels = 0;
		ma_uint32 sampleRate = 0;
		if (ma_data_source_get_data_format(source, &decodedFormat, &channels, &sampleRate, decoder->m_ChannelMap.data(),
		                                   decoder->m_ChannelMap.size()) != MA_SUCCESS ||
		    decodedFormat != ma_format_f32 || channels == 0 || channels > MA_MAX_CHANNELS || sampleRate == 0)
		{
			return Error{"it has no playable audio"};
		}
		decoder->m_ChannelCount = channels;
		decoder->m_SampleRate = sampleRate;
		return decoder;
	}

	AudioDecoder::AudioDecoder(PrivateTag)
	{
	}

	AudioDecoder::~AudioDecoder()
	{
		if (m_HasDecoder)
		{
			ma_decoder_uninit(&m_Decoder);
		}
		if (m_HasVorbis)
		{
			ma_libvorbis_uninit(&m_Vorbis, nullptr);
		}
	}

	ma_data_source* AudioDecoder::GetDataSource()
	{
		if (m_HasVorbis)
		{
			return &m_Vorbis;
		}
		return &m_Decoder;
	}

	AudioMemoryFile::AudioMemoryFile(std::span<uint8_t const> data)
		: m_Data(data)
	{
	}

	size_t AudioMemoryFile::Read(std::span<uint8_t> data)
	{
		size_t const count = static_cast<size_t>(std::min<uint64_t>(data.size(), m_Data.size() - m_Position));
		if (count > 0)
		{
			std::memcpy(data.data(), m_Data.data() + m_Position, count);
			m_Position += count;
		}
		return count;
	}

	bool AudioMemoryFile::Seek(uint64_t position)
	{
		if (position > m_Data.size())
		{
			return false;
		}
		m_Position = position;
		return true;
	}
}
