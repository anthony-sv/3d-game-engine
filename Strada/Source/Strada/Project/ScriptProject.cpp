#include "stpch.h"
#include "Strada/Project/ScriptProject.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Platform.h"
#include "Strada/Platform/Process.h"
#include "Strada/Project/Project.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <system_error>

namespace Strada
{
	namespace
	{
		constexpr std::string_view SourceDirectoryName = "Source";

		// Reserved C# keywords (contextual keywords are valid identifiers).
		constexpr std::array<std::string_view, 77> CSharpKeywords = {
			"abstract", "as",       "base",     "bool",     "break",     "byte",     "case",   "catch",      "char",      "checked",
			"class",    "const",    "continue", "decimal",  "default",   "delegate", "do",     "double",     "else",      "enum",
			"event",    "explicit", "extern",   "false",    "finally",   "fixed",    "float",  "for",        "foreach",   "goto",
			"if",       "implicit", "in",       "int",      "interface", "internal", "is",     "lock",       "long",      "namespace",
			"new",      "null",     "object",   "operator", "out",       "override", "params", "private",    "protected", "public",
			"readonly", "ref",      "return",   "sbyte",    "sealed",    "short",    "sizeof", "stackalloc", "static",    "string",
			"struct",   "switch",   "this",     "throw",    "true",      "try",      "typeof", "uint",       "ulong",     "unchecked",
			"unsafe",   "ushort",   "using",    "virtual",  "void",      "volatile", "while"};

		std::string_view Trim(std::string_view text)
		{
			size_t const first = text.find_first_not_of(" \t\r\n");
			if (first == std::string_view::npos)
			{
				return {};
			}
			size_t const last = text.find_last_not_of(" \t\r\n");
			return text.substr(first, last - first + 1);
		}

		std::string EscapeXml(std::string_view text)
		{
			std::string escaped;
			escaped.reserve(text.size());
			for (char const character : text)
			{
				switch (character)
				{
					case '&':
						escaped += "&amp;";
						break;
					case '<':
						escaped += "&lt;";
						break;
					case '>':
						escaped += "&gt;";
						break;
					case '"':
						escaped += "&quot;";
						break;
					case '\'':
						escaped += "&apos;";
						break;
					default:
						escaped += character;
						break;
				}
			}
			return escaped;
		}

		// The assembly's name: the ScriptModule file's name without its extension.
		std::string GetAssemblyName(Project const& project)
		{
			return FileSystem::PathToUtf8(ScriptProject::GetAssemblyPath(project).stem());
		}

		std::string MakeProjectFile(std::string const& rootNamespace)
		{
			return fmt::format(R"xml(<Project Sdk="Microsoft.NET.Sdk">
	<!-- The game's scripts. The Strada editor builds them into the project's script module; any .NET 10 SDK and C# editor
	     can work with this project. Strada.props, which the editor writes, locates the engine's Strada.ScriptCore.dll. -->
	<Import Project="Strada.props" Condition="Exists('Strada.props')" />

	<PropertyGroup>
		<TargetFramework>net10.0</TargetFramework>
		<RootNamespace>{}</RootNamespace>
		<LangVersion>latest</LangVersion>
		<Nullable>enable</Nullable>
		<ImplicitUsings>disable</ImplicitUsings>
		<!-- The engine loads the assembly like a plugin: dependencies are copied next to it. -->
		<EnableDynamicLoading>true</EnableDynamicLoading>
		<CopyLocalLockFileAssemblies>true</CopyLocalLockFileAssemblies>
		<AppendTargetFrameworkToOutputPath>false</AppendTargetFrameworkToOutputPath>
	</PropertyGroup>

	<ItemGroup>
		<Reference Include="Strada.ScriptCore">
			<HintPath>$(StradaScriptCoreAssembly)</HintPath>
			<!-- The engine provides it at run time. -->
			<Private>false</Private>
		</Reference>
	</ItemGroup>
</Project>
)xml",
			                   EscapeXml(rootNamespace));
		}

