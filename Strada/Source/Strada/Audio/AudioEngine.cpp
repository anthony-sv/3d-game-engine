#include "stpch.h"
#include "Strada/Audio/AudioEngine.h"

#include "Strada/Asset/AudioClipAsset.h"
#include "Strada/Asset/MixerAsset.h"
#include "Strada/Audio/AudioMixer.h"
#include "Strada/Audio/AudioQueue.h"
#include "Strada/Audio/AudioStream.h"
#include "Strada/Audio/AudioVoice.h"
#include "Strada/Audio/MiniaudioContext.h"
#include "Strada/Core/Profiler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		using SteadyClock = std::chrono::steady_clock;

		// A bus of the mixer, as the main thread last made and sent it.
		struct AudioBusState
		{
			Scope<AudioBusNode> Node;
			// The chain last sent (null for a bus without effects).
			Scope<AudioEffectChain> Chain;
			// The values last sent: the mixer's own, or the current snapshot's in their place (§9.8).
			MixerBus Values;
			// The parent's key; empty for Master.
			std::string ParentKey;
			// The ducking last sent (SetBusDucking): the node that ducks the bus and the amount, threshold, attack and release.
			// A new bus is not ducked, so the values matter only once a SetBusDucking is sent: they start as MixerBus's own, so
			// that a bus that never names a ducking bus sends none.
			AudioBusNode* DuckTrigger = nullptr;
			std::array<float, 4> DuckValues = {MixerBus{}.DuckAmountDb, MixerBus{}.DuckThresholdDb, MixerBus{}.DuckAttackSeconds,
			                                   MixerBus{}.DuckReleaseSeconds};
		};

		// The game's value of a bus (§9.5), kept by key.
		struct AudioGameBusValues
		{
			float VolumeDb = 0.0f;
			bool Muted = false;
		};

		struct AudioEngineData
		{
			AudioOutput Output = AudioOutput::Device;
			ma_context Context{};
			bool HasContext = false;
			ma_engine Engine{};
			bool HasDevice = false;
			AudioStreamFileOpener StreamFileOpener;
			// Null when the streaming thread could not be started.
			Scope<AudioStreamer> Streamer;

			// The queue and the return list between the main thread and the mixing thread, the mixer, and the main thread's
			// side of the mix. The queue's ring is 192 KiB, so it lives here on the heap.
			Scope<AudioCommandQueue> Queue;
			AudioReturnList Returns;
			Scope<AudioMixer> Mixer;
			Scope<AudioVoiceManager> Voices;
			AudioSampleCache Samples;
			float MasterVolume = 1.0f;

			// The mixer (§9.5): what was last applied (the asset's own values), its buses by key, the game's values by key, and
			// the names logged as missing since the mixer's buses last changed.
			Strada::Mixer CurrentMixer;
			std::unordered_map<std::string, AudioBusState> Buses;
			std::unordered_map<std::string, AudioGameBusValues> GameValues;
			std::unordered_set<std::string> MissingBuses;
			std::unordered_set<std::string> UnknownGameBuses;
			std::unordered_set<std::string> UnknownMeterBuses;
			// The snapshot the mix blends to or holds (§9.8), as the mixer names it; empty for the mixer's own values. The
			// snapshot names logged as unknown since the mixer last changed.
			std::string CurrentSnapshot;
			std::unordered_set<std::string> UnknownSnapshots;
			// Whether a Reverb that passes its bus through, at a rate its library refuses, has been logged (once per Init:
			// effects are made again at every chain change and at every game's end).
			bool ReverbRateLogged = false;

			// A device that stopped: the main thread holds the mixer until the device runs again.
			bool TakenOver = false;
			SteadyClock::time_point LastRestart;
			uint64_t LastBlockCount = 0;
			SteadyClock::time_point LastBlockTime;
		};

		Scope<AudioEngineData> s_Data;

		// The profiler's state of the device's audio thread, kept here rather than in thread-local storage: on Apple
		// platforms a thread's first access to thread-local storage allocates, and the callback must not. Named by Init
		// before the device starts and ended by Shutdown once it has stopped; only the device's callback uses it between.
		constinit ProfilerThreadState s_MixerThread;
		// The thread that is mixing a block right now (none outside blocks): what IsMixingOnThisThread compares with. Beside
		// the profiler's state, in static storage, so that a hook may read it at any time. Not constinit, since
		// std::thread::id's constructor is not constexpr everywhere: static storage is zeroed before any code runs, and a
		// zero id is no thread's on every platform, so a read before its initialization finds no mixing thread.
		std::atomic<std::thread::id> s_MixingThread;
		// Allocations made through the engine's allocation callbacks on the mixing thread, while it mixed.
		constinit std::atomic<uint64_t> s_MixAllocations{0};
		// The mixer the device's callback renders through: set once everything it reads exists, before the device starts,
		// and cleared once the device has stopped.
		constinit std::atomic<AudioMixer*> s_Mixer{nullptr};
		// Set by the device's stopped notification unless the engine stopped it itself (s_StoppingDevice).
		constinit std::atomic<bool> s_DeviceLost{false};
		constinit std::atomic<bool> s_StoppingDevice{false};

		void CountMixAllocation()
		{
			if (AudioEngine::IsMixingOnThisThread())
			{
				s_MixAllocations.fetch_add(1, std::memory_order_relaxed);
			}
		}

		// The engine's allocation callbacks: every object miniaudio makes for the module (the engine, its device, every sound
		// and group, which inherit them from the engine, miniaudio.h:78433) allocates through them.
		void* OnAllocate(size_t size, void* userData)
		{
			static_cast<void>(userData);
			CountMixAllocation();
			return std::malloc(size);
		}

		void* OnReallocate(void* memory, size_t size, void* userData)
		{
			static_cast<void>(userData);
			CountMixAllocation();
			return std::realloc(memory, size);
		}

		void OnFree(void* memory, void* userData)
		{
			static_cast<void>(userData);
			std::free(memory);
		}

		constexpr ma_allocation_callbacks AllocationCallbacks = {
			.pUserData = nullptr,
			.onMalloc = OnAllocate,
			.onRealloc = OnReallocate,
			.onFree = OnFree,
		};

		// The device's data callback, on miniaudio's audio thread: the mixer's render, inside the profiler's zone of the mix.
		// Neither allocates nor locks, and reaches no thread-local storage: no zone inside the render uses the thread's own
		// profiler state.
		void MixIntoDevice(ma_device* device, void* output, void const* input, ma_uint32 frameCount)
		{
			static_cast<void>(input);
			ST_PROFILE_SCOPE_ON(s_MixerThread, "Audio.Mix");
			if (AudioMixer* const mixer = s_Mixer.load(std::memory_order_acquire))
			{
				mixer->Render(static_cast<float*>(output), frameCount);
			}
			else
			{
				std::fill_n(static_cast<float*>(output), static_cast<size_t>(frameCount) * device->playback.channels, 0.0f);
			}
		}

		// On the device's thread: a stop the engine did not ask for is a lost device.
		void OnDeviceNotification(ma_device_notification const* notification)
		{
			if (notification->type == ma_device_notification_type_stopped && !s_StoppingDevice.load(std::memory_order_acquire))
			{
				s_DeviceLost.store(true, std::memory_order_release);
			}
		}

		void StopDevice(ma_device& device)
		{
			s_StoppingDevice.store(true, std::memory_order_release);
			ma_device_stop(&device);
			s_StoppingDevice.store(false, std::memory_order_release);
		}

		char const* AudioOutputToString(AudioOutput output)
		{
			switch (output)
			{
				case AudioOutput::Device:
					return "device";
				case AudioOutput::Null:
					return "null device";
				case AudioOutput::Manual:
					return "manual";
			}
			return "unknown";
		}

		std::string ToLowerAscii(std::string_view text)
		{
			std::string lower(text);
			std::transform(lower.begin(), lower.end(), lower.begin(),
			               [](char character)
			               {
							   return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
						   });
			return lower;
		}

		// The engine's half of a frame on the main thread: what came back, what ended, and the limit.
		void SyncVoices(AudioEngineData& data)
		{
			data.Voices->TakeReturns(data.Returns);
			data.Voices->Refresh();
		}

		// A bus's volume and mute as a linear gain: MinMixerVolumeDb and below, and a mute, are silence, exactly.
		float ToBusGain(float decibels, bool muted)
		{
			return muted || decibels <= MinMixerVolumeDb ? 0.0f : static_cast<float>(std::pow(10.0, static_cast<double>(decibels) / 20.0));
		}

		void SendBusCommand(AudioEngineData& data, AudioCommandType type, AudioMixerObject* object, AudioMixerObject* other,
		                    std::array<float, 4> const& values = {})
		{
			AudioCommand command;
			command.Type = type;
			command.Object = object;
			command.Other = other;
			command.Values = values;
			data.Queue->Push(command);
		}

		AudioBusNode* FindBusNode(AudioEngineData const& data, std::string const& key)
		{
			auto const found = data.Buses.find(key);
			return found != data.Buses.end() ? found->second.Node.get() : nullptr;
		}

		// The SetEffectParameter commands that take effect slot of the bus from one effect's values to another's, over seconds
		// when that is longer than the parameters' own smoothing (a snapshot's transition).
		void SendEffectChanges(AudioEngineData& data, AudioBusNode& bus, uint8_t slot, MixerEffect const& from, MixerEffect const& to,
		                       float seconds)
		{
			for (uint32_t index = 0; index < MixerEffectParameterCount; index++)
			{
				MixerEffectParameter const parameter = static_cast<MixerEffectParameter>(index);
				float const value = GetMixerEffectParameter(to, parameter);
				if (GetMixerEffectParameter(from, parameter) == value)
				{
					continue;
				}
				AudioCommand command;
				command.Type = AudioCommandType::SetEffectParameter;
				command.Object = &bus;
				command.Slot = slot;
				command.Parameter = parameter;
				command.Values[0] = value;
				command.Values[1] = seconds;
				data.Queue->Push(command);
			}
		}

		// A new effect for the engine's mix.
		Ref<AudioEffect> CreateEffect(AudioEngineData& data, MixerEffect const& effect)
		{
			uint32_t const sampleRate = AudioEngine::GetSampleRate();
			if (effect.Kind == MixerEffectKind::Reverb && !AudioEffect::IsReverbSampleRateSupported(sampleRate) && !data.ReverbRateLogged)
			{
				data.ReverbRateLogged = true;
				ST_CORE_WARN("The mixer's Reverb passes its bus through unchanged: it runs at {} to {} Hz, not {} Hz",
				             AudioEffect::MinReverbSampleRate, AudioEffect::MaxReverbSampleRate, sampleRate);
			}
			return AudioEffect::Create(effect, sampleRate, AudioEngine::GetChannelCount());
		}

		// A chain of new effects for the values (null for none).
		Scope<AudioEffectChain> MakeEffectChain(AudioEngineData& data, std::vector<MixerEffect> const& effects)
		{
			if (effects.empty())
			{
				return nullptr;
			}
			std::vector<Ref<AudioEffect>> objects;
			objects.reserve(effects.size());
			for (MixerEffect const& effect : effects)
			{
				objects.push_back(CreateEffect(data, effect));
			}
			return CreateScope<AudioEffectChain>(std::move(objects));
		}

		// Takes the bus's effects to effects (§9.5): parameter changes only, ramped, while the kinds stay the same in the
		// same order; otherwise a new chain, swapped in under a dip, that carries on each effect whose kind keeps its rank
		// among that kind's effects (the bus's first Reverb stays its first Reverb). With fresh, every effect is new (their
		// state cleared).
		void UpdateBusEffects(AudioEngineData& data, AudioBusState& state, std::vector<MixerEffect> const& effects, bool fresh,
		                      float seconds)
		{
			std::vector<MixerEffect> const& previous = state.Values.Effects;
			bool const sameKinds = !fresh && previous.size() == effects.size() &&
			                       std::equal(previous.begin(), previous.end(), effects.begin(),
			                                  [](MixerEffect const& first, MixerEffect const& second)
			                                  {
												  return first.Kind == second.Kind;
											  });
			if (sameKinds)
			{
				for (size_t slot = 0; slot < effects.size(); slot++)
				{
					SendEffectChanges(data, *state.Node, static_cast<uint8_t>(slot), previous[slot], effects[slot], seconds);
				}
				return;
			}
			std::vector<Ref<AudioEffect>> objects;
			objects.reserve(effects.size());
			// The effects carried on: their slot in the new chain and their index in the previous one.
			std::vector<std::pair<size_t, size_t>> carried;
			for (size_t slot = 0; slot < effects.size(); slot++)
			{
				MixerEffectKind const kind = effects[slot].Kind;
				auto const isKind = [kind](MixerEffect const& candidate)
				{
					return candidate.Kind == kind;
				};
				size_t const rank =
					static_cast<size_t>(std::count_if(effects.begin(), effects.begin() + static_cast<ptrdiff_t>(slot), isKind));
				size_t found = previous.size();
				for (size_t index = 0, seen = 0; index < previous.size(); index++)
				{
					if (isKind(previous[index]) && seen++ == rank)
					{
						found = index;
						break;
					}
				}
				if (!fresh && state.Chain && found < state.Chain->GetOwnedEffects().size())
				{
					objects.push_back(state.Chain->GetOwnedEffects()[found]);
					carried.emplace_back(slot, found);
				}
				else
				{
					objects.push_back(CreateEffect(data, effects[slot]));
				}
			}
			Scope<AudioEffectChain> chain = objects.empty() ? nullptr : CreateScope<AudioEffectChain>(std::move(objects));
			SendBusCommand(data, AudioCommandType::SetBusEffects, state.Node.get(), chain.get());
			// The chain it replaces comes back once swapped out; the effects carried on live on in the new one.
			data.Voices->ReturnLater(std::move(state.Chain));
			state.Chain = std::move(chain);
			for (auto const& [slot, index] : carried)
			{
				SendEffectChanges(data, *state.Node, static_cast<uint8_t>(slot), previous[index], effects[slot], seconds);
			}
		}

		// The main thread's estimate of each bus's gain in amplitude, from the gains of the buses down to Master (§9.3).
		void UpdateChainGains(AudioEngineData& data)
		{
			for (auto& entry : data.Buses)
			{
				AudioBusState& state = entry.second;
				float gain = 1.0f;
				AudioBusState const* current = &state;
				// The tree has at most MaxMixerBuses levels.
				for (uint32_t level = 0; current != nullptr && level <= MaxMixerBuses; level++)
				{
					auto const game = data.GameValues.find(MakeMixerBusKey(current->Values.Name));
					gain *= ToBusGain(current->Values.VolumeDb, current->Values.Muted);
					if (game != data.GameValues.end())
					{
						gain *= ToBusGain(game->second.VolumeDb, game->second.Muted);
					}
					auto const parent = current->ParentKey.empty() ? data.Buses.end() : data.Buses.find(current->ParentKey);
					current = parent != data.Buses.end() ? &parent->second : nullptr;
				}
				state.Node->SetChainGain(gain);
			}
		}

		// The depth of each bus below Master (Master 0), so that parents are made and moved before their children.
		std::vector<size_t> OrderParentsFirst(Mixer const& mixer)
		{
			std::unordered_map<std::string, size_t> indices;
			for (size_t index = 0; index < mixer.Buses.size(); index++)
			{
				indices.emplace(MakeMixerBusKey(mixer.Buses[index].Name), index);
			}
			std::vector<uint32_t> depths(mixer.Buses.size(), 0);
			for (size_t index = 0; index < mixer.Buses.size(); index++)
			{
				// Validated: the parents reach Master within as many steps as there are buses.
				size_t current = index;
				for (uint32_t steps = 0; mixer.Buses[current].Name != MasterBusName && steps < mixer.Buses.size(); steps++)
				{
					auto const parent = indices.find(MakeMixerBusKey(mixer.Buses[current].Parent));
					if (parent == indices.end())
					{
						break;
					}
					current = parent->second;
					depths[index]++;
				}
			}
			std::vector<size_t> order(mixer.Buses.size());
			for (size_t index = 0; index < order.size(); index++)
			{
				order[index] = index;
			}
			std::stable_sort(order.begin(), order.end(),
			                 [&depths](size_t first, size_t second)
			                 {
								 return depths[first] < depths[second];
							 });
			return order;
		}

		// The ducking of each bus, sent after every bus exists (the bus that ducks one may come later in the order buses are
		// made in) and before any bus is removed (whose RemoveBus is the last command that names it).
		void SendDucking(AudioEngineData& data, Mixer const& mixer)
		{
			for (MixerBus const& bus : mixer.Buses)
			{
				auto const found = data.Buses.find(MakeMixerBusKey(bus.Name));
				if (found == data.Buses.end())
				{
					continue;
				}
				AudioBusState& state = found->second;
				AudioBusNode* const trigger = bus.DuckedBy.empty() ? nullptr : FindBusNode(data, MakeMixerBusKey(bus.DuckedBy));
				// The bus's own values travel with a null trigger too: a duck that ends comes back up over the bus's release.
				std::array<float, 4> const values = {bus.DuckAmountDb, bus.DuckThresholdDb, bus.DuckAttackSeconds, bus.DuckReleaseSeconds};
				if (trigger == state.DuckTrigger && values == state.DuckValues)
				{
					continue;
				}
				state.DuckTrigger = trigger;
				state.DuckValues = values;
				SendBusCommand(data, AudioCommandType::SetBusDucking, state.Node.get(), trigger, values);
			}
		}

		// Sends the mixing thread what takes the mix from what it was last sent to mixer (§9.5), which ValidateMixer passed,
		// with the current snapshot's values in place of the mixer's own (§9.8): value changes over seconds when that is
		// longer than their own smoothing (a snapshot's transition), the rest as an edit. A current snapshot the mixer no
		// longer has is dropped, and the mix returns to the mixer's own values.
		void ApplyMixer(AudioEngineData& data, Mixer const& mixer, float seconds)
		{
			MixerSnapshot const* const snapshot = FindMixerSnapshot(mixer, data.CurrentSnapshot);
			if (snapshot == nullptr && !data.CurrentSnapshot.empty())
			{
				ST_CORE_WARN("The mixer has no snapshot '{}' any more: the mix returns to the mixer's own values", data.CurrentSnapshot);
			}
			data.CurrentSnapshot = snapshot != nullptr ? snapshot->Name : std::string();
			Mixer const effective = ApplyMixerSnapshot(mixer, snapshot);

			std::unordered_set<std::string> keys;
			bool structureChanged = false;
			for (size_t const index : OrderParentsFirst(effective))
			{
				MixerBus const& bus = effective.Buses[index];
				std::string key = MakeMixerBusKey(bus.Name);
				keys.insert(key);
				std::string parentKey = bus.Name == MasterBusName ? std::string() : MakeMixerBusKey(bus.Parent);
				AudioBusNode* parent = parentKey.empty() ? nullptr : FindBusNode(data, parentKey);
				if (!parentKey.empty() && parent == nullptr)
				{
					// Its parent could not be made: it feeds Master.
					parent = FindBusNode(data, MakeMixerBusKey(MasterBusName));
				}
				if (auto const found = data.Buses.find(key); found != data.Buses.end())
				{
					AudioBusState& state = found->second;
					if (state.ParentKey != parentKey)
					{
						SendBusCommand(data, AudioCommandType::SetBusParent, state.Node.get(), parent);
						state.ParentKey = std::move(parentKey);
						structureChanged = true;
					}
					if (state.Values.VolumeDb != bus.VolumeDb || state.Values.Muted != bus.Muted)
					{
						SendBusCommand(data, AudioCommandType::SetBusMixValues, state.Node.get(), nullptr,
						               {bus.VolumeDb, bus.Muted ? 1.0f : 0.0f, seconds, 0.0f});
					}
					UpdateBusEffects(data, state, bus.Effects, false, seconds);
					state.Values = bus;
					continue;
				}
				Result<Scope<AudioBusNode>> node = AudioBusNode::Create(bus.Name, AudioEngine::GetChannelCount());
				if (!node)
				{
					ST_CORE_ERROR("The mixer's bus '{}' cannot play: {}; its sounds play on {}", bus.Name, node.GetError(), MasterBusName);
					continue;
				}
				AudioBusState state;
				state.Node = node.TakeValue();
				state.Chain = MakeEffectChain(data, bus.Effects);
				state.Values = bus;
				state.ParentKey = std::move(parentKey);
				auto const game = data.GameValues.find(key);
				state.Node->SetInitialValues(bus.VolumeDb, bus.Muted,
				                             game != data.GameValues.end() ? ToBusGain(game->second.VolumeDb, game->second.Muted) : 1.0f,
				                             state.Chain.get());
				SendBusCommand(data, AudioCommandType::AddBus, state.Node.get(), parent);
				data.Buses.emplace(std::move(key), std::move(state));
				structureChanged = true;
			}
			SendDucking(data, effective);

			// Removed buses leave the table first, so that the groups are pointed at the buses that remain.
			std::vector<AudioBusState> removed;
			for (auto bus = data.Buses.begin(); bus != data.Buses.end();)
			{
				if (keys.contains(bus->first))
				{
					++bus;
					continue;
				}
				removed.push_back(std::move(bus->second));
				bus = data.Buses.erase(bus);
				structureChanged = true;
			}
			if (structureChanged)
			{
				// Each name a source asks for and the mixer lacks is logged once per mixer.
				data.MissingBuses.clear();
				data.UnknownGameBuses.clear();
				data.UnknownMeterBuses.clear();
			}
			if (mixer.Snapshots != data.CurrentMixer.Snapshots)
			{
				data.UnknownSnapshots.clear();
			}
			data.CurrentMixer = mixer;
			for (AudioSceneGroup* const group : data.Voices->GetGroups())
			{
				group->PointAt(Miniaudio::FindBus(group->GetBusKey(), group->GetBusName()));
			}
			// What still plays into a removed bus is moved to Master by the mixing thread, which then hands the bus back.
			for (AudioBusState& state : removed)
			{
				SendBusCommand(data, AudioCommandType::RemoveBus, state.Node.get(), nullptr);
				data.Voices->ReturnLater(std::move(state.Node));
				data.Voices->ReturnLater(std::move(state.Chain));
			}
			UpdateChainGains(data);
		}

		// Device and null output: the device stopped (it said so, or a playback device mixed nothing for
		// AudioMixerStallSeconds). Until it runs again, the main thread applies the queue and takes back what the mixer lets
		// go, so that nothing waits on a device that no longer calls.
		void UpdateDevice(AudioEngineData& data)
		{
			ma_device* const device = ma_engine_get_device(&data.Engine);
			if (device == nullptr)
			{
				return;
			}
			SteadyClock::time_point const now = SteadyClock::now();
			if (!data.TakenOver)
			{
				uint64_t const blocks = data.Mixer->GetBlockCount();
				if (blocks != data.LastBlockCount)
				{
					data.LastBlockCount = blocks;
					data.LastBlockTime = now;
				}
				bool const stalled =
					data.HasDevice && std::chrono::duration<float>(now - data.LastBlockTime).count() >= AudioMixerStallSeconds;
				if (!s_DeviceLost.load(std::memory_order_acquire) && !stalled)
				{
					return;
				}
				// The device's thread has left its callback once stop returns (it waits for it).
				StopDevice(*device);
				s_DeviceLost.store(false, std::memory_order_release);
				data.TakenOver = true;
				data.LastRestart = now;
				ST_CORE_WARN("The audio device stopped; sounds go on silently until it runs again");
			}
			data.Mixer->Drain();
			data.Voices->TakeReturns(data.Returns);
			if (std::chrono::duration<float>(now - data.LastRestart).count() < AudioDeviceRetrySeconds)
			{
				return;
			}
			data.LastRestart = now;
			// Nothing of the main thread touches the mixer once the device runs again.
			if (ma_device_start(device) == MA_SUCCESS)
			{
				data.TakenOver = false;
				data.LastBlockCount = data.Mixer->GetBlockCount();
				data.LastBlockTime = now;
				ST_CORE_INFO("The audio device runs again");
			}
		}
	}

	AudioOutput AudioEngine::ResolveOutput(bool headless, std::optional<std::string> const& variable)
	{
		if (headless || !variable || variable->empty())
		{
			return headless ? AudioOutput::Null : AudioOutput::Device;
		}
		std::string const value = ToLowerAscii(*variable);
		if (value == "device")
		{
			return AudioOutput::Device;
		}
		if (value == "null")
		{
			return AudioOutput::Null;
		}
		ST_CORE_WARN("Ignoring unknown {} value '{}' (expected 'device' or 'null')", AudioOutputVariable, *variable);
		return AudioOutput::Device;
	}

	void AudioEngine::Init(AudioEngineSettings const& settings)
	{
		ST_CORE_ASSERT(!s_Data, "AudioEngine is already initialized");

		// miniaudio keeps pointers into the engine and the context: they must not move once initialized.
		Scope<AudioEngineData> data = CreateScope<AudioEngineData>();
		data->Output = settings.Output;
		s_MixAllocations.store(0, std::memory_order_relaxed);
		s_DeviceLost.store(false, std::memory_order_relaxed);
		// Before the device starts, whose thread then reads it.
		Profiler::SetThreadName(s_MixerThread, "Audio Mixer");
		ma_engine_config config = ma_engine_config_init();
		config.allocationCallbacks = AllocationCallbacks;
		switch (settings.Output)
		{
			case AudioOutput::Device:
				// miniaudio tries every backend of the platform and ends with the null backend.
				config.dataCallback = MixIntoDevice;
				config.notificationCallback = OnDeviceNotification;
				config.noAutoStart = MA_TRUE;
				break;
			case AudioOutput::Null:
			{
				ma_backend const backend = ma_backend_null;
				ma_context_config contextConfig = ma_context_config_init();
				contextConfig.allocationCallbacks = AllocationCallbacks;
				if (ma_result const result = ma_context_init(&backend, 1, &contextConfig, &data->Context); result != MA_SUCCESS)
				{
					ST_CORE_ERROR("The audio engine cannot be initialized: {}", ma_result_description(result));
					return;
				}
				data->HasContext = true;
				config.pContext = &data->Context;
				config.dataCallback = MixIntoDevice;
				config.notificationCallback = OnDeviceNotification;
				config.noAutoStart = MA_TRUE;
				break;
			}
			case AudioOutput::Manual:
				if (settings.ChannelCount < static_cast<uint32_t>(MA_MIN_CHANNELS) ||
				    settings.ChannelCount > static_cast<uint32_t>(MA_MAX_CHANNELS) ||
				    settings.SampleRate < static_cast<uint32_t>(ma_standard_sample_rate_min) ||
				    settings.SampleRate > static_cast<uint32_t>(ma_standard_sample_rate_max))
				{
					ST_CORE_ERROR("The audio engine cannot be initialized: {} channels at {} Hz is not a supported format",
					              settings.ChannelCount, settings.SampleRate);
					return;
				}
				config.noDevice = MA_TRUE;
				config.channels = settings.ChannelCount;
				config.sampleRate = settings.SampleRate;
				break;
		}

		if (ma_result const result = ma_engine_init(&config, &data->Engine); result != MA_SUCCESS)
		{
			ST_CORE_ERROR("The audio engine cannot be initialized: {}", ma_result_description(result));
			if (data->HasContext)
			{
				ma_context_uninit(&data->Context);
			}
			return;
		}

		ma_device* const device = ma_engine_get_device(&data->Engine);
		char const* backend = "none";
		if (device != nullptr)
		{
			ma_backend const deviceBackend = ma_device_get_context(device)->backend;
			backend = ma_get_backend_name(deviceBackend);
			data->HasDevice = deviceBackend != ma_backend_null;
		}
		if (settings.Output == AudioOutput::Device && !data->HasDevice)
		{
			ST_CORE_WARN("No audio device is available; sounds play silently");
		}
		data->StreamFileOpener = settings.StreamFileOpener;
		if (Result<Scope<AudioStreamer>> streamer = AudioStreamer::Create(); streamer)
		{
			data->Streamer = streamer.TakeValue();
		}
		else
		{
			ST_CORE_ERROR("Sounds cannot stream from their files: {}", streamer.GetError());
		}

		data->Queue = CreateScope<AudioCommandQueue>();
		data->Mixer = CreateScope<AudioMixer>(data->Engine, *data->Queue, data->Returns);
		data->Voices = CreateScope<AudioVoiceManager>(*data->Queue);
		data->LastBlockTime = SteadyClock::now();
		s_Data = std::move(data);
		// Master alone, whose node every scene's groups play into: at 0 dB without effects it is the identity, so the
		// engine mixes as it did before buses until a mixer is set. Queued before the device starts.
		ApplyMixer(*s_Data, Mixer{}, 0.0f);
		if (device != nullptr)
		{
			s_Mixer.store(s_Data->Mixer.get(), std::memory_order_release);
			if (ma_result const result = ma_device_start(device); result != MA_SUCCESS)
			{
				// Taken for a lost device: the next Update holds the mixer and tries again.
				ST_CORE_ERROR("The audio device cannot be started: {}", ma_result_description(result));
				s_DeviceLost.store(true, std::memory_order_release);
			}
		}
		ST_CORE_INFO("Audio initialized (miniaudio {}, {} output, {} backend, {} Hz, {} channels)", MA_VERSION_STRING,
		             AudioOutputToString(settings.Output), backend, GetSampleRate(), GetChannelCount());
	}

	void AudioEngine::Shutdown()
	{
		ST_CORE_ASSERT(s_Data, "AudioEngine is not initialized");
		// No block runs once the device has stopped (ma_device_stop waits for a callback in progress), so the main thread
		// owns every object again.
		if (ma_device* const device = ma_engine_get_device(&s_Data->Engine))
		{
			StopDevice(*device);
		}
		s_Mixer.store(nullptr, std::memory_order_release);
		s_Data->Voices->TakeReturns(s_Data->Returns);
		s_Data->Voices->DestroyReturning();
		// The buses and their effects, once nothing that played into them is left; each node detaches what still feeds it.
		s_Data->Buses.clear();
		s_Data->Voices.reset();
		s_Data->Mixer.reset();
		s_Data->Queue.reset();
		// The streams are gone with the voices; the thread ends before the engine it streams for.
		s_Data->Streamer.reset();
		s_StoppingDevice.store(true, std::memory_order_release);
		ma_engine_uninit(&s_Data->Engine);
		s_StoppingDevice.store(false, std::memory_order_release);
		// The device has stopped: its thread's buffer is free again at the profiler's next collection.
		Profiler::EndThread(s_MixerThread);
		if (s_Data->HasContext)
		{
			ma_context_uninit(&s_Data->Context);
		}
		s_Data.reset();
		s_DeviceLost.store(false, std::memory_order_relaxed);
	}

	bool AudioEngine::IsInitialized()
	{
		return s_Data != nullptr;
	}

	void AudioEngine::Update()
	{
		if (s_Data == nullptr)
		{
			return;
		}
		ST_PROFILE_SCOPE("Audio.Update");
		AudioEngineData& data = *s_Data;
		SyncVoices(data);
		if (data.Voices->HasVirtualVoices())
		{
			data.Voices->Balance();
		}
		data.Samples.Prune();
		if (data.Output != AudioOutput::Manual)
		{
			UpdateDevice(data);
		}
		data.Queue->Flush();
	}

	AudioOutput AudioEngine::GetOutput()
	{
		return s_Data != nullptr ? s_Data->Output : AudioOutput::Null;
	}

	bool AudioEngine::HasDevice()
	{
		return s_Data != nullptr && s_Data->HasDevice;
	}

	uint32_t AudioEngine::GetSampleRate()
	{
		return s_Data != nullptr ? ma_engine_get_sample_rate(&s_Data->Engine) : 0;
	}

	uint32_t AudioEngine::GetChannelCount()
	{
		return s_Data != nullptr ? ma_engine_get_channels(&s_Data->Engine) : 0;
	}

	float AudioEngine::GetMasterVolume()
	{
		return s_Data != nullptr ? s_Data->MasterVolume : 0.0f;
	}

	void AudioEngine::SetMasterVolume(float volume)
	{
		if (s_Data == nullptr)
		{
			return;
		}
		// Also replaces NaN.
		volume = volume > 0.0f ? volume : 0.0f;
		if (volume == s_Data->MasterVolume)
		{
			return;
		}
		s_Data->MasterVolume = volume;
		AudioCommand command;
		command.Type = AudioCommandType::SetMasterVolume;
		command.Values[0] = volume;
		s_Data->Queue->Push(command);
	}

	void AudioEngine::SetMixer(Mixer const& mixer)
	{
		if (s_Data == nullptr)
		{
			return;
		}
		// Numbers outside their fields' ranges, which no file holds, are brought within them (a mixer made in code).
		Mixer fitted = mixer;
		for (MixerBus& bus : fitted.Buses)
		{
			FitToFieldRanges<StructTraits<MixerBus>>(bus);
			for (MixerEffect& effect : bus.Effects)
			{
				FitToFieldRanges<StructTraits<MixerEffect>>(effect);
			}
		}
		if (Result<void> valid = ValidateMixer(fitted); !valid)
		{
			ST_CORE_WARN("The mixer cannot be mixed through and is not applied: {}", valid.GetError());
			return;
		}
		if (fitted == s_Data->CurrentMixer)
		{
			return;
		}
		ApplyMixer(*s_Data, fitted, 0.0f);
	}

	Mixer const& AudioEngine::GetMixer()
	{
		static Mixer const s_Default;
		return s_Data != nullptr ? s_Data->CurrentMixer : s_Default;
	}

	bool AudioEngine::TransitionTo(std::string_view snapshot, float seconds)
	{
		if (s_Data == nullptr)
		{
			return false;
		}
		AudioEngineData& data = *s_Data;
		MixerSnapshot const* const target = FindMixerSnapshot(data.CurrentMixer, snapshot);
		if (target == nullptr && !snapshot.empty())
		{
			if (data.UnknownSnapshots.insert(MakeMixerBusKey(snapshot)).second)
			{
				ST_CORE_WARN("The mixer has no snapshot named '{}': the mix stays as it is", snapshot);
			}
			return false;
		}
		// NaN and what is not positive are an edit's ramp; at most MaxSnapshotTransitionSeconds.
		float const clamped = seconds > 0.0f ? std::min(seconds, MaxSnapshotTransitionSeconds) : 0.0f;
		data.CurrentSnapshot = target != nullptr ? target->Name : std::string();
		// The buses and effects stay: only the values that differ from what was last sent move.
		Mixer const own = data.CurrentMixer;
		ApplyMixer(data, own, clamped);
		return true;
	}

	std::string const& AudioEngine::GetCurrentSnapshot()
	{
		static std::string const s_None;
		return s_Data != nullptr ? s_Data->CurrentSnapshot : s_None;
	}

	std::vector<AudioBusMeter> AudioEngine::GetBusMeters()
	{
		std::vector<AudioBusMeter> meters;
		if (s_Data == nullptr)
		{
			return meters;
		}
		meters.reserve(s_Data->CurrentMixer.Buses.size());
		for (MixerBus const& bus : s_Data->CurrentMixer.Buses)
		{
			AudioBusMeter meter;
			meter.Name = bus.Name;
			if (AudioBusNode const* const node = FindBusNode(*s_Data, MakeMixerBusKey(bus.Name)))
			{
				meter.Peak = node->GetPeak();
				meter.Rms = node->GetRms();
			}
			meters.push_back(std::move(meter));
		}
		return meters;
	}

	namespace
	{
		// The node of a bus a meter is read from, or null when the mixer lacks it (logged once per name and mixer).
		AudioBusNode const* FindMeterBus(AudioEngineData& data, std::string_view bus)
		{
			std::string key = MakeMixerBusKey(bus);
			if (AudioBusNode const* const node = FindBusNode(data, key))
			{
				return node;
			}
			if (data.UnknownMeterBuses.insert(std::move(key)).second)
			{
				ST_CORE_WARN("The mixer has no bus named '{}': its meter reads silence", bus);
			}
			return nullptr;
		}
	}

	float AudioEngine::GetBusPeak(std::string_view bus)
	{
		AudioBusNode const* const node = s_Data != nullptr ? FindMeterBus(*s_Data, bus) : nullptr;
		return node != nullptr ? node->GetPeak() : 0.0f;
	}

	float AudioEngine::GetBusRms(std::string_view bus)
	{
		AudioBusNode const* const node = s_Data != nullptr ? FindMeterBus(*s_Data, bus) : nullptr;
		return node != nullptr ? node->GetRms() : 0.0f;
	}

	namespace
	{
		// The bus of a name scripts gave, or null when the mixer lacks it (logged once per name and mixer).
		AudioBusState* FindGameBus(AudioEngineData& data, std::string_view bus)
		{
			std::string key = MakeMixerBusKey(bus);
			if (auto const found = data.Buses.find(key); found != data.Buses.end())
			{
				return &found->second;
			}
			if (data.UnknownGameBuses.insert(std::move(key)).second)
			{
				ST_CORE_WARN("The mixer has no bus named '{}': its game volume reads 0 dB and is not set", bus);
			}
			return nullptr;
		}

		void SetGameValues(AudioEngineData& data, AudioBusState& state, AudioGameBusValues const& values)
		{
			AudioGameBusValues& kept = data.GameValues[MakeMixerBusKey(state.Values.Name)];
			if (kept.VolumeDb == values.VolumeDb && kept.Muted == values.Muted)
			{
				return;
			}
			kept = values;
			SendBusCommand(data, AudioCommandType::SetBusGameValues, state.Node.get(), nullptr,
			               {values.VolumeDb, values.Muted ? 1.0f : 0.0f, 0.0f, 0.0f});
			UpdateChainGains(data);
		}
	}

	float AudioEngine::GetBusVolume(std::string_view bus)
	{
		if (s_Data == nullptr)
		{
			return 0.0f;
		}
		AudioBusState const* const state = FindGameBus(*s_Data, bus);
		if (state == nullptr)
		{
			return 0.0f;
		}
		auto const values = s_Data->GameValues.find(MakeMixerBusKey(bus));
		return values != s_Data->GameValues.end() ? values->second.VolumeDb : 0.0f;
	}

	void AudioEngine::SetBusVolume(std::string_view bus, float decibels)
	{
		if (s_Data == nullptr || std::isnan(decibels))
		{
			return;
		}
		if (AudioBusState* const state = FindGameBus(*s_Data, bus))
		{
			AudioGameBusValues values = s_Data->GameValues[MakeMixerBusKey(bus)];
			values.VolumeDb = std::clamp(decibels, MinMixerVolumeDb, MaxMixerVolumeDb);
			SetGameValues(*s_Data, *state, values);
		}
	}

	bool AudioEngine::IsBusMuted(std::string_view bus)
	{
		if (s_Data == nullptr || FindGameBus(*s_Data, bus) == nullptr)
		{
			return false;
		}
		auto const values = s_Data->GameValues.find(MakeMixerBusKey(bus));
		return values != s_Data->GameValues.end() && values->second.Muted;
	}

	void AudioEngine::SetBusMuted(std::string_view bus, bool muted)
	{
		if (s_Data == nullptr)
		{
			return;
		}
		if (AudioBusState* const state = FindGameBus(*s_Data, bus))
		{
			AudioGameBusValues values = s_Data->GameValues[MakeMixerBusKey(bus)];
			values.Muted = muted;
			SetGameValues(*s_Data, *state, values);
		}
	}

	void AudioEngine::ResetGameValues()
	{
		if (s_Data == nullptr)
		{
			return;
		}
		SetMasterVolume(1.0f);
		for (auto& entry : s_Data->Buses)
		{
			SetGameValues(*s_Data, entry.second, AudioGameBusValues{});
		}
		s_Data->GameValues.clear();
		// No snapshot: the mix is back at the mixer's own values, over an edit's ramp.
		if (!s_Data->CurrentSnapshot.empty())
		{
			s_Data->CurrentSnapshot.clear();
			Mixer const own = s_Data->CurrentMixer;
			ApplyMixer(*s_Data, own, 0.0f);
		}
		UpdateChainGains(*s_Data);
	}

	void AudioEngine::ClearEffectState()
	{
		if (s_Data == nullptr)
		{
			return;
		}
		for (auto& entry : s_Data->Buses)
		{
			if (!entry.second.Values.Effects.empty())
			{
				UpdateBusEffects(*s_Data, entry.second, entry.second.Values.Effects, true, 0.0f);
			}
		}
	}

	uint32_t AudioEngine::GetActiveVoiceCount()
	{
		return s_Data != nullptr ? s_Data->Voices->GetActiveCount() : 0;
	}

	uint32_t AudioEngine::GetMixedVoiceCount()
	{
		return s_Data != nullptr ? s_Data->Mixer->GetMixedVoiceCount() : 0;
	}

	uint64_t AudioEngine::GetStolenVoiceCount()
	{
		return s_Data != nullptr ? s_Data->Voices->GetStolenCount() : 0;
	}

	size_t AudioEngine::GetDeferredCommandCount()
	{
		return s_Data != nullptr ? s_Data->Queue->GetDeferredCount() : 0;
	}

	Result<void> AudioEngine::PreloadClip(Ref<AudioClipAsset> const& clip)
	{
		if (s_Data == nullptr)
		{
			return Error{"the audio engine is not initialized"};
		}
		if (!clip)
		{
			return Error{"no clip to preload"};
		}
		if (Result<Ref<AudioSampleBuffer>> samples = s_Data->Samples.Get(clip); !samples)
		{
			return Error{samples.GetError()};
		}
		return {};
	}

	uint64_t AudioEngine::GetDecodedClipCount()
	{
		return s_Data != nullptr ? s_Data->Samples.GetDecodeCount() : 0;
	}

	size_t AudioEngine::GetReturningObjectCount()
	{
		return s_Data != nullptr ? s_Data->Voices->GetReturningCount() : 0;
	}

	uint64_t AudioEngine::GetBlockCount()
	{
		return s_Data != nullptr ? s_Data->Mixer->GetBlockCount() : 0;
	}

	bool AudioEngine::IsMixingOnThisThread()
	{
		return s_MixingThread.load(std::memory_order_relaxed) == std::this_thread::get_id();
	}

	uint64_t AudioEngine::GetMixAllocationCount()
	{
		return s_MixAllocations.load(std::memory_order_relaxed);
	}

	void AudioEngine::SimulateDeviceLoss()
	{
		if (s_Data == nullptr)
		{
			return;
		}
		if (ma_device* const device = ma_engine_get_device(&s_Data->Engine))
		{
			// Not through StopDevice: the stopped notification takes it for a device that went away.
			ma_device_stop(device);
		}
	}

	bool AudioEngine::IsMixerTakenOver()
	{
		return s_Data != nullptr && s_Data->TakenOver;
	}

	uint64_t AudioEngine::ReadFrames(std::span<float> samples)
	{
		ST_CORE_ASSERT(s_Data && s_Data->Output == AudioOutput::Manual, "AudioEngine::ReadFrames needs manual output");
		// Device output is read by the audio thread; a second reader would race it.
		if (s_Data == nullptr || s_Data->Output != AudioOutput::Manual)
		{
			return 0;
		}
		uint32_t const channelCount = ma_engine_get_channels(&s_Data->Engine);
		uint64_t const frameCount = samples.size() / channelCount;
		if (frameCount == 0)
		{
			return 0;
		}
		ST_PROFILE_SCOPE("Audio.Mix");
		AudioEngineData& data = *s_Data;
		for (uint64_t done = 0; done < frameCount;)
		{
			uint32_t const frames = static_cast<uint32_t>(std::min<uint64_t>(AudioBlockFrames, frameCount - done));
			// Outside the block: the deferred commands' memory is the main thread's.
			data.Queue->Flush();
			data.Mixer->Render(samples.data() + done * channelCount, frames);
			done += frames;
		}
		SyncVoices(data);
		data.Queue->Flush();
		return frameCount;
	}

	void AudioEngine::WaitForStreams()
	{
		if (s_Data != nullptr && s_Data->Streamer)
		{
			s_Data->Streamer->WaitUntilIdle();
		}
	}

	ma_engine& Miniaudio::GetEngine()
	{
		ST_CORE_ASSERT(s_Data, "AudioEngine is not initialized");
		return s_Data->Engine;
	}

	AudioStreamer* Miniaudio::GetStreamer()
	{
		ST_CORE_ASSERT(s_Data, "AudioEngine is not initialized");
		return s_Data->Streamer.get();
	}

	AudioStreamFileOpener const& Miniaudio::GetStreamFileOpener()
	{
		ST_CORE_ASSERT(s_Data, "AudioEngine is not initialized");
		return s_Data->StreamFileOpener;
	}

	AudioVoiceManager& Miniaudio::GetVoiceManager()
	{
		ST_CORE_ASSERT(s_Data && s_Data->Voices, "AudioEngine is not initialized");
		return *s_Data->Voices;
	}

	AudioSampleCache& Miniaudio::GetSampleCache()
	{
		ST_CORE_ASSERT(s_Data, "AudioEngine is not initialized");
		return s_Data->Samples;
	}

	ma_allocation_callbacks const& Miniaudio::GetAllocationCallbacks()
	{
		return AllocationCallbacks;
	}

	AudioBusNode* Miniaudio::FindBus(std::string_view busKey, std::string_view busName)
	{
		ST_CORE_ASSERT(s_Data, "AudioEngine is not initialized");
		std::string const key(busKey);
		if (AudioBusNode* const bus = FindBusNode(*s_Data, key))
		{
			return bus;
		}
		std::string const masterKey = MakeMixerBusKey(MasterBusName);
		if (key != masterKey && s_Data->MissingBuses.emplace(key).second)
		{
			ST_CORE_WARN("The mixer has no bus named '{}': its sounds play on {}", busName, MasterBusName);
		}
		return FindBusNode(*s_Data, masterKey);
	}

	Miniaudio::MixingScope::MixingScope()
	{
		s_MixingThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
	}

	Miniaudio::MixingScope::~MixingScope()
	{
		s_MixingThread.store(std::thread::id(), std::memory_order_relaxed);
	}
}
