#include "stpch.h"
#include "Strada/Audio/AudioEffects.h"

#include "Strada/Audio/AudioEngine.h"

#include <extras/nodes/ma_reverb_node/verblib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

namespace Strada
{
	namespace
	{
		// Values smaller than this in an effect's state are taken as 0: a bus processes in every block, so after its sound
		// ends its filters, delay line and envelopes would otherwise decay into denormal numbers, which are slow on every
		// CPU, for as long as the mix runs. The control registers of the floating-point unit are left alone (manual output
		// mixes on the main thread).
		constexpr float DenormalFloor = 1.0e-20f;

		float Flush(float value)
		{
			return std::abs(value) < DenormalFloor ? 0.0f : value;
		}

		// Decibels as a linear gain: MinMixerVolumeDb and below are silence, exactly.
		float DecibelsToGain(double decibels)
		{
			return decibels <= MinMixerVolumeDb ? 0.0f : static_cast<float>(std::pow(10.0, decibels / 20.0));
		}

		uint32_t SecondsToFrames(float seconds, uint32_t sampleRate)
		{
			return static_cast<uint32_t>(std::lround(static_cast<double>(seconds) * sampleRate));
		}

		// A parameter on its way to a target over AudioParameterSmoothingSeconds, moved linearly on its scale: frequencies
		// and Q on a log2 scale (a move sounds even across octaves), the rest as they are (decibels in decibels).
		class SmoothedParameter
		{
		public:
			explicit SmoothedParameter(bool logarithmic = false)
				: m_Logarithmic(logarithmic)
			{
			}

			// At once, without a ramp.
			void Reset(float value)
			{
				m_Current = ToScale(value);
				m_Target = m_Current;
				m_FramesLeft = 0;
			}

			void SetTarget(float value, uint32_t frames)
			{
				m_Target = ToScale(value);
				if (frames == 0 || m_Target == m_Current)
				{
					m_Current = m_Target;
					m_FramesLeft = 0;
					return;
				}
				m_Step = (m_Target - m_Current) / static_cast<double>(frames);
				m_FramesLeft = frames;
			}

			// Moves on by frames; returns whether the value changed.
			bool Advance(uint32_t frames)
			{
				if (m_FramesLeft == 0)
				{
					return false;
				}
				if (frames >= m_FramesLeft)
				{
					m_Current = m_Target;
					m_FramesLeft = 0;
				}
				else
				{
					m_Current += m_Step * static_cast<double>(frames);
					m_FramesLeft -= frames;
				}
				return true;
			}

			double Get() const { return m_Logarithmic ? std::exp2(m_Current) : m_Current; }
			bool IsMoving() const { return m_FramesLeft != 0; }

		private:
			double ToScale(float value) const
			{
				return m_Logarithmic ? std::log2(std::max(static_cast<double>(value), 1.0e-6)) : static_cast<double>(value);
			}

			bool m_Logarithmic = false;
			double m_Current = 0.0;
			double m_Target = 0.0;
			double m_Step = 0.0;
			uint32_t m_FramesLeft = 0;
		};

		// A gain in decibels that moves between two values across one step of frames, given per frame in amplitude (linear
		// within the step, so a moving level never steps).
		struct GainRamp
		{
			float Start = 1.0f;
			float Delta = 0.0f;

			GainRamp(float start, float end, uint32_t frames)
				: Start(start),
				  Delta(frames > 0 ? (end - start) / static_cast<float>(frames) : 0.0f)
			{
			}

			float At(uint32_t frame) const { return Start + Delta * static_cast<float>(frame); }
		};

		// Calls process(offset, frames) over the interleaved frames in steps of at most AudioEffectStepFrames.
		template<typename TProcess>
		void ForEachStep(std::span<float> samples, uint32_t channelCount, TProcess&& process)
		{
			size_t const frameCount = samples.size() / channelCount;
			for (size_t offset = 0; offset < frameCount;)
			{
				uint32_t const frames = static_cast<uint32_t>(std::min<size_t>(AudioEffectStepFrames, frameCount - offset));
				process(offset, frames);
				offset += frames;
			}
		}

		// LowPass, HighPass, Peaking, LowShelf and HighShelf: the cookbook's biquads in direct form I, the last two inputs
		// and outputs of each channel as its registers; while a parameter moves the coefficients are computed again every
		// step. Direct form I holds only the signal's history, so coefficients that change under a running signal change
		// no internal state: the transposed form II's registers mix the coefficients in, and each update leaves a step in
		// the waveform's slope (a 300 Hz sine through a 20 ms sweep: a second difference about 40 times the sine's, against
		// twice in direct form I).
		class BiquadEffect final : public AudioEffect
		{
		public:
			BiquadEffect(MixerEffect const& effect, uint32_t sampleRate, uint32_t channelCount)
				: AudioEffect(effect.Kind),
				  m_SampleRate(sampleRate),
				  m_ChannelCount(channelCount),
				  m_Frequency(true),
				  m_Q(true),
				  m_Registers(static_cast<size_t>(channelCount) * 4, 0.0f)
			{
				m_Frequency.Reset(effect.Frequency);
				m_Q.Reset(effect.Q);
				m_GainDb.Reset(effect.GainDb);
				m_Coefficients = ComputeCoefficients();
			}

