#include "Editor/ScriptBrowsing.h"

#include "Editor/AssetBrowsing.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Project/ScriptProject.h"
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

			enum class TokenKind : uint8_t
			{
				Identifier = 0,
				Punctuation,
				End
			};

			struct Token
			{
				TokenKind Kind = TokenKind::End;
				// The identifier without its '@', or the punctuation character.
				std::string_view Text;
				// "@class": an identifier named like a keyword.
				bool Verbatim = false;

				bool Is(char character) const { return Kind == TokenKind::Punctuation && Text.size() == 1 && Text.front() == character; }
			};

			// Splits C# source into identifiers and punctuation characters, skipping whitespace, comments, literals, numbers and
			// preprocessor lines.
			class Tokenizer
			{
			public:
				explicit Tokenizer(std::string_view source)
					: m_Source(source)
				{
				}

				Token Next()
				{
					while (m_Index < m_Source.size())
					{
						char const c = m_Source[m_Index];
						if (c == '\n')
						{
							m_LineStart = true;
							m_Index++;
							continue;
						}
						if (std::isspace(static_cast<unsigned char>(c)) != 0)
						{
							m_Index++;
							continue;
						}
						bool const firstOnLine = std::exchange(m_LineStart, false);
						bool const hasNext = m_Index + 1 < m_Source.size();
						char const next = hasNext ? m_Source[m_Index + 1] : '\0';
						// Preprocessor directives and line comments end with their line.
						if ((c == '#' && firstOnLine) || (c == '/' && next == '/'))
						{
							m_Index = std::min(m_Source.find('\n', m_Index), m_Source.size());
							continue;
						}
						if (c == '/' && next == '*')
						{
							size_t const end = m_Source.find("*/", m_Index + 2);
							m_Index = end == std::string_view::npos ? m_Source.size() : end + 2;
							continue;
						}
						if (c == '"' || c == '\'' || ((c == '$' || c == '@') && (next == '"' || next == '$' || next == '@')))
						{
							m_Index = SkipLiteral(m_Source, m_Index);
							continue;
						}
						bool const verbatim = c == '@' && hasNext && IsIdentifierStart(next);
						if (verbatim || IsIdentifierStart(c))
						{
							size_t const start = verbatim ? m_Index + 1 : m_Index;
							size_t end = start;
							while (end < m_Source.size() && IsIdentifierPart(m_Source[end]))
							{
								end++;
							}
							m_Index = end;
							return Token{TokenKind::Identifier, m_Source.substr(start, end - start), verbatim};
						}
						if (std::isdigit(static_cast<unsigned char>(c)) != 0)
						{
							// Numbers with their suffixes and exponents ("1.5e3f", "0xFFul"); a '.' only when a digit follows.
							while (m_Index < m_Source.size())
							{
								char const part = m_Source[m_Index];
								bool const decimalPoint = part == '.' && m_Index + 1 < m_Source.size() &&
								                          std::isdigit(static_cast<unsigned char>(m_Source[m_Index + 1])) != 0;
								if (!decimalPoint && std::isalnum(static_cast<unsigned char>(part)) == 0 && part != '_')
								{
									break;
								}
								m_Index++;
							}
							continue;
						}
						return Token{TokenKind::Punctuation, m_Source.substr(m_Index++, 1), false};
					}
					return Token{};
				}

				// After a '<': skips type parameters or arguments up to the matching '>' and returns the commas between them at
				// their own level (one less than their count). Stops before a brace or ';' of malformed code.
				uint32_t SkipTypeParameters()
				{
					uint32_t commas = 0;
					int angles = 1;
					int brackets = 0;
					while (true)
					{
						size_t const index = m_Index;
						bool const lineStart = m_LineStart;
						Token const token = Next();
						if (token.Kind == TokenKind::End || token.Is('{') || token.Is('}') || token.Is(';'))
						{
							m_Index = index;
							m_LineStart = lineStart;
							return commas;
						}
						if (token.Is('<'))
						{
							angles++;
						}
						else if (token.Is('>') && --angles == 0)
						{
							return commas;
						}
						else if (token.Is('(') || token.Is('['))
						{
							brackets++;
						}
						else if (token.Is(')') || token.Is(']'))
						{
							brackets = std::max(brackets - 1, 0);
						}
						else if (token.Is(',') && angles == 1 && brackets == 0)
						{
							commas++;
						}
					}
				}

			private:
				std::string_view m_Source;
				size_t m_Index = 0;
				bool m_LineStart = true;
			};
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

		std::vector<DeclaredType> FindDeclaredTypes(std::string_view source)
		{
			// What a pair of braces holds: a namespace, the body of a type, or anything else (members' bodies, initializers),
			// where no types are declared.
			enum class ScopeKind : uint8_t
			{
				Namespace = 0,
				Type,
				Other
			};
			struct Scope
			{
				ScopeKind Kind = ScopeKind::Other;
				// The namespace's or the type's full name.
				std::string Name;
				// The parentheses and brackets open around the scope's opening brace.
				int OuterParentheses = 0;
			};
			std::vector<Scope> scopes;
			// A file-scoped namespace ("namespace Game;") applies to the rest of the file.
			std::string fileNamespace;
			auto const enclosingNamespace = [&]() -> std::string const&
			{
				for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope)
				{
					if (scope->Kind == ScopeKind::Namespace)
					{
						return scope->Name;
					}
				}
				return fileNamespace;
			};

			std::vector<DeclaredType> types;
			Tokenizer tokens(source);
			Token token = tokens.Next();
			auto const advance = [&]
			{
				token = tokens.Next();
			};
			// Modifiers since the last declaration, statement or attribute.
			bool partial = false;
			bool fileLocal = false;
			auto const resetModifiers = [&]
			{
				partial = false;
				fileLocal = false;
			};
			// Parentheses and brackets open in the current scope.
			int parentheses = 0;
			// A declared type whose body is the next brace outside parentheses.
			std::optional<std::string> pendingBody;

			// A declaration of the given kind and name (the type's own name, without type parameters) in the current scope.
			auto const makeType = [&](DeclaredTypeKind kind, std::string_view name, uint32_t arity)
			{
				DeclaredType type;
				type.Kind = kind;
				type.Partial = partial;
				type.FileLocal = fileLocal;
				type.Nested = !scopes.empty() && scopes.back().Kind == ScopeKind::Type;
				std::string const& outer = type.Nested ? scopes.back().Name : enclosingNamespace();
				type.Name = outer.empty() ? std::string(name) : outer + (type.Nested ? "+" : ".") + std::string(name);
				if (arity > 0)
				{
					type.Name += fmt::format("`{}", arity);
				}
				resetModifiers();
				return type;
			};
			// A name follows a type keyword: the current token, an identifier that is no keyword, then its type parameters.
			auto const declare = [&](DeclaredTypeKind kind)
			{
				if (token.Kind != TokenKind::Identifier ||
				    (!token.Verbatim && (ScriptProject::IsKeyword(token.Text) || token.Text == "where")))
				{
					return;
				}
				std::string_view const name = token.Text;
				advance();
				uint32_t arity = 0;
				if (token.Is('<'))
				{
					arity = tokens.SkipTypeParameters() + 1;
					advance();
				}
				DeclaredType type = makeType(kind, name, arity);
				pendingBody = type.Name;
				types.push_back(std::move(type));
			};

			while (token.Kind != TokenKind::End)
			{
				bool const canDeclare = scopes.empty() || scopes.back().Kind != ScopeKind::Other;
				if (token.Kind == TokenKind::Identifier && !token.Verbatim && canDeclare)
				{
					std::string_view const word = token.Text;
					if (word == "namespace" && (scopes.empty() || scopes.back().Kind == ScopeKind::Namespace))
					{
						std::string name;
						for (advance(); token.Kind == TokenKind::Identifier || token.Is('.'); advance())
						{
							name += token.Text;
						}
						if (token.Is(';'))
						{
							fileNamespace = std::move(name);
							resetModifiers();
							advance();
						}
						else if (token.Is('{'))
						{
							std::string const& outer = enclosingNamespace();
							scopes.push_back({ScopeKind::Namespace, outer.empty() ? std::move(name) : outer + "." + name, parentheses});
							parentheses = 0;
							resetModifiers();
							advance();
						}
						continue;
					}
					if (word == "class" || word == "struct" || word == "interface" || word == "enum")
					{
						DeclaredTypeKind kind = DeclaredTypeKind::Interface;
						if (word == "class")
						{
							kind = DeclaredTypeKind::Class;
						}
						else if (word == "struct")
						{
							kind = DeclaredTypeKind::Struct;
						}
						else if (word == "enum")
						{
							kind = DeclaredTypeKind::Enum;
						}
						advance();
						// Anything but a name after the keyword ("where T : class, new()") declares nothing.
						declare(kind);
						continue;
					}
					if (word == "record")
					{
						advance();
						DeclaredTypeKind kind = DeclaredTypeKind::RecordClass;
						if (token.Kind == TokenKind::Identifier && !token.Verbatim && (token.Text == "class" || token.Text == "struct"))
						{
							kind = token.Text == "struct" ? DeclaredTypeKind::RecordStruct : DeclaredTypeKind::RecordClass;
							advance();
						}
						declare(kind);
						continue;
					}
					if (word == "delegate")
					{
						// "delegate <return type> Name<T>(...)": the name is the last identifier outside angle brackets before the
						// parameters. Anonymous methods ("delegate (int x) { }") and function pointers ("delegate*<int>") have none.
						advance();
						std::string_view name;
						uint32_t arity = 0;
						while (token.Kind == TokenKind::Identifier || token.Is('<') || token.Is('.') || token.Is('[') || token.Is(']') ||
						       token.Is('?') || token.Is(','))
						{
							if (token.Kind == TokenKind::Identifier)
							{
								name = token.Text;
								arity = 0;
							}
							else if (token.Is('<'))
							{
								arity = tokens.SkipTypeParameters() + 1;
							}
							advance();
						}
						if (!name.empty() && token.Is('(') && !ScriptProject::IsKeyword(name))
						{
							types.push_back(makeType(DeclaredTypeKind::Delegate, name, arity));
						}
						resetModifiers();
						continue;
					}
					if (word == "partial")
					{
						partial = true;
					}
					else if (word == "file")
					{
						fileLocal = true;
					}
					advance();
					continue;
				}

				if (token.Is('(') || token.Is('['))
				{
					parentheses++;
				}
				else if (token.Is(')') || token.Is(']'))
				{
					parentheses = std::max(parentheses - 1, 0);
					// Attributes end before the modifiers of what they annotate.
					if (token.Is(']') && parentheses == 0)
					{
						resetModifiers();
					}
				}
				else if (token.Is('{'))
				{
					bool const body = pendingBody.has_value() && parentheses == 0;
					scopes.push_back(
						{body ? ScopeKind::Type : ScopeKind::Other, body ? std::move(*pendingBody) : std::string(), parentheses});
					pendingBody.reset();
					parentheses = 0;
					resetModifiers();
				}
				else if (token.Is('}'))
				{
					if (!scopes.empty())
					{
						parentheses = scopes.back().OuterParentheses;
						scopes.pop_back();
					}
					pendingBody.reset();
					resetModifiers();
				}
				else if (token.Is(';'))
				{
					// A record without a body ("record Point(int X, int Y);") ends here.
					if (parentheses == 0)
					{
						pendingBody.reset();
					}
					resetModifiers();
				}
				advance();
			}
			return types;
		}

		std::vector<std::string> FindDeclaredClasses(std::string_view source)
		{
			std::vector<std::string> classes;
			for (DeclaredType& type : FindDeclaredTypes(source))
			{
				if (type.Kind == DeclaredTypeKind::Class)
				{
					classes.push_back(std::move(type.Name));
				}
			}
			return classes;
		}

		bool IsDuplicateDeclaration(DeclaredType const& a, DeclaredType const& b)
		{
			if (a.Nested || b.Nested || a.FileLocal || b.FileLocal || a.Name != b.Name)
			{
				return false;
			}
			return !(a.Partial && b.Partial && a.Kind == b.Kind);
		}

		std::string GetTypeDisplayName(std::string_view fullName)
		{
			std::string name;
			name.reserve(fullName.size());
			size_t index = 0;
			while (index < fullName.size())
			{
				char const c = fullName[index];
				if (c != '`')
				{
					name += c == '+' ? '.' : c;
					index++;
					continue;
				}
				// "`2" -> "<,>".
				size_t end = index + 1;
				uint32_t arity = 0;
				while (end < fullName.size() && fullName[end] >= '0' && fullName[end] <= '9')
				{
					arity = std::min<uint32_t>(arity * 10 + static_cast<uint32_t>(fullName[end] - '0'), 64);
					end++;
				}
				name += '<';
				name.append(arity > 0 ? arity - 1 : 0, ',');
				name += '>';
				index = end;
			}
			return name;
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
