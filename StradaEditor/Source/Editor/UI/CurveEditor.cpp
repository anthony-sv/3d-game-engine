#include "Editor/UI/CurveEditor.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace Strada
{
	void DrawCurveKeyDiamond(ImDrawList* drawList, glm::vec2 center, float radius, uint32_t fill, uint32_t outline)
	{
		ImVec2 const top(center.x, center.y - radius);
		ImVec2 const right(center.x + radius, center.y);
		ImVec2 const bottom(center.x, center.y + radius);
		ImVec2 const left(center.x - radius, center.y);
		drawList->AddQuadFilled(top, right, bottom, left, fill);
		drawList->AddQuad(top, right, bottom, left, outline, 1.0f);
	}

	std::pair<double, int> GetCurveRulerLines(double low, double high, double step, int limit)
	{
		if (!std::isfinite(low) || !std::isfinite(high) || !std::isfinite(step) || !(step > 0.0) || !(high >= low))
		{
			return {0.0, 0};
		}
		double const first = std::ceil(low / step);
		double const last = std::floor(high / step);
		if (!std::isfinite(first) || !std::isfinite(last) || !(last >= first))
		{
			return {0.0, 0};
		}
		return {first, static_cast<int>(std::min(last - first + 1.0, static_cast<double>(std::max(limit, 0))))};
	}
}