			void Process(std::span<float> samples) override
			{
				ForEachStep(samples, m_ChannelCount,
				            [this, samples](size_t offset, uint32_t frames)
				            {
								bool const frequencyMoved = m_Frequency.Advance(frames);
								bool const qMoved = m_Q.Advance(frames);
								bool const gainMoved = m_GainDb.Advance(frames);
								// While a parameter moves, the coefficients of the step's end are computed and the step's
					            // frames move to them one by one, so the response glides rather than stepping each step.
								Coefficients const start = m_Coefficients;
								if (frequencyMoved || qMoved || gainMoved)
								{
									m_Coefficients = ComputeCoefficients();
								}
								Filter(samples.subspan(offset * m_ChannelCount, static_cast<size_t>(frames) * m_ChannelCount), start);
							});
				for (float& value : m_Registers)
				{
					value = Flush(value);
				}
			}

			void SetParameter(MixerEffectParameter parameter, float value, uint32_t frames) override
			{
				switch (parameter)
				{
					case MixerEffectParameter::Frequency:
						m_Frequency.SetTarget(value, frames);
						break;
					case MixerEffectParameter::Q:
						m_Q.SetTarget(value, frames);
						break;
					case MixerEffectParameter::GainDb:
						m_GainDb.SetTarget(value, frames);
						break;
					default:
						break;
				}
			}

		private:
			// B0, B1, B2, A1, A2, as BiquadCoefficients orders them, in the float the filter runs in.
			using Coefficients = std::array<float, 5>;

			Coefficients ComputeCoefficients() const
			{
				BiquadCoefficients const coefficients =
					ComputeBiquadCoefficients(GetKind(), m_Frequency.Get(), m_Q.Get(), m_GainDb.Get(), static_cast<double>(m_SampleRate));
				return {static_cast<float>(coefficients.B0), static_cast<float>(coefficients.B1), static_cast<float>(coefficients.B2),
				        static_cast<float>(coefficients.A1), static_cast<float>(coefficients.A2)};
			}

			// Filters the frames with coefficients moving from start to m_Coefficients, reached at the last frame.
			void Filter(std::span<float> samples, Coefficients const& start)
			{
				size_t const frameCount = samples.size() / m_ChannelCount;
				bool const moving = start != m_Coefficients;
				for (uint32_t channel = 0; channel < m_ChannelCount; channel++)
				{
					float* const registers = m_Registers.data() + static_cast<size_t>(channel) * 4;
					float x1 = registers[0];
					float x2 = registers[1];
					float y1 = registers[2];
					float y2 = registers[3];
					Coefficients current = m_Coefficients;
					for (size_t frame = 0; frame < frameCount; frame++)
					{
						if (moving)
						{
							float const along = static_cast<float>(frame + 1) / static_cast<float>(frameCount);
							for (size_t index = 0; index < current.size(); index++)
							{
								current[index] = start[index] + (m_Coefficients[index] - start[index]) * along;
							}
						}
						float& sample = samples[frame * m_ChannelCount + channel];
						float const input = sample;
						float const output = current[0] * input + current[1] * x1 + current[2] * x2 - current[3] * y1 - current[4] * y2;
						x2 = x1;
						x1 = input;
						y2 = y1;
						y1 = output;
						sample = output;
					}
					registers[0] = x1;
					registers[1] = x2;
					registers[2] = y1;
					registers[3] = y2;
				}
			}

			uint32_t m_SampleRate = 0;
			uint32_t m_ChannelCount = 0;
			SmoothedParameter m_Frequency;
			SmoothedParameter m_Q;
			SmoothedParameter m_GainDb;
			Coefficients m_Coefficients = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
			std::vector<float> m_Registers;
		};

		// Delay: Schroeder's feedback comb. The line holds the input plus Feedback times what it held DelaySeconds before;
		// the output is DryDb times the input plus WetDb times the line read DelaySeconds back. A new delay time crossfades
		// the read from the old position to the new one over AudioGainSmoothingSeconds.
		class DelayEffect final : public AudioEffect
		{
		public:
			DelayEffect(MixerEffect const& effect, uint32_t sampleRate, uint32_t channelCount)
				: AudioEffect(effect.Kind),
				  m_SampleRate(sampleRate),
				  m_ChannelCount(channelCount),
				  m_LineFrames(SecondsToFrames(MaxMixerDelaySeconds, sampleRate) + 2),
				  m_Line(static_cast<size_t>(m_LineFrames) * channelCount, 0.0f)
			{
				m_DelayFrames = ToDelayFrames(effect.DelaySeconds);
				m_NextDelayFrames = m_DelayFrames;
				m_Feedback.Reset(effect.Feedback);
				m_WetDb.Reset(effect.WetDb);
				m_DryDb.Reset(effect.DryDb);
			}

