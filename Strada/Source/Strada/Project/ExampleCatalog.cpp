#include "stpch.h"
#include "Strada/Project/ExampleCatalog.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Project/Project.h"
#include "Strada/Project/ProjectTemplate.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <algorithm>
#include <set>
#include <system_error>
#include <utility>

namespace Strada
{
	namespace
	{
		std::filesystem::path MakeAbsolute(std::filesystem::path const& path)
		{
			std::error_code errorCode;
			std::filesystem::path const absolute = std::filesystem::absolute(path, errorCode);
			return errorCode ? path.lexically_normal() : absolute.lexically_normal();
		}

		Result<std::string> ReadString(Json const& object, std::string_view member, bool required)
		{
			auto const found = object.find(member);
			if (found == object.end())
			{
				return required ? Result<std::string>(MakeError("{} is required", member)) : Result<std::string>(std::string());
			}
			if (!found->is_string())
			{
				return MakeError("{}: a string is required", member);
			}
			return found->get<std::string>();
		}

		Result<std::string> ReadText(Json const& object, std::string_view member, bool required, size_t maxBytes)
		{
			Result<std::string> text = ReadString(object, member, required);
			if (!text)
			{
				return text;
			}
			if (Result<void> valid = ProjectContent::CheckText(member, text.GetValue(), maxBytes); !valid)
			{
				return Error{valid.GetError()};
			}
			if (required && text.GetValue().empty())
			{
				return MakeError("{} must not be empty", member);
			}
			return text;
		}

		Result<std::string> ReadPath(Json const& object, std::string_view member, bool required)
		{
			Result<std::string> text = ReadString(object, member, required);
			if (!text || (!required && text.GetValue().empty()))
			{
				return text;
			}
			return ProjectContent::CheckRelativePath(member, text.GetValue());
		}

		Result<ExampleEntry> ParseExample(Json const& json)
		{
			if (!json.is_object())
			{
				return Error{"an example is an object"};
			}
			ExampleEntry example;
			Result<std::string> id = ReadString(json, "Id", true);
			if (!id)
			{
				return Error{id.GetError()};
			}
			if (!ProjectContent::IsValidId(id.GetValue()))
			{
				return MakeError("Id: '{}' is not an id (lowercase letters and digits joined by single hyphens, at most {} bytes)",
				                 id.GetValue(), ProjectContent::MaxIdBytes);
			}
			example.Id = id.TakeValue();

			Result<std::string> name = ReadText(json, "Name", true, ProjectContent::MaxNameBytes);
			if (!name)
			{
				return Error{name.GetError()};
			}
			example.Name = name.TakeValue();
			Result<std::string> description = ReadText(json, "Description", false, ProjectContent::MaxDescriptionBytes);
			if (!description)
			{
				return Error{description.GetError()};
			}
			example.Description = description.TakeValue();

			auto const order = json.find("Order");
			if (order == json.end())
			{
				return Error{"Order is required"};
			}
			bool const inRange = order->is_number_unsigned() ? order->get<uint64_t>() <= static_cast<uint64_t>(ProjectContent::MaxOrder)
			                                                 : order->is_number_integer() && order->get<int64_t>() >= 0 &&
			                                                       order->get<int64_t>() <= ProjectContent::MaxOrder;
			if (!inRange)
			{
				return MakeError("Order: an integer from 0 to {} is required", ProjectContent::MaxOrder);
			}
			example.Order = static_cast<int32_t>(order->get<int64_t>());

			if (auto const requirements = json.find("Requires"); requirements != json.end())
			{
				if (!requirements->is_object())
				{
					return Error{"Requires: an object is required"};
				}
				if (auto const sdk = requirements->find("Sdk"); sdk != requirements->end())
				{
					if (!sdk->is_boolean())
					{
						return Error{"Requires.Sdk: true or false is required"};
					}
					example.RequiresSdk = sdk->get<bool>();
				}
			}

			Result<std::string> directory = ReadPath(json, "Directory", true);
			if (!directory)
			{
				return Error{directory.GetError()};
			}
			example.Directory = directory.TakeValue();
			Result<std::string> projectFile = ReadPath(json, "ProjectFile", true);
			if (!projectFile)
			{
				return Error{projectFile.GetError()};
			}
			if (FileSystem::PathToUtf8(FileSystem::PathFromUtf8(projectFile.GetValue()).extension()) != Project::FileExtension)
			{
				return MakeError("ProjectFile: '{}' is not a project file (.sproj)", projectFile.GetValue());
			}
			example.ProjectFile = projectFile.TakeValue();
			Result<std::string> thumbnail = ReadPath(json, "Thumbnail", false);
			if (!thumbnail)
			{
				return Error{thumbnail.GetError()};
			}
			example.Thumbnail = thumbnail.TakeValue();

			if (auto const needs = json.find("Needs"); needs != json.end())
			{
				if (!needs->is_object())
				{
					return Error{"Needs: an object is required"};
				}
				if (auto const paths = needs->find("Paths"); paths != needs->end())
				{
					if (!paths->is_array() || paths->size() > ExampleCatalog::MaxNeedsPaths)
					{
						return MakeError("Needs.Paths: an array of at most {} paths is required", ExampleCatalog::MaxNeedsPaths);
					}
					for (Json const& path : *paths)
					{
						if (!path.is_string())
						{
							return Error{"Needs.Paths: every path is a string"};
						}
						Result<std::string> checked = ProjectContent::CheckRelativePath("Needs.Paths", path.get<std::string>());
						if (!checked)
						{
							return Error{checked.GetError()};
						}
						example.NeedsPaths.push_back(checked.TakeValue());
					}
				}
				Result<std::string> hint = ReadText(*needs, "Hint", false, ExampleCatalog::MaxHintBytes);
				if (!hint)
				{
					return MakeError("Needs.{}", hint.GetError());
				}
				example.NeedsHint = hint.TakeValue();
			}
			return example;
		}
	}

