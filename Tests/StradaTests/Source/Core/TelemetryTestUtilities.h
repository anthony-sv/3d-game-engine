#pragma once

#include "Strada/Core/Telemetry.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strada::Testing
{
	// Initializes the telemetry recorder for as long as it lives (a test may shut it down earlier).
	class TelemetryScope
	{
	public:
		TelemetryScope() { Telemetry::Init(); }
		~TelemetryScope()
		{
			if (Telemetry::IsInitialized())
			{
				Telemetry::Shutdown();
			}
		}

		TelemetryScope(TelemetryScope const&) = delete;
		TelemetryScope& operator=(TelemetryScope const&) = delete;
	};

	inline TelemetryStep MakeTelemetryStep(uint64_t step, uint64_t source = 1, float stepSeconds = 1.0f / 60.0f)
	{
		return {source, step, stepSeconds};
	}

	// The newest session held.
	inline TelemetrySession const& GetNewestTelemetrySession()
	{
		REQUIRE_FALSE(Telemetry::GetSessions().empty());
		return Telemetry::GetSessions().back();
	}

	// A channel's value at each row of its session, none where it has none (or no such channel).
	inline std::vector<std::optional<float>> GetTelemetryColumn(TelemetrySession const& session, std::string_view name)
	{
		std::vector<std::optional<float>> column(session.Rows);
		if (TelemetryChannel const* const channel = FindTelemetryChannel(session, name))
		{
			for (uint64_t row = 0; row < session.Rows; ++row)
			{
				column[row] = GetTelemetryValue(*channel, row);
			}
		}
		return column;
	}

	// The labels of a row's marks, in their order.
	inline std::vector<std::string> GetTelemetryMarks(TelemetrySession const& session, uint64_t row)
	{
		std::vector<std::string> labels;
		for (TelemetryMark const& mark : session.Marks)
		{
			if (mark.Row == row)
			{
				labels.push_back(mark.Label);
			}
		}
		return labels;
	}
}
