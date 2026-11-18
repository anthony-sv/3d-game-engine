#include "Editor/MixerEditing.h"

#include "Strada/Asset/MixerAsset.h"
#include "Strada/Serialization/JsonSerialization.h"
#include "Strada/Serialization/StructSerialization.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		// The first of "<base>", "<base>2", "<base>3" and so on whose key no element's name has.
		template<typename TElement>
		std::string MakeFreeName(std::vector<TElement> const& elements, std::string_view base)
		{
			for (uint32_t number = 1;; number++)
			{
				std::string const name = number == 1 ? std::string(base) : fmt::format("{}{}", base, number);
				std::string const key = MakeMixerBusKey(name);
				bool const used = std::ranges::any_of(elements,
				                                      [&key](TElement const& element)
				                                      {
														  return MakeMixerBusKey(element.Name) == key;
													  });
				if (!used)
				{
					return name;
				}
			}
		}
	}

	std::string MakeMixerBusName(Mixer const& mixer)
	{
		return MakeFreeName(mixer.Buses, "Bus");
	}

	std::string MakeMixerSnapshotName(Mixer const& mixer)
	{
		return MakeFreeName(mixer.Snapshots, "Snapshot");
	}

	uint32_t AddMixerBus(Mixer& mixer, std::string_view parent)
	{
		MixerBus bus;
		bus.Name = MakeMixerBusName(mixer);
		bus.Parent = parent.empty() ? std::string(MasterBusName) : std::string(parent);
		mixer.Buses.push_back(std::move(bus));
		return static_cast<uint32_t>(mixer.Buses.size() - 1);
	}

	bool RemoveMixerBus(Mixer& mixer, uint32_t index)
	{
		if (index >= mixer.Buses.size() || mixer.Buses[index].Name == MasterBusName)
		{
			return false;
		}
		std::string const key = MakeMixerBusKey(mixer.Buses[index].Name);
		std::string const parent = mixer.Buses[index].Parent.empty() ? std::string(MasterBusName) : mixer.Buses[index].Parent;
		mixer.Buses.erase(mixer.Buses.begin() + index);
		for (MixerBus& bus : mixer.Buses)
		{
			if (MakeMixerBusKey(bus.Parent) == key && bus.Name != MasterBusName)
			{
				bus.Parent = parent;
			}
			if (!bus.DuckedBy.empty() && MakeMixerBusKey(bus.DuckedBy) == key)
			{
				bus.DuckedBy.clear();
			}
		}
		for (MixerSnapshot& snapshot : mixer.Snapshots)
		{
			std::erase_if(snapshot.Values,
			              [&key](MixerSnapshotValue const& value)
			              {
							  return MakeMixerBusKey(value.Bus) == key;
						  });
		}
		if (!mixer.Impacts.Bus.empty() && MakeMixerBusKey(mixer.Impacts.Bus) == key)
		{
			mixer.Impacts.Bus.clear();
		}
		return true;
	}

	void RenameMixerBus(Mixer& mixer, uint32_t index, std::string name)
	{
		if (index >= mixer.Buses.size())
		{
			return;
		}
		std::string const key = MakeMixerBusKey(mixer.Buses[index].Name);
		// An empty Parent or Bus already means Master, whose name stays Master: only names that are written are renamed.
		auto const renameReference = [&key, &name](std::string& reference)
		{
			if (!reference.empty() && MakeMixerBusKey(reference) == key)
			{
				reference = name;
			}
		};
		for (size_t other = 0; other < mixer.Buses.size(); other++)
		{
			if (other != index)
			{
				renameReference(mixer.Buses[other].Parent);
				renameReference(mixer.Buses[other].DuckedBy);
			}
		}
		for (MixerSnapshot& snapshot : mixer.Snapshots)
		{
			for (MixerSnapshotValue& value : snapshot.Values)
			{
				renameReference(value.Bus);
			}
		}
		renameReference(mixer.Impacts.Bus);
		mixer.Buses[index].Name = std::move(name);
	}

	void RemapMixerSnapshotEffects(Mixer& mixer, std::string_view bus, std::span<std::optional<uint32_t> const> previous)
	{
		std::string const key = MakeMixerBusKey(bus);
		// The new index of each effect that was there, the first place it is at (a duplicate's copy is after its original).
		auto const findPlace = [previous](uint32_t effect) -> std::optional<uint32_t>
		{
			for (size_t place = 0; place < previous.size(); place++)
			{
				if (previous[place] == effect)
				{
					return static_cast<uint32_t>(place);
				}
			}
			return std::nullopt;
		};
		for (MixerSnapshot& snapshot : mixer.Snapshots)
		{
			std::vector<MixerSnapshotValue> kept;
			kept.reserve(snapshot.Values.size());
			for (MixerSnapshotValue& value : snapshot.Values)
			{
				if (value.Target != MixerSnapshotTarget::Effect || MakeMixerBusKey(value.Bus) != key)
				{
					kept.push_back(std::move(value));
					continue;
				}
				if (std::optional<uint32_t> const moved = findPlace(value.Effect))
				{
					value.Effect = *moved;
					kept.push_back(std::move(value));
				}
			}
			snapshot.Values = std::move(kept);
		}
	}

	Result<void> ApplyMixerBusChange(Mixer& mixer, uint32_t index, FieldChange const& change)
	{
		if (index >= mixer.Buses.size())
		{
			return MakeError("the mixer has no bus {}", index);
		}
		MixerBus const& bus = mixer.Buses[index];
		Json const values = SerializeFields<StructTraits<MixerBus>>(bus);
		MixerBus edited = bus;
		if (Result<void> read =
		        DeserializeFields<StructTraits<MixerBus>>(change.MakePatch(values), edited, DeserializationContext{}, "bus");
		    !read)
		{
			return read;
		}
		std::vector<std::optional<uint32_t>> previous;
		bool const listEdit = change.ArrayEdit && change.Path.size() == 1 && change.Path[0].Field == "Effects";
		if (listEdit)
		{
			Json indices = Json::array();
			for (uint32_t effect = 0; effect < bus.Effects.size(); effect++)
			{
				indices.push_back(effect);
			}
			Json const moved = change.MakePatch(Json::object({{"Effects", std::move(indices)}}));
			if (auto const places = moved.find("Effects"); places != moved.end())
			{
				for (Json const& place : *places)
				{
					previous.push_back(place.is_number_unsigned() ? std::optional<uint32_t>(place.get<uint32_t>()) : std::nullopt);
				}
			}
		}
		std::string const name = bus.Name;
		mixer.Buses[index] = std::move(edited);
		if (listEdit)
		{
			RemapMixerSnapshotEffects(mixer, name, previous);
		}
		return {};
	}

	Result<void> ApplyMixerChange(Mixer& mixer, FieldChange const& change)
	{
		Json const values = SerializeFields<StructTraits<Mixer>>(mixer);
		return DeserializeFields<StructTraits<Mixer>>(change.MakePatch(values), mixer, DeserializationContext{}, "mixer");
	}
}
