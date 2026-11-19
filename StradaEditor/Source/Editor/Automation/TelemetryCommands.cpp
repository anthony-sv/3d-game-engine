#include "Editor/Automation/TelemetryCommands.h"

#include "Editor/Automation/JsonSchema.h"
#include "Editor/Automation/ProfilerCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTelemetry.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Telemetry.h"
#include "Strada/Core/UUID.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Serialization/TelemetryCsv.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		// The longest decimal UUID of an engine channel's name (<Subsystem>.<entity ID>.<Quantity>).
		constexpr size_t MaxEntityDigits = 20;

		CommandError MakeParameterError(std::string_view parameter, std::string const& message)
		{
			CommandError error = MakeCommandError(AutomationErrorCode::InvalidParams, "{}: {}", parameter, message);
			error.Data = Json::object({{"path", std::string(parameter)}});
			return error;
		}

		std::optional<CommandError> CheckRecorder()
		{
			if (!Telemetry::IsInitialized())
			{
				return MakeCommandError(AutomationErrorCode::Unavailable, "telemetry is not initialized");
			}
			return std::nullopt;
		}

		uint64_t GetStep(TelemetrySession const& session, uint64_t row)
		{
			return session.FirstStep + row;
		}

		Json DescribeSettings(TelemetrySettings const& settings)
		{
			return Json::object({{"maxChannels", settings.MaxChannels},
			                     {"maxSteps", settings.MaxSteps},
			                     {"historySessions", settings.HistorySessions},
			                     {"historyBytes", settings.HistoryBytes},
			                     {"recordsPerThread", settings.RecordsPerThread}});
		}

		Json DescribeStatus(EditorTelemetry const& telemetry)
		{
			TelemetryStatus const status = Telemetry::GetStatus();
			Json startedBy;
			if (telemetry.IsCommandRecording())
			{
				startedBy = "command";
			}
			else if (telemetry.IsPanelRecording())
			{
				startedBy = "panel";
			}
			return Json::object({{"initialized", status.Initialized},
			                     {"recording", status.Recording},
			                     {"startedBy", std::move(startedBy)},
			                     {"panelHeld", telemetry.IsHeld()},
			                     {"settings", DescribeSettings(status.Settings)},
			                     {"sessionsHeld", status.SessionsHeld},
			                     {"bytesHeld", status.BytesHeld},
			                     {"liveSession", status.LiveSession != 0 ? Json(status.LiveSession) : Json()},
			                     {"droppedValues", status.DroppedValues},
			                     {"droppedMarks", status.DroppedMarks},
			                     {"droppedSteps", status.DroppedSteps},
			                     {"buffers", status.Buffers}});
		}

		// A lap and the rows it covers: a finished lap's from its mark to the row before the mark that ends it, the lap in
		// progress's to the session's last row.
		struct LapRows
		{
			TelemetryLap Lap;
			bool InProgress = false;
			uint64_t LastRow = 0;
		};

		LapRows MakeLapRows(TelemetryLap lap, bool inProgress)
		{
			uint64_t const lastRow = inProgress || lap.EndRow <= lap.StartRow ? std::max(lap.EndRow, lap.StartRow) : lap.EndRow - 1;
			return {std::move(lap), inProgress, lastRow};
		}

		Json DescribeLap(TelemetrySession const& session, LapRows const& lap)
		{
			return Json::object({{"label", lap.Lap.Label},
			                     {"number", lap.Lap.Number},
			                     {"firstStep", GetStep(session, lap.Lap.StartRow)},
			                     {"lastStep", GetStep(session, lap.LastRow)},
			                     {"seconds", lap.Lap.Seconds},
			                     {"inProgress", lap.InProgress}});
		}

		// The session a request names (session), or the newest one held.
		CommandValue<TelemetrySession const*> FindRequestedSession(Json const& params)
		{
			std::deque<TelemetrySession> const& sessions = Telemetry::GetSessions();
			if (sessions.empty())
			{
				return MakeCommandError(AutomationErrorCode::InvalidOperation,
				                        "no telemetry session is held: start a recording (telemetry.start) and play a scene first");
			}
			auto const requested = params.find("session");
			if (requested == params.end())
			{
				return &sessions.back();
			}
			uint32_t const id = requested->get<uint32_t>();
			if (TelemetrySession const* const session = Telemetry::FindSession(id))
			{
				return session;
			}
			return MakeParameterError("session", fmt::format("no session {} is held: sessions {} to {} are (telemetry.channels lists them)",
			                                                 id, sessions.front().Id, sessions.back().Id));
		}

		// The rows a request covers, both included: a lap's (lap: its label, the latest of equal ones, or "best"), a range of
		// steps (from, to: either may be left out, and is then the session's first or last step), or the whole session.
		struct RowRange
		{
			uint64_t FirstRow = 0;
			uint64_t LastRow = 0;
			std::optional<LapRows> Lap;
		};

		CommandValue<RowRange> ResolveRange(TelemetrySession const& session, Json const& params)
		{
			auto const lapParameter = params.find("lap");
			auto const fromParameter = params.find("from");
			auto const toParameter = params.find("to");
			if (lapParameter != params.end() && (fromParameter != params.end() || toParameter != params.end()))
			{
				return MakeParameterError("lap", "give a lap or a range of steps (from, to), not both");
			}
			if (session.Rows == 0)
			{
				return MakeParameterError("session", fmt::format("session {} holds no step", session.Id));
			}
			if (lapParameter != params.end())
			{
				std::string const label = lapParameter->get<std::string>();
				std::vector<TelemetryLap> const laps = FindTelemetryLaps(session);
				std::optional<LapRows> found;
				if (label == "best")
				{
					std::optional<size_t> const best = FindBestTelemetryLap(laps);
					if (!best)
					{
						return MakeParameterError("lap", fmt::format("session {} has finished no lap", session.Id));
					}
					found = MakeLapRows(laps[*best], false);
				}
				else if (std::optional<TelemetryLap> progress = FindTelemetryLapInProgress(session); progress && progress->Label == label)
				{
					found = MakeLapRows(std::move(*progress), true);
				}
				else
				{
					auto const latest = std::find_if(laps.rbegin(), laps.rend(),
					                                 [&label](TelemetryLap const& lap)
					                                 {
														 return lap.Label == label;
													 });
					if (latest == laps.rend())
					{
						return MakeParameterError(
							"lap", fmt::format("session {} has no lap '{}' (telemetry.channels lists its laps)", session.Id, label));
					}
					found = MakeLapRows(*latest, false);
				}
				uint64_t const firstRow = found->Lap.StartRow;
				uint64_t const lastRow = found->LastRow;
				return RowRange{firstRow, lastRow, std::move(found)};
			}
			uint64_t const firstStep = GetStep(session, 0);
			uint64_t const lastStep = GetStep(session, session.Rows - 1);
			uint64_t const from = fromParameter != params.end() ? fromParameter->get<uint64_t>() : firstStep;
			uint64_t const to = toParameter != params.end() ? toParameter->get<uint64_t>() : lastStep;
			if (from > to)
			{
				return MakeParameterError("from", fmt::format("step {} comes after to's step {}", from, to));
			}
			if (to < firstStep || from > lastStep)
			{
				return MakeParameterError(
					fromParameter != params.end() ? "from" : "to",
					fmt::format("session {} holds steps {} to {}, and none lies in {} to {}", session.Id, firstStep, lastStep, from, to));
			}
			return RowRange{std::max(from, firstStep) - firstStep, std::min(to, lastStep) - firstStep, std::nullopt};
		}

		// The channels a request names, in its order; each must be the session's, and named once.
		CommandValue<std::vector<TelemetryChannel const*>> FindRequestedChannels(TelemetrySession const& session, Json const& names)
		{
			std::vector<TelemetryChannel const*> channels;
			std::set<std::string> seen;
			for (Json const& entry : names)
			{
				std::string const name = entry.get<std::string>();
				if (!seen.insert(name).second)
				{
					return MakeParameterError("channels", fmt::format("'{}' is named twice", name));
				}
				TelemetryChannel const* const channel = FindTelemetryChannel(session, name);
				if (channel == nullptr)
				{
					return MakeParameterError(
						"channels", fmt::format("session {} has no channel '{}' (telemetry.channels lists them)", session.Id, name));
				}
				channels.push_back(channel);
			}
			return channels;
		}

		// A channel's values over rows, both included.
		struct ChannelSummary
		{
			uint64_t Values = 0;
			float Minimum = 0.0f;
			float Maximum = 0.0f;
			float First = 0.0f;
			float Last = 0.0f;
			uint64_t MinimumRow = 0;
			uint64_t MaximumRow = 0;
			double Sum = 0.0;
		};

		ChannelSummary Summarize(TelemetryChannel const& channel, uint64_t firstRow, uint64_t lastRow)
		{
			ChannelSummary summary;
			uint64_t row = std::max(firstRow, channel.FirstRow);
			uint64_t const end = std::min(lastRow + 1, channel.FirstRow + channel.Cells);
			while (row < end)
			{
				std::span<float const> const cells = GetTelemetryCells(channel, row);
				if (cells.empty())
				{
					break;
				}
				uint64_t const count = std::min<uint64_t>(cells.size(), end - row);
				for (uint64_t index = 0; index < count; ++index)
				{
					float const value = cells[index];
					if (std::isnan(value))
					{
						continue;
					}
					if (summary.Values == 0)
					{
						summary.First = value;
						summary.Minimum = value;
						summary.Maximum = value;
						summary.MinimumRow = row + index;
						summary.MaximumRow = row + index;
					}
					if (value < summary.Minimum)
					{
						summary.Minimum = value;
						summary.MinimumRow = row + index;
					}
					if (value > summary.Maximum)
					{
						summary.Maximum = value;
						summary.MaximumRow = row + index;
					}
					summary.Last = value;
					summary.Sum += value;
					summary.Values++;
				}
				row += count;
			}
			return summary;
		}

		Json DescribeSummary(TelemetrySession const& session, ChannelSummary const& summary)
		{
			if (summary.Values == 0)
			{
				return Json::object({{"values", 0},
				                     {"minimum", Json()},
				                     {"maximum", Json()},
				                     {"mean", Json()},
				                     {"first", Json()},
				                     {"last", Json()},
				                     {"minimumStep", Json()},
				                     {"maximumStep", Json()}});
			}
			return Json::object({{"values", summary.Values},
			                     {"minimum", summary.Minimum},
			                     {"maximum", summary.Maximum},
			                     {"mean", summary.Sum / static_cast<double>(summary.Values)},
			                     {"first", summary.First},
			                     {"last", summary.Last},
			                     {"minimumStep", GetStep(session, summary.MinimumRow)},
			                     {"maximumStep", GetStep(session, summary.MaximumRow)}});
		}

		// The entity an engine channel names (<Subsystem>.<entity ID>.<Quantity>), when its name has that shape.
		std::optional<UUID> ParseChannelEntity(std::string_view name)
		{
			size_t const first = name.find('.');
			if (first == std::string_view::npos || first == 0)
			{
				return std::nullopt;
			}
			size_t const second = name.find('.', first + 1);
			if (second == std::string_view::npos || second + 1 >= name.size())
			{
				return std::nullopt;
			}
			std::string_view const digits = name.substr(first + 1, second - first - 1);
			bool const decimal = !digits.empty() && digits.size() <= MaxEntityDigits &&
			                     std::all_of(digits.begin(), digits.end(),
			                                 [](char character)
			                                 {
												 return character >= '0' && character <= '9';
											 });
			if (!decimal)
			{
				return std::nullopt;
			}
			uint64_t value = 0;
			std::from_chars_result const parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value);
			if (parsed.ec != std::errc() || parsed.ptr != digits.data() + digits.size() || value == 0)
			{
				return std::nullopt;
			}
			return UUID(value);
		}

		// { id, name } of the entity an engine channel names, when the playing (or edited) scene has it; null otherwise.
		Json DescribeChannelEntity(std::string_view name, EditorContext& context)
		{
			std::optional<UUID> const id = ParseChannelEntity(name);
			if (!id)
			{
				return Json();
			}
			Entity const entity = context.GetScene().GetEntityByUUID(*id);
			if (!entity)
			{
				return Json();
			}
			return Json::object({{"id", std::to_string(id->GetValue())}, {"name", entity.GetName()}});
		}

		Json DescribeSessionSummary(TelemetrySession const& session)
		{
			return Json::object({{"session", session.Id},
			                     {"live", session.Live},
			                     {"firstStep", session.FirstStep},
			                     {"lastStep", session.Rows != 0 ? Json(GetStep(session, session.Rows - 1)) : Json()},
			                     {"rows", session.Rows},
			                     {"stepSeconds", session.StepSeconds},
			                     {"channels", session.Channels.size()},
			                     {"marks", session.Marks.size()},
			                     {"laps", FindTelemetryLaps(session).size()}});
		}

		Json DescribeSession(TelemetrySession const& session, EditorContext& context)
		{
			Json channels = Json::array();
			for (TelemetryChannel const& channel : session.Channels)
			{
				channels.push_back(Json::object({{"name", channel.Name},
				                                 {"values", channel.Count},
				                                 {"firstStep", GetStep(session, channel.FirstRow)},
				                                 {"lastStep", GetStep(session, channel.FirstRow + channel.Cells - 1)},
				                                 {"minimum", channel.Minimum},
				                                 {"maximum", channel.Maximum},
				                                 {"last", channel.Last},
				                                 {"entity", DescribeChannelEntity(channel.Name, context)}}));
			}
			Json marks = Json::array();
			size_t const listed = std::min<size_t>(session.Marks.size(), MaxTelemetryListedMarks);
			for (size_t index = 0; index < listed; ++index)
			{
				TelemetryMark const& mark = session.Marks[index];
				marks.push_back(Json::object({{"step", GetStep(session, mark.Row)}, {"label", mark.Label}}));
			}
			std::vector<TelemetryLap> const laps = FindTelemetryLaps(session);
			Json lapList = Json::array();
			for (TelemetryLap const& lap : laps)
			{
				lapList.push_back(DescribeLap(session, MakeLapRows(lap, false)));
			}
			std::optional<size_t> const best = FindBestTelemetryLap(laps);
			std::optional<TelemetryLap> progress = FindTelemetryLapInProgress(session);
			Json described = DescribeSessionSummary(session);
			described["channels"] = std::move(channels);
			described["marks"] = std::move(marks);
			described["marksOmitted"] = session.Marks.size() - listed;
			described["laps"] = std::move(lapList);
			described["bestLap"] = best ? DescribeLap(session, MakeLapRows(laps[*best], false)) : Json();
			described["lapInProgress"] = progress ? DescribeLap(session, MakeLapRows(std::move(*progress), true)) : Json();
			described["dropped"] =
				Json::object({{"values", session.DroppedValues}, {"marks", session.DroppedMarks}, {"steps", session.DroppedSteps}});
			return described;
		}

		bool HasCsvExtension(std::filesystem::path const& path)
		{
			std::string extension = FileSystem::PathToUtf8(path.extension());
			std::transform(extension.begin(), extension.end(), extension.begin(),
			               [](char character)
			               {
							   return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
						   });
			return extension == ".csv";
		}

		// Whether a file is a telemetry export: a regular file whose first line begins as an export's header does. Reads only
		// that beginning.
		bool IsTelemetryExport(std::filesystem::path const& path)
		{
			if (!FileSystem::IsRegularFile(path))
			{
				return false;
			}
			std::ifstream file(path, std::ios::binary);
			std::string start(TelemetryCsvWriter::HeaderStart.size(), '\0');
			file.read(start.data(), static_cast<std::streamsize>(start.size()));
			return file.gcount() == static_cast<std::streamsize>(start.size()) && start == TelemetryCsvWriter::HeaderStart;
		}

		uint64_t GetSetting(Json const& params, char const* name, uint64_t fallback)
		{
			auto const found = params.find(name);
			return found != params.end() ? found->get<uint64_t>() : fallback;
		}

		SchemaBuilder MakeSessionSchema()
		{
			return SchemaBuilder::Integer("The session's number (telemetry.channels lists them); by default the newest held")
			    .Minimum(1)
			    .Maximum(std::numeric_limits<uint32_t>::max());
		}

		Result<void> RegisterStart(CommandRegistry& registry, EditorTelemetry& telemetry)
		{
			TelemetrySettings const defaults;
			CommandDefinition start;
			start.Name = "telemetry.start";
			start.Description = fmt::format(
				"Starts recording telemetry: the channels and marks the playing game's scripts and graphs record "
				"(Strada.Telemetry.Record, Mark and MarkStep) and the engine's own channels, one row per fixed step. The first row "
				"is the playing scene's next fixed step; each play is a session of its own (play, stop, play keeps both), and a "
				"session ends when play stops. A recording in progress (the Telemetry panel's, or an earlier telemetry.start's) is "
				"restarted with these settings; the sessions held are kept (the oldest dropped past historySessions and "
				"historyBytes). The recording runs until telemetry.stop, whether or not the Telemetry panel is open. Settings: "
				"maxChannels (default {}) and maxSteps (default {}) per session, historySessions (default {}) and historyBytes "
				"(default {}) kept over all sessions, recordsPerThread (default {}) a thread other than the main one may record "
				"between two steps; past them values and marks are dropped and counted. Returns the status (as telemetry.stop) "
				"and restarted. Not an undo step. Unavailable when telemetry is not initialized.",
				defaults.MaxChannels, defaults.MaxSteps, defaults.HistorySessions, defaults.HistoryBytes, defaults.RecordsPerThread);
			start.Parameters =
				SchemaBuilder::Object()
					.Property("maxChannels", SchemaBuilder::Integer("The channels a session holds at most")
			                                     .Minimum(Telemetry::MinChannels)
			                                     .Maximum(Telemetry::MaxChannelsLimit)
			                                     .Default(defaults.MaxChannels))
					.Property("maxSteps", SchemaBuilder::Integer("The fixed steps (rows) a session holds at most")
			                                  .Minimum(Telemetry::MinSteps)
			                                  .Maximum(Telemetry::MaxStepsLimit)
			                                  .Default(defaults.MaxSteps))
					.Property("historySessions", SchemaBuilder::Integer("The sessions kept, oldest dropped first")
			                                         .Minimum(Telemetry::MinHistorySessions)
			                                         .Maximum(Telemetry::MaxHistorySessions)
			                                         .Default(defaults.HistorySessions))
					.Property("historyBytes", SchemaBuilder::Integer("The bytes the sessions hold over all of them, oldest dropped first")
			                                      .Minimum(Telemetry::MinHistoryBytes)
			                                      .Maximum(Telemetry::MaxHistoryBytes)
			                                      .Default(defaults.HistoryBytes))
					.Property("recordsPerThread",
			                  SchemaBuilder::Integer("The records a thread other than the main one may make between two steps")
			                      .Minimum(Telemetry::MinRecordsPerThread)
			                      .Maximum(Telemetry::MaxRecordsPerThread)
			                      .Default(defaults.RecordsPerThread))
					.Build();
			start.Handler = [&telemetry, defaults](Json const& params) -> CommandResult
			{
				if (std::optional<CommandError> unavailable = CheckRecorder())
				{
					return *unavailable;
				}
				TelemetrySettings settings;
				settings.MaxChannels = static_cast<uint32_t>(GetSetting(params, "maxChannels", defaults.MaxChannels));
				settings.MaxSteps = static_cast<uint32_t>(GetSetting(params, "maxSteps", defaults.MaxSteps));
				settings.HistorySessions = static_cast<uint32_t>(GetSetting(params, "historySessions", defaults.HistorySessions));
				settings.HistoryBytes = GetSetting(params, "historyBytes", defaults.HistoryBytes);
				settings.RecordsPerThread = static_cast<uint32_t>(GetSetting(params, "recordsPerThread", defaults.RecordsPerThread));
				bool const restarted = Telemetry::IsRecording();
				if (Result<void> started = telemetry.Start(settings); !started)
				{
					return MakeCommandError(AutomationErrorCode::InvalidOperation, "{}", started.GetError());
				}
				Json status = DescribeStatus(telemetry);
				status["restarted"] = restarted;
				return status;
			};
			return registry.Register(std::move(start));
		}

		Result<void> RegisterStop(CommandRegistry& registry, EditorTelemetry& telemetry)
		{
			CommandDefinition stop;
			stop.Name = "telemetry.stop";
			stop.Description =
				"Stops recording telemetry, whoever started it (telemetry.start or the Telemetry panel); the sessions stay for "
				"telemetry.channels, telemetry.read and telemetry.export until a later recording drops them past its bounds. An "
				"open Telemetry panel stays paused until its Record button, or until it is closed and opened again. Returns the "
				"status: { initialized, recording, startedBy (\"command\", \"panel\" or null), panelHeld, settings (the "
				"recording's: maxChannels, maxSteps, historySessions, historyBytes, recordsPerThread), sessionsHeld, bytesHeld, "
				"liveSession (the number of the session the playing scene extends, or null), droppedValues, droppedMarks, "
				"droppedSteps (over the recording), buffers } and stopped (whether a recording was in progress). Not an undo step. "
				"Unavailable when telemetry is not initialized.";
			stop.Handler = [&telemetry](Json const&) -> CommandResult
			{
				if (std::optional<CommandError> unavailable = CheckRecorder())
				{
					return *unavailable;
				}
				bool const stopped = telemetry.Stop();
				Json status = DescribeStatus(telemetry);
				status["stopped"] = stopped;
				return status;
			};
			return registry.Register(std::move(stop));
		}

		Result<void> RegisterChannels(CommandRegistry& registry, EditorContext& context)
		{
			CommandDefinition channels;
			channels.Name = "telemetry.channels";
			channels.Description = fmt::format(
				"What telemetry holds: {{ recording, sessions ([{{ session, live, firstStep, lastStep, rows, stepSeconds, channels, "
				"marks, laps }}] oldest first: each play recorded is a session, counted from 1, its rows its fixed steps), "
				"session (the one asked for, by default the newest; null when none is held) }}. The session gives its summary "
				"with channels ([{{ name, values (how many steps have one), firstStep, lastStep, minimum, maximum, last, entity "
				"({{ id, name }} of the entity an engine channel <Subsystem>.<entity ID>.<Quantity> belongs to, when the playing "
				"or edited scene has it; else null) }}]), marks ([{{ step, label }}], the first {}), marksOmitted, laps ([{{ label, "
				"number, firstStep, lastStep, seconds, inProgress }}]: a lap runs from a mark Lap n to the mark Lap n+1), bestLap "
				"(the shortest, or null), lapInProgress (or null) and dropped {{ values, marks, steps }}. Changes nothing. "
				"InvalidParams for a session not held; Unavailable when telemetry is not initialized.",
				MaxTelemetryListedMarks);
			channels.Parameters = SchemaBuilder::Object().Property("session", MakeSessionSchema()).Build();
			channels.ReadOnly = true;
			channels.Handler = [&context](Json const& params) -> CommandResult
			{
				if (std::optional<CommandError> unavailable = CheckRecorder())
				{
					return *unavailable;
				}
				std::deque<TelemetrySession> const& sessions = Telemetry::GetSessions();
				Json list = Json::array();
				for (TelemetrySession const& session : sessions)
				{
					list.push_back(DescribeSessionSummary(session));
				}
				Json chosen;
				if (!sessions.empty() || params.contains("session"))
				{
					CommandValue<TelemetrySession const*> session = FindRequestedSession(params);
					if (!session)
					{
						return session.TakeError();
					}
					chosen = DescribeSession(*session.TakeValue(), context);
				}
				return Json::object(
					{{"recording", Telemetry::IsRecording()}, {"sessions", std::move(list)}, {"session", std::move(chosen)}});
			};
			return registry.Register(std::move(channels));
		}

		Result<void> RegisterRead(CommandRegistry& registry)
		{
			CommandDefinition read;
			read.Name = "telemetry.read";
			read.Description = fmt::format(
				"Reads channels of a telemetry session (session, by default the newest) over a range of fixed steps (from and to, "
				"both included, each by default the session's first or last step) or over a lap (lap: its label such as \"Lap 2\", "
				"the latest of equal ones or the lap in progress, or \"best\" for the shortest finished lap), in at most "
				"maxSamples samples a channel (default {}, at most {}): every stride-th step from the range's first, the stride "
				"the smallest that fits. Returns {{ session, stepSeconds, from, to, rows, stride, steps (the steps sampled), lap "
				"({{ label, number, firstStep, lastStep, seconds, inProgress }} or null), channels ([{{ name, samples (a value per "
				"step sampled, null where the step has none), summary ({{ values, minimum, maximum, mean, first, last, "
				"minimumStep, maximumStep }} over every step of the range, null but values when it has none) }}]) }}. A step's "
				"time is step times stepSeconds. Changes nothing. InvalidParams for an unknown session, channel or lap, a channel "
				"named twice, a lap with a range, and a range holding no step of the session; InvalidOperation when no session is "
				"held; Unavailable when telemetry is not initialized.",
				DefaultTelemetryReadSamples, MaxTelemetryReadSamples);
			read.Parameters =
				SchemaBuilder::Object()
					.Property(
						"channels",
						SchemaBuilder::Array(SchemaBuilder::String("A channel's name").MinLength(1), "The channels, in the order answered")
							.MinItems(1)
							.MaxItems(MaxTelemetryReadChannels),
						true)
					.Property("session", MakeSessionSchema())
					.Property("from", SchemaBuilder::Integer("The range's first fixed step (default the session's first)").Minimum(1))
					.Property("to", SchemaBuilder::Integer("The range's last fixed step (default the session's last)").Minimum(1))
					.Property("lap", SchemaBuilder::String("A lap's label (\"Lap 2\"), or \"best\"; not with from or to").MinLength(1))
					.Property("maxSamples", SchemaBuilder::Integer("The samples a channel gets at most")
			                                    .Minimum(1)
			                                    .Maximum(MaxTelemetryReadSamples)
			                                    .Default(DefaultTelemetryReadSamples))
					.Build();
			read.ReadOnly = true;
			read.Handler = [](Json const& params) -> CommandResult
			{
				if (std::optional<CommandError> unavailable = CheckRecorder())
				{
					return *unavailable;
				}
				CommandValue<TelemetrySession const*> found = FindRequestedSession(params);
				if (!found)
				{
					return found.TakeError();
				}
				TelemetrySession const& session = *found.TakeValue();
				CommandValue<RowRange> resolved = ResolveRange(session, params);
				if (!resolved)
				{
					return resolved.TakeError();
				}
				RowRange const range = resolved.TakeValue();
				CommandValue<std::vector<TelemetryChannel const*>> named = FindRequestedChannels(session, params["channels"]);
				if (!named)
				{
					return named.TakeError();
				}
				std::vector<TelemetryChannel const*> const channels = named.TakeValue();
				auto const maxSamples = params.find("maxSamples");
				uint64_t const samples = maxSamples != params.end() ? maxSamples->get<uint64_t>() : DefaultTelemetryReadSamples;
				uint64_t const rows = range.LastRow - range.FirstRow + 1;
				uint64_t const stride = (rows + samples - 1) / samples;

				Json steps = Json::array();
				for (uint64_t row = range.FirstRow; row <= range.LastRow; row += stride)
				{
					steps.push_back(GetStep(session, row));
				}
				Json channelList = Json::array();
				for (TelemetryChannel const* const channel : channels)
				{
					Json values = Json::array();
					for (uint64_t row = range.FirstRow; row <= range.LastRow; row += stride)
					{
						std::optional<float> const value = GetTelemetryValue(*channel, row);
						values.push_back(value ? Json(*value) : Json());
					}
					channelList.push_back(
						Json::object({{"name", channel->Name},
					                  {"samples", std::move(values)},
					                  {"summary", DescribeSummary(session, Summarize(*channel, range.FirstRow, range.LastRow))}}));
				}
				return Json::object({{"session", session.Id},
				                     {"stepSeconds", session.StepSeconds},
				                     {"from", GetStep(session, range.FirstRow)},
				                     {"to", GetStep(session, range.LastRow)},
				                     {"rows", rows},
				                     {"stride", stride},
				                     {"steps", std::move(steps)},
				                     {"lap", range.Lap ? DescribeLap(session, *range.Lap) : Json()},
				                     {"channels", std::move(channelList)}});
			};
			return registry.Register(std::move(read));
		}

		Result<void> RegisterExport(CommandRegistry& registry, EditorContext& context)
		{
			CommandDefinition exported;
			exported.Name = "telemetry.export";
			exported.Description =
				"Writes a telemetry session (session, by default the newest) as CSV for spreadsheets and analysis tools: a header "
				"line of quoted names (\"Step\", \"Time\", each channel, \"Marks\") and a line per fixed step (its step, its time "
				"in seconds, each channel's value or an empty field, its marks joined by a line feed), lines ended by CR LF. "
				"Optionally one lap (lap: its label, the latest of equal ones or the lap in progress, or \"best\") or a range of "
				"steps (from, to, both included), and only some channels (channels, in that order; by default every one). path "
				"ends in .csv and is absolute, or relative to the project directory and inside it (.strada/Captures/ is ignored "
				"by git); missing folders are created. An existing file is replaced only with overwrite true, and only when it is "
				"a telemetry export (its first line begins \"Step\",\"Time\"), so that nothing else is lost to a mistyped path. "
				"Returns { path, bytes, session, from, to, rows, channels (the names written), lap (or null) }. Not an undo step. "
				"InvalidParams for a path that is not a .csv file inside the project or absolute, or a folder, and for an unknown "
				"session, channel or lap, a channel named twice, a lap with a range or a range holding no step; InvalidOperation "
				"when no session is held, without a project for a relative path, for a file that exists (unless overwrite and "
				"an export) and for an export over 512 MiB; Unavailable when telemetry is not initialized; FileError when the "
				"file cannot be written.";
			exported.Parameters =
				SchemaBuilder::Object()
					.Property(
						"path",
						SchemaBuilder::String("The CSV file: absolute, or relative to the project directory, ending in .csv").MinLength(1),
						true)
					.Property("session", MakeSessionSchema())
					.Property("from", SchemaBuilder::Integer("The first fixed step written (default the session's first)").Minimum(1))
					.Property("to", SchemaBuilder::Integer("The last fixed step written (default the session's last)").Minimum(1))
					.Property("lap", SchemaBuilder::String("A lap's label (\"Lap 2\"), or \"best\"; not with from or to").MinLength(1))
					.Property("channels", SchemaBuilder::Array(SchemaBuilder::String("A channel's name").MinLength(1),
			                                                   "The channels written, in this order (default every channel of the session)")
			                                  .MinItems(1)
			                                  .MaxItems(Telemetry::MaxChannelsLimit))
					.Property("overwrite", SchemaBuilder::Boolean("Replace a telemetry export at path (default false)"))
					.Build();
			exported.Handler = [&context](Json const& params) -> CommandResult
			{
				std::string const text = params["path"].get<std::string>();
				CommandValue<std::filesystem::path> resolved = ResolveCommandFilePath(text, context);
				if (!resolved)
				{
					return resolved.TakeError();
				}
				std::filesystem::path const path = resolved.TakeValue();
				if (!HasCsvExtension(path))
				{
					return MakeParameterError("path", fmt::format("'{}' does not end in .csv", text));
				}
				if (std::optional<CommandError> unavailable = CheckRecorder())
				{
					return *unavailable;
				}
				CommandValue<TelemetrySession const*> found = FindRequestedSession(params);
				if (!found)
				{
					return found.TakeError();
				}
				TelemetrySession const& session = *found.TakeValue();
				CommandValue<RowRange> range = ResolveRange(session, params);
				if (!range)
				{
					return range.TakeError();
				}
				RowRange const rows = range.TakeValue();
				TelemetryCsvOptions options;
				options.FirstRow = rows.FirstRow;
				options.LastRow = rows.LastRow;
				if (auto const channels = params.find("channels"); channels != params.end())
				{
					CommandValue<std::vector<TelemetryChannel const*>> named = FindRequestedChannels(session, *channels);
					if (!named)
					{
						return named.TakeError();
					}
					for (TelemetryChannel const* const channel : named.TakeValue())
					{
						options.Channels.push_back(channel->Name);
					}
				}
				if (FileSystem::IsDirectory(path))
				{
					return MakeParameterError("path", fmt::format("'{}' is a folder", text));
				}
				if (FileSystem::Exists(path))
				{
					auto const overwrite = params.find("overwrite");
					if (overwrite == params.end() || !overwrite->get<bool>())
					{
						return MakeCommandError(AutomationErrorCode::InvalidOperation,
						                        "'{}' exists: pass overwrite true to replace an export, or name another file", text);
					}
					if (!IsTelemetryExport(path))
					{
						return MakeCommandError(AutomationErrorCode::InvalidOperation,
						                        "'{}' is not a telemetry export, and telemetry.export replaces nothing else", text);
					}
				}
				Result<std::string> written = TelemetryCsvWriter::Write(session, options);
				if (!written)
				{
					return MakeCommandError(AutomationErrorCode::InvalidOperation, "the export was not written: {}", written.GetError());
				}
				if (Result<void> saved = FileSystem::WriteTextFile(path, written.GetValue()); !saved)
				{
					return MakeCommandError(AutomationErrorCode::FileError, "the export could not be written: {}", saved.GetError());
				}
				Json names = Json::array();
				if (options.Channels.empty())
				{
					for (TelemetryChannel const& channel : session.Channels)
					{
						names.push_back(channel.Name);
					}
				}
				else
				{
					for (std::string const& name : options.Channels)
					{
						names.push_back(name);
					}
				}
				return Json::object({{"path", FileSystem::PathToUtf8(path)},
				                     {"bytes", written.GetValue().size()},
				                     {"session", session.Id},
				                     {"from", GetStep(session, rows.FirstRow)},
				                     {"to", GetStep(session, rows.LastRow)},
				                     {"rows", rows.LastRow - rows.FirstRow + 1},
				                     {"channels", std::move(names)},
				                     {"lap", rows.Lap ? DescribeLap(session, *rows.Lap) : Json()}});
			};
			return registry.Register(std::move(exported));
		}
	}

	Result<void> RegisterTelemetryCommands(CommandRegistry& registry, EditorTelemetry& telemetry, EditorContext& context)
	{
		Result<void> registered = RegisterStart(registry, telemetry);
		registered = registered ? RegisterStop(registry, telemetry) : registered;
		registered = registered ? RegisterChannels(registry, context) : registered;
		registered = registered ? RegisterRead(registry) : registered;
		return registered ? RegisterExport(registry, context) : registered;
	}
}
