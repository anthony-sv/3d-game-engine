#pragma once

#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/Mixer.h"
#include "Strada/Serialization/JsonSerialization.h"
#include "Strada/Serialization/StructDocument.h"
#include "Strada/Serialization/StructSerialization.h"

#include <array>
#include <string_view>
#include <tuple>
#include <utility>

// The Mixer data asset type (.smixer, Docs/Architecture.md §9.5 and §7.3): the engine's Mixer (Strada/Asset/Mixer.h) as
// field tables, and its registration's specification (DataAssetRegistry::AddEngineTypes). C++ readers of a mixer asset
// include this header; code that only mixes includes Mixer.h.
//
// The format is version 1 with every field below, those slice 3's second mixer track added (ducking, snapshots) included,
// and has no migration: no .smixer existed outside the slice before them, and a file without them reads their defaults.
// Changing a field's meaning or removing one from now on bumps the version with a migration.
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

	// The parameters' names are their MixerEffect fields' names.
	template<>
	struct EnumTraits<MixerEffectParameter>
	{
		static constexpr std::array<std::pair<MixerEffectParameter, std::string_view>, MixerEffectParameterCount> Values = {{
			{MixerEffectParameter::Frequency, "Frequency"},
			{MixerEffectParameter::Q, "Q"},
			{MixerEffectParameter::GainDb, "GainDb"},
			{MixerEffectParameter::DelaySeconds, "DelaySeconds"},
			{MixerEffectParameter::Feedback, "Feedback"},
			{MixerEffectParameter::RoomSize, "RoomSize"},
			{MixerEffectParameter::Damping, "Damping"},
			{MixerEffectParameter::Width, "Width"},
			{MixerEffectParameter::WetDb, "WetDb"},
			{MixerEffectParameter::DryDb, "DryDb"},
			{MixerEffectParameter::ThresholdDb, "ThresholdDb"},
			{MixerEffectParameter::Ratio, "Ratio"},
			{MixerEffectParameter::KneeDb, "KneeDb"},
			{MixerEffectParameter::AttackSeconds, "AttackSeconds"},
			{MixerEffectParameter::ReleaseSeconds, "ReleaseSeconds"},
			{MixerEffectParameter::MakeupDb, "MakeupDb"},
			{MixerEffectParameter::CeilingDb, "CeilingDb"},
		}};
	};

	template<>
	struct EnumTraits<MixerSnapshotTarget>
	{
		static constexpr std::array<std::pair<MixerSnapshotTarget, std::string_view>, 3> Values = {{
			{MixerSnapshotTarget::Volume, "Volume"},
			{MixerSnapshotTarget::Muted, "Muted"},
			{MixerSnapshotTarget::Effect, "Effect"},
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
				.Doc("The effects the bus's signal runs through, in order (at most 8)."),
			Field("DuckedBy", &MixerBus::DuckedBy)
				.Doc("The bus whose level lowers this one while it sounds (a voice bus ducking music); empty for none."),
			Field("DuckAmountDb", &MixerBus::DuckAmountDb)
				.Range(MinMixerVolumeDb, 0.0)
				.Doc("dB: how far the bus is lowered while the bus named by DuckedBy sounds; -80 silences it."),
			Field("DuckThresholdDb", &MixerBus::DuckThresholdDb)
				.Range(MinMixerVolumeDb, 0.0)
				.Doc("dBFS: the level (RMS) of the bus named by DuckedBy at and above which this bus is lowered."),
			Field("DuckAttackSeconds", &MixerBus::DuckAttackSeconds)
				.Range(0.001, 5.0)
				.Doc("Seconds the bus takes to go down by the whole amount, moving evenly in dB."),
			Field("DuckReleaseSeconds", &MixerBus::DuckReleaseSeconds)
				.Range(0.001, 10.0)
				.Doc("Seconds the bus takes to come back up by the whole amount once the other bus falls below the threshold."));
	};

	template<>
	struct StructTraits<MixerSnapshotValue>
	{
		static constexpr std::string_view Name = "MixerSnapshotValue";
		static constexpr auto Fields = std::make_tuple(
			Field("Bus", &MixerSnapshotValue::Bus).Doc("The bus the value is set on (letter case ignored)."),
			Field("Target", &MixerSnapshotValue::Target)
				.Doc("What the value sets: the bus's Volume (dB), its Muted (1 mutes, 0 does not) or a parameter of an Effect."),
			Field("Effect", &MixerSnapshotValue::Effect)
				.Range(0.0, MaxBusEffects - 1)
				.VisibleWhen("Target", "Effect")
				.Doc("The index of the effect in the bus's Effects, from 0."),
			Field("Parameter", &MixerSnapshotValue::Parameter)
				.VisibleWhen("Target", "Effect")
				.Doc("The effect's parameter, one its kind uses (a filter's Frequency, a reverb's WetDb)."),
			Field("Value", &MixerSnapshotValue::Value)
				.Doc("The value, within the range of what it sets: dB for Volume, 0 or 1 for Muted, the parameter's own range."));
	};

	template<>
	struct StructTraits<MixerSnapshot>
	{
		static constexpr std::string_view Name = "MixerSnapshot";
		static constexpr auto Fields = std::make_tuple(
			Field("Name", &MixerSnapshot::Name).Doc("The snapshot's name, which scripts blend to; unique, ignoring letter case."),
			Field("Values", &MixerSnapshot::Values)
				.MaxCount(MaxMixerSnapshotValues)
				.Doc("What the snapshot sets in place of the mixer's own values (at most 128); everything else keeps them."));
	};

	template<>
	struct StructTraits<Mixer>
	{
		static constexpr std::string_view Name = "Mixer";
		static constexpr auto Fields = std::make_tuple(
			Field("Buses", &Mixer::Buses)
				.MaxCount(MaxMixerBuses)
				.Doc("The buses (at most 32): Master, the root, and the buses below it, each naming its parent."),
			Field("Snapshots", &Mixer::Snapshots)
				.MaxCount(MaxMixerSnapshots)
				.Doc("Named sets of bus values the mix blends to while a game plays (a pause that muffles everything; at most 32)."));
	};

	inline constexpr std::array<DocumentMigration, 0> MixerMigrations = {};

	inline constexpr DataAssetTypeSpecification<Mixer> MixerAssetSpecification = {
		.Name = "Mixer",
		.Extension = ".smixer",
		.FormatVersion = 1,
		.Description = "The buses sounds play on, in a tree under Master, each with a volume in decibels, a mute, a chain of effects and "
					   "ducking by another bus, and the snapshots the mix blends to.",
		.Migrations = MixerMigrations,
		.Validate = &ValidateMixer,
	};
}
