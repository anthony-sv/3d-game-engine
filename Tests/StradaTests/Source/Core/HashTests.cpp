#include "Strada/Core/Hash.h"

#include <doctest/doctest.h>

using namespace Strada;

TEST_CASE("Hash: FNV-1a matches reference values")
{
	// Reference values for 64-bit FNV-1a.
	CHECK(Hash::FNV1a("") == 14695981039346656037ull);
	CHECK(Hash::FNV1a("a") == 0xaf63dc4c8601ec8cull);
	CHECK(Hash::FNV1a("foobar") == 0x85944171f73967e8ull);
}

TEST_CASE("Hash: FNV-1a is usable at compile time")
{
	constexpr uint64_t Value = Hash::FNV1a("Transform");
	static_assert(Value == Hash::FNV1a("Transform"));
	CHECK(Value != Hash::FNV1a("transform"));
}

TEST_CASE("Hash: combine is order dependent")
{
	uint64_t const ab = Hash::Combine(Hash::Combine(0, 1), 2);
	uint64_t const ba = Hash::Combine(Hash::Combine(0, 2), 1);
	CHECK(ab != ba);
}
