#include "stpch.h"
#include "Strada/Serialization/JsonScanner.h"

#include <array>
#include <limits>

namespace Strada
{
	namespace
	{
		bool IsWhitespace(char character)
		{
			return character == ' ' || character == '\t' || character == '\n' || character == '\r';
		}

		// The characters of numbers and of the literals true, false and null (and of malformed tokens of letters and digits,
		// which the parser refuses).
		bool IsScalarCharacter(char character)
		{
			return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'z') ||
			       (character >= 'A' && character <= 'Z') || character == '+' || character == '-' || character == '.';
		}

		bool IsHexDigit(char character)
		{
			return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
			       (character >= 'A' && character <= 'F');
		}

		// What a value can begin with. A byte order mark cannot: the parser skips one only at the start of its input, so an
		// element that began with one would parse alone but not inside the text.
		bool CanBeginValue(char character)
		{
			return character == '{' || character == '[' || character == '"' || character == '-' || (character >= '0' && character <= '9') ||
			       character == 't' || character == 'f' || character == 'n';
		}

		class Scanner
		{
		public:
			explicit Scanner(std::string_view text)
				: m_Text(text)
			{
			}

			uint32_t GetPosition() const { return static_cast<uint32_t>(m_Position); }
			bool IsAtEnd() const { return m_Position >= m_Text.size(); }
			char Peek() const { return m_Text[m_Position]; }

			void SkipWhitespace()
			{
				while (m_Position < m_Text.size() && IsWhitespace(m_Text[m_Position]))
				{
					m_Position++;
				}
			}

			// Consumes the character when it is the next one.
			bool Accept(char character)
			{
				if (m_Position < m_Text.size() && m_Text[m_Position] == character)
				{
					m_Position++;
					return true;
				}
				return false;
			}

			// A string from its opening quote to after its closing one: escapes of JSON's forms, no control characters.
			bool SkipString()
			{
				m_Position++;
				while (m_Position < m_Text.size())
				{
					char const character = m_Text[m_Position++];
					if (character == '"')
					{
						return true;
					}
					if (static_cast<unsigned char>(character) < 0x20)
					{
						return false;
					}
					if (character != '\\')
					{
						continue;
					}
					if (m_Position >= m_Text.size())
					{
						return false;
					}
					char const escaped = m_Text[m_Position++];
					switch (escaped)
					{
						case '"':
						case '\\':
						case '/':
						case 'b':
						case 'f':
						case 'n':
						case 'r':
						case 't':
							break;
						case 'u':
							for (int digit = 0; digit < 4; digit++)
							{
								if (m_Position >= m_Text.size() || !IsHexDigit(m_Text[m_Position]))
								{
									return false;
								}
								m_Position++;
							}
							break;
						default:
							return false;
					}
				}
				return false;
			}

			// A value from its first character to after its last. openAround: the brackets open around it.
			bool SkipValue(size_t openAround)
			{
				if (IsAtEnd() || !CanBeginValue(Peek()))
				{
					return false;
				}
				char const first = Peek();
				if (first == '"')
				{
					return SkipString();
				}
				if (first != '{' && first != '[')
				{
					while (m_Position < m_Text.size() && IsScalarCharacter(m_Text[m_Position]))
					{
						m_Position++;
					}
					return true;
				}
				return SkipContainer(openAround);
			}

		private:
			// A container from its opening bracket to after the bracket that closes it, delimited by its brackets and strings.
			bool SkipContainer(size_t openAround)
			{
				size_t open = 0;
				while (m_Position < m_Text.size())
				{
					char const character = m_Text[m_Position];
					switch (character)
					{
						case '"':
							if (!SkipString())
							{
								return false;
							}
							continue;
						case '{':
						case '[':
							if (openAround + open >= MaxJsonScanDepth)
							{
								return false;
							}
							m_Closers[open++] = character == '{' ? '}' : ']';
							break;
						case '}':
						case ']':
							if (m_Closers[open - 1] != character)
							{
								return false;
							}
							if (--open == 0)
							{
								m_Position++;
								return true;
							}
							break;
						case ',':
						case ':':
							break;
						default:
							if (!IsWhitespace(character) && !IsScalarCharacter(character))
							{
								return false;
							}
							break;
					}
					m_Position++;
				}
				return false;
			}

			std::string_view m_Text;
			size_t m_Position = 0;
			// The bracket that closes each container open in SkipContainer.
			std::array<char, MaxJsonScanDepth> m_Closers{};
		};
	}

	std::optional<std::vector<JsonScanMember>> ScanJsonTopLevel(std::string_view text)
	{
		if (text.size() >= std::numeric_limits<uint32_t>::max())
		{
			return std::nullopt;
		}
		Scanner scanner(text);
		scanner.SkipWhitespace();
		if (!scanner.Accept('{'))
		{
			return std::nullopt;
		}
		std::vector<JsonScanMember> members;
		scanner.SkipWhitespace();
		if (!scanner.Accept('}'))
		{
			while (true)
			{
				if (scanner.IsAtEnd() || scanner.Peek() != '"')
				{
					return std::nullopt;
				}
				uint32_t const keyStart = scanner.GetPosition();
				if (!scanner.SkipString())
				{
					return std::nullopt;
				}
				JsonScanMember& member = members.emplace_back();
				member.Key = text.substr(keyStart + 1, scanner.GetPosition() - keyStart - 2);
				scanner.SkipWhitespace();
				if (!scanner.Accept(':'))
				{
					return std::nullopt;
				}
				scanner.SkipWhitespace();
				uint32_t const valueStart = scanner.GetPosition();
				if (scanner.Accept('['))
				{
					// The array's elements, each delimited inside the object and the array.
					scanner.SkipWhitespace();
					if (!scanner.Accept(']'))
					{
						while (true)
						{
							uint32_t const elementStart = scanner.GetPosition();
							if (!scanner.SkipValue(2))
							{
								return std::nullopt;
							}
							member.Elements.emplace_back(elementStart, scanner.GetPosition() - elementStart);
							scanner.SkipWhitespace();
							if (scanner.Accept(']'))
							{
								break;
							}
							if (!scanner.Accept(','))
							{
								return std::nullopt;
							}
							scanner.SkipWhitespace();
						}
					}
				}
				else if (!scanner.SkipValue(1))
				{
					return std::nullopt;
				}
				member.Value = {valueStart, scanner.GetPosition() - valueStart};
				scanner.SkipWhitespace();
				if (scanner.Accept('}'))
				{
					break;
				}
				if (!scanner.Accept(','))
				{
					return std::nullopt;
				}
				scanner.SkipWhitespace();
			}
		}
		scanner.SkipWhitespace();
		if (!scanner.IsAtEnd())
		{
			return std::nullopt;
		}
		return members;
	}
}
