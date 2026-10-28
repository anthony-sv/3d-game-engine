#pragma once

#include "Strada/Serialization/JsonSerialization.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	// Writes JSON text exactly as nlohmann's dump writes it (pretty: dump(IndentStep, IndentChar); compact: dump(-1)) with
	// ensure_ascii off and error_handler_t::replace, without building a Json document: writers of large documents call it
	// element by element. Uses nlohmann's public API only. Any thread; no state beyond the text.
	//
	// Calls form one JSON value: inside an object every value follows a Key, inside an array values follow each other, and
	// every container that was begun is ended before TakeText.
	class JsonTextWriter
	{
	public:
		struct Style
		{
			bool Pretty = true;
			char IndentChar = '\t';
			uint32_t IndentStep = 1;
		};

		// reserve: the bytes the text is expected to take, reserved up front.
		explicit JsonTextWriter(Style style, size_t reserve = 0);

		void BeginObject();
		void EndObject();
		void BeginArray();
		void EndArray();
		void Key(std::string_view key);
		void String(std::string_view value);
		void Integer(int64_t value);
		void Unsigned(uint64_t value);
		// What Json(value).dump() writes. A float of a document is Double(static_cast<double>(value)), as JsonTraits<float>
		// stores it.
		void Double(double value);
		void Bool(bool value);
		void Null();
		// A whole value at the current depth, as nlohmann writes it there.
		void Value(Json const& value);

		// The text written; the writer is empty afterwards.
		[[nodiscard]] std::string TakeText();

	private:
		// What comes before a value: nothing after a key or at the top, the separator and indentation in an array.
		void BeforeValue();
		// The separator from the previous element of the open container (none before its first) and, pretty, the line break
		// and indentation of its elements.
		void BeforeElement();
		void Close(char bracket);
		void WriteIndent(size_t depth);
		// A string with its quotes, escaped as nlohmann escapes it.
		void WriteString(std::string_view text);
		// A value nlohmann writes itself (binary values, discarded values), indented to the current depth.
		void WriteDumped(Json const& value);

		Style m_Style;
		std::string m_Text;
		// Per open container: whether it has an element yet (an empty one is written "{}" or "[]").
		std::vector<uint8_t> m_HasElements;
		// A key was written and its value has not been.
		bool m_AfterKey = false;
	};
}
