#include "stpch.h"
#include "Strada/Core/CommandLine.h"

#include <algorithm>
#include <charconv>
#include <locale>
#include <sstream>

namespace Strada
{
	bool CommandLineArguments::HasFlag(std::string_view name) const
	{
		return std::find(m_Flags.begin(), m_Flags.end(), name) != m_Flags.end();
	}

	std::optional<std::string> CommandLineArguments::GetValue(std::string_view name) const
	{
		auto const it = m_Values.find(std::string(name));
		if (it == m_Values.end())
		{
			return std::nullopt;
		}
		return it->second;
	}

	std::optional<int64_t> CommandLineArguments::GetInt(std::string_view name) const
	{
		std::optional<std::string> const value = GetValue(name);
		if (!value || value->empty())
		{
			return std::nullopt;
		}

		int64_t result = 0;
		char const* end = value->data() + value->size();
		auto const [pointer, errorCode] = std::from_chars(value->data(), end, result, 10);
		if (errorCode != std::errc() || pointer != end)
		{
			return std::nullopt;
		}
		return result;
	}

	std::optional<double> CommandLineArguments::GetDouble(std::string_view name) const
	{
		std::optional<std::string> const value = GetValue(name);
		if (!value || value->empty())
		{
			return std::nullopt;
		}

		// A classic-locale stream is used because std::from_chars<double> is missing from some standard libraries and
		// std::strtod depends on the global locale (which toolkits such as GTK change).
		std::istringstream stream(*value);
		stream.imbue(std::locale::classic());
		double result = 0.0;
		stream >> result;
		if (stream.fail() || stream.peek() != std::char_traits<char>::eof())
		{
			return std::nullopt;
		}
		return result;
	}

	CommandLineParser::CommandLineParser(std::string description)
		: m_Description(std::move(description))
	{
	}

	CommandLineParser& CommandLineParser::AddFlag(std::string name, std::string description)
	{
		ST_CORE_ASSERT(FindOption(name) == nullptr, "Duplicate command line option '{}'", name);
		m_Options.push_back({std::move(name), {}, std::move(description), false});
		return *this;
	}

	CommandLineParser& CommandLineParser::AddOption(std::string name, std::string valueName, std::string description)
	{
		ST_CORE_ASSERT(FindOption(name) == nullptr, "Duplicate command line option '{}'", name);
		m_Options.push_back({std::move(name), std::move(valueName), std::move(description), true});
		return *this;
	}

	Result<CommandLineArguments> CommandLineParser::Parse(int argc, char const* const* argv) const
	{
		std::vector<std::string> arguments;
		for (int i = 1; i < argc; i++)
		{
			arguments.emplace_back(argv[i]);
		}
		return Parse(arguments);
	}

	Result<CommandLineArguments> CommandLineParser::Parse(std::span<std::string const> arguments) const
	{
		CommandLineArguments result;
		bool optionsEnded = false;

		for (size_t i = 0; i < arguments.size(); i++)
		{
			std::string const& argument = arguments[i];

			if (optionsEnded)
			{
				result.m_Positionals.push_back(argument);
				continue;
			}
			if (argument == "--")
			{
				optionsEnded = true;
				continue;
			}
			if (argument.size() < 3 || argument.compare(0, 2, "--") != 0)
			{
				result.m_Positionals.push_back(argument);
				continue;
			}

			std::string_view body = std::string_view(argument).substr(2);
			std::optional<std::string> inlineValue;
			if (size_t const equals = body.find('='); equals != std::string_view::npos)
			{
				inlineValue = std::string(body.substr(equals + 1));
				body = body.substr(0, equals);
			}

			OptionSpecification const* option = FindOption(body);
			if (option == nullptr)
			{
				return MakeError("Unknown option '--{}'", body);
			}

			if (!option->TakesValue)
			{
				if (inlineValue)
				{
					return MakeError("Option '--{}' does not take a value", body);
				}
				if (!result.HasFlag(option->Name))
				{
					result.m_Flags.push_back(option->Name);
				}
				continue;
			}

			if (inlineValue)
			{
				result.m_Values[option->Name] = *inlineValue;
				continue;
			}

			if (i + 1 >= arguments.size())
			{
				return MakeError("Option '--{}' requires a value <{}>", body, option->ValueName);
			}
			result.m_Values[option->Name] = arguments[++i];
		}

		return result;
	}

	std::string CommandLineParser::GetHelpText(std::string_view programName) const
	{
		std::string text = fmt::format("{}\n\nUsage: {} [options]\n\nOptions:\n", m_Description, programName);

		size_t width = 0;
		std::vector<std::string> labels;
		labels.reserve(m_Options.size());
		for (OptionSpecification const& option : m_Options)
		{
			std::string label = "--" + option.Name;
			if (option.TakesValue)
			{
				label += " <" + option.ValueName + ">";
			}
			width = std::max(width, label.size());
			labels.push_back(std::move(label));
		}

		for (size_t i = 0; i < m_Options.size(); i++)
		{
			text += fmt::format("  {:<{}}  {}\n", labels[i], width, m_Options[i].Description);
		}
		return text;
	}

	CommandLineParser::OptionSpecification const* CommandLineParser::FindOption(std::string_view name) const
	{
		for (OptionSpecification const& option : m_Options)
		{
			if (option.Name == name)
			{
				return &option;
			}
		}
		return nullptr;
	}
}
