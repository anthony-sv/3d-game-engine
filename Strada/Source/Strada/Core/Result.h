#pragma once

#include "Strada/Core/Assert.h"
#include "Strada/Core/Base.h"

#include <spdlog/fmt/fmt.h>

#include <string>
#include <utility>
#include <variant>

namespace Strada
{
	// Failure value for Result<T>: `return Error{"File not found"};` or `return MakeError("Bad index {}", index);`.
	struct Error
	{
		std::string Message;
	};

	template<typename... Args>
	[[nodiscard]] Error MakeError(fmt::format_string<Args...> format, Args&&... args)
	{
		return Error{fmt::format(format, std::forward<Args>(args)...)};
	}

	// Holds either a value or an error message. Used by fallible operations instead of exceptions.
	template<typename T>
	class [[nodiscard]] Result
	{
	public:
		Result(T value)
			: m_Storage(std::in_place_index<0>, std::move(value))
		{
		}

		Result(Error error)
			: m_Storage(std::in_place_index<1>, std::move(error))
		{
		}

		bool IsOk() const { return m_Storage.index() == 0; }
		bool IsError() const { return m_Storage.index() == 1; }
		explicit operator bool() const { return IsOk(); }

		T& GetValue() &
		{
			ST_CORE_ASSERT(IsOk(), "Result::GetValue called on an error result: {}", GetError());
			return std::get<0>(m_Storage);
		}

		T const& GetValue() const&
		{
			ST_CORE_ASSERT(IsOk(), "Result::GetValue called on an error result: {}", GetError());
			return std::get<0>(m_Storage);
		}

		// Moves the value out of the result.
		T TakeValue()
		{
			ST_CORE_ASSERT(IsOk(), "Result::TakeValue called on an error result: {}", GetError());
			return std::move(std::get<0>(m_Storage));
		}

		T ValueOr(T fallback) const& { return IsOk() ? std::get<0>(m_Storage) : std::move(fallback); }

		std::string const& GetError() const
		{
			static std::string const s_NoError;
			return IsError() ? std::get<1>(m_Storage).Message : s_NoError;
		}

	private:
		std::variant<T, Error> m_Storage;
	};

	template<>
	class [[nodiscard]] Result<void>
	{
	public:
		Result() = default;

		Result(Error error)
			: m_Error(std::move(error.Message)),
			  m_HasError(true)
		{
		}

		bool IsOk() const { return !m_HasError; }
		bool IsError() const { return m_HasError; }
		explicit operator bool() const { return IsOk(); }

		std::string const& GetError() const { return m_Error; }

	private:
		std::string m_Error;
		bool m_HasError = false;
	};
}
