#include "stpch.h"
#include "Strada/Project/Project.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"

#include <system_error>
#include <utility>

namespace Strada
{
	namespace
	{
		constexpr std::string_view ProjectFileType = "Project";
		constexpr std::string_view StartScenePath = "Scenes/Main.sscene";

		std::filesystem::path MakeAbsolute(std::filesystem::path const& path)
		{
			std::error_code errorCode;
			std::filesystem::path const absolute = std::filesystem::absolute(path, errorCode);
			return errorCode ? path.lexically_normal() : absolute.lexically_normal();
		}

		Result<Json> ReadDocument(std::filesystem::path const& file)
		{
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
			return document;
		}

		// Removes what a failed Create wrote: the directory was empty or missing before.
		class CreationCleanup
		{
		public:
			CreationCleanup(std::filesystem::path directory, bool removeDirectory)
				: m_Directory(std::move(directory)),
				  m_RemoveDirectory(removeDirectory)
			{
			}

			~CreationCleanup()
			{
				if (m_Committed)
				{
					return;
				}
				if (m_RemoveDirectory)
				{
					(void)FileSystem::RemoveAll(m_Directory);
					return;
				}
				std::error_code errorCode;
				for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(m_Directory, errorCode))
				{
					(void)FileSystem::RemoveAll(entry.path());
				}
			}

			CreationCleanup(CreationCleanup const&) = delete;
			CreationCleanup& operator=(CreationCleanup const&) = delete;

			void Commit() { m_Committed = true; }

