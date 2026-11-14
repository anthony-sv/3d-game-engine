#include "Editor/UI/CurveView.h"

#include <cmath>

namespace Strada
{
	double GetNiceCurveStep(double minimum)
	{
		if (!(minimum > 0.0) || !std::isfinite(minimum))
		{
			return 1.0;
		}
		double const power = std::pow(10.0, std::floor(std::log10(minimum)));
		for (double const factor : {1.0, 2.0, 5.0, 10.0})
		{
			if (factor * power >= minimum)
			{
				return factor * power;
			}
		}
		return 10.0 * power;
	}
}
