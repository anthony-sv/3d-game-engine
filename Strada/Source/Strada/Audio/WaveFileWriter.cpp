#include "stpch.h"
#include "Strada/Audio/WaveFileWriter.h"

#include "Strada/Core/FileSystem.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Strada
{
	namespace
	{
		void PutLittleEndian(uint8_t* bytes, uint32_t value, uint32_t byteCount)
		{
			for (uint32_t index = 0; index < byteCount; index++)
			{
				bytes[index] = static_cast<uint8_t>((value >> (8 * index)) & 0xFFu);
			}
		}

		// The 44-byte header of a PCM file of 16-bit samples holding dataBytes of them.
		std::array<uint8_t, WaveFileWriter::HeaderBytes> MakeHeader(uint32_t sampleRate, uint32_t channelCount, uint32_t dataBytes)
		{
			std::array<uint8_t, WaveFileWriter::HeaderBytes> header{};
			auto const text = [&header](size_t offset, char const(&chars)[5])
			{
				std::copy_n(chars, 4, header.begin() + static_cast<std::ptrdiff_t>(offset));
			};
			uint32_t const blockAlign = channelCount * 2;
			text(0, "RIFF");
			PutLittleEndian(&header[4], 36 + dataBytes, 4);
			text(8, "WAVE");
			text(12, "fmt ");
			PutLittleEndian(&header[16], 16, 4);
			PutLittleEndian(&header[20], 1, 2); // PCM
			PutLittleEndian(&header[22], channelCount, 2);
			PutLittleEndian(&header[24], sampleRate, 4);
			PutLittleEndian(&header[28], sampleRate * blockAlign, 4);
			PutLittleEndian(&header[32], blockAlign, 2);
			PutLittleEndian(&header[34], 16, 2);
			text(36, "data");
			PutLittleEndian(&header[40], dataBytes, 4);
			return header;
		}
	}

	Result<Scope<WaveFileWriter>> WaveFileWriter::Create(std::filesystem::path const& path, uint32_t sampleRate, uint32_t channelCount)
	{
		if (sampleRate == 0 || channelCount == 0 || channelCount > std::numeric_limits<uint16_t>::max() ||
		    static_cast<uint64_t>(sampleRate) * channelCount * 2 > std::numeric_limits<uint32_t>::max())
		{
			return MakeError("a WAV file cannot hold {} channels at {} Hz", channelCount, sampleRate);
		}
		std::ofstream stream(path, std::ios::binary | std::ios::trunc);
		if (!stream)
		{
			return MakeError("'{}' cannot be opened for writing", FileSystem::PathToUtf8(path));
		}
		std::array<uint8_t, HeaderBytes> const header = MakeHeader(sampleRate, channelCount, 0);
		stream.write(reinterpret_cast<char const*>(header.data()), static_cast<std::streamsize>(header.size()));
		if (!stream)
		{
			return MakeError("'{}' cannot be written", FileSystem::PathToUtf8(path));
		}
		return CreateScope<WaveFileWriter>(PrivateTag{}, std::move(stream), sampleRate, channelCount);
	}

	WaveFileWriter::WaveFileWriter(PrivateTag, std::ofstream stream, uint32_t sampleRate, uint32_t channelCount)
		: m_Stream(std::move(stream)),
		  m_SampleRate(sampleRate),
		  m_ChannelCount(channelCount)
	{
	}

	int16_t WaveFileWriter::ToPcm16(float sample)
	{
		if (std::isnan(sample))
		{
			return 0;
		}
		return static_cast<int16_t>(std::lround(std::clamp(sample, -1.0f, 1.0f) * 32767.0f));
	}

	Result<void> WaveFileWriter::Write(std::span<float const> samples)
	{
		if (m_Finished || m_Failed)
		{
			return Error{"the WAV file is closed"};
		}
		if (samples.size() % m_ChannelCount != 0)
		{
			return MakeError("{} samples are not a whole number of frames of {} channels", samples.size(), m_ChannelCount);
		}
		uint64_t const bytes = samples.size() * 2ull;
		if (m_DataBytes + bytes > MaxDataBytes)
		{
			m_Failed = true;
			return MakeError("a WAV file holds at most {} bytes of samples", MaxDataBytes);
		}
		m_Bytes.resize(bytes);
		for (size_t index = 0; index < samples.size(); index++)
		{
			uint16_t const value = static_cast<uint16_t>(ToPcm16(samples[index]));
			m_Bytes[index * 2] = static_cast<uint8_t>(value & 0xFFu);
			m_Bytes[index * 2 + 1] = static_cast<uint8_t>(value >> 8);
		}
		m_Stream.write(reinterpret_cast<char const*>(m_Bytes.data()), static_cast<std::streamsize>(bytes));
		if (!m_Stream)
		{
			m_Failed = true;
			return Error{"the WAV file cannot be written"};
		}
		m_DataBytes += bytes;
		return {};
	}

	Result<void> WaveFileWriter::Finish()
	{
		if (m_Finished || m_Failed)
		{
			return Error{"the WAV file is closed"};
		}
		m_Finished = true;
		std::array<uint8_t, HeaderBytes> const header = MakeHeader(m_SampleRate, m_ChannelCount, static_cast<uint32_t>(m_DataBytes));
		m_Stream.seekp(0);
		m_Stream.write(reinterpret_cast<char const*>(header.data()), static_cast<std::streamsize>(header.size()));
		m_Stream.flush();
		bool const written = static_cast<bool>(m_Stream);
		m_Stream.close();
		if (!written || m_Stream.fail())
		{
			return Error{"the WAV file's header cannot be written"};
		}
		return {};
	}
}
