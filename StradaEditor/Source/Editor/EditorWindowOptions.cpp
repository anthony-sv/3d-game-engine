#include "Editor/EditorWindowOptions.h"

#include <charconv>
#include <system_error>

namespace Strada
{
	namespace
	{
		// A whole positive number of at most MaxEditorWindowSize, digits only (no sign, no spaces).
		std::optional<uint32_t> ParseDimension(std::string_view text)
		{
			if (text.empty())
			{
				return std::nullopt;
			}
			for (char const character : text)
			{
				if (character < '0' || character > '9')
				{
					return std::nullopt;
				}
			}
			uint64_t value = 0;
			auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
			if (error != std::errc() || end != text.data() + text.size() || value == 0 || value > MaxEditorWindowSize)
			{
				return std::nullopt;
			}
			return static_cast<uint32_t>(value);
		}
	}

	std::optional<glm::uvec2> ParseEditorWindowSize(std::string_view text)
	{
		size_t const separator = text.find_first_of("xX");
		if (separator == std::string_view::npos)
		{
			return std::nullopt;
		}
		std::optional<uint32_t> const width = ParseDimension(text.substr(0, separator));
		std::optional<uint32_t> const height = ParseDimension(text.substr(separator + 1));
		if (!width || !height)
		{
			return std::nullopt;
		}
		return glm::uvec2(*width, *height);
	}
}
