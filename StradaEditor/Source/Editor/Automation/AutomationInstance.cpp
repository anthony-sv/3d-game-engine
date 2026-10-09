#include "Editor/Automation/AutomationInstance.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Platform.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <algorithm>
#include <system_error>

namespace Strada
{
	namespace
	{
		constexpr int InstanceFormatVersion = 1;

		void RestrictToOwner(std::filesystem::path const& path, std::filesystem::perms permissions)
		{
#if !defined(ST_PLATFORM_WINDOWS)
			std::error_code errorCode;
			std::filesystem::permissions(path, permissions, std::filesystem::perm_options::replace, errorCode);
#else
			(void)path;
			(void)permissions;
#endif
		}
	}

	std::filesystem::path AutomationInstance::GetDirectory()
	{
		return FileSystem::GetUserDataDirectory() / "Editor" / "Instances";
	}

	std::filesystem::path AutomationInstance::GetPath(uint32_t processID)
	{
		return GetDirectory() / (std::to_string(processID) + ".json");
	}

	Result<std::filesystem::path> AutomationInstance::Write(AutomationInstanceInfo const& info)
	{
		std::filesystem::path const directory = GetDirectory();
		if (Result<void> created = FileSystem::CreateDirectories(directory); !created)
		{
			return Error{created.GetError()};
		}
		// Owner-only before the token is written (the atomic write creates a temporary file inside this directory).
		RestrictToOwner(directory, std::filesystem::perms::owner_all);

		Json document = Json::object();
		document["Strada"] = MakeFileHeader("AutomationInstance", InstanceFormatVersion);
		document["ProcessID"] = info.ProcessID;
		document["Port"] = info.Port;
		document["Token"] = info.Token;
		document["Project"] = info.Project;
		document["Version"] = info.Version;

		std::filesystem::path const path = GetPath(info.ProcessID);
		if (Result<void> written = FileSystem::WriteTextFile(path, DumpJson(document)); !written)
		{
			return Error{written.GetError()};
		}
		RestrictToOwner(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
		return path;
	}

	Result<AutomationInstanceInfo> AutomationInstance::Read(std::filesystem::path const& path)
	{
		Result<std::string> text = FileSystem::ReadTextFile(path);
		if (!text)
		{
			return Error{text.GetError()};
		}
		Result<Json> json = ParseJson(text.GetValue());
		if (!json)
		{
			return Error{json.GetError()};
		}
		Json const& document = json.GetValue();
		if (Result<int> header = ReadFileHeader(document, "AutomationInstance", InstanceFormatVersion); !header)
		{
			return Error{header.GetError()};
		}

		AutomationInstanceInfo info;
		DeserializationContext const context;
		auto const read = [&](char const* key, auto& out) -> Result<void>
		{
			auto const it = document.find(key);
			if (it == document.end())
			{
				return MakeError("instance file has no \"{}\"", key);
			}
			using T = std::remove_reference_t<decltype(out)>;
			if (Result<void> result = JsonTraits<T>::FromJson(*it, out, context); !result)
			{
				return MakeError("{}: {}", key, result.GetError());
			}
			return {};
		};
		for (Result<void> result : {read("ProcessID", info.ProcessID), read("Port", info.Port), read("Token", info.Token),
		                            read("Project", info.Project), read("Version", info.Version)})
		{
			if (!result)
			{
				return Error{result.GetError()};
			}
		}
		return info;
	}

	void AutomationInstance::Remove(uint32_t processID)
	{
		std::error_code errorCode;
		std::filesystem::remove(GetPath(processID), errorCode);
	}

	std::vector<AutomationInstanceInfo> AutomationInstance::FindRunningInstances()
	{
		std::vector<AutomationInstanceInfo> instances;
		std::error_code errorCode;
		std::filesystem::directory_iterator it(GetDirectory(), errorCode);
		if (errorCode)
		{
			return instances;
		}
		for (std::filesystem::directory_iterator const end; it != end; it.increment(errorCode))
		{
			if (errorCode)
			{
				break;
			}
			if (it->path().extension() != ".json")
			{
				continue;
			}
			Result<AutomationInstanceInfo> info = Read(it->path());
			if (info && Platform::IsProcessRunning(info.GetValue().ProcessID))
			{
				instances.push_back(std::move(info.GetValue()));
			}
			else
			{
				std::error_code removeError;
				std::filesystem::remove(it->path(), removeError);
			}
		}
		std::sort(instances.begin(), instances.end(),
		          [](AutomationInstanceInfo const& a, AutomationInstanceInfo const& b)
		          {
					  return a.ProcessID < b.ProcessID;
				  });
		return instances;
	}
}
