#pragma once

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/AssetRegistry.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"
#include "Strada/Script/ScriptEngine.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Strada::Testing
{
	// The game scripts of Tests/TestScripts, built next to the test executable.
	inline std::filesystem::path GetTestScriptsPath()
	{
		return FileSystem::GetExecutableDirectory() / "TestScripts" / "Strada.TestScripts.dll";
	}

	// The scripts of Projects/FeatureTest, which CMake builds next to the test executable.
	inline std::filesystem::path GetFeatureTestScriptsPath()
	{
		return FileSystem::GetExecutableDirectory() / "FeatureTest" / "FeatureTest.dll";
	}

	// Opens a copy of a project's asset registry and visual scripts (the files of its Assets directory) in the directory as
	// the asset directory, so that graphs naming other graphs find them by handle. A copy, because opening a directory saves
	// its registry; the project's other files are missing from it, which the asset manager records without failing. Needs
	// an initialized AssetManager; the directory must outlive what the asset manager has open.
	inline void OpenVisualScriptsCopy(std::filesystem::path const& assets, std::filesystem::path const& directory)
	{
		Result<AssetRegistry> registry = AssetRegistry::LoadFromFile(assets / AssetRegistry::FileName, DeserializationContext{});
		REQUIRE_MESSAGE(registry.IsOk(), (registry ? std::string() : registry.GetError()));
		REQUIRE(FileSystem::CreateDirectories(directory).IsOk());
		REQUIRE(FileSystem::Copy(assets / AssetRegistry::FileName, directory / AssetRegistry::FileName, false).IsOk());
		for (AssetMetadata const& asset : registry.GetValue().GetAll())
		{
			if (asset.Type == AssetType::VisualScript)
			{
				std::filesystem::path const target = directory / FileSystem::PathFromUtf8(asset.Path);
				REQUIRE(FileSystem::CreateDirectories(target.parent_path()).IsOk());
				REQUIRE(FileSystem::Copy(assets / FileSystem::PathFromUtf8(asset.Path), target, false).IsOk());
			}
		}
		Result<AssetRefreshResult> const opened = AssetManager::OpenAssetDirectory(directory);
		REQUIRE_MESSAGE(opened.IsOk(), (opened ? std::string() : opened.GetError()));
	}

	// Initializes the ScriptEngine with game scripts loaded (the test scripts unless told otherwise) for as long as it lives.
	class ScriptEngineScope
	{
	public:
		explicit ScriptEngineScope(std::filesystem::path const& assembly = GetTestScriptsPath())
		{
			Result<void> const initialized = ScriptEngine::Init();
			REQUIRE_MESSAGE(initialized.IsOk(), (initialized ? std::string() : initialized.GetError()));
			Result<void> const loaded = ScriptEngine::LoadGameAssembly(assembly);
			if (!loaded)
			{
				ScriptEngine::Shutdown();
			}
			REQUIRE_MESSAGE(loaded.IsOk(), (loaded ? std::string() : loaded.GetError()));
		}

		~ScriptEngineScope()
		{
			if (ScriptEngine::IsInitialized())
			{
				ScriptEngine::Shutdown();
			}
		}

		ScriptEngineScope(ScriptEngineScope const&) = delete;
		ScriptEngineScope& operator=(ScriptEngineScope const&) = delete;
	};

	// Initializes the ScriptEngine without game scripts for as long as it lives: visual scripts run, C# scripts do not.
	class ScriptRuntimeScope
	{
	public:
		ScriptRuntimeScope()
		{
			Result<void> const initialized = ScriptEngine::Init();
			REQUIRE_MESSAGE(initialized.IsOk(), (initialized ? std::string() : initialized.GetError()));
		}

		~ScriptRuntimeScope()
		{
			if (ScriptEngine::IsInitialized())
			{
				ScriptEngine::Shutdown();
			}
		}

		ScriptRuntimeScope(ScriptRuntimeScope const&) = delete;
		ScriptRuntimeScope& operator=(ScriptRuntimeScope const&) = delete;
	};

	// Whether a log entry since firstIndex contains the text.
	inline bool WasLogged(uint64_t firstIndex, std::string_view text)
	{
		std::vector<LogEntry> const entries = Log::GetEntries(firstIndex);
		return std::any_of(entries.begin(), entries.end(),
		                   [text](LogEntry const& entry)
		                   {
							   return entry.Message.find(text) != std::string::npos;
						   });
	}
}
