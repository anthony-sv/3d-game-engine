#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>

namespace Strada
{
	// A file a streamed sound plays (AudioSourceDesc::StreamFile). The main thread opens it and reads its format and
	// headers; afterwards only the audio streaming thread reads and seeks it, for as long as the sound exists. Never used
	// by two threads at once.
	class AudioStreamFile
	{
	public:
		virtual ~AudioStreamFile() = default;

		// Reads up to data.size() bytes from the current position and returns how many were read: fewer only at the end of
		// the file or when it cannot be read.
		virtual size_t Read(std::span<uint8_t> data) = 0;
		// Moves the position; false for positions past the end of the file, or when it cannot.
		virtual bool Seek(uint64_t position) = 0;
		virtual uint64_t GetPosition() const = 0;
		virtual uint64_t GetSize() const = 0;

		// A file of the file system (what sounds stream from unless AudioEngineSettings::StreamFileOpener says otherwise).
		// Fails for missing files and for directories.
		[[nodiscard]] static Result<Scope<AudioStreamFile>> Open(std::filesystem::path const& path);
	};

	// Opens the files that sounds stream from (called on the main thread).
	using AudioStreamFileOpener = std::function<Result<Scope<AudioStreamFile>>(std::filesystem::path const& path)>;
}