		std::string MakePropsFile(std::filesystem::path const& scriptCoreAssembly)
		{
			return fmt::format(R"xml(<Project>
	<!-- Written by the Strada editor for this machine; it is rewritten when the engine moves. Not meant for version control. -->
	<PropertyGroup>
		<StradaScriptCoreAssembly>{}</StradaScriptCoreAssembly>
	</PropertyGroup>
</Project>
)xml",
			                   EscapeXml(FileSystem::PathToNativeUtf8(scriptCoreAssembly)));
		}

		constexpr std::string_view GitIgnore = "# Build output and the machine-specific Strada.props, written by the Strada editor.\n"
											   "Binaries/\n"
											   "bin/\n"
											   "obj/\n"
											   "Strada.props\n";

		// Writes the file unless it has exactly this content.
		Result<void> WriteIfChanged(std::filesystem::path const& path, std::string_view content)
		{
			std::error_code error;
			if (std::filesystem::is_regular_file(path, error))
			{
				Result<std::string> current = FileSystem::ReadTextFile(path);
				if (current && current.GetValue() == content)
				{
					return {};
				}
			}
			return FileSystem::WriteTextFile(path, content);
		}

		Result<void> WriteIfMissing(std::filesystem::path const& path, std::string_view content)
		{
			std::error_code error;
			if (std::filesystem::exists(path, error))
			{
				return {};
			}
			return FileSystem::WriteTextFile(path, content);
		}

		bool ParseNumber(std::string_view text, uint32_t& value)
		{
			text = Trim(text);
			auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
			return error == std::errc() && end == text.data() + text.size();
		}

		std::optional<ScriptDiagnostic> ParseDiagnosticLine(std::string_view line)
		{
			line = Trim(line);
			// MSBuild names the project a diagnostic belongs to at the end: " [C:\Game\Scripts\Game.csproj]".
			if (line.ends_with("proj]"))
			{
				if (size_t const open = line.rfind(" ["); open != std::string_view::npos)
				{
					line = Trim(line.substr(0, open));
				}
			}

			// "<origin>: <error|warning> <code>: <message>"; the origin is a file (with a position) or a tool name.
			size_t marker = std::string_view::npos;
			ScriptDiagnosticSeverity severity = ScriptDiagnosticSeverity::Error;
			size_t keywordLength = 0;
			for (auto const& [keyword, kind] : {std::pair{std::string_view(": error "), ScriptDiagnosticSeverity::Error},
			                                    std::pair{std::string_view(": warning "), ScriptDiagnosticSeverity::Warning}})
			{
				size_t const position = line.find(keyword);
				if (position != std::string_view::npos && position < marker)
				{
					marker = position;
					severity = kind;
					keywordLength = keyword.size();
				}
			}
			if (marker == std::string_view::npos || Trim(line.substr(0, marker)).empty())
			{
				return std::nullopt;
			}

			ScriptDiagnostic diagnostic;
			diagnostic.Severity = severity;
			std::string_view const rest = line.substr(marker + keywordLength);
			size_t const colon = rest.find(':');
			std::string_view const code = colon != std::string_view::npos ? Trim(rest.substr(0, colon)) : std::string_view();
			if (colon != std::string_view::npos && code.find(' ') == std::string_view::npos)
			{
				diagnostic.Code = std::string(code);
				diagnostic.Message = std::string(Trim(rest.substr(colon + 1)));
			}
			else
			{
				diagnostic.Message = std::string(Trim(rest));
			}

			std::string_view origin = Trim(line.substr(0, marker));
			if (origin.ends_with(')'))
			{
				if (size_t const open = origin.rfind('('); open != std::string_view::npos)
				{
					std::string_view const position = origin.substr(open + 1, origin.size() - open - 2);
					size_t const comma = position.find(',');
					uint32_t lineNumber = 0;
					uint32_t column = 0;
					if (ParseNumber(position.substr(0, comma), lineNumber))
					{
						if (comma != std::string_view::npos)
						{
							std::string_view const columnText = position.substr(comma + 1);
							ParseNumber(columnText.substr(0, columnText.find(',')), column);
						}
						diagnostic.Line = lineNumber;
						diagnostic.Column = column;
						origin = Trim(origin.substr(0, open));
					}
				}
			}
			// Tools report problems without a file under their own names.
			if (origin != "MSBUILD" && origin != "CSC" && origin != "EXEC")
			{
				diagnostic.File = FileSystem::PathFromUtf8(origin);
			}
			return diagnostic;
		}

		bool IsSourceExtension(std::filesystem::path const& path)
		{
			std::string const extension = FileSystem::PathToUtf8(path.extension());
			return extension == ".cs" || extension == ".csproj" || extension == ".props" || extension == ".targets";
		}

		// The newest change to the C# project and its sources; build output directories are skipped.
		std::optional<std::filesystem::file_time_type> FindNewestSource(std::filesystem::path const& directory)
		{
			std::optional<std::filesystem::file_time_type> newest;
			std::error_code error;
			std::filesystem::recursive_directory_iterator iterator(directory, std::filesystem::directory_options::skip_permission_denied,
			                                                       error);
			for (; !error && iterator != std::filesystem::recursive_directory_iterator(); iterator.increment(error))
			{
				std::filesystem::directory_entry const& entry = *iterator;
				std::string const name = FileSystem::PathToUtf8(entry.path().filename());
				std::error_code entryError;
				if (entry.is_directory(entryError) && (name == "Binaries" || name == "bin" || name == "obj"))
				{
					iterator.disable_recursion_pending();
					continue;
				}
				if (!entry.is_regular_file(entryError) || !IsSourceExtension(entry.path()))
				{
					continue;
				}
				std::filesystem::file_time_type const time = entry.last_write_time(entryError);
				if (!entryError && (!newest || time > *newest))
				{
					newest = time;
				}
			}
			return newest;
		}

		std::optional<std::filesystem::path> FindDriverIn(std::filesystem::path const& directory)
		{
#if defined(ST_PLATFORM_WINDOWS)
			std::filesystem::path const driver = directory / "dotnet.exe";
#else
			std::filesystem::path const driver = directory / "dotnet";
#endif
			std::error_code error;
			return std::filesystem::is_regular_file(driver, error) ? std::optional(driver) : std::nullopt;
		}
	}

	std::filesystem::path ScriptProject::GetAssemblyPath(Project const& project)
	{
		return project.GetDirectory() / FileSystem::PathFromUtf8(project.GetSettings().ScriptModule);
	}

	std::filesystem::path ScriptProject::GetProjectFile(Project const& project)
	{
		return project.GetDirectory() / DirectoryName / FileSystem::PathFromUtf8(GetAssemblyName(project) + ".csproj");
	}

	bool ScriptProject::Exists(Project const& project)
	{
		std::error_code error;
		return std::filesystem::is_regular_file(GetProjectFile(project), error);
	}

	Result<void> ScriptProject::Generate(Project const& project, std::filesystem::path const& scriptCoreAssembly)
	{
		std::filesystem::path const directory = project.GetDirectory() / DirectoryName;
		if (Result<void> created = FileSystem::CreateDirectories(directory / SourceDirectoryName); !created)
		{
			return created;
		}
		if (Result<void> written = WriteIfMissing(GetProjectFile(project), MakeProjectFile(GetAssemblyName(project))); !written)
		{
			return written;
		}
		if (Result<void> written = WriteIfMissing(directory / ".gitignore", GitIgnore); !written)
		{
			return written;
		}
		return WriteIfChanged(directory / PropsFileName, MakePropsFile(scriptCoreAssembly));
	}

	Result<void> ScriptProject::UpdateProps(Project const& project, std::filesystem::path const& scriptCoreAssembly)
	{
		if (!Exists(project))
		{
			return MakeError("the project has no C# project ({})", FileSystem::PathToUtf8(GetProjectFile(project)));
		}
		return WriteIfChanged(project.GetDirectory() / DirectoryName / PropsFileName, MakePropsFile(scriptCoreAssembly));
	}

	Result<std::filesystem::path> ScriptProject::CreateScript(Project const& project, std::string_view className)
	{
		if (!IsValidClassName(className))
		{
			return MakeError("'{}' is not a valid script class name: use letters, digits and underscores, starting with a letter",
			                 className);
		}
		std::filesystem::path const file =
			project.GetDirectory() / DirectoryName / SourceDirectoryName / FileSystem::PathFromUtf8(std::string(className) + ".cs");
		std::error_code error;
		if (std::filesystem::exists(file, error))
		{
			return MakeError("'{}' already exists", FileSystem::PathToUtf8(file));
		}
		if (Result<void> created = FileSystem::CreateDirectories(file.parent_path()); !created)
		{
			return Error{created.GetError()};
		}
		std::string const source = fmt::format(R"cs(using Strada;

namespace {};

public class {} : Script
{{
	protected override void OnCreate()
	{{
	}}

	protected override void OnUpdate(float deltaTime)
	{{
	}}
}}
)cs",
		                                       GetAssemblyName(project), className);
		if (Result<void> written = FileSystem::WriteTextFile(file, source); !written)
		{
			return Error{written.GetError()};
		}
		return file;
	}

	ScriptBuildSettings ScriptProject::MakeBuildSettings(Project const& project, ScriptBuildConfiguration configuration)
	{
		ScriptBuildSettings settings;
		settings.ProjectFile = GetProjectFile(project);
		settings.OutputDirectory = GetAssemblyPath(project).parent_path();
		settings.Configuration = configuration;
		return settings;
	}

	Result<ScriptBuildResult> ScriptProject::Build(ScriptBuildSettings const& settings, std::atomic<bool> const* cancel)
	{
		std::optional<std::filesystem::path> const dotnet = settings.DotNet.empty() ? FindDotNet() : std::optional(settings.DotNet);
		if (!dotnet)
		{
			return Error{"the .NET SDK was not found: install the .NET 10 SDK (https://dot.net) or set DOTNET_ROOT"};
		}

		ProcessSpecification process;
		process.Executable = *dotnet;
		process.Arguments = {"build", FileSystem::PathToNativeUtf8(settings.ProjectFile), "--configuration",
		                     settings.Configuration == ScriptBuildConfiguration::Debug ? "Debug" : "Release", "--output",
		                     FileSystem::PathToNativeUtf8(settings.OutputDirectory), "--nologo", "--verbosity", "quiet",
		                     // Build servers would outlive the build and keep its output open.
		                     "--disable-build-servers", "-terminalLogger:off", "-consoleLoggerParameters:NoSummary;ForceNoAlign",
		                     "-property:GenerateFullPaths=true"};
		process.WorkingDirectory = settings.ProjectFile.parent_path();
		// English, UTF-8 output for the diagnostics parser; no first-run banner or telemetry.
		process.Environment = {{"DOTNET_CLI_UI_LANGUAGE", "en"},           {"DOTNET_CLI_FORCE_UTF8_ENCODING", "true"},
		                       {"DOTNET_CLI_TELEMETRY_OPTOUT", "1"},       {"DOTNET_NOLOGO", "1"},
		                       {"DOTNET_SKIP_FIRST_TIME_EXPERIENCE", "1"}, {"MSBUILDDISABLENODEREUSE", "1"}};

		Result<ProcessResult> run = Process::Run(process, cancel);
		if (!run)
		{
			return MakeError("the .NET SDK cannot be run: {}", run.GetError());
		}
		if (run.GetValue().Cancelled)
		{
			return Error{"the build was cancelled"};
		}

		ScriptBuildResult result;
		result.Output = std::move(run.GetValue().Output);
		result.Diagnostics = ParseDiagnostics(result.Output);
		bool const hasErrors = std::any_of(result.Diagnostics.begin(), result.Diagnostics.end(),
		                                   [](ScriptDiagnostic const& diagnostic)
		                                   {
											   return diagnostic.Severity == ScriptDiagnosticSeverity::Error;
										   });
		std::filesystem::path const assembly =
			settings.OutputDirectory / FileSystem::PathFromUtf8(FileSystem::PathToUtf8(settings.ProjectFile.stem()) + ".dll");
		std::error_code error;
		result.Succeeded = run.GetValue().ExitCode == 0 && !hasErrors && std::filesystem::is_regular_file(assembly, error);
		if (result.Succeeded)
		{
			result.Assembly = assembly;
		}
		else if (!hasErrors)
		{
			ScriptDiagnostic failure;
			failure.Message =
				fmt::format("dotnet build failed (exit code {}) without reporting an error; see its output", run.GetValue().ExitCode);
			result.Diagnostics.push_back(std::move(failure));
		}
		return result;
	}

	bool ScriptProject::IsOutOfDate(Project const& project)
	{
		std::error_code error;
		std::filesystem::path const assembly = GetAssemblyPath(project);
		if (!std::filesystem::is_regular_file(assembly, error))
		{
			return true;
		}
		std::filesystem::file_time_type const built = std::filesystem::last_write_time(assembly, error);
		if (error)
		{
			return true;
		}
		std::optional<std::filesystem::file_time_type> const newest = FindNewestSource(project.GetDirectory() / DirectoryName);
		return newest && *newest > built;
	}

	std::optional<std::filesystem::path> ScriptProject::FindDotNet()
	{
		if (std::optional<std::string> const root = Platform::ReadEnvironmentVariable("DOTNET_ROOT"); root && !root->empty())
		{
			if (std::optional<std::filesystem::path> driver = FindDriverIn(FileSystem::PathFromUtf8(*root)))
			{
				return driver;
			}
		}
		if (std::optional<std::filesystem::path> driver = Process::FindExecutable("dotnet"))
		{
			return driver;
		}
		std::vector<std::filesystem::path> defaults;
#if defined(ST_PLATFORM_WINDOWS)
		if (std::optional<std::string> const programFiles = Platform::ReadEnvironmentVariable("ProgramFiles"))
		{
			defaults.push_back(FileSystem::PathFromUtf8(*programFiles) / "dotnet");
		}
		if (std::optional<std::string> const profile = Platform::ReadEnvironmentVariable("USERPROFILE"))
		{
			defaults.push_back(FileSystem::PathFromUtf8(*profile) / ".dotnet");
		}
#else
		defaults = {"/usr/local/share/dotnet", "/usr/share/dotnet", "/usr/lib/dotnet"};
		if (std::optional<std::string> const home = Platform::ReadEnvironmentVariable("HOME"))
		{
			defaults.push_back(FileSystem::PathFromUtf8(*home) / ".dotnet");
		}
#endif
		for (std::filesystem::path const& directory : defaults)
		{
			if (std::optional<std::filesystem::path> driver = FindDriverIn(directory))
			{
				return driver;
			}
		}
		return std::nullopt;
	}

	std::vector<ScriptDiagnostic> ScriptProject::ParseDiagnostics(std::string_view output)
	{
		std::vector<ScriptDiagnostic> diagnostics;
		size_t start = 0;
		while (start < output.size())
		{
			size_t const end = std::min(output.find('\n', start), output.size());
			if (std::optional<ScriptDiagnostic> diagnostic = ParseDiagnosticLine(output.substr(start, end - start)))
			{
				if (std::find(diagnostics.begin(), diagnostics.end(), *diagnostic) == diagnostics.end())
				{
					diagnostics.push_back(std::move(*diagnostic));
				}
			}
			start = end + 1;
		}
		return diagnostics;
	}

	bool ScriptProject::IsValidClassName(std::string_view name)
	{
		if (name.empty() || name.size() > 255 || name == "Script")
		{
			return false;
		}
		auto const isLetter = [](char character)
		{
			return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || character == '_';
		};
		if (!isLetter(name.front()))
		{
			return false;
		}
		bool const identifier = std::all_of(name.begin(), name.end(),
		                                    [&isLetter](char character)
		                                    {
												return isLetter(character) || (character >= '0' && character <= '9');
											});
		return identifier && std::find(CSharpKeywords.begin(), CSharpKeywords.end(), name) == CSharpKeywords.end();
	}
}
