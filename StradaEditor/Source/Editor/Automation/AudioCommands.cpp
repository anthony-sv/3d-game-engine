#include "Editor/Automation/AudioCommands.h"

#include "Strada/Asset/Mixer.h"
#include "Strada/Audio/AudioEngine.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		// A level as the answer gives it: null when it is not a finite number (a mix gone wrong), which JSON cannot hold.
		Json DescribeLevel(float level)
		{
			return std::isfinite(level) ? Json(level) : Json();
		}
	}

	float ToMeterDecibels(float level)
	{
		if (!(level > 0.0f))
		{
			return MinMixerVolumeDb;
		}
		return std::max(static_cast<float>(20.0 * std::log10(static_cast<double>(level))), MinMixerVolumeDb);
	}

	Result<void> RegisterAudioCommands(CommandRegistry& registry)
	{
		CommandDefinition meters;
		meters.Name = "audio.meters";
		meters.Description =
			"The audio mix as the engine last mixed it: { initialized (false when the audio engine is not running: no buses and "
			"counts of 0), buses ([{ name, peak, rms, peakDb, rmsDb }] in the mixer's order: the linear peak and RMS of each "
			"bus's output over the last block mixed, after its effects and every gain, and the same in dBFS, silence reading "
			"-80; a level that is not a finite number reads null), snapshot (the snapshot the mix is at or blends to; empty for the mixer's own values), activeVoices (voices "
			"playing in scenes that are not paused), mixedVoices (voices mixed in the last block), stolenVoices (voices stolen "
			"since the engine started) }. The buses are the mixer the last game applied, Master alone before any; the levels "
			"move only while the engine mixes (a playing game, or the editor's null device). Not an undo step; changes nothing.";
		meters.ReadOnly = true;
		meters.Handler = [](Json const&) -> CommandResult
		{
			Json buses = Json::array();
			for (AudioBusMeter const& meter : AudioEngine::GetBusMeters())
			{
				buses.push_back(Json{
					{"name", meter.Name},
					{"peak", DescribeLevel(meter.Peak)},
					{"rms", DescribeLevel(meter.Rms)},
					{"peakDb", DescribeLevel(ToMeterDecibels(meter.Peak))},
					{"rmsDb", DescribeLevel(ToMeterDecibels(meter.Rms))},
				});
			}
			return Json{
				{"initialized", AudioEngine::IsInitialized()},      {"buses", std::move(buses)},
				{"snapshot", AudioEngine::GetCurrentSnapshot()},    {"activeVoices", AudioEngine::GetActiveVoiceCount()},
				{"mixedVoices", AudioEngine::GetMixedVoiceCount()}, {"stolenVoices", AudioEngine::GetStolenVoiceCount()},
			};
		};
		return registry.Register(std::move(meters));
	}
}
