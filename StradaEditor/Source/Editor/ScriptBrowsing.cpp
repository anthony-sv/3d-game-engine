#include "Editor/ScriptBrowsing.h"

#include "Editor/AssetBrowsing.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Script/ScriptEngine.h"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

namespace Strada
{
	namespace ScriptBrowsing
	{
		namespace
		{
			bool IsIdentifierStart(char c)
			{
				unsigned char const byte = static_cast<unsigned char>(c);
				// Bytes of non-ASCII characters: C# identifiers may use letters of any script.
				return std::isalpha(byte) != 0 || c == '_' || byte >= 0x80;
			}

			bool IsIdentifierPart(char c)
			{
				return IsIdentifierStart(c) || std::isdigit(static_cast<unsigned char>(c)) != 0;
			}

			// The index after the string or character literal at start: '"', '\'' or a string with '$' and '@' prefixes.
			size_t SkipLiteral(std::string_view source, size_t start)
			{
				size_t index = start;
				bool verbatim = false;
				while (index < source.size() && (source[index] == '$' || source[index] == '@'))
				{
					verbatim = verbatim || source[index] == '@';
					index++;
				}
				if (index >= source.size())
				{
					return index;
				}
				char const quote = source[index];
				// Raw strings open with three or more quotes and close with as many.
				size_t quotes = 0;
				while (index + quotes < source.size() && source[index + quotes] == '"')
				{
					quotes++;
				}
				if (quote == '"' && quotes >= 3)
				{
					size_t const end = source.find(std::string(quotes, '"'), index + quotes);
					return end == std::string_view::npos ? source.size() : end + quotes;
				}
				index++;
				while (index < source.size())
				{
					char const c = source[index];
					if (verbatim)
					{
						// "" is an escaped quote in verbatim strings, which may span lines.
						if (c == '"' && index + 1 < source.size() && source[index + 1] == '"')
						{
							index += 2;
							continue;
						}
						if (c == '"')
						{
							return index + 1;
						}
						index++;
						continue;
					}
					if (c == '\\')
					{
						index += 2;
						continue;
					}
					if (c == quote)
					{
						return index + 1;
					}
					// An unterminated literal ends with its line.
					if (c == '\n')
					{
						return index;
					}
					index++;
				}
				return source.size();
			}

			std::string_view GetSimpleName(std::string_view className)
			{
				size_t const separator = className.find_last_of(".+");
				return separator == std::string_view::npos ? className : className.substr(separator + 1);
			}
		}

		bool IsScriptFile(std::filesystem::path const& path)
		{
			std::string extension = FileSystem::PathToUtf8(path.extension());
			std::transform(extension.begin(), extension.end(), extension.begin(),
			               [](unsigned char c)
			               {
							   return static_cast<char>(std::tolower(c));
						   });
			return extension == Extension;
		}

		std::vector<std::string> ListFolders(std::filesystem::path const& directory, std::string_view folder)
		{
			std::vector<std::string> folders;
			for (std::string const& name : AssetBrowsing::ListFolderNames(directory / FileSystem::PathFromUtf8(folder)))
			{
				folders.push_back(AssetBrowsing::JoinPath(folder, name));
			}
			return folders;
		}

		std::vector<std::string> ListScripts(std::filesystem::path const& directory, std::string_view folder)
		{
			std::vector<std::string> names;
			std::error_code errorCode;
			for (std::filesystem::directory_entry const& entry :
			     std::filesystem::directory_iterator(directory / FileSystem::PathFromUtf8(folder), errorCode))
			{
				std::string name = FileSystem::PathToUtf8(entry.path().filename());
				std::error_code entryError;
				if (!name.starts_with(".") && entry.is_regular_file(entryError) && IsScriptFile(entry.path()))
				{
					names.push_back(std::move(name));
				}
			}
			std::sort(names.begin(), names.end(), AssetBrowsing::LessIgnoringCase);
			std::vector<std::string> scripts;
			scripts.reserve(names.size());
			for (std::string const& name : names)
			{
				scripts.push_back(AssetBrowsing::JoinPath(folder, name));
			}
			return scripts;
		}

		std::vector<std::string> SearchScripts(std::filesystem::path const& directory, std::string_view text)
		{
			std::vector<std::string> scripts;
			std::error_code errorCode;
			std::filesystem::recursive_directory_iterator iterator(directory, std::filesystem::directory_options::skip_permission_denied,
			                                                       errorCode);
			for (std::filesystem::recursive_directory_iterator const end; !errorCode && iterator != end; iterator.increment(errorCode))
			{
				std::filesystem::directory_entry const& entry = *iterator;
				std::error_code entryError;
				if (FileSystem::PathToUtf8(entry.path().filename()).starts_with("."))
				{
					if (entry.is_directory(entryError))
					{
						iterator.disable_recursion_pending();
					}
					continue;
				}
				if (!entry.is_regular_file(entryError) || !IsScriptFile(entry.path()))
				{
					continue;
				}
				std::string const path = FileSystem::PathToUtf8(entry.path().lexically_relative(directory));
				if (AssetBrowsing::ContainsIgnoringCase(path, text))
				{
					scripts.push_back(path);
				}
			}
			std::sort(scripts.begin(), scripts.end(), AssetBrowsing::LessIgnoringCase);
			return scripts;
		}

