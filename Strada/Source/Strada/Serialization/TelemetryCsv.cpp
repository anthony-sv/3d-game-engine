#include "stpch.h"
#include "Strada/Serialization/TelemetryCsv.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Serialization/JsonTextWriter.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <string>
#include <vector>

namespace Strada
{
	namespace
	{
		constexpr std::string_view LineEnd = "\r\n";

		// A field in quotes, a quote inside doubled (RFC 4180, rule 7).
		void AppendQuoted(std::string& text, std::string_view field)
		{
			text.push_back('"');
			for (char const character : field)
			{
				if (character == '"')
				{
					text.push_back('"');
				}
				text.push_back(character);
			}
			text.push_back('"');
		}

		void AppendUnsigned(std::string& text, uint64_t value)
		{
			std::array<char, 24> digits{};
			auto const [end, error] = std::to_chars(digits.data(), digits.data() + digits.size(), value);
			text.append(digits.data(), error == std::errc() ? end : digits.data());
		}

		Error MakeSizeError(uint64_t maxBytes)
		{
			return MakeError("The export would be larger than {} bytes", maxBytes);
		}
	}

	TelemetryCsvOptions TelemetryCsvWriter::ForLap(TelemetryLap const& lap)
	{
		TelemetryCsvOptions options;
		options.FirstRow = lap.StartRow;
		options.LastRow = lap.EndRow > lap.StartRow ? lap.EndRow - 1 : lap.StartRow;
		return options;
	}

	Result<std::string> TelemetryCsvWriter::Write(TelemetrySession const& session, TelemetryCsvOptions const& options, uint64_t maxBytes)
	{
		std::vector<TelemetryChannel const*> channels;
		if (options.Channels.empty())
		{
			channels.reserve(session.Channels.size());
			for (TelemetryChannel const& channel : session.Channels)
			{
				channels.push_back(&channel);
			}
		}
		else
		{
			channels.reserve(options.Channels.size());
			for (std::string const& name : options.Channels)
			{
				TelemetryChannel const* const channel = FindTelemetryChannel(session, name);
				if (channel == nullptr)
				{
					return MakeError("The session has no channel '{}'", name);
				}
				if (std::find(channels.begin(), channels.end(), channel) != channels.end())
				{
					return MakeError("The channel '{}' is named twice", name);
				}
				channels.push_back(channel);
			}
		}

		std::string text;
		text.append(HeaderStart);
		for (TelemetryChannel const* const channel : channels)
		{
			text.push_back(',');
			AppendQuoted(text, channel->Name);
		}
		text.push_back(',');
		AppendQuoted(text, "Marks");
		text.append(LineEnd);
		if (text.size() > maxBytes)
		{
			return MakeSizeError(maxBytes);
		}

		if (session.Rows > 0 && options.FirstRow < session.Rows && options.FirstRow <= options.LastRow)
		{
			uint64_t const lastRow = std::min(options.LastRow, session.Rows - 1);
			auto mark = std::lower_bound(session.Marks.begin(), session.Marks.end(), options.FirstRow,
			                             [](TelemetryMark const& candidate, uint64_t row)
			                             {
											 return candidate.Row < row;
										 });
			for (uint64_t row = options.FirstRow; row <= lastRow; ++row)
			{
				AppendUnsigned(text, session.FirstStep + row);
				text.push_back(',');
				JsonTextWriter::AppendDouble(text, GetTelemetryTime(session, row));
				for (TelemetryChannel const* const channel : channels)
				{
					text.push_back(',');
					if (std::optional<float> const value = GetTelemetryValue(*channel, row))
					{
						JsonTextWriter::AppendDouble(text, static_cast<double>(*value));
					}
				}
				text.push_back(',');
				bool first = true;
				for (; mark != session.Marks.end() && mark->Row == row; ++mark)
				{
					text.append(first ? "\"" : "\n");
					first = false;
					for (char const character : mark->Label)
					{
						if (character == '"')
						{
							text.push_back('"');
						}
						text.push_back(character);
					}
				}
				if (!first)
				{
					text.push_back('"');
				}
				text.append(LineEnd);
				if (text.size() > maxBytes)
				{
					return MakeSizeError(maxBytes);
				}
			}
		}
		return text;
	}

	Result<void> TelemetryCsvWriter::Save(TelemetrySession const& session, std::filesystem::path const& path,
	                                      TelemetryCsvOptions const& options, uint64_t maxBytes)
	{
		Result<std::string> const text = Write(session, options, maxBytes);
		if (!text)
		{
			return Error{text.GetError()};
		}
		return FileSystem::WriteTextFile(path, text.GetValue());
	}
}
