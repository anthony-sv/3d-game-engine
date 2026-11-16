#pragma once

#include "Strada/Asset/Mixer.h"
#include "Strada/Serialization/JsonSerialization.h"
#include "Strada/Serialization/StructSerialization.h"

#include <array>
#include <string_view>
#include <tuple>
#include <utility>

// The Mixer data asset type's format (.smixer, Docs/Architecture.md §9.5 and §7.3): the engine's Mixer
// (Strada/Asset/Mixer.h) as field tables. The registration (DataAssetRegistry::AddEngineTypes, with ValidateMixer and
// the migrations) joins them with the mixer's implementation; until then nothing reads or writes the format.
namespace Strada
{
	template<>
	struct EnumTraits<MixerEffectKind>
	{
		static constexpr std::array<std::pair<MixerEffectKind, std::string_view>, 9> Values = {{
			{MixerEffectKind::LowPass, "LowPass"},
			{MixerEffectKind::HighPass, "HighPass"},
			{MixerEffectKind::Peaking, "Peaking"},
			{MixerEffectKind::LowShelf, "LowShelf"},
			{MixerEffectKind::HighShelf, "HighShelf"},
			{MixerEffectKind::Delay, "Delay"},
			{MixerEffectKind::Reverb, "Reverb"},
			{MixerEffectKind::Compressor, "Compressor"},
			{MixerEffectKind::Limiter, "Limiter"},
		}};
	};

	template<>
	struct StructTraits<MixerEffect>
	{
		static constexpr std::string_view Name = "MixerEffect";
		static constexpr auto Fields = std::make_tuple(
			Field("Kind", &MixerEffect::Kind)
				.Doc("What the effect does: LowPass, HighPass, Peaking, LowShelf or HighShelf (filters), Delay, Reverb, Compressor or "
		             "Limiter."),
			Field("Frequency", &MixerEffect::Frequency)
				.Range(10.0, 24000.0)
				.VisibleWhen("Kind", "LowPass", "HighPass", "Peaking", "LowShelf", "HighShelf")
				.Doc("Hz: the cutoff (LowPass, HighPass), the centre (Peaking) or the corner (the shelves)."),
			Field("Q", &MixerEffect::Q)
				.Range(0.1, 20.0)
				.VisibleWhen("Kind", "LowPass", "HighPass", "Peaking", "LowShelf", "HighShelf")
				.Doc("The resonance of a cutoff, the narrowness of a peak, or the steepness of a shelf; 0.7071 is the flattest."),
			Field("GainDb", &MixerEffect::GainDb)
				.Range(-24.0, 24.0)
				.VisibleWhen("Kind", "Peaking", "LowShelf", "HighShelf")
				.Doc("dB: the boost (positive) or cut (negative) at the peak or the shelf."),
			Field("DelaySeconds", &MixerEffect::DelaySeconds)
				.Range(0.001, MaxMixerDelaySeconds)
				.VisibleWhen("Kind", "Delay")
				.Doc("Seconds between echoes."),
			Field("Feedback", &MixerEffect::Feedback)
				.Range(0.0, 0.95)
				.VisibleWhen("Kind", "Delay")
				.Doc("Each echo's level relative to the one before: 0 echoes once."),
			Field("RoomSize", &MixerEffect::RoomSize)
				.Range(0.0, 1.0)
				.VisibleWhen("Kind", "Reverb")
				.Doc("How long the tail lasts: 0 is a small room, 1 a hall."),
			Field("Damping", &MixerEffect::Damping)
				.Range(0.0, 1.0)
				.VisibleWhen("Kind", "Reverb")
				.Doc("How fast the tail loses its high frequencies: 0 keeps them, 1 dulls it."),
			Field("Width", &MixerEffect::Width)
				.Range(0.0, 1.0)
				.VisibleWhen("Kind", "Reverb")
				.Doc("How far the tail spreads between left and right: 0 is mono."),
			Field("WetDb", &MixerEffect::WetDb)
				.Range(MinMixerVolumeDb, 0.0)
				.VisibleWhen("Kind", "Delay", "Reverb")
				.Doc("dB: the level of the echoes or the tail; -80 is none."),
			Field("DryDb", &MixerEffect::DryDb)
				.Range(MinMixerVolumeDb, 0.0)
				.VisibleWhen("Kind", "Delay", "Reverb")
				.Doc("dB: the level of the signal passed through; -80 is none (a bus that carries only the effect)."),
			Field("ThresholdDb", &MixerEffect::ThresholdDb)
				.Range(-60.0, 0.0)
				.VisibleWhen("Kind", "Compressor")
				.Doc("dBFS: the level above which the compressor lowers the signal."),
			Field("Ratio", &MixerEffect::Ratio)
				.Range(1.0, 20.0)
				.VisibleWhen("Kind", "Compressor")
				.Doc("Above the threshold, the input rises this many dB for each dB the output rises."),
			Field("KneeDb", &MixerEffect::KneeDb)
				.Range(0.0, 24.0)
				.VisibleWhen("Kind", "Compressor")
				.Doc("dB: the width of the soft knee round the threshold; 0 is a hard knee."),
			Field("AttackSeconds", &MixerEffect::AttackSeconds)
				.Range(0.0001, 1.0)
				.VisibleWhen("Kind", "Compressor")
				.Doc("Seconds the compressor takes to follow a rise in level."),
			Field("ReleaseSeconds", &MixerEffect::ReleaseSeconds)
				.Range(0.001, 5.0)
				.VisibleWhen("Kind", "Compressor", "Limiter")
				.Doc("Seconds the compressor or the limiter takes to let the level back up."),
			Field("MakeupDb", &MixerEffect::MakeupDb).Range(0.0, 24.0).VisibleWhen("Kind", "Compressor").Doc("dB added after compressing."),
			Field("CeilingDb", &MixerEffect::CeilingDb)
				.Range(-24.0, 0.0)
				.VisibleWhen("Kind", "Limiter")
				.Doc("dBFS: no sample leaves the limiter above it."));
	};

	template<>
	struct StructTraits<MixerBus>
	{
		static constexpr std::string_view Name = "MixerBus";
		static constexpr auto Fields = std::make_tuple(
			Field("Name", &MixerBus::Name).Doc("The bus's name, which sources and scripts name it by; unique, ignoring letter case."),
			Field("Parent", &MixerBus::Parent).Doc("The bus this one feeds; empty for Master. Master itself has none."),
			Field("VolumeDb", &MixerBus::VolumeDb)
				.Range(MinMixerVolumeDb, MaxMixerVolumeDb)
				.Doc("dB applied after the bus's effects; -80 is silence."),
			Field("Muted", &MixerBus::Muted).Doc("Silences the bus and everything that feeds it."),
			Field("Effects", &MixerBus::Effects)
				.MaxCount(MaxBusEffects)
				.Doc("The effects the bus's signal runs through, in order (at most 8)."));
	};

	template<>
	struct StructTraits<Mixer>
	{
		static constexpr std::string_view Name = "Mixer";
		static constexpr auto Fields =
			std::make_tuple(Field("Buses", &Mixer::Buses)
		                        .MaxCount(MaxMixerBuses)
		                        .Doc("The buses (at most 32): Master, the root, and the buses below it, each naming its parent."));
	};

	// The data asset registry takes only types with an editor and a schema for every value: no custom value and no array
	// directly of arrays (a bus's effects are an array inside each element of the buses, which is allowed).
	static_assert(Detail::IsDescribedFieldType<Mixer>(), "The Mixer must be a data asset type (Docs/Architecture.md §6.4)");
}
