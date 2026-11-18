#include "TestUtilities.h"

#include "Strada/Audio/WaveFileWriter.h"
#include "Strada/Core/Buffer.h"
#include "Strada/Core/FileSystem.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <vector>

using namespace Strada;

namespace
{
	uint32_t ReadLittleEndian(std::span<uint8_t const> bytes, size_t offset, uint32_t byteCount)
	{
		uint32_t value = 0;
		for (uint32_t index = 0; index < byteCount; index++)
		{
			value |= static_cast<uint32_t>(bytes[offset + index]) << (8 * index);
		}
		return value;
	}

	std::string ReadTag(std::span<uint8_t const> bytes, size_t offset)
	{
		return std::string(reinterpret_cast<char const*>(bytes.data() + offset), 4);
	}

	std::vector<uint8_t> ReadFile(std::filesystem::path const& path)
	{
		Result<Buffer> read = FileSystem::ReadBinaryFile(path);
		REQUIRE(read.IsOk());
		Buffer const& buffer = read.GetValue();
		return std::vector<uint8_t>(buffer.GetData(), buffer.GetData() + buffer.GetSize());
	}
}

TEST_CASE("WaveFileWriter: writes a PCM header with the sizes of what was written, and 16-bit samples rounded and clamped")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const file = directory.GetPath() / "Mix.wav";
	Result<Scope<WaveFileWriter>> created = WaveFileWriter::Create(file, 48000, 2);
	REQUIRE(created.IsOk());
	Scope<WaveFileWriter> writer = created.TakeValue();
	float const nan = std::numeric_limits<float>::quiet_NaN();
	std::vector<float> const first = {0.0f, 1.0f, -1.0f, 0.5f};
	std::vector<float> const second = {2.0f, -3.0f, nan, -0.25f};
	REQUIRE(writer->Write(first).IsOk());
	REQUIRE(writer->Write(second).IsOk());
	REQUIRE(writer->Write({}).IsOk());
	CHECK(writer->GetFrameCount() == 4);
	// A part of a frame is refused and writes nothing.
	CHECK_FALSE(writer->Write(std::vector<float>{0.5f}).IsOk());
	REQUIRE(writer->Finish().IsOk());
	CHECK(writer->IsFinished());
	CHECK_FALSE(writer->Write(first).IsOk());
	CHECK_FALSE(writer->Finish().IsOk());

	std::vector<uint8_t> const bytes = ReadFile(file);
	REQUIRE(bytes.size() == WaveFileWriter::HeaderBytes + 16);
	CHECK(ReadTag(bytes, 0) == "RIFF");
	CHECK(ReadLittleEndian(bytes, 4, 4) == 36 + 16);
	CHECK(ReadTag(bytes, 8) == "WAVE");
	CHECK(ReadTag(bytes, 12) == "fmt ");
	CHECK(ReadLittleEndian(bytes, 16, 4) == 16);
	CHECK(ReadLittleEndian(bytes, 20, 2) == 1);
	CHECK(ReadLittleEndian(bytes, 22, 2) == 2);
	CHECK(ReadLittleEndian(bytes, 24, 4) == 48000);
	CHECK(ReadLittleEndian(bytes, 28, 4) == 48000 * 4);
	CHECK(ReadLittleEndian(bytes, 32, 2) == 4);
	CHECK(ReadLittleEndian(bytes, 34, 2) == 16);
	CHECK(ReadTag(bytes, 36) == "data");
	CHECK(ReadLittleEndian(bytes, 40, 4) == 16);

	std::vector<int16_t> samples;
	for (size_t offset = WaveFileWriter::HeaderBytes; offset < bytes.size(); offset += 2)
	{
		samples.push_back(static_cast<int16_t>(static_cast<uint16_t>(ReadLittleEndian(bytes, offset, 2))));
	}
	CHECK(samples == std::vector<int16_t>{0, 32767, -32767, 16384, 32767, -32767, 0, -8192});
	CHECK(WaveFileWriter::ToPcm16(0.25f / 32767.0f) == 0);
	CHECK(WaveFileWriter::ToPcm16(0.75f / 32767.0f) == 1);
}

TEST_CASE("WaveFileWriter: a file that cannot be opened and a format a WAV file cannot say are refused")
{
	Testing::TemporaryDirectory directory;
	CHECK_FALSE(WaveFileWriter::Create(directory.GetPath() / "Missing" / "Mix.wav", 48000, 2).IsOk());
	CHECK_FALSE(WaveFileWriter::Create(directory.GetPath(), 48000, 2).IsOk());
	CHECK_FALSE(WaveFileWriter::Create(directory.GetPath() / "Mix.wav", 0, 2).IsOk());
	CHECK_FALSE(WaveFileWriter::Create(directory.GetPath() / "Mix.wav", 48000, 0).IsOk());
	CHECK_FALSE(WaveFileWriter::Create(directory.GetPath() / "Mix.wav", 48000, 70000).IsOk());

	// A file finished at once holds the header of no samples.
	Result<Scope<WaveFileWriter>> empty = WaveFileWriter::Create(directory.GetPath() / "Empty.wav", 44100, 1);
	REQUIRE(empty.IsOk());
	REQUIRE(empty.GetValue()->Finish().IsOk());
	std::vector<uint8_t> const bytes = ReadFile(directory.GetPath() / "Empty.wav");
	REQUIRE(bytes.size() == WaveFileWriter::HeaderBytes);
	CHECK(ReadLittleEndian(bytes, 4, 4) == 36);
	CHECK(ReadLittleEndian(bytes, 22, 2) == 1);
	CHECK(ReadLittleEndian(bytes, 24, 4) == 44100);
	CHECK(ReadLittleEndian(bytes, 28, 4) == 88200);
	CHECK(ReadLittleEndian(bytes, 40, 4) == 0);
}
