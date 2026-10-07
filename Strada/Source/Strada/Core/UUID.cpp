#include "stpch.h"
#include "Strada/Core/UUID.h"

#include <charconv>
#include <limits>
#include <random>

namespace Strada
{
	namespace
	{
		uint64_t GenerateRandomValue()
		{
			// One engine per thread: seeded once from the OS entropy source, no locking required.
			thread_local std::mt19937_64 s_Engine = []()
			{
				std::random_device device;
				std::seed_seq seed{device(), device(), device(), device(), device(), device(), device(), device()};
				return std::mt19937_64(seed);
			}();
			thread_local std::uniform_int_distribution<uint64_t> s_Distribution(1, std::numeric_limits<uint64_t>::max());
			return s_Distribution(s_Engine);
		}
	}

	UUID::UUID()
		: m_Value(GenerateRandomValue())
	{
	}

	std::string UUID::ToString() const
	{
		return std::to_string(m_Value);
	}

	std::optional<UUID> UUID::FromString(std::string_view text)
	{
		if (text.empty())
		{
			return std::nullopt;
		}

		uint64_t value = 0;
		char const* begin = text.data();
		char const* end = text.data() + text.size();
		auto const [pointer, errorCode] = std::from_chars(begin, end, value, 10);
		if (errorCode != std::errc() || pointer != end)
		{
			return std::nullopt;
		}
		return UUID(value);
	}
}
