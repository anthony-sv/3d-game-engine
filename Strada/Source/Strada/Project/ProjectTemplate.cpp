#include "stpch.h"
#include "Strada/Project/ProjectTemplate.h"

#include "Strada/Asset/AssetRegistry.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"
#include "Strada/Project/Project.h"
#include "Strada/Project/ProjectCopy.h"
#include "Strada/Scene/SceneSerializer.h"

#include <algorithm>
#include <array>
#include <system_error>
#include <utility>

namespace Strada
{
	namespace
	{
		constexpr std::string_view StartScenePath = "Scenes/Main.sscene";
		constexpr std::array<uint8_t, 8> PngSignature = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

		std::filesystem::path MakeAbsolute(std::filesystem::path const& path)
		{
			std::error_code errorCode;
			std::filesystem::path const absolute = std::filesystem::absolute(path, errorCode);
			return errorCode ? path.lexically_normal() : absolute.lexically_normal();
		}

		// The folder's own name, also when the path ends with a separator.
		std::string GetFolderName(std::filesystem::path const& directory)
		{
			std::filesystem::path const normal = MakeAbsolute(directory);
			std::filesystem::path const name = normal.has_filename() ? normal.filename() : normal.parent_path().filename();
			return FileSystem::PathToUtf8(name);
		}