			void Process(std::span<float> samples) override
			{
				uint32_t const crossfadeFrames = std::max(SecondsToFrames(AudioGainSmoothingSeconds, m_SampleRate), 1u);
				ForEachStep(samples, m_ChannelCount,
				            [&](size_t offset, uint32_t frames)
				            {
								float const feedbackStart = static_cast<float>(m_Feedback.Get());
								float const wetStart = DecibelsToGain(m_WetDb.Get());
								float const dryStart = DecibelsToGain(m_DryDb.Get());
								m_Feedback.Advance(frames);
								m_WetDb.Advance(frames);
								m_DryDb.Advance(frames);
								GainRamp const feedback(feedbackStart, static_cast<float>(m_Feedback.Get()), frames);
								GainRamp const wet(wetStart, DecibelsToGain(m_WetDb.Get()), frames);
								GainRamp const dry(dryStart, DecibelsToGain(m_DryDb.Get()), frames);
								for (uint32_t frame = 0; frame < frames; frame++)
								{
									// How far the read has moved from the old delay to the new one.
									float const blend = m_CrossfadeLeft == 0 ? 1.0f
						                                                     : 1.0f - static_cast<float>(m_CrossfadeLeft) /
						                                                                  static_cast<float>(crossfadeFrames);
									size_t const oldRead = static_cast<size_t>((m_Write + m_LineFrames - m_DelayFrames) % m_LineFrames);
									size_t const newRead = static_cast<size_t>((m_Write + m_LineFrames - m_NextDelayFrames) % m_LineFrames);
									float* const frameSamples = samples.data() + (offset + frame) * m_ChannelCount;
									for (uint32_t channel = 0; channel < m_ChannelCount; channel++)
									{
										float const delayed = m_Line[oldRead * m_ChannelCount + channel] * (1.0f - blend) +
							                                  m_Line[newRead * m_ChannelCount + channel] * blend;
										float const input = frameSamples[channel];
										m_Line[static_cast<size_t>(m_Write) * m_ChannelCount + channel] =
											Flush(input + feedback.At(frame) * delayed);
										frameSamples[channel] = dry.At(frame) * input + wet.At(frame) * delayed;
									}
									m_Write = (m_Write + 1) % m_LineFrames;
									if (m_CrossfadeLeft > 0 && --m_CrossfadeLeft == 0)
									{
										m_DelayFrames = m_NextDelayFrames;
									}
								}
							});
			}

			void SetParameter(MixerEffectParameter parameter, float value, uint32_t frames) override
			{
				switch (parameter)
				{
					case MixerEffectParameter::DelaySeconds:
					{
						uint32_t const delay = ToDelayFrames(value);
						if (delay == m_NextDelayFrames)
						{
							break;
						}
						// A crossfade in progress is cut short where it is nearer: the read starts its next one from there.
						uint32_t const crossfadeFrames = std::max(SecondsToFrames(AudioGainSmoothingSeconds, m_SampleRate), 1u);
						if (m_CrossfadeLeft > 0 && m_CrossfadeLeft * 2 < crossfadeFrames)
						{
							m_DelayFrames = m_NextDelayFrames;
						}
						m_NextDelayFrames = delay;
						m_CrossfadeLeft = m_DelayFrames == delay ? 0 : crossfadeFrames;
						break;
					}
					case MixerEffectParameter::Feedback:
						m_Feedback.SetTarget(value, frames);
						break;
					case MixerEffectParameter::WetDb:
						m_WetDb.SetTarget(value, frames);
						break;
					case MixerEffectParameter::DryDb:
						m_DryDb.SetTarget(value, frames);
						break;
					default:
						break;
				}
			}

		private:
			uint32_t ToDelayFrames(float seconds) const { return std::clamp(SecondsToFrames(seconds, m_SampleRate), 1u, m_LineFrames - 1); }

			uint32_t m_SampleRate = 0;
			uint32_t m_ChannelCount = 0;
			uint32_t m_LineFrames = 0;
			std::vector<float> m_Line;
			uint32_t m_Write = 0;
			uint32_t m_DelayFrames = 1;
			uint32_t m_NextDelayFrames = 1;
			uint32_t m_CrossfadeLeft = 0;
			SmoothedParameter m_Feedback;
			SmoothedParameter m_WetDb;
			SmoothedParameter m_DryDb;
		};

