#pragma once

#include "Strada/Core/Result.h"

#include <cstdint>
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

		enum class DeclaredTypeKind : uint8_t
		{
			Class = 0,
			Struct,
			Interface,
			Enum,
			Delegate,
			RecordClass,
			RecordStruct
		};

		// A type a C# source declares.
		struct DeclaredType
		{
			// The full name as .NET writes it (Type.FullName): namespaces, enclosing types after '+' and the number of type
			// parameters after '`' ("Game.Player", "Game.Outer+Inner", "Game.Pool`1").
			std::string Name;
			DeclaredTypeKind Kind = DeclaredTypeKind::Class;
			// Declared 'partial': partial declarations of one kind and name, in any number of files, make one type.
			bool Partial = false;
			// Declared 'file': visible only in its own source file.
			bool FileLocal = false;
			// Declared inside another type.
			bool Nested = false;

			bool operator==(DeclaredType const& other) const = default;
		};

		// The types a C# source declares (classes, structs, interfaces, enums, delegates and records), in order. Comments,
		// string and character literals and preprocessor lines are skipped.
		std::vector<DeclaredType> FindDeclaredTypes(std::string_view source);
		// The classes a C# source declares, named like FindDeclaredTypes names them, in order.
		std::vector<std::string> FindDeclaredClasses(std::string_view source);
		// Whether declarations of two source files of one assembly clash, so that it does not build (C# error CS0101): the
		// same top-level type twice, unless both declarations are partial and of one kind, or either is visible only in its
		// file. Nested types are left to the compiler.
		bool IsDuplicateDeclaration(DeclaredType const& a, DeclaredType const& b);
		// A full name as C# code writes it: "Game.Pool`1+Node" -> "Game.Pool<>.Node", "Game.Map`2" -> "Game.Map<,>".
		std::string GetTypeDisplayName(std::string_view fullName);
		// The loaded script class a C# source defines: the class named like the file ("Player" for Player.cs) when the file
		// declares it, else the first declared class that is loaded; for a source without readable declarations, the only
		// loaded class named like the file. Empty when there is none (not built yet, or no script class).
		std::optional<std::string> FindScriptClass(std::string_view source, std::string_view fileStem,
		                                           std::span<ScriptClassInfo const> classes);
		// FindScriptClass for a file, which is read first. Fails when it cannot be read or defines no loaded class.
		[[nodiscard]] Result<std::string> FindScriptClass(std::filesystem::path const& file, std::span<ScriptClassInfo const> classes);
	}
}