		// Whether the text is well-formed UTF-8 (no overlong forms, surrogates or values past U+10FFFF).
		bool IsValidUtf8(std::string_view text)
		{
			size_t index = 0;
			while (index < text.size())
			{
				auto const byte = static_cast<uint8_t>(text[index]);
				size_t length = 0;
				uint32_t codePoint = 0;
				if (byte < 0x80)
				{
					index++;
					continue;
				}
				if ((byte & 0xE0) == 0xC0)
				{
					length = 2;
					codePoint = byte & 0x1Fu;
				}
				else if ((byte & 0xF0) == 0xE0)
				{
					length = 3;
					codePoint = byte & 0x0Fu;
				}
				else if ((byte & 0xF8) == 0xF0)
				{
					length = 4;
					codePoint = byte & 0x07u;
				}
				else
				{
					return false;
				}
				if (index + length > text.size())
				{
					return false;
				}
				for (size_t offset = 1; offset < length; offset++)
				{
					auto const continuation = static_cast<uint8_t>(text[index + offset]);
					if ((continuation & 0xC0) != 0x80)
					{
						return false;
					}
					codePoint = (codePoint << 6) | (continuation & 0x3Fu);
				}
				constexpr std::array<uint32_t, 5> Smallest = {0, 0, 0x80, 0x800, 0x10000};
				if (codePoint < Smallest[length] || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
				{
					return false;
				}
				index += length;
			}
			return true;
		}

		// Members of the "Template" object.
		Result<std::string> ReadString(Json const& object, char const* member, bool required)
		{
			auto const found = object.find(member);
			if (found == object.end())
			{
				return required ? Result<std::string>(MakeError("Template.{} is required", member)) : Result<std::string>(std::string());
			}
			if (!found->is_string())
			{
				return MakeError("Template.{}: a string is required", member);
			}
			return found->get<std::string>();
		}

		Result<ProjectTemplate> ParseTemplate(Json const& document, std::filesystem::path const& directory)
		{
			auto const object = document.find("Template");
			if (object == document.end() || !object->is_object())
			{
				return Error{"the \"Template\" object is missing"};
			}
			Json const& json = *object;
			ProjectTemplate result;
			result.Directory = MakeAbsolute(directory);

			Result<std::string> id = ReadString(json, "Id", true);
			if (!id)
			{
				return Error{id.GetError()};
			}
			if (!ProjectContent::IsValidId(id.GetValue()))
			{
				return MakeError("Template.Id: '{}' is not an id (lowercase letters and digits joined by single hyphens, at most {} bytes)",
				                 id.GetValue(), ProjectContent::MaxIdBytes);
			}
			if (id.GetValue() != GetFolderName(directory))
			{
				return MakeError("Template.Id: '{}' is not the name of the template's folder", id.GetValue());
			}
			result.Id = id.TakeValue();

			Result<std::string> name = ReadString(json, "Name", true);
			if (!name)
			{
				return Error{name.GetError()};
			}
			if (Result<void> valid = ProjectContent::CheckText("Template.Name", name.GetValue(), ProjectContent::MaxNameBytes); !valid)
			{
				return Error{valid.GetError()};
			}
			if (name.GetValue().empty())
			{
				return Error{"Template.Name must not be empty"};
			}
			result.Name = name.TakeValue();

			Result<std::string> description = ReadString(json, "Description", false);
			if (!description)
			{
				return Error{description.GetError()};
			}
			if (Result<void> valid =
			        ProjectContent::CheckText("Template.Description", description.GetValue(), ProjectContent::MaxDescriptionBytes);
			    !valid)
			{
				return Error{valid.GetError()};
			}
			result.Description = description.TakeValue();

			auto const order = json.find("Order");
			if (order == json.end())
			{
				return Error{"Template.Order is required"};
			}
			bool const inRange = order->is_number_unsigned() ? order->get<uint64_t>() <= static_cast<uint64_t>(ProjectContent::MaxOrder)
			                                                 : order->is_number_integer() && order->get<int64_t>() >= 0 &&
			                                                       order->get<int64_t>() <= ProjectContent::MaxOrder;
			if (!inRange)
			{
				return MakeError("Template.Order: an integer from 0 to {} is required", ProjectContent::MaxOrder);
			}
			result.Order = static_cast<int32_t>(order->get<int64_t>());

			if (auto const requirements = json.find("Requires"); requirements != json.end())
			{
				if (!requirements->is_object())
				{
					return Error{"Template.Requires: an object is required"};
				}
				if (auto const sdk = requirements->find("Sdk"); sdk != requirements->end())
				{
					if (!sdk->is_boolean())
					{
						return Error{"Template.Requires.Sdk: true or false is required"};
					}
					result.RequiresSdk = sdk->get<bool>();
				}
			}

			if (auto const tags = json.find("Tags"); tags != json.end())
			{
				if (!tags->is_array() || tags->size() > ProjectTemplate::MaxTags)
				{
					return MakeError("Template.Tags: an array of at most {} strings is required", ProjectTemplate::MaxTags);
				}
				for (Json const& tag : *tags)
				{
					if (!tag.is_string())
					{
						return Error{"Template.Tags: every tag is a string"};
					}
					std::string const text = tag.get<std::string>();
					if (Result<void> valid = ProjectContent::CheckText("Template.Tags", text, ProjectTemplate::MaxTagBytes); !valid)
					{
						return Error{valid.GetError()};
					}
					if (text.empty())
					{
						return Error{"Template.Tags: a tag must not be empty"};
					}
					result.Tags.push_back(text);
				}
			}

			for (auto const& [member, target] : {std::pair<char const*, std::string*>{"Thumbnail", &result.Thumbnail},
			                                     std::pair<char const*, std::string*>{"ProjectFile", &result.ProjectFile}})
			{
				Result<std::string> text = ReadString(json, member, false);
				if (!text)
				{
					return Error{text.GetError()};
				}
				if (text.GetValue().empty())
				{
					continue;
				}
				Result<std::string> path = ProjectContent::CheckRelativePath(fmt::format("Template.{}", member), text.GetValue());
				if (!path)
				{
					return Error{path.GetError()};
				}
				*target = path.TakeValue();
			}
			if (!result.Thumbnail.empty() && FileSystem::PathToUtf8(FileSystem::PathFromUtf8(result.Thumbnail).extension()) != ".png")
			{
				return MakeError("Template.Thumbnail: '{}' is not a PNG image (.png)", result.Thumbnail);
			}
			if (!result.ProjectFile.empty())
			{
				std::filesystem::path const file = result.Directory / FileSystem::PathFromUtf8(result.ProjectFile);
				if (FileSystem::PathToUtf8(file.extension()) != Project::FileExtension)
				{
					return MakeError("Template.ProjectFile: '{}' is not a project file (.sproj)", result.ProjectFile);
				}
				if (!FileSystem::IsRegularFile(file))
				{
					return MakeError("Template.ProjectFile: '{}' is not a file of the template", result.ProjectFile);
				}
			}
			return result;
		}

		// Writes what Project::Create writes for a start scene, without opening the project: the scene file, the asset registry
		// that registers it and the project file naming it.
		Result<std::filesystem::path> WriteDefaultSceneProject(std::filesystem::path const& root, std::string const& name,
		                                                       std::string const& fileName, Scene const& startScene)
		{
			ProjectSettings settings;
			settings.Name = name;
			std::filesystem::path const assets = root / FileSystem::PathFromUtf8(settings.AssetDirectory);
			if (Result<void> saved = SceneSerializer::SaveToFile(startScene, assets / FileSystem::PathFromUtf8(StartScenePath)); !saved)
			{
				return Error{saved.GetError()};
			}
			AssetHandle const scene = AssetHandle::Generate();
			AssetRegistry registry;
			AssetMetadata metadata;
			metadata.Handle = scene;
			metadata.Type = AssetType::Scene;
			metadata.Path = std::string(StartScenePath);
			if (Result<void> added = registry.Add(std::move(metadata)); !added)
			{
				return Error{added.GetError()};
			}
			if (Result<void> saved = registry.SaveToFile(assets / FileSystem::PathFromUtf8(AssetRegistry::FileName)); !saved)
			{
				return Error{saved.GetError()};
			}
			settings.StartScene = scene;
			std::filesystem::path const file = root / FileSystem::PathFromUtf8(fileName);
			if (Result<void> saved = Project(file, std::move(settings)).Save(); !saved)
			{
				return Error{saved.GetError()};
			}
			return file;
		}

		// Renames the copied project file and gives it the project's name; the rest of the file stays as the template has it.
		Result<std::filesystem::path> RenameProjectFile(std::filesystem::path const& copied, std::string const& name,
		                                                std::string const& fileName)
		{
			Result<Json> read = ProjectContent::ReadDocument(copied, "Project", Project::FormatVersion);
			if (!read)
			{
				return Error{read.GetError()};
			}
			Json document = read.TakeValue();
			auto const settings = document.find("Project");
			if (settings == document.end() || !settings->is_object())
			{
				return MakeError("'{}': missing \"Project\" settings", FileSystem::PathToUtf8(copied));
			}
			auto const templateName = settings->find("Name");
			std::string const ownName =
				templateName != settings->end() && templateName->is_string() ? templateName->get<std::string>() : "";
			(*settings)["Name"] = name;
			if (auto const window = settings->find("Window"); window != settings->end() && window->is_object())
			{
				auto const title = window->find("Title");
				if (title != window->end() && title->is_string() && title->get<std::string>() == ownName)
				{
					*title = name;
				}
			}

			std::filesystem::path const renamed = copied.parent_path() / FileSystem::PathFromUtf8(fileName);
			bool const sameFile = renamed == copied;
			if (!sameFile && FileSystem::Exists(renamed))
			{
				return MakeError("the template has a file '{}' already, which the project file cannot take the place of",
				                 FileSystem::PathToUtf8(renamed.filename()));
			}
			if (Result<void> saved = FileSystem::WriteTextFile(renamed, DumpJson(document)); !saved)
			{
				return Error{saved.GetError()};
			}
			if (!sameFile)
			{
				if (Result<void> removed = FileSystem::Remove(copied); !removed)
				{
					return Error{removed.GetError()};
				}
			}
			return renamed;
		}
	}

