#pragma once

#include "Strada/Core/Assert.h"
#include "Strada/Core/Base.h"

#include <cstring>
#include <memory>
#include <span>

namespace Strada
{
	// Owning, move-only block of bytes. Unlike std::vector it does not zero-initialize on allocation.
	class Buffer
	{
	public:
		Buffer() = default;

		explicit Buffer(uint64_t size) { Allocate(size); }

		Buffer(Buffer const&) = delete;
		Buffer& operator=(Buffer const&) = delete;

		Buffer(Buffer&& other) noexcept
			: m_Data(std::move(other.m_Data)),
			  m_Size(other.m_Size)
		{
			other.m_Size = 0;
		}

		Buffer& operator=(Buffer&& other) noexcept
		{
			if (this != &other)
			{
				m_Data = std::move(other.m_Data);
				m_Size = other.m_Size;
				other.m_Size = 0;
			}
			return *this;
		}

		[[nodiscard]] static Buffer Copy(void const* data, uint64_t size)
		{
			Buffer buffer(size);
			if (size > 0)
			{
				std::memcpy(buffer.m_Data.get(), data, static_cast<size_t>(size));
			}
			return buffer;
		}

		[[nodiscard]] Buffer Clone() const { return Copy(m_Data.get(), m_Size); }

		void Allocate(uint64_t size)
		{
			m_Data.reset();
			m_Size = 0;
			if (size > 0)
			{
				m_Data = std::make_unique_for_overwrite<uint8_t[]>(static_cast<size_t>(size));
				m_Size = size;
			}
		}

		void Release()
		{
			m_Data.reset();
			m_Size = 0;
		}

		void ZeroInitialize()
		{
			if (m_Size > 0)
			{
				std::memset(m_Data.get(), 0, static_cast<size_t>(m_Size));
			}
		}

		uint8_t* GetData() { return m_Data.get(); }
		uint8_t const* GetData() const { return m_Data.get(); }
		uint64_t GetSize() const { return m_Size; }
		bool IsEmpty() const { return m_Size == 0; }

		std::span<uint8_t> GetSpan() { return {m_Data.get(), static_cast<size_t>(m_Size)}; }
		std::span<uint8_t const> GetSpan() const { return {m_Data.get(), static_cast<size_t>(m_Size)}; }

		template<typename T>
		T* As()
		{
			return reinterpret_cast<T*>(m_Data.get());
		}

		template<typename T>
		T const* As() const
		{
			return reinterpret_cast<T const*>(m_Data.get());
		}

		explicit operator bool() const { return m_Size > 0; }

	private:
		std::unique_ptr<uint8_t[]> m_Data;
		uint64_t m_Size = 0;
	};
}
