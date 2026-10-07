#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Strada
{
	// Parsed command line: flags (--headless), valued options (--project <path> or --project=<path>) and positionals.
	class CommandLineArguments
	{
	public:
		bool HasFlag(std::string_view name) const;
		std::optional<std::string> GetValue(std::string_view name) const;
		std::optional<int64_t> GetInt(std::string_view name) const;
		std::optional<double> GetDouble(std::string_view name) const;
		std::vector<std::string> const& GetPositionals() const { return m_Positionals; }

	private:
		friend class CommandLineParser;

		std::unordered_map<std::string, std::string> m_Values;
		std::vector<std::string> m_Flags;
		std::vector<std::string> m_Positionals;
	};

	// Declarative parser; unknown options and missing values are reported as errors. "--" ends option parsing.
	class CommandLineParser
	{
	public:
		explicit CommandLineParser(std::string description);

		// Names are given without the leading dashes.
		CommandLineParser& AddFlag(std::string name, std::string description);
		CommandLineParser& AddOption(std::string name, std::string valueName, std::string description);

		[[nodiscard]] Result<CommandLineArguments> Parse(int argc, char const* const* argv) const;
		// Parses arguments that do not include the program name.
		[[nodiscard]] Result<CommandLineArguments> Parse(std::span<std::string const> arguments) const;

		std::string GetHelpText(std::string_view programName) const;

	private:
		struct OptionSpecification
		{
			std::string Name;
			std::string ValueName;
			std::string Description;
			bool TakesValue = false;
		};

		OptionSpecification const* FindOption(std::string_view name) const;

		std::string m_Description;
		std::vector<OptionSpecification> m_Options;
	};
}