		static_assert(AudioEffect::MaxReverbSampleRate == 44100 * verblib_max_sample_rate_multiplier,
		              "The highest rate a Reverb runs at is verblib's (verblib.h:422-430)");

		// Reverb: verblib (Freeverb) on the first two channels, run for its two tails alone (its own wet at full scale, dry
		// at none: verblib_set_wet scales by verblib_scalewet, verblib.h:621-624, so 1 / 3 is unity; its width at 1, so each
		// tail comes out unmixed, verblib.h:360-361), mixed with the input by WetDb and DryDb here, ramped per frame.
		// RoomSize and Damping reach verblib once a step: they set its combs' feedback and damping, which change what the
		// combs hold, not what leaves them at once. Width is verblib's mix of the two tails, a pair of gains, so it is mixed
		// here and ramped per frame as well: given to verblib once a step, it moved the output at each step by the step times
		// the tails' difference, a zipper as large as the tails differ, and they differ by platform (verblib sizes its delay
		// lines in long double, verblib.h:406-416). A mono tail has no width.
		class ReverbEffect final : public AudioEffect
		{
		public:
			ReverbEffect(MixerEffect const& effect, uint32_t sampleRate, uint32_t channelCount)
				: AudioEffect(effect.Kind),
				  m_ChannelCount(channelCount),
				  m_ReverbChannels(std::min(channelCount, 2u))
			{
				m_RoomSize.Reset(effect.RoomSize);
				m_Damping.Reset(effect.Damping);
				m_Width.Reset(effect.Width);
				m_WetDb.Reset(effect.WetDb);
				m_DryDb.Reset(effect.DryDb);
				// Logged by AudioEngine, once: effects are made again at every chain change.
				if (!IsReverbSampleRateSupported(sampleRate))
				{
					return;
				}
				// About 400 KB of combs and allpasses sized for the highest rate it takes, held inside the structure.
				m_Reverb = CreateScope<verblib>();
				if (verblib_initialize(m_Reverb.get(), sampleRate, m_ReverbChannels) == 0)
				{
					ST_CORE_WARN("The mixer's Reverb passes its bus through unchanged: verblib refuses {} channels at {} Hz",
					             m_ReverbChannels, sampleRate);
					m_Reverb.reset();
					return;
				}
				verblib_set_wet(m_Reverb.get(), 1.0f / verblib_scalewet);
				verblib_set_dry(m_Reverb.get(), 0.0f);
				verblib_set_width(m_Reverb.get(), 1.0f);
				ApplyShape();
			}

			void Process(std::span<float> samples) override
			{
				if (!m_Reverb)
				{
					return;
				}
				ForEachStep(samples, m_ChannelCount,
				            [&](size_t offset, uint32_t frames)
				            {
								bool const roomMoved = m_RoomSize.Advance(frames);
								bool const dampingMoved = m_Damping.Advance(frames);
								if (roomMoved || dampingMoved)
								{
									ApplyShape();
								}
								float const widthStart = static_cast<float>(m_Width.Get());
								float const wetStart = DecibelsToGain(m_WetDb.Get());
								float const dryStart = DecibelsToGain(m_DryDb.Get());
								m_Width.Advance(frames);
								m_WetDb.Advance(frames);
								m_DryDb.Advance(frames);
								GainRamp const width(widthStart, static_cast<float>(m_Width.Get()), frames);
								GainRamp const wet(wetStart, DecibelsToGain(m_WetDb.Get()), frames);
								GainRamp const dry(dryStart, DecibelsToGain(m_DryDb.Get()), frames);
								float* const block = samples.data() + offset * m_ChannelCount;
								for (uint32_t frame = 0; frame < frames; frame++)
								{
									for (uint32_t channel = 0; channel < m_ReverbChannels; channel++)
									{
										m_Input[frame * m_ReverbChannels + channel] = block[frame * m_ChannelCount + channel];
									}
								}
								verblib_process(m_Reverb.get(), m_Input.data(), m_Tail.data(), frames);
								for (uint32_t frame = 0; frame < frames; frame++)
								{
									float* const output = block + frame * m_ChannelCount;
									float const* const tail = m_Tail.data() + frame * m_ReverbChannels;
									if (m_ReverbChannels == 1)
									{
										output[0] = dry.At(frame) * output[0] + wet.At(frame) * tail[0];
										continue;
									}
									// verblib's own mix of its two tails at this frame's width (verblib.h:360-361 and 588-589).
									float const frameWidth = width.At(frame);
									float const direct = frameWidth / 2.0f + 0.5f;
									float const cross = (1.0f - frameWidth) / 2.0f;
									float const left = tail[0] * direct + tail[1] * cross;
									float const right = tail[1] * direct + tail[0] * cross;
									output[0] = dry.At(frame) * output[0] + wet.At(frame) * left;
									output[1] = dry.At(frame) * output[1] + wet.At(frame) * right;
								}
							});
			}

