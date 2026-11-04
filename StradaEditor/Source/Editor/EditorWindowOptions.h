#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace Strada
{
	// The largest width or height --window-size accepts (pixels).
	inline constexpr uint32_t MaxEditorWindowSize = 16384;

	// The editor window's size as --window-size gives it: "<width>x<height>" (or "X"), two positive decimal numbers of at
	// most MaxEditorWindowSize, nothing else. Nothing for any other text.
	[[nodiscard]] std::optional<glm::uvec2> ParseEditorWindowSize(std::string_view text);
}