	namespace ProjectContent
	{
		bool IsValidId(std::string_view id)
		{
			if (id.empty() || id.size() > MaxIdBytes || id.front() == '-' || id.back() == '-')
			{
				return false;
			}
			char previous = 0;
			for (char const character : id)
			{
				bool const letterOrDigit = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
				if (!letterOrDigit && (character != '-' || previous == '-'))
				{
					return false;
				}
				previous = character;
			}
			return true;
		}

		Result<void> CheckText(std::string_view member, std::string_view text, size_t maxBytes)
		{
			if (text.size() > maxBytes)
			{
				return MakeError("{}: at most {} bytes, got {}", member, maxBytes, text.size());
			}
			if (!IsValidUtf8(text))
			{
				return MakeError("{}: not valid UTF-8", member);
			}
			bool const control = std::ranges::any_of(text,
			                                         [](char character)
			                                         {
														 auto const byte = static_cast<uint8_t>(character);
														 return byte < 0x20 || byte == 0x7F;
													 });
			if (control)
			{
				return MakeError("{}: control characters are not allowed", member);
			}
			return {};
		}

		Result<std::string> CheckRelativePath(std::string_view member, std::string_view path)
		{
			if (path.find('\\') != std::string_view::npos)
			{
				return MakeError("{}: '{}' uses backslashes; paths use forward slashes", member, path);
			}
			size_t start = 0;
			while (start <= path.size())
			{
				size_t const end = std::min(path.find('/', start), path.size());
				if (path.substr(start, end - start) == "..")
				{
					return MakeError("{}: '{}' must not climb out with \"..\"", member, path);
				}
				start = end + 1;
			}
			Result<std::string> normalized = NormalizeAssetPath(path);
			if (!normalized)
			{
				return MakeError("{}: {}", member, normalized.GetError());
			}
			if (normalized.GetValue() != path)
			{
				return MakeError("{}: '{}' is not in its plain form ('{}')", member, path, normalized.GetValue());
			}
			return normalized;
		}

