#include "stpch.h"
#include "Strada/Serialization/JsonTextWriter.h"

#include "Strada/Core/Assert.h"
#include "Strada/Core/Utf8.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <utility>

namespace Strada
{
	namespace
	{
		// nlohmann's serializer indents with a string of 512 indent characters, which it lengthens with spaces when a document
		// nests deeper (serializer.hpp:74, :131): the indentation past its 512th character is spaces.
		constexpr size_t IndentCharacters = 512;

		// Integral doubles below this magnitude are written in fixed form, as the integer followed by ".0": nlohmann formats
		// numbers below 10^digits10 (10^15) so (to_chars.hpp:1108, :993).
		constexpr double FixedIntegralLimit = 1.0e15;

		template<typename TInteger>
		void AppendInteger(std::string& text, TInteger value)
		{
			std::array<char, 24> digits{};
			auto const [end, error] = std::to_chars(digits.data(), digits.data() + digits.size(), value);
			ST_CORE_ASSERT(error == std::errc(), "24 characters hold every 64-bit integer");
			text.append(digits.data(), end);
		}

		// The escapes of nlohmann's dump_escaped (serializer.hpp:391, :464): '"', '\\' and the code points below U+0020; with
		// ensure_ascii off nothing else is escaped, DEL included.
		void AppendEscaped(std::string& text, std::string_view value)
		{
			constexpr std::string_view HexDigits = "0123456789abcdef";
			size_t run = 0;
			for (size_t index = 0; index < value.size(); index++)
			{
				auto const byte = static_cast<unsigned char>(value[index]);
				if (byte >= 0x20 && byte != '"' && byte != '\\')
				{
					continue;
				}
				text.append(value.data() + run, index - run);
				run = index + 1;
				text.push_back('\\');
				switch (byte)
				{
					case '"':
						text.push_back('"');
						break;
					case '\\':
						text.push_back('\\');
						break;
					case '\b':
						text.push_back('b');
						break;
					case '\t':
						text.push_back('t');
						break;
					case '\n':
						text.push_back('n');
						break;
					case '\f':
						text.push_back('f');
						break;
					case '\r':
						text.push_back('r');
						break;
					default:
						text.append("u00");
						text.push_back(HexDigits[byte >> 4]);
						text.push_back(HexDigits[byte & 0x0F]);
						break;
				}
			}
			text.append(value.data() + run, value.size() - run);
		}
	}

	JsonTextWriter::JsonTextWriter(Style style, size_t reserve)
		: m_Style(style)
	{
		m_Text.reserve(reserve);
	}

	void JsonTextWriter::BeginObject()
	{
		BeforeValue();
		m_Text.push_back('{');
		m_HasElements.push_back(0);
	}

	void JsonTextWriter::EndObject()
	{
		Close('}');
	}

	void JsonTextWriter::BeginArray()
	{
		BeforeValue();
		m_Text.push_back('[');
		m_HasElements.push_back(0);
	}

	void JsonTextWriter::EndArray()
	{
		Close(']');
	}

	void JsonTextWriter::Key(std::string_view key)
	{
		ST_CORE_ASSERT(!m_HasElements.empty() && !m_AfterKey, "A key is written in an object, before its value");
		BeforeElement();
		WriteString(key);
		if (m_Style.Pretty)
		{
			m_Text.append(": ");
		}
		else
		{
			m_Text.push_back(':');
		}
		m_AfterKey = true;
	}

	void JsonTextWriter::String(std::string_view value)
	{
		BeforeValue();
		WriteString(value);
	}

	void JsonTextWriter::Integer(int64_t value)
	{
		BeforeValue();
		AppendInteger(m_Text, value);
	}

	void JsonTextWriter::Unsigned(uint64_t value)
	{
		BeforeValue();
		AppendInteger(m_Text, value);
	}

	void JsonTextWriter::Double(double value)
	{
		BeforeValue();
		if (!std::isfinite(value))
		{
			m_Text.append("null");
			return;
		}
		if (std::abs(value) < FixedIntegralLimit && std::trunc(value) == value)
		{
			// nlohmann keeps the sign of zero.
			if (value == 0.0)
			{
				m_Text.append(std::signbit(value) ? "-0.0" : "0.0");
				return;
			}
			AppendInteger(m_Text, static_cast<int64_t>(value));
			m_Text.append(".0");
			return;
		}
		// nlohmann's Grisu2 does not always give the shortest digits that read back, so std::to_chars could differ.
		m_Text.append(Json(value).dump());
	}

