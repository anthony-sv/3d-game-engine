#pragma once

// Internal to the Audio module (and its tests: it includes no miniaudio header).

#include "Strada/Asset/Mixer.h"
#include "Strada/Core/Base.h"

#include <cstdint>
#include <span>

// The effects of a bus (Docs/Architecture.md §9.6), each written from the published method its section names. Their DSP
// is the engine's own, run by the bus's node on the mixing thread; only the reverb's comes from a library (verblib, in
// miniaudio's extras).
namespace Strada
{
	// Frames between two updates of a moving parameter's derived values (a filter's coefficients): parameters move over
	// AudioParameterSmoothingSeconds in steps of this many frames.
	inline constexpr uint32_t AudioEffectStepFrames = 32;
	// How far a Limiter looks ahead, in seconds: the latency it adds to its bus.
	inline constexpr float AudioLimiterLookaheadSeconds = 0.002f;
	// Filter frequencies are lowered to this fraction of the sample rate when an effect runs (a bilinear filter's response
	// folds at half the rate).
	inline constexpr double MaxFilterFrequencyFraction = 0.45;

	// A biquad's coefficients, normalized so that a0 is 1: y[n] = B0 x[n] + B1 x[n-1] + B2 x[n-2] - A1 y[n-1] - A2 y[n-2].
	struct BiquadCoefficients
	{
		double B0 = 1.0;
		double B1 = 0.0;
		double B2 = 0.0;
		double A1 = 0.0;
		double A2 = 0.0;
	};

	// The coefficients of a filter kind (LowPass, HighPass, Peaking, LowShelf or HighShelf) by Robert Bristow-Johnson's
	// "Cookbook formulae for audio EQ biquad filter coefficients" (the Audio EQ Cookbook), with Q for every kind (the
	// shelves' Q form). The frequency is lowered to MaxFilterFrequencyFraction of the rate. Other kinds give the identity.
	// Any thread.
	[[nodiscard]] BiquadCoefficients ComputeBiquadCoefficients(MixerEffectKind kind, double frequency, double q, double gainDb,
	                                                           double sampleRate);
	// The filter's gain at a frequency: |H(e^jw)| with w = 2 pi frequency / sampleRate, the closed form the filters' tests
	// compare measured gains with. Any thread.
	[[nodiscard]] double EvaluateBiquadGain(BiquadCoefficients const& coefficients, double frequency, double sampleRate);
	// The compressor's static curve: the output level in dB for a steady input level in dB, before makeup (Giannoulis,
	// Massberg and Reiss, "Digital Dynamic Range Compressor Design: A Tutorial and Analysis", J. Audio Eng. Soc. 60(6),
	// 2012, eq. 4: unchanged below the knee, threshold + (input - threshold) / ratio above it, and a quadratic within a knee
	// of kneeDb round the threshold). Any thread.
	[[nodiscard]] double ComputeCompressorOutputDb(double inputDb, double thresholdDb, double ratio, double kneeDb);

	// One effect of a bus's chain, with its state (a filter's registers, a delay's line, a reverb's combs, a dynamics
	// processor's envelope). Made on the main thread with everything it will need allocated; run by the bus's node on the
	// mixing thread, where it neither allocates, locks nor logs. The same effect object may pass from one chain to the next
	// when a bus's effects change around it (§9.5), so that its state, a reverb's tail, carries on.
	class AudioEffect
	{
	public:
		// Main thread: the effect for a mix of channelCount channels at sampleRate, with the effect's values as its first
		// values (taken at once, not smoothed). A Reverb runs on the first two channels and passes any others unchanged,
		// and passes everything unchanged at a sample rate its library refuses (IsReverbSampleRateSupported), which
		// AudioEngine logs once per Init (the effect does not log it).
		[[nodiscard]] static Ref<AudioEffect> Create(MixerEffect const& effect, uint32_t sampleRate, uint32_t channelCount);

		// The sample rates a Reverb runs at, verblib's (verblib.h:422-430): 22,050 Hz up to 4 times 44,100 Hz.
		static constexpr uint32_t MinReverbSampleRate = 22050;
		static constexpr uint32_t MaxReverbSampleRate = 176400;
		static constexpr bool IsReverbSampleRateSupported(uint32_t sampleRate)
		{
			return sampleRate >= MinReverbSampleRate && sampleRate <= MaxReverbSampleRate;
		}

		virtual ~AudioEffect() = default;

		AudioEffect(AudioEffect const&) = delete;
		AudioEffect& operator=(AudioEffect const&) = delete;

		MixerEffectKind GetKind() const { return m_Kind; }

		// Mixing thread: processes interleaved frames (the channel count it was made for) in place.
		virtual void Process(std::span<float> frames) = 0;
		// Mixing thread: a new target for a parameter, reached over AudioParameterSmoothingSeconds on the parameter's scale
		// (frequencies and Q on a log2 scale, decibels in decibels, the rest linearly). Parameters the kind does not use are
		// ignored.
		virtual void SetParameter(MixerEffectParameter parameter, float value) = 0;

	protected:
		explicit AudioEffect(MixerEffectKind kind)
			: m_Kind(kind)
		{
		}

	private:
		MixerEffectKind m_Kind;
	};
}