			void SetParameter(MixerEffectParameter parameter, float value, uint32_t frames) override
			{
				switch (parameter)
				{
					case MixerEffectParameter::RoomSize:
						m_RoomSize.SetTarget(value, frames);
						break;
					case MixerEffectParameter::Damping:
						m_Damping.SetTarget(value, frames);
						break;
					case MixerEffectParameter::Width:
						m_Width.SetTarget(value, frames);
						break;
					case MixerEffectParameter::WetDb:
						m_WetDb.SetTarget(value, frames);
						break;
					case MixerEffectParameter::DryDb:
						m_DryDb.SetTarget(value, frames);
						break;
					default:
						break;
				}
			}

		private:
			void ApplyShape()
			{
				verblib_set_room_size(m_Reverb.get(), static_cast<float>(m_RoomSize.Get()));
				verblib_set_damping(m_Reverb.get(), static_cast<float>(m_Damping.Get()));
			}

			uint32_t m_ChannelCount = 0;
			uint32_t m_ReverbChannels = 0;
			Scope<verblib> m_Reverb;
			SmoothedParameter m_RoomSize;
			SmoothedParameter m_Damping;
			SmoothedParameter m_Width;
			SmoothedParameter m_WetDb;
			SmoothedParameter m_DryDb;
			std::array<float, AudioEffectStepFrames * 2> m_Input = {};
			std::array<float, AudioEffectStepFrames * 2> m_Tail = {};
		};

		// One-pole smoothing coefficient of a time constant (Giannoulis, Massberg and Reiss, eq. 7): exp(-1 / (time * rate)).
		double SmoothingCoefficient(double seconds, uint32_t sampleRate)
		{
			return std::exp(-1.0 / (std::max(seconds, 1.0e-6) * static_cast<double>(sampleRate)));
		}

		// The level of a frame for the dynamics: its largest magnitude over the channels (linked), in dB.
		double FramePeakDb(float const* frame, uint32_t channelCount)
		{
			float peak = 0.0f;
			for (uint32_t channel = 0; channel < channelCount; channel++)
			{
				peak = std::max(peak, std::abs(frame[channel]));
			}
			// -200 dB stands for silence: far below any threshold, and finite.
			return peak > 1.0e-10f ? 20.0 * std::log10(static_cast<double>(peak)) : -200.0;
		}

		// Compressor: Giannoulis, Massberg and Reiss (2012), feed-forward in the log domain: the static curve of eq. 4
		// (ComputeCompressorOutputDb), the gain reduction it asks for smoothed by their smooth decoupled peak detector
		// (eq. 17: a release stage, then an attack stage), linked across channels, then MakeupDb.
		class CompressorEffect final : public AudioEffect
		{
		public:
			CompressorEffect(MixerEffect const& effect, uint32_t sampleRate, uint32_t channelCount)
				: AudioEffect(effect.Kind),
				  m_SampleRate(sampleRate),
				  m_ChannelCount(channelCount)
			{
				m_ThresholdDb.Reset(effect.ThresholdDb);
				m_Ratio.Reset(effect.Ratio);
				m_KneeDb.Reset(effect.KneeDb);
				m_AttackSeconds.Reset(effect.AttackSeconds);
				m_ReleaseSeconds.Reset(effect.ReleaseSeconds);
				m_MakeupDb.Reset(effect.MakeupDb);
			}

			void Process(std::span<float> samples) override
			{
				ForEachStep(samples, m_ChannelCount,
				            [&](size_t offset, uint32_t frames)
				            {
								// The makeup is a gain on the output, so it glides frame by frame; the other values reach the
					            // output through the detector's smoothing.
								double const makeupStart = m_MakeupDb.Get();
								m_ThresholdDb.Advance(frames);
								m_Ratio.Advance(frames);
								m_KneeDb.Advance(frames);
								m_AttackSeconds.Advance(frames);
								m_ReleaseSeconds.Advance(frames);
								m_MakeupDb.Advance(frames);
								double const threshold = m_ThresholdDb.Get();
								double const ratio = m_Ratio.Get();
								double const knee = m_KneeDb.Get();
								double const attack = SmoothingCoefficient(m_AttackSeconds.Get(), m_SampleRate);
								double const release = SmoothingCoefficient(m_ReleaseSeconds.Get(), m_SampleRate);
								double const makeupStep = (m_MakeupDb.Get() - makeupStart) / static_cast<double>(frames);
								float* const block = samples.data() + offset * m_ChannelCount;
								for (uint32_t frame = 0; frame < frames; frame++)
								{
									float* const frameSamples = block + static_cast<size_t>(frame) * m_ChannelCount;
									double const inputDb = FramePeakDb(frameSamples, m_ChannelCount);
									double const reduction = inputDb - ComputeCompressorOutputDb(inputDb, threshold, ratio, knee);
									m_Release = std::max(reduction, release * m_Release + (1.0 - release) * reduction);
									m_Envelope = attack * m_Envelope + (1.0 - attack) * m_Release;
									double const makeup = makeupStart + makeupStep * static_cast<double>(frame + 1);
									float const gain = static_cast<float>(std::pow(10.0, (makeup - m_Envelope) / 20.0));
									for (uint32_t channel = 0; channel < m_ChannelCount; channel++)
									{
										frameSamples[channel] *= gain;
									}
								}
								if (m_Release < 1.0e-12)
								{
									m_Release = 0.0;
								}
								if (m_Envelope < 1.0e-12)
								{
									m_Envelope = 0.0;
								}
							});
			}

