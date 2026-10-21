#pragma once

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
