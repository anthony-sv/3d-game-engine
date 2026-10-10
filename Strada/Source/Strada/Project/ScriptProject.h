#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	class Project;

	enum class ScriptBuildConfiguration : uint8_t
	{
		Debug = 0,
		Release
	};

	enum class ScriptDiagnosticSeverity : uint8_t
	{
		Error = 0,
		Warning
	};

	// An error or warning of a script build (compiler, MSBuild or NuGet).
	struct ScriptDiagnostic
	{
		ScriptDiagnosticSeverity Severity = ScriptDiagnosticSeverity::Error;
		// The source file (absolute, as the build reports it); empty for problems without a file ("MSBUILD", "CSC").
		std::filesystem::path File;
		// 1-based; 0 when the diagnostic has no position.
		uint32_t Line = 0;
		uint32_t Column = 0;
		// "CS1002", "MSB1009", "NU1101", ...
		std::string Code;
		std::string Message;

		bool operator==(ScriptDiagnostic const& other) const = default;
	};

	// What a build needs, taken from a project on the main thread so the build can run on any thread.
	struct ScriptBuildSettings
	{
		std::filesystem::path ProjectFile;
		std::filesystem::path OutputDirectory;
		ScriptBuildConfiguration Configuration = ScriptBuildConfiguration::Debug;
		// The .NET SDK driver (dotnet); empty finds it like ScriptProject::FindDotNet.
		std::filesystem::path DotNet;
	};

	struct ScriptBuildResult
	{
		bool Succeeded = false;
		// Errors and warnings, each once, in the order the build reported them.
		std::vector<ScriptDiagnostic> Diagnostics;
		// Everything the build printed.
		std::string Output;
		// The built game assembly; empty when the build failed.
		std::filesystem::path Assembly;
	};

	// The game's C# scripts: <project>/Scripts/<Name>.csproj, compiled by the .NET SDK into the project's ScriptModule
	// (<Name>.dll). The editor writes the C# project once (it then belongs to the user, who may add packages and files),
	// keeps Scripts/Strada.props pointing at this machine's Strada.ScriptCore.dll, and adds new scripts from a template to
	// Scripts/Source/.
	class ScriptProject
	{
	public:
		static constexpr std::string_view DirectoryName = "Scripts";
		static constexpr std::string_view PropsFileName = "Strada.props";

		// <project>/Scripts/<ScriptModule's name>.csproj.
		static std::filesystem::path GetProjectFile(Project const& project);
		// The game assembly (the project's ScriptModule, absolute).
		static std::filesystem::path GetAssemblyPath(Project const& project);
		static bool Exists(Project const& project);

		// Writes the C# project and Scripts/.gitignore unless they exist, and Strada.props (the engine's
		// Strada.ScriptCore.dll for this machine) when its content changes.
		[[nodiscard]] static Result<void> Generate(Project const& project, std::filesystem::path const& scriptCoreAssembly);
		// Rewrites Strada.props of an existing C# project when it changes (after the editor moved, for one).
		[[nodiscard]] static Result<void> UpdateProps(Project const& project, std::filesystem::path const& scriptCoreAssembly);
		// A script class from the template in Scripts/Source/<ClassName>.cs, in the namespace named after the assembly.
		// Fails when the name is not a C# identifier (ASCII) or the file exists.
		[[nodiscard]] static Result<std::filesystem::path> CreateScript(Project const& project, std::string_view className);

		static ScriptBuildSettings MakeBuildSettings(Project const& project, ScriptBuildConfiguration configuration);
		// Builds with the .NET SDK (dotnet build), blocking the calling thread; setting *cancel from another thread stops it.
		// Fails when the SDK cannot be run or the build was cancelled; a build with errors succeeds with Succeeded false.
		[[nodiscard]] static Result<ScriptBuildResult> Build(ScriptBuildSettings const& settings,
		                                                     std::atomic<bool> const* cancel = nullptr);
		// Whether a C# project file or source changed after the game assembly was built (true without the assembly).
		static bool IsOutOfDate(Project const& project);

		// The .NET SDK driver: DOTNET_ROOT, PATH, then the default install locations.
		static std::optional<std::filesystem::path> FindDotNet();
		// The errors and warnings in MSBuild's output format ("file(line,col): error CODE: message [project]").
		static std::vector<ScriptDiagnostic> ParseDiagnostics(std::string_view output);
		// A name usable for a script class and file: an ASCII C# identifier that is not a keyword.
		static bool IsValidClassName(std::string_view name);
	};
}