			void SetParameter(MixerEffectParameter parameter, float value, uint32_t frames) override
			{
				switch (parameter)
				{
					case MixerEffectParameter::ThresholdDb:
						m_ThresholdDb.SetTarget(value, frames);
						break;
					case MixerEffectParameter::Ratio:
						m_Ratio.SetTarget(value, frames);
						break;
					case MixerEffectParameter::KneeDb:
						m_KneeDb.SetTarget(value, frames);
						break;
					case MixerEffectParameter::AttackSeconds:
						m_AttackSeconds.SetTarget(value, frames);
						break;
					case MixerEffectParameter::ReleaseSeconds:
						m_ReleaseSeconds.SetTarget(value, frames);
						break;
					case MixerEffectParameter::MakeupDb:
						m_MakeupDb.SetTarget(value, frames);
						break;
					default:
						break;
				}
			}

		private:
			uint32_t m_SampleRate = 0;
			uint32_t m_ChannelCount = 0;
			SmoothedParameter m_ThresholdDb;
			SmoothedParameter m_Ratio;
			SmoothedParameter m_KneeDb;
			SmoothedParameter m_AttackSeconds;
			SmoothedParameter m_ReleaseSeconds;
			SmoothedParameter m_MakeupDb;
			// The detector's two stages, in dB of gain reduction.
			double m_Release = 0.0;
			double m_Envelope = 0.0;
		};

		// Limiter: the compressor's gain computer with an infinite ratio at CeilingDb, made exact by looking ahead W frames
		// (AudioLimiterLookaheadSeconds). Each frame needs a gain of at most ceiling / its peak; the gain used is the minimum of
		// those needs over the last W + 1 frames (Lemire's streaming minimum, "Streaming Maximum-Minimum Filter Using No More
		// than Three Comparisons per Element", 2006, in a fixed ring), let back up by ReleaseSeconds and never above that
		// minimum, then averaged over the last W frames and applied to the signal delayed by W. Every term of a frame's
		// average covers that frame, so its gain is at most the one it needs and no sample leaves above the ceiling.
		class LimiterEffect final : public AudioEffect
		{
		public:
			LimiterEffect(MixerEffect const& effect, uint32_t sampleRate, uint32_t channelCount)
				: AudioEffect(effect.Kind),
				  m_SampleRate(sampleRate),
				  m_ChannelCount(channelCount),
				  m_Lookahead(std::max(SecondsToFrames(AudioLimiterLookaheadSeconds, sampleRate), 1u)),
				  m_Delay(static_cast<size_t>(m_Lookahead) * channelCount, 0.0f),
				  m_Gains(m_Lookahead, 1.0f),
				  m_GainSum(static_cast<double>(m_Lookahead)),
				  m_MinimumIndices(m_Lookahead + 1, 0),
				  m_MinimumValues(m_Lookahead + 1, 1.0f)
			{
				m_CeilingDb.Reset(effect.CeilingDb);
				m_ReleaseSeconds.Reset(effect.ReleaseSeconds);
			}

