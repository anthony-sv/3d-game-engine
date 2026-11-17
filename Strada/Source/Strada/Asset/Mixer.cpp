#include "stpch.h"
#include "Strada/Asset/Mixer.h"

#include <string>
#include <unordered_map>

namespace Strada
{
	std::string MakeMixerBusKey(std::string_view name)
	{
		std::string key(name.empty() ? MasterBusName : name);
		for (char& character : key)
		{
			if (character >= 'A' && character <= 'Z')
			{
				character = static_cast<char>(character - 'A' + 'a');
			}
		}
		return key;
	}

	float GetMixerEffectParameter(MixerEffect const& effect, MixerEffectParameter parameter)
	{
		switch (parameter)
		{
			case MixerEffectParameter::Frequency:
				return effect.Frequency;
			case MixerEffectParameter::Q:
				return effect.Q;
			case MixerEffectParameter::GainDb:
				return effect.GainDb;
			case MixerEffectParameter::DelaySeconds:
				return effect.DelaySeconds;
			case MixerEffectParameter::Feedback:
				return effect.Feedback;
			case MixerEffectParameter::RoomSize:
				return effect.RoomSize;
			case MixerEffectParameter::Damping:
				return effect.Damping;
			case MixerEffectParameter::Width:
				return effect.Width;
			case MixerEffectParameter::WetDb:
				return effect.WetDb;
			case MixerEffectParameter::DryDb:
				return effect.DryDb;
			case MixerEffectParameter::ThresholdDb:
				return effect.ThresholdDb;
			case MixerEffectParameter::Ratio:
				return effect.Ratio;
			case MixerEffectParameter::KneeDb:
				return effect.KneeDb;
			case MixerEffectParameter::AttackSeconds:
				return effect.AttackSeconds;
			case MixerEffectParameter::ReleaseSeconds:
				return effect.ReleaseSeconds;
			case MixerEffectParameter::MakeupDb:
				return effect.MakeupDb;
			case MixerEffectParameter::CeilingDb:
				return effect.CeilingDb;
		}
		return 0.0f;
	}

	Result<void> ValidateMixer(Mixer const& mixer)
	{
		if (mixer.Buses.size() > MaxMixerBuses)
		{
			return MakeError("Buses: a mixer has at most {} buses, not {}", MaxMixerBuses, mixer.Buses.size());
		}
		// Each bus's index by its key, so that parents match ignoring letter case as names do.
		std::unordered_map<std::string, size_t> indices;
		size_t master = mixer.Buses.size();
		for (size_t index = 0; index < mixer.Buses.size(); index++)
		{
			MixerBus const& bus = mixer.Buses[index];
			if (bus.Name.empty())
			{
				return MakeError("Buses[{}].Name: a bus has a name", index);
			}
			if (bus.Name.size() > MaxMixerBusNameBytes)
			{
				return MakeError("Buses[{}].Name: a bus's name is at most {} bytes, not {}", index, MaxMixerBusNameBytes, bus.Name.size());
			}
			if (bus.Effects.size() > MaxBusEffects)
			{
				return MakeError("Buses[{}].Effects: '{}' has at most {} effects, not {}", index, bus.Name, MaxBusEffects,
				                 bus.Effects.size());
			}
			auto const [existing, added] = indices.emplace(MakeMixerBusKey(bus.Name), index);
			if (!added)
			{
				return MakeError("Buses[{}].Name: '{}' is the name of Buses[{}] ('{}'): names are unique, ignoring letter case", index,
				                 bus.Name, existing->second, mixer.Buses[existing->second].Name);
			}
			if (bus.Name == MasterBusName)
			{
				master = index;
			}
		}
		if (master == mixer.Buses.size())
		{
			return MakeError("Buses: a mixer has a bus named {}, the root every other bus feeds", MasterBusName);
		}
		if (!mixer.Buses[master].Parent.empty())
		{
			return MakeError("Buses[{}].Parent: {} is the root and feeds no bus, not '{}'", master, MasterBusName,
			                 mixer.Buses[master].Parent);
		}
		for (size_t index = 0; index < mixer.Buses.size(); index++)
		{
			if (index == master)
			{
				continue;
			}
			// An empty parent is Master. Following the parents reaches Master in fewer steps than there are buses, or they
			// go round.
			size_t current = index;
			for (size_t steps = 0; current != master; steps++)
			{
				MixerBus const& bus = mixer.Buses[current];
				auto const parent = indices.find(MakeMixerBusKey(bus.Parent));
				if (parent == indices.end())
				{
					return MakeError("Buses[{}].Parent: the parent of '{}', '{}', is not a bus of the mixer", current, bus.Name,
					                 bus.Parent);
				}
				if (parent->second == current || steps >= mixer.Buses.size())
				{
					return MakeError("Buses[{}].Parent: '{}' feeds itself through its parents, which never reach {}", index,
					                 mixer.Buses[index].Name, MasterBusName);
				}
				current = parent->second;
			}
		}
		return {};
	}
}
