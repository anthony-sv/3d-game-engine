#include "stpch.h"
#include "Strada/Audio/AudioStreamFile.h"

#include "Strada/Core/FileSystem.h"

#include <algorithm>
#include <fstream>
#include <ios>
#include <limits>

namespace Strada
{
	namespace
	{
		// A file of the file system, read through a std::filebuf (which takes wide paths on Windows).
		class FileSystemStreamFile final : public AudioStreamFile
		{
		public:
			bool Open(std::filesystem::path const& path)
			{
				if (m_Buffer.open(path, std::ios::in | std::ios::binary) == nullptr)
				{
					return false;
				}
				std::streampos const end = m_Buffer.pubseekoff(0, std::ios::end, std::ios::in);
				if (end == InvalidPosition() || m_Buffer.pubseekpos(0, std::ios::in) == InvalidPosition())
				{
					return false;
				}
				m_Size = static_cast<uint64_t>(static_cast<std::streamoff>(end));
				return true;
			}

			size_t Read(std::span<uint8_t> data) override
			{
				size_t const size = std::min<size_t>(data.size(), static_cast<size_t>(std::numeric_limits<std::streamsize>::max()));
				if (size == 0)
				{
					return 0;
				}
				std::streamsize const count = m_Buffer.sgetn(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
				size_t const read = count > 0 ? static_cast<size_t>(count) : 0;
				m_Position += read;
				return read;
			}

			bool Seek(uint64_t position) override
			{
				if (position > m_Size)
				{
					return false;
				}
				if (m_Buffer.pubseekpos(static_cast<std::streamoff>(position), std::ios::in) == InvalidPosition())
				{
					return false;
				}
				m_Position = position;
				return true;
			}

			uint64_t GetPosition() const override { return m_Position; }
			uint64_t GetSize() const override { return m_Size; }

		private:
			static std::streampos InvalidPosition() { return std::streampos(std::streamoff(-1)); }

			std::filebuf m_Buffer;
			uint64_t m_Position = 0;
			uint64_t m_Size = 0;
		};
	}

	Result<Scope<AudioStreamFile>> AudioStreamFile::Open(std::filesystem::path const& path)
	{
		if (!FileSystem::Exists(path))
		{
			return MakeError("{} does not exist", FileSystem::PathToUtf8(path));
		}
		if (!FileSystem::IsRegularFile(path))
		{
			return MakeError("{} is not a file", FileSystem::PathToUtf8(path));
		}
		Scope<FileSystemStreamFile> file = CreateScope<FileSystemStreamFile>();
		if (!file->Open(path))
		{
			return MakeError("{} cannot be opened", FileSystem::PathToUtf8(path));
		}
		return Scope<AudioStreamFile>(std::move(file));
	}
}