	void JsonTextWriter::Bool(bool value)
	{
		BeforeValue();
		m_Text.append(value ? "true" : "false");
	}

	void JsonTextWriter::Null()
	{
		BeforeValue();
		m_Text.append("null");
	}

	void JsonTextWriter::Value(Json const& value)
	{
		if (value.is_object())
		{
			BeginObject();
			for (auto const& member : value.items())
			{
				Key(member.key());
				Value(member.value());
			}
			EndObject();
		}
		else if (value.is_array())
		{
			BeginArray();
			for (Json const& element : value)
			{
				Value(element);
			}
			EndArray();
		}
		else if (value.is_string())
		{
			String(value.get_ref<std::string const&>());
		}
		else if (value.is_boolean())
		{
			Bool(value.get<bool>());
		}
		// Unsigned integers are integers too (is_number_integer): asked first.
		else if (value.is_number_unsigned())
		{
			Unsigned(value.get<uint64_t>());
		}
		else if (value.is_number_integer())
		{
			Integer(value.get<int64_t>());
		}
		else if (value.is_number_float())
		{
			Double(value.get<double>());
		}
		else if (value.is_null())
		{
			Null();
		}
		else
		{
			WriteDumped(value);
		}
	}

	std::string JsonTextWriter::TakeText()
	{
		ST_CORE_ASSERT(m_HasElements.empty() && !m_AfterKey, "Every container is ended before the text is taken");
		m_HasElements.clear();
		m_AfterKey = false;
		return std::exchange(m_Text, std::string());
	}

	void JsonTextWriter::BeforeValue()
	{
		if (m_AfterKey)
		{
			m_AfterKey = false;
			return;
		}
		if (!m_HasElements.empty())
		{
			BeforeElement();
		}
	}

	void JsonTextWriter::BeforeElement()
	{
		uint8_t& hasElements = m_HasElements.back();
		if (hasElements != 0)
		{
			m_Text.push_back(',');
		}
		hasElements = 1;
		if (m_Style.Pretty)
		{
			m_Text.push_back('\n');
			WriteIndent(m_HasElements.size());
		}
	}

	void JsonTextWriter::Close(char bracket)
	{
		ST_CORE_ASSERT(!m_HasElements.empty() && !m_AfterKey, "A container is ended after it was begun, never between a key and its value");
		bool const hadElements = m_HasElements.back() != 0;
		m_HasElements.pop_back();
		if (hadElements && m_Style.Pretty)
		{
			m_Text.push_back('\n');
			WriteIndent(m_HasElements.size());
		}
		m_Text.push_back(bracket);
	}

	void JsonTextWriter::WriteIndent(size_t depth)
	{
		size_t const count = depth * m_Style.IndentStep;
		size_t const indentCharacters = std::min(count, IndentCharacters);
		m_Text.append(indentCharacters, m_Style.IndentChar);
		m_Text.append(count - indentCharacters, ' ');
	}

	void JsonTextWriter::WriteString(std::string_view text)
	{
		if (!Utf8::IsValid(text))
		{
			// nlohmann's replacement of invalid bytes, exactly (it may read a byte again after an invalid sequence).
			m_Text.append(Json(std::string(text)).dump(-1, ' ', false, Json::error_handler_t::replace));
			return;
		}
		m_Text.push_back('"');
		AppendEscaped(m_Text, text);
		m_Text.push_back('"');
	}

	void JsonTextWriter::WriteDumped(Json const& value)
	{
		BeforeValue();
		if (!m_Style.Pretty)
		{
			m_Text.append(value.dump(-1, ' ', false, Json::error_handler_t::replace));
			return;
		}
		// Dumped alone, a value is indented from depth 0; it holds no strings with line breaks (they are escaped), so every
		// line break starts a line that is one level of the current depth further in.
		std::string const dumped =
			value.dump(static_cast<int>(m_Style.IndentStep), m_Style.IndentChar, false, Json::error_handler_t::replace);
		size_t start = 0;
		for (size_t lineBreak = dumped.find('\n'); lineBreak != std::string::npos; lineBreak = dumped.find('\n', start))
		{
			m_Text.append(dumped, start, lineBreak + 1 - start);
			WriteIndent(m_HasElements.size());
			start = lineBreak + 1;
		}
		m_Text.append(dumped, start, std::string::npos);
	}
}
