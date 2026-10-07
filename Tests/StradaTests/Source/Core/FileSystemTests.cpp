#include "TestUtilities.h"

#include "Strada/Core/FileSystem.h"

#include <doctest/doctest.h>

#include <array>
#include <string>

using namespace Strada;

TEST_CASE("FileSystem: text files round trip and parent directories are created")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const path = directory.GetPath() / "nested" / "deeper" / "file.txt";

	REQUIRE(FileSystem::WriteTextFile(path, "hello\nworld").IsOk());
	CHECK(FileSystem::Exists(path));
	CHECK(FileSystem::IsRegularFile(path));

	Result<std::string> const text = FileSystem::ReadTextFile(path);
	REQUIRE(text.IsOk());
	CHECK(text.GetValue() == "hello\nworld");
}

TEST_CASE("FileSystem: overwriting replaces the whole file atomically")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const path = directory.GetPath() / "file.txt";
	REQUIRE(FileSystem::WriteTextFile(path, "a much longer first version").IsOk());
	REQUIRE(FileSystem::WriteTextFile(path, "short").IsOk());
	CHECK(FileSystem::ReadTextFile(path).GetValue() == "short");

	// No temporary files are left behind.
	size_t fileCount = 0;
	for ([[maybe_unused]] auto const& entry : std::filesystem::directory_iterator(directory.GetPath()))
	{
		fileCount++;
	}
	CHECK(fileCount == 1);
}

TEST_CASE("FileSystem: UTF-8 byte order marks are stripped from text files")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const path = directory.GetPath() / "bom.txt";
	std::string const withBom = "\xEF\xBB\xBF{}";
	REQUIRE(FileSystem::WriteTextFile(path, withBom).IsOk());
	CHECK(FileSystem::ReadTextFile(path).GetValue() == "{}");
}

TEST_CASE("FileSystem: binary files round trip, including empty files")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const path = directory.GetPath() / "data.bin";
	std::array<uint8_t, 6> const bytes = {0, 255, 1, 128, 0, 7};
	REQUIRE(FileSystem::WriteBinaryFile(path, bytes).IsOk());

	Result<Buffer> const buffer = FileSystem::ReadBinaryFile(path);
	REQUIRE(buffer.IsOk());
	REQUIRE(buffer.GetValue().GetSize() == bytes.size());
	for (size_t i = 0; i < bytes.size(); i++)
	{
		CHECK(buffer.GetValue().GetData()[i] == bytes[i]);
	}

	std::filesystem::path const emptyPath = directory.GetPath() / "empty.bin";
	REQUIRE(FileSystem::WriteBinaryFile(emptyPath, {}).IsOk());
	Result<Buffer> const empty = FileSystem::ReadBinaryFile(emptyPath);
	REQUIRE(empty.IsOk());
	CHECK(empty.GetValue().IsEmpty());
}

TEST_CASE("FileSystem: reading a missing file reports an error")
{
	Testing::TemporaryDirectory directory;
	Result<std::string> const text = FileSystem::ReadTextFile(directory.GetPath() / "missing.txt");
	CHECK(text.IsError());
	CHECK(text.GetError().find("missing.txt") != std::string::npos);
	CHECK(FileSystem::ReadBinaryFile(directory.GetPath() / "missing.bin").IsError());
}

TEST_CASE("FileSystem: copy, move and remove")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const source = directory.GetPath() / "source.txt";
	std::filesystem::path const copy = directory.GetPath() / "copies" / "copy.txt";
	std::filesystem::path const moved = directory.GetPath() / "moved" / "moved.txt";
	REQUIRE(FileSystem::WriteTextFile(source, "content").IsOk());

	REQUIRE(FileSystem::Copy(source, copy, false).IsOk());
	CHECK(FileSystem::ReadTextFile(copy).GetValue() == "content");
	CHECK(FileSystem::Copy(source, copy, false).IsError());
	CHECK(FileSystem::Copy(source, copy, true).IsOk());

	REQUIRE(FileSystem::Move(copy, moved).IsOk());
	CHECK_FALSE(FileSystem::Exists(copy));
	CHECK(FileSystem::Exists(moved));

	REQUIRE(FileSystem::Remove(moved).IsOk());
	CHECK_FALSE(FileSystem::Exists(moved));
	CHECK(FileSystem::Remove(moved).IsOk());

	REQUIRE(FileSystem::RemoveAll(directory.GetPath() / "copies").IsOk());
	CHECK_FALSE(FileSystem::IsDirectory(directory.GetPath() / "copies"));
}

TEST_CASE("FileSystem: directory trees are copied recursively")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const source = directory.GetPath() / "tree";
	REQUIRE(FileSystem::WriteTextFile(source / "a.txt", "a").IsOk());
	REQUIRE(FileSystem::WriteTextFile(source / "sub" / "b.txt", "b").IsOk());

	std::filesystem::path const destination = directory.GetPath() / "copy";
	REQUIRE(FileSystem::CopyDirectory(source, destination).IsOk());
	CHECK(FileSystem::ReadTextFile(destination / "a.txt").GetValue() == "a");
	CHECK(FileSystem::ReadTextFile(destination / "sub" / "b.txt").GetValue() == "b");
}

TEST_CASE("FileSystem: UTF-8 path conversion handles non-ASCII names")
{
	Testing::TemporaryDirectory directory;
	std::string const utf8Name = "Caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC.txt";
	std::filesystem::path const path = directory.GetPath() / FileSystem::PathFromUtf8(utf8Name);
	REQUIRE(FileSystem::WriteTextFile(path, "unicode").IsOk());
	CHECK(FileSystem::ReadTextFile(path).GetValue() == "unicode");

	std::string const roundTrip = FileSystem::PathToUtf8(path.filename());
	CHECK(roundTrip == utf8Name);
}

TEST_CASE("FileSystem: paths are written with forward slashes")
{
	std::filesystem::path const path = std::filesystem::path("Assets") / "Textures" / "Brick.png";
	CHECK(FileSystem::PathToUtf8(path) == "Assets/Textures/Brick.png");
}

TEST_CASE("FileSystem: relative paths and containment")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const base = directory.GetPath() / "Project";
	std::filesystem::path const inside = base / "Assets" / "Meshes" / "Cube.glb";
	std::filesystem::path const outside = directory.GetPath() / "Other" / "file.txt";

	CHECK(FileSystem::PathToUtf8(FileSystem::GetRelativePath(inside, base)) == "Assets/Meshes/Cube.glb");
	CHECK(FileSystem::IsInside(inside, base));
	CHECK(FileSystem::IsInside(base, base));
	CHECK_FALSE(FileSystem::IsInside(outside, base));
	CHECK_FALSE(FileSystem::IsInside(base / ".." / "Other", base));
}

TEST_CASE("FileSystem: executable and user data locations")
{
	std::filesystem::path const executable = FileSystem::GetExecutablePath();
	REQUIRE_FALSE(executable.empty());
	CHECK(FileSystem::Exists(executable));
	CHECK(FileSystem::IsDirectory(FileSystem::GetExecutableDirectory()));

	std::filesystem::path const userData = FileSystem::GetUserDataDirectory();
	CHECK_FALSE(userData.empty());
	CHECK(userData.is_absolute());
}

TEST_CASE("FileSystem: last write time is available for existing files only")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const path = directory.GetPath() / "file.txt";
	CHECK_FALSE(FileSystem::GetLastWriteTime(path).has_value());
	REQUIRE(FileSystem::WriteTextFile(path, "x").IsOk());
	CHECK(FileSystem::GetLastWriteTime(path).has_value());
}
