#include "stpch.h"
#include "Strada/Core/EngineLayout.h"

#include "Strada/Core/FileSystem.h"
// The marker's header is read as every Strada file's. Core sits below Serialization, but only this translation unit reads
// the file, as Application.cpp starts the modules above it: the header stays free of it.
#include "Strada/Serialization/JsonSerialization.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Strada
{
	namespace
	{
#if defined(ST_PLATFORM_WINDOWS)
		constexpr std::string_view ToolFileName = "strada.exe";
#else
		constexpr std::string_view ToolFileName = "strada";
#endif

		// "C:/Strada/" and "C:/Strada" name one folder; the content paths are made by appending to it.
		std::filesystem::path WithoutTrailingSeparator(std::filesystem::path path)
		{
			path = path.lexically_normal();
			if (!path.has_filename() && path.has_relative_path())
			{
				path = path.parent_path();
			}
			return path;
		}

		// The checkout engine.json names: empty when it names none (no Content), an error saying what is wrong otherwise.
		Result<std::filesystem::path> ReadManifest(std::filesystem::path const& file)
		{
			if (!FileSystem::IsRegularFile(file))
			{
				return Error{"it is not a file"};
			}
			std::optional<uint64_t> const size = FileSystem::GetFileSize(file);
			if (!size)
			{
				return Error{"its size cannot be read"};
			}
			if (*size > EngineLayout::MaxManifestBytes)
			{
				return MakeError("it is larger than {} bytes", EngineLayout::MaxManifestBytes);
			}
			Result<std::string> text = FileSystem::ReadTextFile(file);
			if (!text)
			{
				return Error{text.GetError()};
			}
			Result<Json> document = ParseJson(text.GetValue());
			if (!document)
			{
				return MakeError("it is not valid JSON: {}", document.GetError());
			}
			Json const& manifest = document.GetValue();
			if (Result<int> header = ReadFileHeader(manifest, EngineLayout::ManifestType, EngineLayout::ManifestVersion); !header)
			{
				return Error{header.GetError()};
			}

			auto const content = manifest.find("Content");
			if (content == manifest.end())
			{
				return std::filesystem::path();
			}
			if (!content->is_string())
			{
				return Error{"\"Content\" must be a string"};
			}
			std::string const& checkout = content->get_ref<std::string const&>();
			// A NUL would end the path where the file system reads it, and other control characters name no checkout.
			for (char const character : checkout)
			{
				if (static_cast<unsigned char>(character) < 0x20)
				{
					return Error{"\"Content\" holds a control character"};
				}
			}
			std::filesystem::path const directory = FileSystem::PathFromUtf8(checkout);
			// A relative path would depend on the working directory of whoever reads the layout.
			if (!directory.is_absolute())
			{
				return MakeError("\"Content\" must be an absolute path, not '{}'", checkout);
			}
			if (!FileSystem::IsDirectory(directory))
			{
				return MakeError("\"Content\" names '{}', which is not a directory", checkout);
			}
			return WithoutTrailingSeparator(directory);
		}
	}

	Result<EngineLayout> EngineLayout::Resolve(std::filesystem::path const& executablePath)
	{
		if (executablePath.empty())
		{
			return Error{"the executable's path is empty"};
		}

		EngineLayout layout;
		std::filesystem::path executableDirectory = executablePath.lexically_normal().parent_path();
		layout.m_ExecutableDirectory = executableDirectory.empty() ? std::filesystem::path(".") : std::move(executableDirectory);
		layout.m_ResourcesDirectory = layout.m_ExecutableDirectory;
		// Path components are compared by name, so a bundle is recognized wherever the layout is resolved (tests too).
		std::filesystem::path const contents = layout.m_ExecutableDirectory.parent_path();
		if (layout.m_ExecutableDirectory.filename() == "MacOS" && contents.filename() == "Contents" &&
		    FileSystem::IsRegularFile(contents / "Info.plist"))
		{
			layout.m_ResourcesDirectory = contents / "Resources";
		}

		std::filesystem::path const manifest = layout.m_ResourcesDirectory / ManifestFileName;
		if (!FileSystem::Exists(manifest))
		{
			return layout;
		}
		Result<std::filesystem::path> checkout = ReadManifest(manifest);
		if (!checkout)
		{
			layout.m_Warnings.push_back(fmt::format("'{}' is ignored: {}; the engine's content is looked for in '{}'",
			                                        FileSystem::PathToUtf8(manifest), checkout.GetError(),
			                                        FileSystem::PathToUtf8(layout.m_ResourcesDirectory)));
			return layout;
		}
		layout.m_SourceDirectory = checkout.TakeValue();
		return layout;
	}

	EngineLayout const& EngineLayout::Get()
	{
		static EngineLayout const s_Layout = []
		{
			Result<EngineLayout> resolved = Resolve(FileSystem::GetExecutablePath());
			if (resolved)
			{
				return resolved.TakeValue();
			}
			EngineLayout unknown;
			unknown.m_Warnings.push_back(fmt::format("the engine's content cannot be found: {}", resolved.GetError()));
			return unknown;
		}();
		return s_Layout;
	}

	std::filesystem::path EngineLayout::GetContentPath(EngineContent content) const
	{
		if (m_ResourcesDirectory.empty())
		{
			return {};
		}
		if (IsSourceTree())
		{
			switch (content)
			{
				case EngineContent::Templates:
					return m_SourceDirectory / "Templates";
				case EngineContent::Examples:
					return m_SourceDirectory / "Projects";
				case EngineContent::Docs:
					return m_SourceDirectory / "Docs";
				case EngineContent::AgentKit:
					return m_SourceDirectory / "Agents";
				case EngineContent::MakeGameSkill:
					return m_SourceDirectory / ".claude" / "skills" / "strada-make-game";
			}
			return {};
		}
		switch (content)
		{
			case EngineContent::Templates:
				return m_ResourcesDirectory / "Templates";
			case EngineContent::Examples:
				return m_ResourcesDirectory / "Examples";
			case EngineContent::Docs:
				return m_ResourcesDirectory / "Docs";
			case EngineContent::AgentKit:
				return m_ResourcesDirectory / "Agents";
			case EngineContent::MakeGameSkill:
				return m_ResourcesDirectory / "Agents" / "skills" / "strada-make-game";
		}
		return {};
	}

	std::filesystem::path EngineLayout::GetExampleCatalogPath() const
	{
		std::filesystem::path const examples = GetContentPath(EngineContent::Examples);
		return examples.empty() ? examples : examples / "catalog.json";
	}

	std::filesystem::path EngineLayout::GetToolPath() const
	{
		return m_ExecutableDirectory.empty() ? std::filesystem::path() : m_ExecutableDirectory / FileSystem::PathFromUtf8(ToolFileName);
	}
}
