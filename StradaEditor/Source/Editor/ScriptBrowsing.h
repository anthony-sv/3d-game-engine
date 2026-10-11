#pragma once

#include "Strada/Core/Result.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	struct ScriptClassInfo;

	// Content browser logic for the open project's C# sources, independent of the UI. Folders and files are relative to the
	// scripts directory (ScriptProject::GetSourceDirectory) with forward slashes; "" is the directory itself.
	namespace ScriptBrowsing
	{
		// The extension of C# sources.
		inline constexpr std::string_view Extension = ".cs";

		// Whether the path names a C# source (by its extension, ignoring case).
		bool IsScriptFile(std::filesystem::path const& path);
		// The folders directly inside a folder of the scripts directory, sorted by name ignoring case; hidden folders are
		// skipped. Empty when the folder does not exist.
		std::vector<std::string> ListFolders(std::filesystem::path const& directory, std::string_view folder);
		// The C# sources directly inside a folder, sorted by name ignoring case.
		std::vector<std::string> ListScripts(std::filesystem::path const& directory, std::string_view folder);
		// Every C# source whose path contains the text (ignoring case), sorted by path.
		std::vector<std::string> SearchScripts(std::filesystem::path const& directory, std::string_view text);

		// The classes a C# source declares, with their namespaces ("Game.Player"), in order. Comments, string and character
		// literals and preprocessor lines are skipped; nested classes are named like top-level ones.
		std::vector<std::string> FindDeclaredClasses(std::string_view source);
		// The loaded script class a C# source defines: the class named like the file ("Player" for Player.cs) when the file
		// declares it, else the first declared class that is loaded; for a source without readable declarations, the only
		// loaded class named like the file. Empty when there is none (not built yet, or no script class).
		std::optional<std::string> FindScriptClass(std::string_view source, std::string_view fileStem,
		                                           std::span<ScriptClassInfo const> classes);
		// FindScriptClass for a file, which is read first. Fails when it cannot be read or defines no loaded class.
		[[nodiscard]] Result<std::string> FindScriptClass(std::filesystem::path const& file, std::span<ScriptClassInfo const> classes);
	}
}