		std::vector<std::string> FindDeclaredClasses(std::string_view source)
		{
			// A file-scoped namespace ("namespace Game;") applies to the rest of the file, a block namespace to its braces.
			struct BlockNamespace
			{
				std::string Name;
				// The brace depth inside the block.
				int Depth = 0;
			};
			std::vector<BlockNamespace> blocks;
			std::string fileNamespace;
			auto const currentNamespace = [&]() -> std::string const&
			{
				return blocks.empty() ? fileNamespace : blocks.back().Name;
			};

			std::vector<std::string> classes;
			std::string pendingNamespace;
			bool readingNamespace = false;
			bool expectClassName = false;
			bool lineStart = true;
			int depth = 0;
			size_t index = 0;
			while (index < source.size())
			{
				char const c = source[index];
				if (c == '\n')
				{
					lineStart = true;
					index++;
					continue;
				}
				if (std::isspace(static_cast<unsigned char>(c)) != 0)
				{
					index++;
					continue;
				}
				bool const firstOnLine = std::exchange(lineStart, false);
				bool const hasNext = index + 1 < source.size();
				char const next = hasNext ? source[index + 1] : '\0';
				// Preprocessor directives and line comments end with their line.
				if ((c == '#' && firstOnLine) || (c == '/' && next == '/'))
				{
					index = std::min(source.find('\n', index), source.size());
					continue;
				}
				if (c == '/' && next == '*')
				{
					size_t const end = source.find("*/", index + 2);
					index = end == std::string_view::npos ? source.size() : end + 2;
					continue;
				}
				if (c == '"' || c == '\'' || ((c == '$' || c == '@') && (next == '"' || next == '$' || next == '@')))
				{
					index = SkipLiteral(source, index);
					continue;
				}
				// "@class" is an identifier named class, never the keyword.
				bool const verbatimIdentifier = c == '@' && hasNext && IsIdentifierStart(next);
				if (verbatimIdentifier || IsIdentifierStart(c))
				{
					size_t const start = verbatimIdentifier ? index + 1 : index;
					size_t end = start;
					while (end < source.size() && IsIdentifierPart(source[end]))
					{
						end++;
					}
					std::string_view const word = source.substr(start, end - start);
					index = end;
					if (readingNamespace)
					{
						pendingNamespace += word;
					}
					else if (expectClassName)
					{
						expectClassName = false;
						std::string const& outer = currentNamespace();
						classes.push_back(outer.empty() ? std::string(word) : outer + "." + std::string(word));
					}
					else if (!verbatimIdentifier && word == "namespace")
					{
						readingNamespace = true;
						pendingNamespace.clear();
					}
					else if (!verbatimIdentifier && word == "class")
					{
						expectClassName = true;
					}
					continue;
				}

				if (readingNamespace)
				{
					if (c == '.')
					{
						pendingNamespace += '.';
						index++;
						continue;
					}
					readingNamespace = false;
					if (c == ';')
					{
						fileNamespace = pendingNamespace;
						index++;
						continue;
					}
					if (c == '{')
					{
						depth++;
						std::string const& outer = currentNamespace();
						std::string name = outer.empty() ? pendingNamespace : outer + "." + pendingNamespace;
						blocks.push_back({std::move(name), depth});
						index++;
						continue;
					}
				}
				// "class" followed by anything but a name is a constraint ("where T : class").
				expectClassName = false;
				if (c == '{')
				{
					depth++;
				}
				else if (c == '}')
				{
					if (!blocks.empty() && blocks.back().Depth == depth)
					{
						blocks.pop_back();
					}
					depth = std::max(depth - 1, 0);
				}
				index++;
			}
			return classes;
		}

		std::optional<std::string> FindScriptClass(std::string_view source, std::string_view fileStem,
		                                           std::span<ScriptClassInfo const> classes)
		{
			auto const isLoaded = [&classes](std::string_view name)
			{
				return std::any_of(classes.begin(), classes.end(),
				                   [name](ScriptClassInfo const& scriptClass)
				                   {
									   return scriptClass.Name == name;
								   });
			};
			std::vector<std::string> const declared = FindDeclaredClasses(source);
			for (std::string const& name : declared)
			{
				if (GetSimpleName(name) == fileStem && isLoaded(name))
				{
					return name;
				}
			}
			for (std::string const& name : declared)
			{
				if (isLoaded(name))
				{
					return name;
				}
			}
			if (!declared.empty())
			{
				return std::nullopt;
			}
			// Nothing readable was declared: the file's name decides when exactly one loaded class carries it.
			std::optional<std::string> match;
			for (ScriptClassInfo const& scriptClass : classes)
			{
				if (GetSimpleName(scriptClass.Name) != fileStem)
				{
					continue;
				}
				if (match)
				{
					return std::nullopt;
				}
				match = scriptClass.Name;
			}
			return match;
		}

		Result<std::string> FindScriptClass(std::filesystem::path const& file, std::span<ScriptClassInfo const> classes)
		{
			Result<std::string> const source = FileSystem::ReadTextFile(file);
			if (!source)
			{
				return Error{source.GetError()};
			}
			if (std::optional<std::string> found = FindScriptClass(source.GetValue(), FileSystem::PathToUtf8(file.stem()), classes))
			{
				return std::move(*found);
			}
			return MakeError("'{}' has no built script class yet: build the scripts (Scripts > Build Scripts) and try again; script "
			                 "classes derive from Strada.Script",
			                 FileSystem::PathToUtf8(file.filename()));
		}
	}
}
