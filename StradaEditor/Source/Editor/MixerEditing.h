#pragma once

#include "Strada/Asset/Mixer.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// What the Mixer panel does to a mixer beyond setting a field (Docs/Editor.md, "The Mixer panel"): buses added, removed
// and renamed with everything that names them, and snapshot values that follow an edit of their bus's effect list. Plain
// functions on the mixer's data, without ImGui, so that they are tested without a panel; the panel applies their result
// as one undo step of the asset, which validation may still refuse. Any thread.
namespace Strada
{
	// "Bus", else "Bus2", "Bus3" and so on: the first name no bus of the mixer has in any letter case.
	[[nodiscard]] std::string MakeMixerBusName(Mixer const& mixer);
	// "Snapshot", else "Snapshot2" and so on: the first name no snapshot of the mixer has in any letter case.
	[[nodiscard]] std::string MakeMixerSnapshotName(Mixer const& mixer);

	// Adds a bus named MakeMixerBusName at the end of the list, feeding the bus named parent (empty: Master), at 0 dB, not
	// muted, without effects or ducking; returns its index.
	uint32_t AddMixerBus(Mixer& mixer, std::string_view parent);

	// Removes the bus at index and what only it named: the buses it fed feed its parent instead, a bus it ducked is no
	// longer ducked, the snapshots' values that set it go, and impact sounds that played on it play on none. Master (and an
	// index past the list) is left as it is; returns whether a bus was removed.
	bool RemoveMixerBus(Mixer& mixer, uint32_t index);

	// Gives the bus at index a new name, and every name of it the mixer holds (the parents of the buses it feeds, the
	// DuckedBy of the buses it ducks, its snapshot values' Bus, Impacts.Bus) the same name. Names are matched by their key
	// (MakeMixerBusKey). Whether the new name is allowed (not empty, unique, Master's kept) is ValidateMixer's to say; an
	// index past the list changes nothing.
	void RenameMixerBus(Mixer& mixer, uint32_t index, std::string name);

	// After the effect list of the bus named bus changed as a list (an effect added, removed, moved or duplicated), the
	// snapshots' values that set its effects follow their effects: previous[k] is the index, before the change, of the
	// effect now at k (none for an effect added). A value whose effect is gone goes; a duplicated effect keeps its values on
	// the original.
	void RemapMixerSnapshotEffects(Mixer& mixer, std::string_view bus, std::span<std::optional<uint32_t> const> previous);
}
