#pragma once

#include "Strada/Core/Profiler.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Strada::Testing
{
	// Initializes the profiler for as long as it lives (a test may shut it down earlier).
	class ProfilerScope
	{
	public:
		ProfilerScope() { Profiler::Init(); }
		~ProfilerScope()
		{
			if (Profiler::IsInitialized())
			{
				Profiler::Shutdown();
			}
		}

		ProfilerScope(ProfilerScope const&) = delete;
		ProfilerScope& operator=(ProfilerScope const&) = delete;
	};

	// Builds a capture by hand: frames with consecutive indices from 1, zones and GPU passes added to the last frame.
	class CaptureBuilder
	{
	public:
		CaptureBuilder() { m_Capture.Threads.push_back({"Main"}); }

		uint32_t AddThread(std::string name)
		{
			m_Capture.Threads.push_back({std::move(name)});
			return static_cast<uint32_t>(m_Capture.Threads.size() - 1);
		}

		uint32_t GetName(std::string_view text)
		{
			auto const found = std::find(m_Capture.Names.begin(), m_Capture.Names.end(), text);
			if (found != m_Capture.Names.end())
			{
				return static_cast<uint32_t>(found - m_Capture.Names.begin());
			}
			m_Capture.Names.emplace_back(text);
			return static_cast<uint32_t>(m_Capture.Names.size() - 1);
		}

		void AddFrame(int64_t start, int64_t end, uint32_t dropped = 0)
		{
			ProfilerFrame frame;
			frame.Index = m_Capture.Frames.empty() ? 1 : m_Capture.Frames.back().Index + 1;
			frame.StartNanoseconds = start;
			frame.EndNanoseconds = end;
			frame.DroppedZones = dropped;
			m_Capture.Frames.push_back(std::move(frame));
		}

		ProfilerZone& AddZone(uint32_t thread, std::string_view name, uint32_t depth, int64_t start, int64_t duration)
		{
			REQUIRE_FALSE(m_Capture.Frames.empty());
			ProfilerZone zone;
			zone.Thread = thread;
			zone.Name = GetName(name);
			zone.Depth = depth;
			zone.StartNanoseconds = start;
			zone.DurationNanoseconds = duration;
			m_Capture.Frames.back().Zones.push_back(zone);
			return m_Capture.Frames.back().Zones.back();
		}

		void SetValue(ProfilerZone& zone, std::string_view label, uint64_t value)
		{
			zone.ValueLabel = GetName(label);
			zone.Value = value;
		}

		void AddGpuPass(std::string_view view, std::string_view name, int64_t duration)
		{
			REQUIRE_FALSE(m_Capture.Frames.empty());
			uint32_t const viewName = GetName(view);
			uint32_t const passName = GetName(name);
			m_Capture.Frames.back().GpuPasses.push_back({viewName, passName, duration});
		}

		ProfilerCapture const& Get() const { return m_Capture; }

	private:
		ProfilerCapture m_Capture;
	};

	// A capture as text lines that compare by content, not by table indices: each frame, then its zones (thread name, name,
	// depth, times, value) and GPU passes (view, name, duration) in their order.
	inline std::vector<std::string> DescribeCapture(ProfilerCapture const& capture)
	{
		auto const name = [&capture](uint32_t index) -> std::string
		{
			return index < capture.Names.size() ? capture.Names[index] : std::string("<no name>");
		};
		std::vector<std::string> lines;
		for (ProfilerFrame const& frame : capture.Frames)
		{
			lines.push_back("Frame " + std::to_string(frame.Index) + " " + std::to_string(frame.StartNanoseconds) + "-" +
			                std::to_string(frame.EndNanoseconds) + " dropped " + std::to_string(frame.DroppedZones));
			for (ProfilerZone const& zone : frame.Zones)
			{
				std::string const thread = zone.Thread < capture.Threads.size() ? capture.Threads[zone.Thread].Name : "<no thread>";
				std::string line = "  [" + thread + "] " + name(zone.Name) + " depth " + std::to_string(zone.Depth) + " at " +
				                   std::to_string(zone.StartNanoseconds) + " for " + std::to_string(zone.DurationNanoseconds);
				if (zone.ValueLabel != ProfilerNoName)
				{
					line += " " + name(zone.ValueLabel) + "=" + std::to_string(zone.Value);
				}
				lines.push_back(std::move(line));
			}
			for (ProfilerGpuPass const& pass : frame.GpuPasses)
			{
				lines.push_back("  GPU " + name(pass.View) + "/" + name(pass.Name) + " for " + std::to_string(pass.DurationNanoseconds));
			}
		}
		return lines;
	}

	// The zones of a capture's frames whose names are the given one.
	inline std::vector<ProfilerZone> FindZones(ProfilerCapture const& capture, std::string_view zoneName)
	{
		std::vector<ProfilerZone> zones;
		for (ProfilerFrame const& frame : capture.Frames)
		{
			for (ProfilerZone const& zone : frame.Zones)
			{
				if (zone.Name < capture.Names.size() && capture.Names[zone.Name] == zoneName)
				{
					zones.push_back(zone);
				}
			}
		}
		return zones;
	}
}