			void Process(std::span<float> samples) override
			{
				ForEachStep(samples, m_ChannelCount,
				            [&](size_t offset, uint32_t frames)
				            {
								float const ceilingStart = DecibelsToGain(m_CeilingDb.Get());
								m_CeilingDb.Advance(frames);
								m_ReleaseSeconds.Advance(frames);
								GainRamp const ceiling(ceilingStart, DecibelsToGain(m_CeilingDb.Get()), frames);
								float const recovery =
									1.0f - static_cast<float>(SmoothingCoefficient(m_ReleaseSeconds.Get(), m_SampleRate));
								float* const block = samples.data() + offset * m_ChannelCount;
								for (uint32_t frame = 0; frame < frames; frame++)
								{
									float* const frameSamples = block + static_cast<size_t>(frame) * m_ChannelCount;
									float peak = 0.0f;
									for (uint32_t channel = 0; channel < m_ChannelCount; channel++)
									{
										peak = std::max(peak, std::abs(frameSamples[channel]));
									}
									float const needed = peak > ceiling.At(frame) ? ceiling.At(frame) / peak : 1.0f;
									PushNeed(needed);
									float const minimum = m_MinimumValues[m_MinimumHead];
									m_Released = std::min(minimum, m_Released + (1.0f - m_Released) * recovery);

									// The average of the released gains over the window.
									m_GainSum += static_cast<double>(m_Released) - static_cast<double>(m_Gains[m_Position]);
									m_Gains[m_Position] = m_Released;
									float const gain = static_cast<float>(m_GainSum / static_cast<double>(m_Lookahead));

									float* const delayed = m_Delay.data() + static_cast<size_t>(m_Position) * m_ChannelCount;
									for (uint32_t channel = 0; channel < m_ChannelCount; channel++)
									{
										float const input = frameSamples[channel];
										frameSamples[channel] = delayed[channel] * gain;
										delayed[channel] = input;
									}
									m_Position = (m_Position + 1) % m_Lookahead;
									m_Frame++;
								}
								// The running sum is taken again from its terms once a step, so rounding never builds up.
								double sum = 0.0;
								for (float const value : m_Gains)
								{
									sum += static_cast<double>(value);
								}
								m_GainSum = sum;
							});
			}

			void SetParameter(MixerEffectParameter parameter, float value, uint32_t frames) override
			{
				switch (parameter)
				{
					case MixerEffectParameter::CeilingDb:
						m_CeilingDb.SetTarget(value, frames);
						break;
					case MixerEffectParameter::ReleaseSeconds:
						m_ReleaseSeconds.SetTarget(value, frames);
						break;
					default:
						break;
				}
			}

		private:
			// The sliding minimum over the needs of frames m_Frame - W to m_Frame: a deque, in a ring of W + 1 places, of
			// needs that rise from front to back.
			void PushNeed(float needed)
			{
				uint32_t const capacity = m_Lookahead + 1;
				while (m_MinimumCount > 0)
				{
					uint32_t const back = (m_MinimumHead + m_MinimumCount - 1) % capacity;
					if (m_MinimumValues[back] < needed)
					{
						break;
					}
					m_MinimumCount--;
				}
				uint32_t const slot = (m_MinimumHead + m_MinimumCount) % capacity;
				m_MinimumIndices[slot] = m_Frame;
				m_MinimumValues[slot] = needed;
				m_MinimumCount++;
				while (m_MinimumIndices[m_MinimumHead] + m_Lookahead < m_Frame)
				{
					m_MinimumHead = (m_MinimumHead + 1) % capacity;
					m_MinimumCount--;
				}
			}

			uint32_t m_SampleRate = 0;
			uint32_t m_ChannelCount = 0;
			uint32_t m_Lookahead = 1;
			SmoothedParameter m_CeilingDb;
			SmoothedParameter m_ReleaseSeconds;
			// The input of the last W frames, and the released gains of the last W frames, in rings at m_Position.
			std::vector<float> m_Delay;
			std::vector<float> m_Gains;
			double m_GainSum = 0.0;
			uint32_t m_Position = 0;
			float m_Released = 1.0f;
			uint64_t m_Frame = 0;
			std::vector<uint64_t> m_MinimumIndices;
			std::vector<float> m_MinimumValues;
			uint32_t m_MinimumHead = 0;
			uint32_t m_MinimumCount = 0;
		};
	}