		private:
			std::filesystem::path m_Directory;
			bool m_RemoveDirectory;
			bool m_Committed = false;
		};
	}

	Project::Project(std::filesystem::path filePath, ProjectSettings settings)
		: m_FilePath(MakeAbsolute(filePath)),
		  m_Settings(std::move(settings))
	{
	}

	Result<Ref<Project>> Project::Create(std::filesystem::path const& directory, std::string const& name, Scene const& startScene)
	{
		ProjectSettings settings;
		settings.Name = name;
		if (Result<void> valid = ValidateProjectSettings(settings); !valid)
		{
			return Error{valid.GetError()};
		}

		std::filesystem::path const root = MakeAbsolute(directory);
		bool const existed = FileSystem::Exists(root);
		if (existed)
		{
			if (!FileSystem::IsDirectory(root))
			{
				return MakeError("'{}' is not a directory", FileSystem::PathToUtf8(root));
			}
			std::error_code errorCode;
			if (!std::filesystem::is_empty(root, errorCode) || errorCode)
			{
				return MakeError("'{}' is not empty; projects are created in empty or new directories", FileSystem::PathToUtf8(root));
			}
		}

		std::string fileName = FileSystem::MakePortableFileName(name);
		if (fileName.empty())
		{
			fileName = "Project";
		}
		CreationCleanup cleanup(root, !existed);
		Ref<Project> project =
			CreateRef<Project>(root / FileSystem::PathFromUtf8(fileName + std::string(FileExtension)), std::move(settings));
		std::filesystem::path const scenePath = project->GetAssetDirectory() / FileSystem::PathFromUtf8(StartScenePath);
		if (Result<void> created = FileSystem::CreateDirectories(scenePath.parent_path()); !created)
		{
			return Error{created.GetError()};
		}
		if (Result<void> saved = SceneSerializer::SaveToFile(startScene, scenePath); !saved)
		{
			return Error{saved.GetError()};
		}
		if (Result<AssetRefreshResult> opened = AssetManager::OpenAssetDirectory(project->GetAssetDirectory()); !opened)
		{
			return Error{opened.GetError()};
		}
		project->m_Settings.StartScene = AssetManager::FindByPath(StartScenePath);
		if (Result<void> saved = project->Save(); !saved)
		{
			AssetManager::CloseAssetDirectory();
			return Error{saved.GetError()};
		}
		cleanup.Commit();
		return project;
	}

	Result<Ref<Project>> Project::Open(std::filesystem::path const& file, std::vector<std::string>* warnings)
	{
		std::filesystem::path const absolute = MakeAbsolute(file);
		Result<Json> document = ReadDocument(absolute);
		if (!document)
		{
			return Error{document.GetError()};
		}
		if (Result<int> header = ReadFileHeader(document.GetValue(), ProjectFileType, FormatVersion); !header)
		{
			return MakeError("'{}': {}", FileSystem::PathToUtf8(absolute), header.GetError());
		}

		// Asset references (the start scene) resolve through the asset directory, so it opens before the settings are read.
		ProjectSettings location;
		Json const& settings = document.GetValue().contains("Project") ? document.GetValue()["Project"] : Json();
		if (settings.is_object() && settings.contains("AssetDirectory"))
		{
			if (Result<void> read = JsonTraits<std::string>::FromJson(settings["AssetDirectory"], location.AssetDirectory, {}); !read)
			{
				return MakeError("Project.AssetDirectory: {}", read.GetError());
			}
		}
		if (Result<void> valid = ValidateProjectSettings(location); !valid)
		{
			return Error{valid.GetError()};
		}
		Project probe(absolute, location);
		if (Result<AssetRefreshResult> opened = AssetManager::OpenAssetDirectory(probe.GetAssetDirectory()); !opened)
		{
			return MakeError("the project's asset directory cannot be opened: {}", opened.GetError());
		}

		Result<ProjectSettings> loaded =
			Deserialize(document.GetValue(), AssetManager::CreateDeserializationContext(UnknownFieldPolicy::Warn, warnings));
		if (!loaded)
		{
			AssetManager::CloseAssetDirectory();
			return MakeError("'{}': {}", FileSystem::PathToUtf8(absolute), loaded.GetError());
		}
		return CreateRef<Project>(absolute, loaded.TakeValue());
	}

	Result<void> Project::Save() const
	{
		return FileSystem::WriteTextFile(m_FilePath, DumpJson(Serialize(m_Settings)));
	}

	Result<void> Project::ApplySettings(Json const& patch)
	{
		DeserializationContext const context =
			AssetManager::IsInitialized() ? AssetManager::CreateDeserializationContext() : DeserializationContext();
		ProjectSettings updated = m_Settings;
		if (Result<void> result = DeserializeFields<StructTraits<ProjectSettings>>(patch, updated, context, "project"); !result)
		{
			return result;
		}
		if (updated.AssetDirectory != m_Settings.AssetDirectory)
		{
			return Error{"Project.AssetDirectory: cannot change while the project is open"};
		}
		if (Result<void> valid = ValidateProjectSettings(updated); !valid)
		{
			return valid;
		}
		m_Settings = std::move(updated);
		return {};
	}

	std::filesystem::path Project::GetAssetDirectory() const
	{
		return (GetDirectory() / FileSystem::PathFromUtf8(m_Settings.AssetDirectory)).lexically_normal();
	}

	Json Project::Serialize(ProjectSettings const& settings)
	{
		Json document = Json::object();
		document["Strada"] = MakeFileHeader(ProjectFileType, FormatVersion);
		document["Project"] = SerializeFields<StructTraits<ProjectSettings>>(settings);
		return document;
	}

	Result<ProjectSettings> Project::Deserialize(Json const& document, DeserializationContext const& context)
	{
		if (Result<int> header = ReadFileHeader(document, ProjectFileType, FormatVersion); !header)
		{
			return Error{header.GetError()};
		}
		if (!document.contains("Project"))
		{
			return Error{"missing \"Project\" settings"};
		}
		ProjectSettings settings;
		if (Result<void> result = DeserializeFields<StructTraits<ProjectSettings>>(document["Project"], settings, context, "project");
		    !result)
		{
			return Error{result.GetError()};
		}
		if (Result<void> valid = ValidateProjectSettings(settings); !valid)
		{
			return Error{valid.GetError()};
		}
		return settings;
	}
}
