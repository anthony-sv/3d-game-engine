#include "Strada/Audio/AudioQueue.h"

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

using namespace Strada;

namespace
{
	AudioCommand MakeCommand(uint64_t serial)
	{
		AudioCommand command;
		command.Type = AudioCommandType::SetVoiceVolume;
		command.Serial = serial;
		return command;
	}

	// An object of the mixer's that remembers its number.
	class NumberedObject final : public AudioMixerObject
	{
	public:
		explicit NumberedObject(uint32_t number)
			: m_Number(number)
		{
		}

		uint32_t GetNumber() const { return m_Number; }

	private:
		uint32_t m_Number = 0;
	};
}

TEST_CASE("AudioQueue: commands come out in the order they went in, and a burst beyond the ring waits, none lost")
{
	Scope<AudioCommandQueue> const queue = CreateScope<AudioCommandQueue>();
	std::vector<uint64_t> applied;
	auto const consume = [&queue, &applied]
	{
		return queue->Consume(
			[&applied](AudioCommand const& command)
			{
				applied.push_back(command.Serial);
			});
	};

	for (uint64_t serial = 1; serial <= 3; serial++)
	{
		queue->Push(MakeCommand(serial));
	}
	CHECK(queue->GetDeferredCount() == 0);
	CHECK(consume() == 3);
	CHECK(applied == std::vector<uint64_t>{1, 2, 3});
	CHECK(consume() == 0);

	// 10,000 commands: the ring takes its capacity, the rest wait in order on the main thread's side.
	applied.clear();
	constexpr uint64_t Burst = 10000;
	for (uint64_t serial = 1; serial <= Burst; serial++)
	{
		queue->Push(MakeCommand(serial));
	}
	CHECK(queue->GetDeferredCount() == Burst - AudioCommandQueue::Capacity);
	CHECK(queue->GetPushedCount() == Burst + 3);
	// Each block applies what the ring holds; each flush before it moves the next of the waiting ones in.
	uint32_t blocks = 0;
	while (applied.size() < Burst && blocks < 10)
	{
		consume();
		queue->Flush();
		blocks++;
	}
	CHECK(blocks == 3);
	CHECK(queue->GetDeferredCount() == 0);
	REQUIRE(applied.size() == Burst);
	bool inOrder = true;
	for (uint64_t index = 0; index < Burst; index++)
	{
		inOrder = inOrder && applied[index] == index + 1;
	}
	CHECK(inOrder);

	// A command pushed while others wait goes after them, even when the ring has room again.
	applied.clear();
	for (uint64_t serial = 1; serial <= AudioCommandQueue::Capacity + 1; serial++)
	{
		queue->Push(MakeCommand(serial));
	}
	consume();
	queue->Push(MakeCommand(AudioCommandQueue::Capacity + 2));
	CHECK(queue->GetDeferredCount() == 0);
	consume();
	CHECK(applied.size() == AudioCommandQueue::Capacity + 2);
	CHECK(applied.back() == AudioCommandQueue::Capacity + 2);
	CHECK(applied[AudioCommandQueue::Capacity] == AudioCommandQueue::Capacity + 1);
}

TEST_CASE("AudioQueue: the return list gives back every object, oldest first")
{
	AudioReturnList returns;
	std::vector<Scope<NumberedObject>> objects;
	for (uint32_t number = 0; number < 5; number++)
	{
		objects.push_back(CreateScope<NumberedObject>(number));
	}
	std::vector<uint32_t> taken;
	auto const take = [&returns, &taken]
	{
		returns.TakeAll(
			[&taken](AudioMixerObject& object)
			{
				taken.push_back(static_cast<NumberedObject&>(object).GetNumber());
			});
	};

	take();
	CHECK(taken.empty());
	returns.Push(*objects[2]);
	returns.Push(*objects[0]);
	returns.Push(*objects[4]);
	take();
	CHECK(taken == std::vector<uint32_t>{2, 0, 4});
	// What was taken can come back again.
	taken.clear();
	returns.Push(*objects[0]);
	returns.Push(*objects[1]);
	take();
	CHECK(taken == std::vector<uint32_t>{0, 1});
}

TEST_CASE("AudioQueue: commands and returned objects cross between two threads without a lock")
{
	// The main thread pushes commands and takes returns while another thread consumes and returns, as a device's thread
	// does: every command arrives once and in order, every object comes back once.
	Scope<AudioCommandQueue> const queue = CreateScope<AudioCommandQueue>();
	AudioReturnList returns;
	constexpr uint32_t ObjectCount = 2000;
	std::vector<Scope<NumberedObject>> objects;
	for (uint32_t number = 0; number < ObjectCount; number++)
	{
		objects.push_back(CreateScope<NumberedObject>(number));
	}

	std::atomic<bool> finished{false};
	std::atomic<bool> inOrder{true};
	std::thread mixer(
		[&queue, &returns, &finished, &inOrder]
		{
			uint64_t expected = 1;
			while (!finished.load(std::memory_order_acquire) || expected <= ObjectCount)
			{
				queue->Consume(
					[&returns, &inOrder, &expected](AudioCommand const& command)
					{
						if (command.Serial != expected)
						{
							inOrder = false;
						}
						expected++;
						returns.Push(*command.Object);
					});
				std::this_thread::yield();
			}
		});

	std::vector<bool> seen(ObjectCount, false);
	uint32_t taken = 0;
	uint32_t duplicates = 0;
	auto const take = [&returns, &seen, &taken, &duplicates]
	{
		returns.TakeAll(
			[&seen, &taken, &duplicates](AudioMixerObject& object)
			{
				uint32_t const number = static_cast<NumberedObject&>(object).GetNumber();
				duplicates += seen[number] ? 1 : 0;
				seen[number] = true;
				taken++;
			});
	};
	for (uint32_t number = 0; number < ObjectCount; number++)
	{
		AudioCommand pushed = MakeCommand(number + 1);
		pushed.Object = objects[number].get();
		queue->Push(pushed);
		if (number % 64 == 0)
		{
			take();
		}
	}
	while (queue->Flush() > 0)
	{
		std::this_thread::yield();
	}
	finished.store(true, std::memory_order_release);
	mixer.join();
	take();
	CHECK(inOrder);
	CHECK(taken == ObjectCount);
	CHECK(duplicates == 0);
}
