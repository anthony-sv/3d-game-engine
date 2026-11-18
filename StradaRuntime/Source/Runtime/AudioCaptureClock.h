#pragma once

#include <cmath>
#include <cstdint>

namespace Strada
{
	// How many frames of audio each frame of a run records (the player's --audio-capture): the frame's time at the mix's
	// sample rate, the fraction left over carried into the next frame, so that a run's frames of audio add up to its time
	// whatever its step. The same steps give the same counts in every run.
	class AudioCaptureClock
	{
	public:
		explicit AudioCaptureClock(uint32_t sampleRate)
			: m_SampleRate(sampleRate)
		{
		}

		// The frames of audio a frame of `seconds` covers; a time that is not positive and finite covers none.
		uint64_t Advance(double seconds)
		{
			if (!(seconds > 0.0) || !std::isfinite(seconds))
			{
				return 0;
			}
			m_Pending += seconds * static_cast<double>(m_SampleRate);
			double const whole = std::floor(m_Pending);
			m_Pending -= whole;
			return static_cast<uint64_t>(whole);
		}

	private:
		uint32_t m_SampleRate = 0;
		// The fraction of a frame the frames so far have covered and not yet recorded (0 to 1).
		double m_Pending = 0.0;
	};
}
