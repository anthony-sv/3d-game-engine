#include "stpch.h"
#include "Strada/Audio/AudioQueue.h"

namespace Strada
{
	bool AudioCommandQueue::HasRoom() const
	{
		uint64_t const written = m_Written.load(std::memory_order_relaxed);
		uint64_t const read = m_Read.load(std::memory_order_acquire);
		return written - read < Capacity;
	}

	void AudioCommandQueue::Push(AudioCommand const& command)
	{
		m_Pushed++;
		if (Flush() == 0 && HasRoom())
		{
			uint64_t const written = m_Written.load(std::memory_order_relaxed);
			m_Commands[written & IndexMask] = command;
			m_Written.store(written + 1, std::memory_order_release);
			return;
		}
		m_Deferred.push_back(command);
	}

	size_t AudioCommandQueue::Flush()
	{
		if (m_Deferred.empty())
		{
			return 0;
		}
		uint64_t written = m_Written.load(std::memory_order_relaxed);
		uint64_t const read = m_Read.load(std::memory_order_acquire);
		uint64_t moved = 0;
		while (!m_Deferred.empty() && written - read < Capacity)
		{
			m_Commands[written & IndexMask] = m_Deferred.front();
			m_Deferred.pop_front();
			written++;
			moved++;
		}
		if (moved > 0)
		{
			m_Written.store(written, std::memory_order_release);
		}
		return m_Deferred.size();
	}

	void AudioReturnList::Push(AudioMixerObject& object)
	{
		AudioMixerObject* head = m_Head.load(std::memory_order_relaxed);
		do
		{
			object.m_NextReturned = head;
		} while (!m_Head.compare_exchange_weak(head, &object, std::memory_order_release, std::memory_order_relaxed));
	}
}
