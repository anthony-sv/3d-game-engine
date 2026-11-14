#include "Editor/Automation/ProfilerCommands.h"

#include "Editor/Automation/JsonSchema.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorProfiler.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Project/Project.h"
#include "Strada/Serialization/ProfilerCaptureSerializer.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		// How many zones the commands' medians list at most.
		constexpr size_t MaxReportedZones = 50;

		CommandError MakePathError(std::string const& message)
		{
			CommandError error = MakeCommandError(AutomationErrorCode::InvalidParams, "path: {}", message);
			error.Data = Json::object({{"path", "path"}});
			return error;
		}

		// A path a command names: absolute as given, else relative to the open project's directory and inside it.
		CommandValue<std::filesystem::path> ResolvePath(std::string const& text, EditorContext const& context)
		{
			std::filesystem::path const path = FileSystem::PathFromUtf8(text);
			if (path.is_absolute())
			{
				return path.lexically_normal();
			}
			// "C:file" and "\file" name places that depend on the process's drive or directory.
			if (path.has_root_name() || path.has_root_directory())
			{
				return MakePathError(fmt::format("'{}' is neither absolute nor relative to the project", text));
			}
			Project const* const project = context.GetProject();
			if (project == nullptr)
			{
				return MakeCommandError(AutomationErrorCode::InvalidOperation,
				                        "no project is open: give an absolute path, or open a project for '{}' to be in", text);
			}
			std::filesystem::path const directory = project->GetDirectory().lexically_normal();
			std::filesystem::path const resolved = (directory / path).lexically_normal();
			if (resolved == directory || !FileSystem::IsInside(resolved, directory))
			{
				return MakePathError(fmt::format("'{}' is not a file inside the project", text));
			}
			return resolved;
		}

		bool HasJsonExtension(std::filesystem::path const& path)
		{
			std::string extension = FileSystem::PathToUtf8(path.extension());
			std::transform(extension.begin(), extension.end(), extension.begin(),
			               [](char character)
			               {
							   return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
						   });
			return extension == ".json";
		}

		std::optional<CommandError> CheckRecorder()
		{
			if (!Profiler::IsInitialized())
			{
				return MakeCommandError(AutomationErrorCode::Unavailable, "the profiler is not initialized");
			}
			return std::nullopt;
		}

		Json DescribeStatus(EditorProfiler const& profiler)
		{
			ProfilerStatus const status = Profiler::GetStatus();
			Json startedBy;
			if (profiler.IsCommandRecording())
			{
				startedBy = "command";
			}
			else if (profiler.IsPanelRecording())
			{
				startedBy = "panel";
			}
			Json capture;
			if (ProfilerCapture const* const opened = profiler.GetCapture())
			{
				capture = Json::object(
					{{"name", profiler.GetCaptureName()},
				     {"path", profiler.GetCapturePath().empty() ? Json() : Json(FileSystem::PathToUtf8(profiler.GetCapturePath()))},
				     {"frames", opened->Frames.size()}});
			}
			return Json::object({{"initialized", status.Initialized},
			                     {"available", status.Available},
			                     {"recording", status.Recording},
			                     {"startedBy", std::move(startedBy)},
			                     {"panelHeld", profiler.IsHeld()},
			                     {"historyFrames", status.HistoryFrames},
			                     {"framesHeld", status.FramesHeld},
			                     {"historyZones", status.HistoryZones},
			                     {"zonesHeld", status.ZonesHeld},
			                     {"threads", status.Threads},
			                     {"droppedZones", status.DroppedZones},
			                     {"frameIndex", status.FrameIndex},
			                     {"capture", std::move(capture)}});
		}

		// What a capture holds: { frames, threads, zones, gpuPasses, droppedZones, medians }.
		Json DescribeCapture(ProfilerCapture const& capture)
		{
			size_t zones = 0;
			size_t gpuPasses = 0;
			uint64_t dropped = 0;
			for (ProfilerFrame const& frame : capture.Frames)
			{
				zones += frame.Zones.size();
				gpuPasses += frame.GpuPasses.size();
				dropped += frame.DroppedZones;
			}
			return Json::object({{"frames", capture.Frames.size()},
			                     {"threads", capture.Threads.size()},
			                     {"zones", zones},
			                     {"gpuPasses", gpuPasses},
			                     {"droppedZones", dropped},
			                     {"medians", DescribeProfilerMedians(ComputeProfilerMedians(capture), MaxReportedZones)}});
		}

		Json DescribeMedianList(std::vector<ProfilerMedian> const& medians)
		{
			Json list = Json::array();
			for (ProfilerMedian const& median : medians)
			{
				list.push_back(Json::object({{"name", median.Name}, {"milliseconds", median.Milliseconds}}));
			}
			return list;
		}

		uint32_t GetSetting(Json const& params, char const* name, uint32_t fallback)
		{
			auto const found = params.find(name);
			return found != params.end() ? found->get<uint32_t>() : fallback;
		}

		Result<void> RegisterStart(CommandRegistry& registry, EditorProfiler& profiler)
		{
			ProfilerSettings const defaults;
			CommandDefinition start;
			start.Name = "profiler.start";
			start.Description = fmt::format(
				"Starts recording the editor's frames with the profiler: CPU zones of every thread and GPU pass times, kept in a "
				"history of the last historyFrames frames (default {}) and at most historyZones zones (default {}). A recording "
				"in progress (the Profiler panel's, or an earlier profiler.start's) is restarted with these settings and its "
				"history cleared. The first frame recorded is the editor's next one; the recording runs until profiler.stop, "
				"whether or not the Profiler panel is open. play.advance runs its frames inside one editor frame, so they are one "
				"profiler frame. Returns the status (as profiler.status) and restarted. Not an undo step. Unavailable when the "
				"profiler is not initialized or in Dist builds.",
				defaults.HistoryFrames, defaults.HistoryZones);
			start.Parameters = SchemaBuilder::Object()
			                       .Property("historyFrames", SchemaBuilder::Integer("The frames the history keeps, oldest dropped first")
			                                                      .Minimum(Profiler::MinHistoryFrames)
			                                                      .Maximum(Profiler::MaxHistoryFrames)
			                                                      .Default(defaults.HistoryFrames))
			                       .Property("historyZones", SchemaBuilder::Integer("The zones the history keeps over all its frames")
			                                                     .Minimum(Profiler::MinHistoryZones)
			                                                     .Maximum(Profiler::MaxHistoryZones)
			                                                     .Default(defaults.HistoryZones))
			                       .Property("zonesPerThread",
			                                 SchemaBuilder::Integer("The zones a thread may end in one frame; more are dropped and counted")
			                                     .Minimum(Profiler::MinZonesPerThread)
			                                     .Maximum(Profiler::MaxZonesPerThread)
			                                     .Default(defaults.ZonesPerThread))
			                       .Build();
			start.Handler = [&profiler, defaults](Json const& params) -> CommandResult
			{
				if (std::optional<CommandError> unavailable = CheckRecorder())
				{
					return *unavailable;
				}
				if (!Profiler::GetStatus().Available)
				{
					return MakeCommandError(AutomationErrorCode::Unavailable, "the profiler records nothing in Dist builds");
				}
				ProfilerSettings settings;
				settings.HistoryFrames = GetSetting(params, "historyFrames", defaults.HistoryFrames);
				settings.HistoryZones = GetSetting(params, "historyZones", defaults.HistoryZones);
				settings.ZonesPerThread = GetSetting(params, "zonesPerThread", defaults.ZonesPerThread);
				bool const restarted = Profiler::IsRecording();
				if (Result<void> started = profiler.Start(settings); !started)
				{
					return MakeCommandError(AutomationErrorCode::InvalidOperation, "{}", started.GetError());
				}
				Json status = DescribeStatus(profiler);
				status["restarted"] = restarted;
				return status;
			};
			return registry.Register(std::move(start));
		}

		Result<void> RegisterStop(CommandRegistry& registry, EditorProfiler& profiler)
		{
			CommandDefinition stop;
			stop.Name = "profiler.stop";
			stop.Description =
				"Stops the profiler's recording, whoever started it (profiler.start or the Profiler panel), and keeps its history "
				"for profiler.capture and profiler.status until the next recording starts; the frame in progress is not kept. "
				"An open Profiler panel stays paused until its Record button, or until it is closed and opened again. Returns the "
				"status (as profiler.status) and stopped (whether a recording was in progress). Not an undo step. Unavailable "
				"when the profiler is not initialized.";
			stop.Handler = [&profiler](Json const&) -> CommandResult
			{
				if (std::optional<CommandError> unavailable = CheckRecorder())
				{
					return *unavailable;
				}
				bool const stopped = profiler.Stop();
				Json status = DescribeStatus(profiler);
				status["stopped"] = stopped;
				return status;
			};
			return registry.Register(std::move(stop));
		}

		Result<void> RegisterStatus(CommandRegistry& registry, EditorProfiler& profiler)
		{
			CommandDefinition status;
			status.Name = "profiler.status";
			status.Description = fmt::format(
				"The profiler's state: {{ initialized, available (false in Dist builds), recording, startedBy (\"command\", "
				"\"panel\" or null), panelHeld (profiler.stop keeps the open Profiler panel from recording), historyFrames, "
				"framesHeld, historyZones, zonesHeld, threads, droppedZones (over the whole recording), frameIndex, capture (the "
				"capture the Profiler panel shows: {{ name, path, frames }}, or null) }}. With medians true, also medians of the "
				"frames held (null when none is): {{ frameMilliseconds, framePercentile99Milliseconds, subsystems, threads, "
				"gpuPasses ([{{ name, milliseconds }}] per frame: subsystems on the main thread, other threads' busy time, GPU "
				"passes as \"<view>/<pass>\"), zones ([{{ name, millisecondsPerFrame, millisecondsPerCall, callsPerFrame }}], the "
				"{} costliest per frame), zonesOmitted }}. Changes nothing.",
				MaxReportedZones);
			status.Parameters = SchemaBuilder::Object()
			                        .Property("medians", SchemaBuilder::Boolean("Also the medians of the frames held (default false)"))
			                        .Build();
			status.ReadOnly = true;
			status.Handler = [&profiler](Json const& params) -> CommandResult
			{
				Json described = DescribeStatus(profiler);
				auto const medians = params.find("medians");
				if (medians != params.end() && medians->get<bool>())
				{
					ProfilerCapture const& recording = Profiler::GetRecording();
					described["medians"] =
						recording.Frames.empty() ? Json() : DescribeProfilerMedians(ComputeProfilerMedians(recording), MaxReportedZones);
				}
				return described;
			};
			return registry.Register(std::move(status));
		}

		Result<void> RegisterCapture(CommandRegistry& registry, EditorContext& context)
		{
			CommandDefinition capture;
			capture.Name = "profiler.capture";
			capture.Description = fmt::format(
				"Writes the frames the profiler holds (the recording in progress or the last one) to a capture file: a Chrome "
				"trace event document with Strada's header, which profiler.open, StradaRuntime and trace viewers read. path ends "
				"in .json and is absolute, or relative to the project directory and inside it (.strada/Captures/ is ignored by "
				"git and left out of project copies); missing folders are created. An existing file is replaced only with "
				"overwrite true and only when it is a profiler capture. A recording in progress goes on. Returns {{ path, bytes, "
				"frames, threads, zones, gpuPasses, droppedZones, medians (as profiler.status gives them, the {} costliest zones) "
				"}}. Not an undo step. InvalidParams for a path that is not a .json file inside the project or absolute; "
				"InvalidOperation without frames to write, without a project for a relative path, and for a file that exists; "
				"Unavailable when the profiler is not initialized; FileError when the file cannot be written.",
				MaxReportedZones);
			capture.Parameters =
				SchemaBuilder::Object()
					.Property("path",
			                  SchemaBuilder::String("The capture file: absolute, or relative to the project directory, ending in .json")
			                      .MinLength(1),
			                  true)
					.Property("overwrite", SchemaBuilder::Boolean("Replace a profiler capture at path (default false)"))
					.Build();
			capture.Handler = [&context](Json const& params) -> CommandResult
			{
				std::string const text = params["path"].get<std::string>();
				CommandValue<std::filesystem::path> resolved = ResolvePath(text, context);
				if (!resolved)
				{
					return resolved.TakeError();
				}
				std::filesystem::path const path = resolved.TakeValue();
				if (!HasJsonExtension(path))
				{
					return MakePathError(fmt::format("'{}' does not end in .json", text));
				}
				if (std::optional<CommandError> unavailable = CheckRecorder())
				{
					return *unavailable;
				}
				ProfilerCapture const& recording = Profiler::GetRecording();
				if (recording.Frames.empty())
				{
					return MakeCommandError(AutomationErrorCode::InvalidOperation,
					                        "the profiler holds no frames: start a recording (profiler.start) and let the editor run "
					                        "frames first");
				}
				if (FileSystem::IsDirectory(path))
				{
					return MakePathError(fmt::format("'{}' is a folder", text));
				}
				if (FileSystem::Exists(path))
				{
					auto const overwrite = params.find("overwrite");
					if (overwrite == params.end() || !overwrite->get<bool>())
					{
						return MakeCommandError(AutomationErrorCode::InvalidOperation,
						                        "'{}' exists: pass overwrite true to replace a capture, or name another file", text);
					}
					if (!ProfilerCaptureSerializer::Load(path))
					{
						return MakeCommandError(AutomationErrorCode::InvalidOperation,
						                        "'{}' is not a profiler capture, and profiler.capture replaces nothing else", text);
					}
				}
				if (Result<void> saved = ProfilerCaptureSerializer::Save(recording, path); !saved)
				{
					return MakeCommandError(AutomationErrorCode::FileError, "the capture could not be written: {}", saved.GetError());
				}
				Json described = DescribeCapture(recording);
				described["path"] = FileSystem::PathToUtf8(path);
				described["bytes"] = FileSystem::GetFileSize(path).value_or(0);
				return described;
			};
			return registry.Register(std::move(capture));
		}

		Result<void> RegisterOpen(CommandRegistry& registry, EditorProfiler& profiler, EditorContext& context,
		                          std::function<void()> showPanel)
		{
			CommandDefinition open;
			open.Name = "profiler.open";
			open.Description = fmt::format(
				"Opens a profiler capture (written by profiler.capture or StradaRuntime --profile) and shows it in the Profiler "
				"panel in place of the live history, bringing the panel forward; a recording in progress goes on. path is "
				"absolute, or relative to the project directory and inside it. Returns {{ path, frames, threads, zones, "
				"gpuPasses, droppedZones, medians (as profiler.status gives them, the {} costliest zones), shown (whether a "
				"Profiler panel shows it: false when headless) }}. Not an undo step. InvalidParams for a path that is neither; "
				"InvalidOperation without a project for a relative path; FileError when the file is missing, too large or not a "
				"capture (what is wrong is in the message).",
				MaxReportedZones);
			open.Parameters =
				SchemaBuilder::Object()
					.Property("path",
			                  SchemaBuilder::String("The capture file: absolute, or relative to the project directory").MinLength(1), true)
					.Build();
			open.Handler = [&profiler, &context, showPanel = std::move(showPanel)](Json const& params) -> CommandResult
			{
				std::string const text = params["path"].get<std::string>();
				CommandValue<std::filesystem::path> resolved = ResolvePath(text, context);
				if (!resolved)
				{
					return resolved.TakeError();
				}
				std::filesystem::path const path = resolved.TakeValue();
				if (Result<void> opened = profiler.Open(path); !opened)
				{
					return MakeCommandError(AutomationErrorCode::FileError, "{}", opened.GetError());
				}
				if (showPanel)
				{
					showPanel();
				}
				Json described = DescribeCapture(*profiler.GetCapture());
				described["path"] = FileSystem::PathToUtf8(path);
				described["shown"] = static_cast<bool>(showPanel);
				return described;
			};
			return registry.Register(std::move(open));
		}
	}

	Json DescribeProfilerMedians(ProfilerMedians const& medians, size_t maxZones)
	{
		std::vector<ProfilerZoneMedian const*> zones;
		zones.reserve(medians.Zones.size());
		for (ProfilerZoneMedian const& zone : medians.Zones)
		{
			zones.push_back(&zone);
		}
		std::stable_sort(zones.begin(), zones.end(),
		                 [](ProfilerZoneMedian const* first, ProfilerZoneMedian const* second)
		                 {
							 return first->MillisecondsPerFrame > second->MillisecondsPerFrame;
						 });
		Json zoneList = Json::array();
		for (size_t index = 0; index < std::min(zones.size(), maxZones); index++)
		{
			ProfilerZoneMedian const& zone = *zones[index];
			zoneList.push_back(Json::object({{"name", zone.Name},
			                                 {"millisecondsPerFrame", zone.MillisecondsPerFrame},
			                                 {"millisecondsPerCall", zone.MillisecondsPerCall},
			                                 {"callsPerFrame", zone.CallsPerFrame}}));
		}
		return Json::object({{"frameMilliseconds", medians.FrameMilliseconds},
		                     {"framePercentile99Milliseconds", medians.FramePercentile99Milliseconds},
		                     {"subsystems", DescribeMedianList(medians.Subsystems)},
		                     {"threads", DescribeMedianList(medians.Threads)},
		                     {"gpuPasses", DescribeMedianList(medians.GpuPasses)},
		                     {"zones", std::move(zoneList)},
		                     {"zonesOmitted", zones.size() - std::min(zones.size(), maxZones)}});
	}

	Result<void> RegisterProfilerCommands(CommandRegistry& registry, EditorProfiler& profiler, EditorContext& context,
	                                      std::function<void()> showPanel)
	{
		Result<void> registered = RegisterStart(registry, profiler);
		registered = registered ? RegisterStop(registry, profiler) : registered;
		registered = registered ? RegisterStatus(registry, profiler) : registered;
		registered = registered ? RegisterCapture(registry, context) : registered;
		return registered ? RegisterOpen(registry, profiler, context, std::move(showPanel)) : registered;
	}
}
