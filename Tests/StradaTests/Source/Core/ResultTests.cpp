#include "Strada/Core/Result.h"

#include <doctest/doctest.h>

#include <memory>
#include <string>

using namespace Strada;

namespace
{
	Result<int> ParsePositive(int value)
	{
		if (value <= 0)
		{
			return MakeError("Value {} is not positive", value);
		}
		return value;
	}

	Result<void> Validate(bool ok)
	{
		if (!ok)
		{
			return Error{"validation failed"};
		}
		return {};
	}
}

TEST_CASE("Result: holds a value on success")
{
	Result<int> const result = ParsePositive(5);
	CHECK(result.IsOk());
	CHECK_FALSE(result.IsError());
	CHECK(static_cast<bool>(result));
	CHECK(result.GetValue() == 5);
	CHECK(result.GetError().empty());
}

TEST_CASE("Result: holds a formatted error on failure")
{
	Result<int> const result = ParsePositive(-3);
	CHECK(result.IsError());
	CHECK_FALSE(static_cast<bool>(result));
	CHECK(result.GetError() == "Value -3 is not positive");
	CHECK(result.ValueOr(9) == 9);
}

TEST_CASE("Result: move-only values can be taken out")
{
	Result<std::unique_ptr<int>> result = std::make_unique<int>(11);
	REQUIRE(result.IsOk());
	std::unique_ptr<int> value = result.TakeValue();
	REQUIRE(value != nullptr);
	CHECK(*value == 11);
}

TEST_CASE("Result: string values are not confused with errors")
{
	Result<std::string> const result = std::string("payload");
	CHECK(result.IsOk());
	CHECK(result.GetValue() == "payload");
}

TEST_CASE("Result<void>: success and failure")
{
	CHECK(Validate(true).IsOk());
	Result<void> const failure = Validate(false);
	CHECK(failure.IsError());
	CHECK(failure.GetError() == "validation failed");
}
