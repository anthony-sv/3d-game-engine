#include "Strada/Core/Buffer.h"

#include <doctest/doctest.h>

#include <cstring>
#include <utility>

using namespace Strada;

TEST_CASE("Buffer: default buffer is empty")
{
	Buffer const buffer;
	CHECK(buffer.IsEmpty());
	CHECK(buffer.GetSize() == 0);
	CHECK(buffer.GetData() == nullptr);
	CHECK_FALSE(static_cast<bool>(buffer));
}

TEST_CASE("Buffer: allocation and zero initialization")
{
	Buffer buffer(64);
	REQUIRE(buffer.GetSize() == 64);
	REQUIRE(buffer.GetData() != nullptr);
	buffer.ZeroInitialize();
	for (uint8_t const byte : buffer.GetSpan())
	{
		CHECK(byte == 0);
	}
}

TEST_CASE("Buffer: copy and clone are deep")
{
	uint8_t const source[] = {1, 2, 3, 4, 5};
	Buffer original = Buffer::Copy(source, sizeof(source));
	REQUIRE(original.GetSize() == sizeof(source));
	CHECK(std::memcmp(original.GetData(), source, sizeof(source)) == 0);

	Buffer clone = original.Clone();
	clone.GetData()[0] = 42;
	CHECK(original.GetData()[0] == 1);
	CHECK(clone.GetData()[0] == 42);
}

TEST_CASE("Buffer: moving transfers ownership")
{
	Buffer first(16);
	uint8_t* data = first.GetData();

	Buffer second(std::move(first));
	CHECK(second.GetData() == data);
	CHECK(second.GetSize() == 16);
	CHECK(first.GetSize() == 0);

	Buffer third;
	third = std::move(second);
	CHECK(third.GetData() == data);
	CHECK(second.GetSize() == 0);
}

TEST_CASE("Buffer: typed access")
{
	Buffer buffer(sizeof(uint32_t) * 4);
	uint32_t* values = buffer.As<uint32_t>();
	for (uint32_t i = 0; i < 4; i++)
	{
		values[i] = i * 10;
	}
	CHECK(buffer.As<uint32_t>()[3] == 30);
}

TEST_CASE("Buffer: release frees the memory")
{
	Buffer buffer(8);
	buffer.Release();
	CHECK(buffer.IsEmpty());
	CHECK(buffer.GetData() == nullptr);

	Buffer empty = Buffer::Copy(nullptr, 0);
	CHECK(empty.IsEmpty());
}