	BiquadCoefficients ComputeBiquadCoefficients(MixerEffectKind kind, double frequency, double q, double gainDb, double sampleRate)
	{
		if (kind != MixerEffectKind::LowPass && kind != MixerEffectKind::HighPass && kind != MixerEffectKind::Peaking &&
		    kind != MixerEffectKind::LowShelf && kind != MixerEffectKind::HighShelf)
		{
			return {};
		}
		double const f0 = std::clamp(frequency, 1.0, MaxFilterFrequencyFraction * sampleRate);
		double const w0 = 2.0 * std::numbers::pi * f0 / sampleRate;
		double const cosine = std::cos(w0);
		double const alpha = std::sin(w0) / (2.0 * std::max(q, 1.0e-3));
		double const amplitude = std::pow(10.0, gainDb / 40.0);
		double const shelfAlpha = 2.0 * std::sqrt(amplitude) * alpha;
		double b0 = 1.0;
		double b1 = 0.0;
		double b2 = 0.0;
		double a0 = 1.0;
		double a1 = 0.0;
		double a2 = 0.0;
		switch (kind)
		{
			case MixerEffectKind::LowPass:
				b0 = (1.0 - cosine) / 2.0;
				b1 = 1.0 - cosine;
				b2 = (1.0 - cosine) / 2.0;
				a0 = 1.0 + alpha;
				a1 = -2.0 * cosine;
				a2 = 1.0 - alpha;
				break;
			case MixerEffectKind::HighPass:
				b0 = (1.0 + cosine) / 2.0;
				b1 = -(1.0 + cosine);
				b2 = (1.0 + cosine) / 2.0;
				a0 = 1.0 + alpha;
				a1 = -2.0 * cosine;
				a2 = 1.0 - alpha;
				break;
			case MixerEffectKind::Peaking:
				b0 = 1.0 + alpha * amplitude;
				b1 = -2.0 * cosine;
				b2 = 1.0 - alpha * amplitude;
				a0 = 1.0 + alpha / amplitude;
				a1 = -2.0 * cosine;
				a2 = 1.0 - alpha / amplitude;
				break;
			case MixerEffectKind::LowShelf:
				b0 = amplitude * ((amplitude + 1.0) - (amplitude - 1.0) * cosine + shelfAlpha);
				b1 = 2.0 * amplitude * ((amplitude - 1.0) - (amplitude + 1.0) * cosine);
				b2 = amplitude * ((amplitude + 1.0) - (amplitude - 1.0) * cosine - shelfAlpha);
				a0 = (amplitude + 1.0) + (amplitude - 1.0) * cosine + shelfAlpha;
				a1 = -2.0 * ((amplitude - 1.0) + (amplitude + 1.0) * cosine);
				a2 = (amplitude + 1.0) + (amplitude - 1.0) * cosine - shelfAlpha;
				break;
			case MixerEffectKind::HighShelf:
				b0 = amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cosine + shelfAlpha);
				b1 = -2.0 * amplitude * ((amplitude - 1.0) + (amplitude + 1.0) * cosine);
				b2 = amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cosine - shelfAlpha);
				a0 = (amplitude + 1.0) - (amplitude - 1.0) * cosine + shelfAlpha;
				a1 = 2.0 * ((amplitude - 1.0) - (amplitude + 1.0) * cosine);
				a2 = (amplitude + 1.0) - (amplitude - 1.0) * cosine - shelfAlpha;
				break;
			default:
				break;
		}
		return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
	}

	double EvaluateBiquadGain(BiquadCoefficients const& coefficients, double frequency, double sampleRate)
	{
		double const w = 2.0 * std::numbers::pi * frequency / sampleRate;
		std::complex<double> const z1 = std::polar(1.0, -w);
		std::complex<double> const z2 = std::polar(1.0, -2.0 * w);
		std::complex<double> const numerator = coefficients.B0 + coefficients.B1 * z1 + coefficients.B2 * z2;
		std::complex<double> const denominator = 1.0 + coefficients.A1 * z1 + coefficients.A2 * z2;
		return std::abs(numerator) / std::abs(denominator);
	}

	double ComputeCompressorOutputDb(double inputDb, double thresholdDb, double ratio, double kneeDb)
	{
		double const over = inputDb - thresholdDb;
		if (2.0 * over < -kneeDb)
		{
			return inputDb;
		}
		if (kneeDb > 0.0 && 2.0 * std::abs(over) <= kneeDb)
		{
			double const into = over + kneeDb / 2.0;
			return inputDb + (1.0 / ratio - 1.0) * into * into / (2.0 * kneeDb);
		}
		return thresholdDb + over / ratio;
	}

	Ref<AudioEffect> AudioEffect::Create(MixerEffect const& effect, uint32_t sampleRate, uint32_t channelCount)
	{
		ST_CORE_ASSERT(sampleRate > 0 && channelCount > 0, "An effect needs a sample rate and channels");
		switch (effect.Kind)
		{
			case MixerEffectKind::LowPass:
			case MixerEffectKind::HighPass:
			case MixerEffectKind::Peaking:
			case MixerEffectKind::LowShelf:
			case MixerEffectKind::HighShelf:
				return CreateRef<BiquadEffect>(effect, sampleRate, channelCount);
			case MixerEffectKind::Delay:
				return CreateRef<DelayEffect>(effect, sampleRate, channelCount);
			case MixerEffectKind::Reverb:
				return CreateRef<ReverbEffect>(effect, sampleRate, channelCount);
			case MixerEffectKind::Compressor:
				return CreateRef<CompressorEffect>(effect, sampleRate, channelCount);
			case MixerEffectKind::Limiter:
				return CreateRef<LimiterEffect>(effect, sampleRate, channelCount);
		}
		// A kind no file can name: ComputeBiquadCoefficients gives it the identity, so it passes its bus unchanged.
		return CreateRef<BiquadEffect>(effect, sampleRate, channelCount);
	}
}