	Result<ExampleCatalog> ExampleCatalog::Load(std::filesystem::path const& catalogPath)
	{
		if (catalogPath.empty())
		{
			return Error{"there is no catalog of examples"};
		}
		std::filesystem::path const file = MakeAbsolute(catalogPath);
		Result<Json> document = ProjectContent::ReadDocument(file, FileType, FormatVersion);
		if (!document)
		{
			return Error{document.GetError()};
		}
		auto const examples = document.GetValue().find("Examples");
		if (examples == document.GetValue().end() || !examples->is_array())
		{
			return MakeError("'{}': the \"Examples\" array is missing", FileSystem::PathToUtf8(file));
		}

		ExampleCatalog catalog;
		catalog.m_Root = file.parent_path();
		std::set<std::string> ids;
		for (size_t index = 0; index < examples->size(); index++)
		{
			if (catalog.m_Examples.size() == MaxExamples)
			{
				catalog.m_Warnings.push_back(fmt::format("the catalog lists more than {} examples: the rest were left out", MaxExamples));
				break;
			}
			Result<ExampleEntry> example = ParseExample((*examples)[index]);
			if (!example)
			{
				catalog.m_Warnings.push_back(fmt::format("example {} was left out: {}", index, example.GetError()));
				continue;
			}
			if (!ids.insert(example.GetValue().Id).second)
			{
				catalog.m_Warnings.push_back(
					fmt::format("example {} was left out: an earlier example has the id '{}'", index, example.GetValue().Id));
				continue;
			}
			catalog.m_Examples.push_back(example.TakeValue());
		}
		std::sort(catalog.m_Examples.begin(), catalog.m_Examples.end(),
		          [](ExampleEntry const& left, ExampleEntry const& right)
		          {
					  return left.Order != right.Order ? left.Order < right.Order : left.Id < right.Id;
				  });
		return catalog;
	}

	ExampleEntry const* ExampleCatalog::Find(std::string_view id) const
	{
		auto const found = std::ranges::find_if(m_Examples,
		                                        [id](ExampleEntry const& example)
		                                        {
													return example.Id == id;
												});
		return found != m_Examples.end() ? &*found : nullptr;
	}

	std::filesystem::path ExampleCatalog::GetDirectory(ExampleEntry const& example) const
	{
		return (m_Root / FileSystem::PathFromUtf8(example.Directory)).lexically_normal();
	}

	std::filesystem::path ExampleCatalog::GetProjectFile(ExampleEntry const& example) const
	{
		return GetDirectory(example) / FileSystem::PathFromUtf8(example.ProjectFile);
	}

	std::filesystem::path ExampleCatalog::GetThumbnailPath(ExampleEntry const& example) const
	{
		return ProjectContent::FindThumbnail(m_Root, example.Thumbnail);
	}

	bool ExampleCatalog::IsAvailable(ExampleEntry const& example) const
	{
		if (m_Root.empty() || !FileSystem::IsRegularFile(GetProjectFile(example)))
		{
			return false;
		}
		std::filesystem::path const directory = GetDirectory(example);
		return std::ranges::all_of(example.NeedsPaths,
		                           [&directory](std::string const& path)
		                           {
									   return FileSystem::Exists(directory / FileSystem::PathFromUtf8(path));
								   });
	}
}
