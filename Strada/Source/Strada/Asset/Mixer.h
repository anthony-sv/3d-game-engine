#pragma once

#include "Strada/Core/Result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The mixer (Docs/Architecture.md §9.5 and §9.6): the buses sounds play on, in a tree under Master, each with a volume, a
// mute flag and a chain of effects. The data of a .smixer asset (Strada/Asset/MixerAsset.h); the project names its mixer
// (ProjectSettings::Mixer) and the audio engine mixes through it (AudioEngine::SetMixer). Plain data, any thread.
namespace Strada
{
	// The root bus: every mixer has one bus of this name, with no parent. Sources whose bus is empty, or names a bus the
	// mixer lacks, play on it.
	inline constexpr std::string_view MasterBusName = "Master";
	// Buses in a mixer and effects on a bus, at most (validation refuses more).
	inline constexpr uint32_t MaxMixerBuses = 32;
	inline constexpr uint32_t MaxBusEffects = 8;
	// The longest bus name, in bytes of UTF-8.
	inline constexpr uint32_t MaxMixerBusNameBytes = 64;
	// Volumes in decibels run from MinMixerVolumeDb, which is silence, to MaxMixerVolumeDb.
	inline constexpr float MinMixerVolumeDb = -80.0f;
	inline constexpr float MaxMixerVolumeDb = 24.0f;
	// The longest echo a Delay effect makes; its line is allocated for it at the engine's sample rate.
	inline constexpr float MaxMixerDelaySeconds = 2.0f;

	// What an effect does to its bus's signal (§9.6 names the published method of each). The order is the format's: files
	// store the names.
	enum class MixerEffectKind : uint8_t
	{
		// Biquad filters (the Audio EQ Cookbook): Frequency is the cutoff, Q the resonance.
		LowPass = 0,
		HighPass,
		// Biquad filters with a gain: Frequency is the centre (Peaking) or the corner (the shelves), Q the bandwidth or
		// the slope, GainDb the boost or cut.
		Peaking,
		LowShelf,
		HighShelf,
		// A feedback delay line: echoes DelaySeconds apart, each Feedback times the one before, mixed by WetDb and DryDb.
		Delay,
		// A Freeverb reverb (verblib): RoomSize, Damping and Width shape the tail, WetDb and DryDb mix it.
		Reverb,
		// A feed-forward compressor: above ThresholdDb the level rises 1 / Ratio as fast, over a soft knee of KneeDb,
		// following the signal with AttackSeconds and ReleaseSeconds; MakeupDb is added after.
		Compressor,
		// A lookahead peak limiter: no sample leaves above CeilingDb; ReleaseSeconds sets how fast the gain recovers.
		Limiter
	};

	// Every parameter an effect may have, so that a command or a snapshot can name one (§9.6). A kind reads only its own.
	enum class MixerEffectParameter : uint8_t
	{
		Frequency = 0,
		Q,
		GainDb,
		DelaySeconds,
		Feedback,
		RoomSize,
		Damping,
		Width,
		WetDb,
		DryDb,
		ThresholdDb,
		Ratio,
		KneeDb,
		AttackSeconds,
		ReleaseSeconds,
		MakeupDb,
		CeilingDb
	};
	inline constexpr uint32_t MixerEffectParameterCount = static_cast<uint32_t>(MixerEffectParameter::CeilingDb) + 1;

	// One effect of a bus's chain: the kind first, then the parameters of every kind (the editor shows those the kind uses).
	// Values outside a parameter's range are refused when read; frequencies above 0.45 times the engine's sample rate are
	// lowered to it when the effect runs.
	struct MixerEffect
	{
		MixerEffectKind Kind = MixerEffectKind::LowPass;
		// Hz, 10 to 24,000 (filters).
		float Frequency = 1000.0f;
		// 0.1 to 20 (filters); 0.7071 is the flattest response.
		float Q = 0.7071f;
		// -24 to 24 dB (Peaking and the shelves).
		float GainDb = 0.0f;
		// 0.001 to MaxMixerDelaySeconds (Delay).
		float DelaySeconds = 0.25f;
		// 0 to 0.95: each echo's level relative to the one before (Delay).
		float Feedback = 0.3f;
		// 0 to 1 (Reverb).
		float RoomSize = 0.5f;
		float Damping = 0.5f;
		float Width = 1.0f;
		// MinMixerVolumeDb to 0 dB: the effect's own signal and the signal passed through (Delay and Reverb).
		float WetDb = -6.0f;
		float DryDb = 0.0f;
		// -60 to 0 dB (Compressor).
		float ThresholdDb = -12.0f;
		// 1 to 20 (Compressor).
		float Ratio = 4.0f;
		// 0 to 24 dB (Compressor).
		float KneeDb = 6.0f;
		// 0.0001 to 1 s and 0.001 to 5 s (Compressor; Limiter's release).
		float AttackSeconds = 0.005f;
		float ReleaseSeconds = 0.1f;
		// 0 to 24 dB (Compressor).
		float MakeupDb = 0.0f;
		// -24 to 0 dBFS (Limiter).
		float CeilingDb = -1.0f;

		bool operator==(MixerEffect const&) const = default;
	};

	// A bus: the voices that play on it, and the buses below it, are summed, run through its effects in list order, and
	// scaled by its volume before they reach its parent.
	struct MixerBus
	{
		// Unique in its mixer, ignoring letter case; not empty, at most MaxMixerBusNameBytes.
		std::string Name;
		// The bus this one feeds: empty for Master (the root, and the parent of a bus that names none).
		std::string Parent;
		// MinMixerVolumeDb (silence) to MaxMixerVolumeDb.
		float VolumeDb = 0.0f;
		bool Muted = false;
		// At most MaxBusEffects, run in list order.
		std::vector<MixerEffect> Effects;

		bool operator==(MixerBus const&) const = default;
	};

	struct Mixer
	{
		// At most MaxMixerBuses. Exactly one is named Master, with no parent; every other bus's parent is a bus of the list,
		// and following parents from any bus reaches Master. A new mixer has Master alone, which mixes exactly as an engine
		// without a mixer does.
		std::vector<MixerBus> Buses = {MixerBus{std::string(MasterBusName), std::string(), 0.0f, false, std::vector<MixerEffect>()}};

		bool operator==(Mixer const&) const = default;
	};

	// Whether the mixer can be mixed through: the rules on Mixer::Buses and MixerBus (names, parents, no cycle, the counts).
	// Master is the bus named exactly MasterBusName; parents match names ignoring letter case. The error names the first bus
	// at fault. Ranges are the field table's to check. Any thread.
	[[nodiscard]] Result<void> ValidateMixer(Mixer const& mixer);

	// The value of one of the effect's parameters (an effect holds every kind's; its kind reads only its own). Any thread.
	[[nodiscard]] float GetMixerEffectParameter(MixerEffect const& effect, MixerEffectParameter parameter);

	// The key a bus name is matched by: its ASCII letters in lower case, the empty name being Master's ("master"). Any
	// thread.
	[[nodiscard]] std::string MakeMixerBusKey(std::string_view name);
}
