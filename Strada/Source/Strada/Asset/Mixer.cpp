#include "stpch.h"
#include "Strada/Asset/Mixer.h"

#include "Strada/Asset/MixerAsset.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		// The hints of the MixerEffect field a parameter is (the field of the same name): its range and the kinds that use
		// it, read from the one field table, so that a snapshot's value is held to what the effect's own field accepts.
		FieldHints const* FindEffectParameterHints(MixerEffectParameter parameter)
		{
			std::string_view const name = EnumToString(parameter);
			FieldHints const* found = nullptr;
			std::apply(
				[name, &found](auto const&... fields)
				{
					static_cast<void>(((fields.Name == name ? (found = &fields.Hints, true) : false) || ...));
				},
				StructTraits<MixerEffect>::Fields);
			return found;
		}

		bool IsParameterOfKind(FieldHints const& hints, MixerEffectKind kind)
		{
			std::span<std::string_view const> const kinds = hints.GetVisibleWhenValues();
			return std::find(kinds.begin(), kinds.end(), EnumToString(kind)) != kinds.end();
		}

		// What a snapshot value sets, so that two values of one snapshot that set the same thing are found.
		std::tuple<std::string, MixerSnapshotTarget, uint32_t, MixerEffectParameter> GetSnapshotValueKey(MixerSnapshotValue const& value)
		{
			bool const isEffect = value.Target == MixerSnapshotTarget::Effect;
			return {MakeMixerBusKey(value.Bus), value.Target, isEffect ? value.Effect : 0,
			        isEffect ? value.Parameter : MixerEffectParameter::Frequency};
		}

		Result<void> ValidateDucking(Mixer const& mixer, std::unordered_map<std::string, size_t> const& indices, size_t master)
		{
			// A bus's level depends on its own output (what feeds its parent is measured after its gains), on what feeds it
			// (its child buses) and on the bus that ducks it. Ducking by a level that depends on the bus itself would feed
			// the bus's output back into its own gain: refused, as a cycle of that dependency.
			size_t const count = mixer.Buses.size();
			std::vector<std::vector<size_t>> dependsOn(count);
			for (size_t index = 0; index < count; index++)
			{
				MixerBus const& bus = mixer.Buses[index];
				if (index != master)
				{
					size_t const parent = indices.at(MakeMixerBusKey(bus.Parent));
					dependsOn[parent].push_back(index);
				}
				if (bus.DuckedBy.empty())
				{
					continue;
				}
				auto const ducker = indices.find(MakeMixerBusKey(bus.DuckedBy));
				if (ducker == indices.end())
				{
					return MakeError("Buses[{}].DuckedBy: '{}' is ducked by '{}', which is not a bus of the mixer", index, bus.Name,
					                 bus.DuckedBy);
				}
				if (ducker->second == index)
				{
					return MakeError("Buses[{}].DuckedBy: '{}' cannot be ducked by its own level", index, bus.Name);
				}
				dependsOn[index].push_back(ducker->second);
			}
			// Depth-first search for a cycle (0: not visited, 1: on the path, 2: done), from the buses in list order.
			std::vector<uint8_t> states(count, 0);
			std::vector<std::pair<size_t, size_t>> path;
			for (size_t start = 0; start < count; start++)
			{
				if (states[start] != 0)
				{
					continue;
				}
				states[start] = 1;
				path.emplace_back(start, 0);
				while (!path.empty())
				{
					auto& [current, next] = path.back();
					if (next == dependsOn[current].size())
					{
						states[current] = 2;
						path.pop_back();
						continue;
					}
					size_t const neighbour = dependsOn[current][next++];
					if (states[neighbour] == 1)
					{
						// The cycle runs through a ducking link (the parents alone have none): name the first ducked bus on it.
						auto const onCycle = std::find_if(path.begin(), path.end(),
						                                  [neighbour](std::pair<size_t, size_t> const& entry)
						                                  {
															  return entry.first == neighbour;
														  });
						for (auto step = onCycle; step != path.end(); ++step)
						{
							size_t const from = step->first;
							size_t const to = std::next(step) != path.end() ? std::next(step)->first : neighbour;
							MixerBus const& ducked = mixer.Buses[from];
							if (!ducked.DuckedBy.empty() && indices.at(MakeMixerBusKey(ducked.DuckedBy)) == to)
							{
								return MakeError("Buses[{}].DuckedBy: '{}' is ducked by '{}', whose level depends on '{}' itself", from,
								                 ducked.Name, ducked.DuckedBy, ducked.Name);
							}
						}
						return MakeError("Buses: the ducking of the mixer's buses depends on itself");
					}
					if (states[neighbour] == 0)
					{
						states[neighbour] = 1;
						path.emplace_back(neighbour, 0);
					}
				}
			}
			return {};
		}

		Result<void> ValidateSnapshots(Mixer const& mixer, std::unordered_map<std::string, size_t> const& indices)
		{
			std::unordered_map<std::string, size_t> names;
			for (size_t index = 0; index < mixer.Snapshots.size(); index++)
			{
				MixerSnapshot const& snapshot = mixer.Snapshots[index];
				if (snapshot.Name.empty())
				{
					return MakeError("Snapshots[{}].Name: a snapshot has a name", index);
				}
				if (snapshot.Name.size() > MaxMixerSnapshotNameBytes)
				{
					return MakeError("Snapshots[{}].Name: a snapshot's name is at most {} bytes, not {}", index, MaxMixerSnapshotNameBytes,
					                 snapshot.Name.size());
				}
				auto const [existing, added] = names.emplace(MakeMixerBusKey(snapshot.Name), index);
				if (!added)
				{
					return MakeError("Snapshots[{}].Name: '{}' is the name of Snapshots[{}] ('{}'): names are unique, ignoring letter case",
					                 index, snapshot.Name, existing->second, mixer.Snapshots[existing->second].Name);
				}
				std::vector<std::tuple<std::string, MixerSnapshotTarget, uint32_t, MixerEffectParameter>> keys;
				for (size_t valueIndex = 0; valueIndex < snapshot.Values.size(); valueIndex++)
				{
					MixerSnapshotValue const& value = snapshot.Values[valueIndex];
					auto const bus = indices.find(MakeMixerBusKey(value.Bus));
					if (bus == indices.end())
					{
						return MakeError("Snapshots[{}].Values[{}].Bus: '{}' of snapshot '{}' is not a bus of the mixer", index, valueIndex,
						                 value.Bus, snapshot.Name);
					}
					switch (value.Target)
					{
						case MixerSnapshotTarget::Volume:
							if (!(value.Value >= MinMixerVolumeDb && value.Value <= MaxMixerVolumeDb))
							{
								return MakeError("Snapshots[{}].Values[{}].Value: a volume is {} to {} dB, not {}", index, valueIndex,
								                 MinMixerVolumeDb, MaxMixerVolumeDb, value.Value);
							}
							break;
						case MixerSnapshotTarget::Muted:
							if (value.Value != 0.0f && value.Value != 1.0f)
							{
								return MakeError("Snapshots[{}].Values[{}].Value: a mute is 1 (muted) or 0, not {}", index, valueIndex,
								                 value.Value);
							}
							break;
						case MixerSnapshotTarget::Effect:
						{
							MixerBus const& target = mixer.Buses[bus->second];
							if (value.Effect >= target.Effects.size())
							{
								return MakeError("Snapshots[{}].Values[{}].Effect: '{}' has {} effects, so no effect {}", index, valueIndex,
								                 target.Name, target.Effects.size(), value.Effect);
							}
							MixerEffectKind const kind = target.Effects[value.Effect].Kind;
							FieldHints const* const hints = FindEffectParameterHints(value.Parameter);
							if (hints == nullptr || !IsParameterOfKind(*hints, kind))
							{
								return MakeError("Snapshots[{}].Values[{}].Parameter: effect {} of '{}' is a {}, which has no {}", index,
								                 valueIndex, value.Effect, target.Name, EnumToString(kind), EnumToString(value.Parameter));
							}
							double const number = static_cast<double>(value.Value);
							if (!(number >= hints->Min && number <= hints->Max))
							{
								return MakeError("Snapshots[{}].Values[{}].Value: {} is {} to {}, not {}", index, valueIndex,
								                 EnumToString(value.Parameter), hints->Min, hints->Max, value.Value);
							}
							break;
						}
						default:
							return MakeError("Snapshots[{}].Values[{}].Target: not a target a snapshot sets", index, valueIndex);
					}
					auto key = GetSnapshotValueKey(value);
					if (std::find(keys.begin(), keys.end(), key) != keys.end())
					{
						return MakeError("Snapshots[{}].Values[{}]: snapshot '{}' already sets what this value sets on '{}'", index,
						                 valueIndex, snapshot.Name, value.Bus);
					}
					keys.push_back(std::move(key));
				}
			}
			return {};
		}
	}

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

	void SetMixerEffectParameter(MixerEffect& effect, MixerEffectParameter parameter, float value)
	{
		switch (parameter)
		{
			case MixerEffectParameter::Frequency:
				effect.Frequency = value;
				break;
			case MixerEffectParameter::Q:
				effect.Q = value;
				break;
			case MixerEffectParameter::GainDb:
				effect.GainDb = value;
				break;
			case MixerEffectParameter::DelaySeconds:
				effect.DelaySeconds = value;
				break;
			case MixerEffectParameter::Feedback:
				effect.Feedback = value;
				break;
			case MixerEffectParameter::RoomSize:
				effect.RoomSize = value;
				break;
			case MixerEffectParameter::Damping:
				effect.Damping = value;
				break;
			case MixerEffectParameter::Width:
				effect.Width = value;
				break;
			case MixerEffectParameter::WetDb:
				effect.WetDb = value;
				break;
			case MixerEffectParameter::DryDb:
				effect.DryDb = value;
				break;
			case MixerEffectParameter::ThresholdDb:
				effect.ThresholdDb = value;
				break;
			case MixerEffectParameter::Ratio:
				effect.Ratio = value;
				break;
			case MixerEffectParameter::KneeDb:
				effect.KneeDb = value;
				break;
			case MixerEffectParameter::AttackSeconds:
				effect.AttackSeconds = value;
				break;
			case MixerEffectParameter::ReleaseSeconds:
				effect.ReleaseSeconds = value;
				break;
			case MixerEffectParameter::MakeupDb:
				effect.MakeupDb = value;
				break;
			case MixerEffectParameter::CeilingDb:
				effect.CeilingDb = value;
				break;
		}
	}

	MixerSnapshot const* FindMixerSnapshot(Mixer const& mixer, std::string_view name)
	{
		if (name.empty())
		{
			return nullptr;
		}
		std::string const key = MakeMixerBusKey(name);
		auto const found = std::find_if(mixer.Snapshots.begin(), mixer.Snapshots.end(),
		                                [&key](MixerSnapshot const& snapshot)
		                                {
											return MakeMixerBusKey(snapshot.Name) == key;
										});
		return found != mixer.Snapshots.end() ? &*found : nullptr;
	}

	Mixer ApplyMixerSnapshot(Mixer const& mixer, MixerSnapshot const* snapshot)
	{
		Mixer applied = mixer;
		if (snapshot == nullptr)
		{
			return applied;
		}
		for (MixerSnapshotValue const& value : snapshot->Values)
		{
			std::string const key = MakeMixerBusKey(value.Bus);
			auto const bus = std::find_if(applied.Buses.begin(), applied.Buses.end(),
			                              [&key](MixerBus const& candidate)
			                              {
											  return MakeMixerBusKey(candidate.Name) == key;
										  });
			if (bus == applied.Buses.end())
			{
				continue;
			}
			switch (value.Target)
			{
				case MixerSnapshotTarget::Volume:
					bus->VolumeDb = value.Value;
					break;
				case MixerSnapshotTarget::Muted:
					bus->Muted = value.Value != 0.0f;
					break;
				case MixerSnapshotTarget::Effect:
					if (value.Effect < bus->Effects.size())
					{
						SetMixerEffectParameter(bus->Effects[value.Effect], value.Parameter, value.Value);
					}
					break;
			}
		}
		return applied;
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
		if (Result<void> ducking = ValidateDucking(mixer, indices, master); !ducking)
		{
			return ducking;
		}
		return ValidateSnapshots(mixer, indices);
	}
}