		std::filesystem::path FindThumbnail(std::filesystem::path const& folder, std::string_view relativePath)
		{
			if (folder.empty() || relativePath.empty())
			{
				return {};
			}
			std::filesystem::path const file = (folder / FileSystem::PathFromUtf8(relativePath)).lexically_normal();
			std::optional<uint64_t> const size = FileSystem::GetFileSize(file);
			if (!size || *size > MaxThumbnailBytes || *size < PngSignature.size())
			{
				return {};
			}
			Result<Buffer> const bytes = FileSystem::ReadBinaryFile(file);
			if (!bytes || bytes.GetValue().GetSize() < PngSignature.size() ||
			    !std::equal(PngSignature.begin(), PngSignature.end(), bytes.GetValue().As<uint8_t>()))
			{
				return {};
			}
			return file;
		}

		Result<Json> ReadDocument(std::filesystem::path const& file, std::string_view type, int version)
		{
			std::optional<uint64_t> const size = FileSystem::GetFileSize(file);
			if (!size)
			{
				return MakeError("'{}' does not exist or cannot be read", FileSystem::PathToUtf8(file));
			}
			if (*size > MaxDocumentBytes)
			{
				return MakeError("'{}' is larger than {} bytes", FileSystem::PathToUtf8(file), MaxDocumentBytes);
			}
			Result<std::string> text = FileSystem::ReadTextFile(file);
			if (!text)
			{
				return Error{text.GetError()};
			}
			Result<Json> document = ParseJson(text.GetValue());
			if (!document)
			{
				return MakeError("'{}' is not valid JSON: {}", FileSystem::PathToUtf8(file), document.GetError());
			}
			if (Result<int> header = ReadFileHeader(document.GetValue(), type, version); !header)
			{
				return MakeError("'{}': {}", FileSystem::PathToUtf8(file), header.GetError());
			}
			return document;
		}
	}

	std::filesystem::path ProjectTemplate::GetThumbnailPath() const
	{
		return ProjectContent::FindThumbnail(Directory, Thumbnail);
	}

	ProjectTemplate ProjectTemplate::MakeDefault()
	{
		ProjectTemplate result;
		result.Id = std::string(DefaultId);
		result.Name = "Empty 3D";
		result.Description = "A camera, a sun, a sky, a floor and a few shapes to start a 3D game from.";
		result.Order = 1;
		result.Tags = {"3D"};
		return result;
	}

	Result<ProjectTemplate> ProjectTemplate::Load(std::filesystem::path const& directory)
	{
		Result<Json> document = ProjectContent::ReadDocument(directory / FileSystem::PathFromUtf8(FileName), FileType, FormatVersion);
		if (!document)
		{
			return Error{document.GetError()};
		}
		Result<ProjectTemplate> parsed = ParseTemplate(document.GetValue(), directory);
		if (!parsed)
		{
			return MakeError("'{}': {}", FileSystem::PathToUtf8(directory / FileSystem::PathFromUtf8(FileName)), parsed.GetError());
		}
		return parsed;
	}

