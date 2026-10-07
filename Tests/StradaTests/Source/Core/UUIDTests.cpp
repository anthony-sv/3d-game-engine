#include "Strada/Core/UUID.h"

#include <doctest/doctest.h>
#include <spdlog/fmt/fmt.h>

#include <thread>
#include <unordered_set>
#include <vector>

using namespace Strada;

TEST_CASE("UUID: generated identifiers are valid and unique")
{
	std::unordered_set<uint64_t> seen;
	for (int i = 0; i < 10000; i++)
	{
		UUID const id;
		CHECK(id.IsValid());
		CHECK(seen.insert(id.GetValue()).second);
	}
}

TEST_CASE("UUID: generation is thread-safe and unique across threads")
{
	constexpr int ThreadCount = 8;
	constexpr int IdsPerThread = 2000;
	std::vector<std::vector<uint64_t>> results(ThreadCount);
	std::vector<std::thread> threads;
	for (int t = 0; t < ThreadCount; t++)
	{
		threads.emplace_back(
			[&results, t]()
			{
				results[static_cast<size_t>(t)].reserve(IdsPerThread);
				for (int i = 0; i < IdsPerThread; i++)
				{
					results[static_cast<size_t>(t)].push_back(UUID().GetValue());
				}
			});
	}
	for (std::thread& thread : threads)
	{
		thread.join();
	}

	std::unordered_set<uint64_t> seen;
	for (std::vector<uint64_t> const& ids : results)
	{
		for (uint64_t const id : ids)
		{
			CHECK(id != 0);
			CHECK(seen.insert(id).second);
		}
	}
}

TEST_CASE("UUID: invalid identifier is zero")
{
	CHECK_FALSE(UUID::Invalid().IsValid());
	CHECK(UUID::Invalid().GetValue() == 0);
	CHECK_FALSE(UUID(0).IsValid());
}

TEST_CASE("UUID: string round trip preserves the full 64-bit value")
{
	UUID const maximum(18446744073709551615ull);
	CHECK(maximum.ToString() == "18446744073709551615");
	std::optional<UUID> const parsed = UUID::FromString("18446744073709551615");
	REQUIRE(parsed.has_value());
	CHECK(*parsed == maximum);

	UUID const random;
	std::optional<UUID> const roundTrip = UUID::FromString(random.ToString());
	REQUIRE(roundTrip.has_value());
	CHECK(*roundTrip == random);
}

TEST_CASE("UUID: malformed strings are rejected")
{
	CHECK_FALSE(UUID::FromString("").has_value());
	CHECK_FALSE(UUID::FromString("abc").has_value());
	CHECK_FALSE(UUID::FromString("123abc").has_value());
	CHECK_FALSE(UUID::FromString("-5").has_value());
	CHECK_FALSE(UUID::FromString(" 12").has_value());
	CHECK_FALSE(UUID::FromString("18446744073709551616").has_value());
}

TEST_CASE("UUID: formats as its decimal value")
{
	CHECK(fmt::format("{}", UUID(42)) == "42");
}

TEST_CASE("UUID: hashable for unordered containers")
{
	std::unordered_set<UUID> ids;
	ids.insert(UUID(7));
	ids.insert(UUID(7));
	ids.insert(UUID(8));
	CHECK(ids.size() == 2);
}
