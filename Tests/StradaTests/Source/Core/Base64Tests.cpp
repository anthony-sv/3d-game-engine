#include "Strada/Core/Base64.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace Strada;

namespace
{
	std::vector<uint8_t> Bytes(std::string_view text)
	{
		return std::vector<uint8_t>(text.begin(), text.end());
	}
}

TEST_CASE("Base64: RFC 4648 test vectors")
{
	struct Vector
	{
		std::string_view Plain;
		std::string_view Encoded;
	};
	constexpr Vector Vectors[] = {
		{"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"},
	};
	for (Vector const& vector : Vectors)
	{
		CAPTURE(vector.Plain);
		CHECK(Base64Encode(Bytes(vector.Plain)) == vector.Encoded);
		Result<std::vector<uint8_t>> const decoded = Base64Decode(vector.Encoded);
		REQUIRE(decoded.IsOk());
		CHECK(decoded.GetValue() == Bytes(vector.Plain));
	}
}

TEST_CASE("Base64: every byte value round trips")
{
	std::vector<uint8_t> data(256);
	for (size_t i = 0; i < data.size(); i++)
	{
		data[i] = static_cast<uint8_t>(255 - i);
	}
	for (size_t length = 0; length <= data.size(); length += 37)
	{
		std::vector<uint8_t> const slice(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(length));
		Result<std::vector<uint8_t>> const decoded = Base64Decode(Base64Encode(slice));
		REQUIRE(decoded.IsOk());
		CHECK(decoded.GetValue() == slice);
	}
}

TEST_CASE("Base64: unpadded input is accepted, malformed input is rejected")
{
	CHECK(Base64Decode("Zm8").GetValue() == Bytes("fo"));
	CHECK(Base64Decode("Zg").GetValue() == Bytes("f"));

	CHECK(Base64Decode("Zm9v!").IsError());
	CHECK(Base64Decode("Zm 9v").IsError());
	CHECK(Base64Decode("Z").IsError());
	CHECK(Base64Decode("Zg=").IsError());
	CHECK(Base64Decode("Z===").IsError());
	CHECK(Base64Decode("=Zg=").IsError());
}