	std::vector<ProjectTemplate> ProjectTemplate::List(std::filesystem::path const& root, std::vector<std::string>& warnings)
	{
		std::vector<ProjectTemplate> templates;
		if (root.empty() || !FileSystem::IsDirectory(root))
		{
			return templates;
		}
		std::vector<std::filesystem::path> folders;
		std::error_code errorCode;
		for (auto it = std::filesystem::directory_iterator(root, errorCode); !errorCode && it != std::filesystem::directory_iterator();
		     it.increment(errorCode))
		{
			std::error_code statusError;
			if (it->is_directory(statusError) && !it->is_symlink(statusError) &&
			    FileSystem::Exists(it->path() / FileSystem::PathFromUtf8(FileName)))
			{
				folders.push_back(it->path());
			}
		}
		if (errorCode)
		{
			warnings.push_back(
				fmt::format("the templates in '{}' cannot be listed: {}", FileSystem::PathToUtf8(root), errorCode.message()));
		}
		std::sort(folders.begin(), folders.end());
		for (std::filesystem::path const& folder : folders)
		{
			Result<ProjectTemplate> loaded = Load(folder);
			if (!loaded)
			{
				warnings.push_back(
					fmt::format("the template '{}' was left out: {}", FileSystem::PathToUtf8(folder.filename()), loaded.GetError()));
				continue;
			}
			templates.push_back(loaded.TakeValue());
		}
		std::sort(templates.begin(), templates.end(),
		          [](ProjectTemplate const& left, ProjectTemplate const& right)
		          {
					  return left.Order != right.Order ? left.Order < right.Order : left.Id < right.Id;
				  });
		return templates;
	}

	Result<std::filesystem::path> ProjectTemplate::Instantiate(ProjectTemplate const& projectTemplate,
	                                                           std::filesystem::path const& directory, std::string const& name,
	                                                           TemplateOptions const& options, Scene const* startScene)
	{
		ProjectSettings named;
		named.Name = name;
		if (Result<void> valid = ValidateProjectSettings(named); !valid)
		{
			return Error{valid.GetError()};
		}
		if (projectTemplate.IsDefaultScene() && startScene == nullptr)
		{
			return MakeError("the template '{}' is the editor's default scene, which needs the start scene", projectTemplate.Id);
		}
		if (!projectTemplate.IsDefaultScene() && projectTemplate.Directory.empty())
		{
			return MakeError("the template '{}' has no folder to copy", projectTemplate.Id);
		}
		std::filesystem::path const root = MakeAbsolute(directory);
		if (Result<void> available = CheckNewProjectDirectory(root); !available)
		{
			return Error{available.GetError()};
		}
		std::string portable = FileSystem::MakePortableFileName(name);
		if (portable.empty())
		{
			portable = "Project";
		}
		std::string const fileName = portable + std::string(Project::FileExtension);

		NewDirectoryCleanup cleanup(root);
		std::filesystem::path projectFile;
		if (projectTemplate.IsDefaultScene())
		{
			Result<std::filesystem::path> written = WriteDefaultSceneProject(root, name, fileName, *startScene);
			if (!written)
			{
				return Error{written.GetError()};
			}
			projectFile = written.TakeValue();
		}
		else
		{
			ProjectCopyOptions copyOptions;
			copyOptions.ExcludedFiles.push_back(std::string(FileName));
			if (!projectTemplate.Thumbnail.empty())
			{
				copyOptions.ExcludedFiles.push_back(projectTemplate.Thumbnail);
			}
			Result<ProjectCopyResult> copied = ProjectCopy::Copy(projectTemplate.Directory, root, copyOptions);
			if (!copied)
			{
				return Error{copied.GetError()};
			}
			for (std::string const& warning : copied.GetValue().Warnings)
			{
				ST_CORE_WARN("Template '{}': {}", projectTemplate.Id, warning);
			}
			Result<std::filesystem::path> renamed =
				RenameProjectFile(root / FileSystem::PathFromUtf8(projectTemplate.ProjectFile), name, fileName);
			if (!renamed)
			{
				return Error{renamed.GetError()};
			}
			projectFile = renamed.TakeValue();
		}

		std::filesystem::path const gitIgnore = root / ".gitignore";
		if (options.GitIgnore && !FileSystem::Exists(gitIgnore))
		{
			if (Result<void> written = FileSystem::WriteTextFile(gitIgnore, GitIgnoreText); !written)
			{
				return Error{written.GetError()};
			}
		}
		cleanup.Commit();
		return projectFile;
	}
}
