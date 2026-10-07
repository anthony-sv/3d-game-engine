#pragma once

#include <chrono>

namespace Strada
{
	// Monotonic stopwatch.
	class Timer
	{
	public:
		Timer() { Reset(); }

		void Reset() { m_Start = std::chrono::steady_clock::now(); }

		double GetElapsedSeconds() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_Start).count(); }

		double GetElapsedMilliseconds() const { return GetElapsedSeconds() * 1000.0; }

	private:
		std::chrono::steady_clock::time_point m_Start;
	};
}
