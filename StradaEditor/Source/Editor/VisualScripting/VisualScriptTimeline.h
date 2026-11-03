#pragma once

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The curve model of a Flow.Timeline's Timeline setting (spec-slice3 §3.8.2, §6.3): reading it with the scripting runtime's
// rules and words, writing its canonical form, evaluating a track for drawing (§3.8.3; the runtime is the semantics), and the
// edits Details and the drawer's curve view make. Pure functions of values: no ImGui, no state; any thread.
namespace Strada
{
	// How the segment from a curve key to the next one interpolates.
	enum class VisualScriptKeyInterpolation : uint8_t
	{
		Cubic = 0,
		Linear,
		Constant
	};

	struct VisualScriptTimelineKey
	{
		// Seconds, in [0, Length].
		float Time = 0.0f;
		// The first Components numbers are used (R, G, B, A for colours).
		glm::vec4 Value{0.0f};
		VisualScriptKeyInterpolation Interpolation = VisualScriptKeyInterpolation::Cubic;
		// The slopes (value units per second) the key's segments enter and leave it with; absent: auto.
		std::optional<glm::vec4> In;
		std::optional<glm::vec4> Out;

		bool operator==(VisualScriptTimelineKey const& other) const = default;
	};

	struct VisualScriptTimelineTrack
	{
		std::string Name;
		// float, Strada.Vector2, Strada.Vector3, Strada.Vector4, Strada.Color, or exec (an event track).
		std::string Type;
		// 0 for exec, 1 for float, 2 to 4 for vectors, 4 for colours.
		uint32_t Components = 1;
		// In time order (several keys at one time make a jump).
		std::vector<VisualScriptTimelineKey> Keys;

		bool operator==(VisualScriptTimelineTrack const& other) const = default;
	};

	struct VisualScriptTimeline
	{
		float Length = 1.0f;
		bool Loop = false;
		std::vector<VisualScriptTimelineTrack> Tracks;

		bool operator==(VisualScriptTimeline const& other) const = default;
	};

	// A key of a timeline: (track index, key index).
	using VisualScriptTimelineKeyRef = std::pair<uint32_t, uint32_t>;

	// The scripting runtime's bounds of a Timeline setting.
	inline constexpr float MaxVisualScriptTimelineLength = 3600.0f;
	inline constexpr size_t MaxVisualScriptTimelineTracks = 32;
	inline constexpr size_t MaxVisualScriptTimelineKeys = 1024;
	// The type of an event track.
	inline constexpr std::string_view VisualScriptTimelineEventType = "exec";
	// The node type and its setting.
	inline constexpr std::string_view VisualScriptTimelineNodeType = "Flow.Timeline";
	inline constexpr std::string_view VisualScriptTimelineSetting = "Timeline";

	// The pins every Flow.Timeline has, whose names no track may take.
	inline constexpr std::array<std::string_view, 13> VisualScriptTimelineFixedPins = {
		"Play", "PlayFromStart", "Reverse",  "ReverseFromEnd", "Stop",      "SetTime", "NewTime",
		"Rate", "Update",        "Finished", "Time",           "Direction", "Playing"};

	// A kind of track as the "+ Add Track" menus offer it.
	struct VisualScriptTimelineTrackKind
	{
		// "Float", "Vector2", "Vector3", "Vector4", "Color", "Event".
		std::string_view Label;
		std::string_view Type;
	};
	inline constexpr std::array<VisualScriptTimelineTrackKind, 6> VisualScriptTimelineTrackKinds = {{
		{"Float", "float"},
		{"Vector2", "Strada.Vector2"},
		{"Vector3", "Strada.Vector3"},
		{"Vector4", "Strada.Vector4"},
		{"Color", "Strada.Color"},
		{"Event", "exec"},
	}};

	// The numbers a value of a track type has: 0 for exec, 1 for float, 2 to 4 for vectors, 4 for Strada.Color; nothing for
	// a type that is no track type.
	[[nodiscard]] std::optional<uint32_t> GetVisualScriptTimelineComponents(std::string_view type);
	// The label of a track type ("Float", "Vector3", "Event"); the type itself for another.
	[[nodiscard]] std::string_view GetVisualScriptTimelineTypeLabel(std::string_view type);

	// Reads a Timeline setting with the scripting runtime's rules (§3.8.2; floats through VisualScriptSerializer::ReadFloat):
	// the error is the first problem the runtime reports, with its path and words ("Timeline.Tracks[1].Keys[3].Time: keys
	// are in time order: 0.5 comes after 0.75"). Never throws.
	[[nodiscard]] Result<VisualScriptTimeline> ParseVisualScriptTimeline(Json const& setting);
	// The canonical form: Length, Loop and Tracks; each track's Name, Type and Keys; each key's Time, Value (curve keys),
	// Interpolation (when not Cubic), In and Out (when not auto); floats as the literal codec writes them.
	[[nodiscard]] Json WriteVisualScriptTimeline(VisualScriptTimeline const& timeline);
	// The timeline of a node's Timeline setting: the setting's (ParseVisualScriptTimeline), or the default one (1 s, no loop,
	// no tracks) when the node has none.
	[[nodiscard]] Result<VisualScriptTimeline> ReadVisualScriptTimeline(VisualScriptNode const& node);
	// Whether a timeline is one a Timeline setting can hold: what WriteVisualScriptTimeline writes reads back.
	[[nodiscard]] Result<void> CheckVisualScriptTimeline(VisualScriptTimeline const& timeline);

	// The tracks of a Timeline setting that the scripting runtime gives pins, in order, even when other parts of the setting
	// are wrong: each with a valid, unique name that no fixed pin has and a track type. (name, type) pairs.
	[[nodiscard]] std::vector<std::pair<std::string, std::string>> ListVisualScriptTimelineTrackPins(Json const& setting);

	// Why a track cannot be named so (the runtime's name rule, reserved and fixed pin names, unique among the other tracks;
	// except is the track being renamed); nothing when it can.
	[[nodiscard]] std::optional<std::string> CheckVisualScriptTimelineTrackName(VisualScriptTimeline const& timeline, std::string_view name,
	                                                                            std::optional<uint32_t> except = std::nullopt);
	// A name no track has: base, else base followed by 2, 3, ...
	[[nodiscard]] std::string MakeVisualScriptTimelineTrackName(VisualScriptTimeline const& timeline, std::string_view base);

	// §3.8.3's evaluation, for drawing: the track's value at a time (in double, as floats). No key: the type's default (0,
	// zero vectors, white); the components a track does not have are 0. An event track evaluates to 0.
	[[nodiscard]] glm::vec4 EvaluateVisualScriptTimelineTrack(VisualScriptTimelineTrack const& track, double time);
	// A curve key's slopes with auto tangents resolved as the runtime resolves them: (In, Out).
	[[nodiscard]] std::pair<glm::vec4, glm::vec4> GetVisualScriptTimelineKeyTangents(VisualScriptTimelineTrack const& track, uint32_t key);

	// A value of a curve track as its type's canonical literal (a number, or an array of Components numbers), and back;
	// nothing for a literal that is no value of the track's type.
	[[nodiscard]] Json MakeVisualScriptTimelineValueLiteral(VisualScriptTimelineTrack const& track, glm::vec4 value);
	[[nodiscard]] std::optional<glm::vec4> ReadVisualScriptTimelineValueLiteral(VisualScriptTimelineTrack const& track,
	                                                                            Json const& literal);

	// Edits. Each checks before it changes and leaves the timeline unchanged when it fails; keys stay sorted and within
	// [0, Length].

	// Moves the selected keys of a timeline by one total offset, as a block: the time offset is limited so that no selected
	// key passes an unselected neighbour of its track or leaves [0, Length], so the selection keeps its spacing and keys stay
	// sorted (and keep their indices). valueOffset is added to the values of the selected curve keys (event keys have none).
	// Callers pass the timeline at the start of the interaction and the total offset so far (§9.5), never the previous
	// frame's result.
	[[nodiscard]] Result<void> MoveVisualScriptTimelineKeys(VisualScriptTimeline& timeline,
	                                                        std::span<VisualScriptTimelineKeyRef const> keys, float timeOffset,
	                                                        glm::vec4 valueOffset);
	// Sets the length (greater than 0, at most MaxVisualScriptTimelineLength): keys after the new length move onto it. Callers
	// pass the timeline at the start of the interaction (§9.5), so a length dragged down and back up restores every key.
	[[nodiscard]] Result<void> SetVisualScriptTimelineLength(VisualScriptTimeline& timeline, float length);
	// How many keys of a timeline lie after a time (those a shorter length moves).
	[[nodiscard]] uint32_t CountVisualScriptTimelineKeysAfter(VisualScriptTimeline const& timeline, float time);

	// Adds a key at a time of [0, Length] (after the keys at that time) with a value (ignored for event tracks); returns its
	// index.
	[[nodiscard]] Result<uint32_t> AddVisualScriptTimelineKey(VisualScriptTimeline& timeline, uint32_t track, float time, glm::vec4 value);
	[[nodiscard]] Result<void> RemoveVisualScriptTimelineKeys(VisualScriptTimeline& timeline,
	                                                          std::span<VisualScriptTimelineKeyRef const> keys);
	// Sets a curve key's value.
	[[nodiscard]] Result<void> SetVisualScriptTimelineKeyValue(VisualScriptTimeline& timeline, VisualScriptTimelineKeyRef key,
	                                                           glm::vec4 value);
	// Sets the interpolation of the curve keys among keys (event keys have none); fails when none is a curve key.
	[[nodiscard]] Result<void> SetVisualScriptTimelineKeyInterpolation(VisualScriptTimeline& timeline,
	                                                                   std::span<VisualScriptTimelineKeyRef const> keys,
	                                                                   VisualScriptKeyInterpolation interpolation);
	// Sets a curve key's tangents (nothing: auto).
	[[nodiscard]] Result<void> SetVisualScriptTimelineKeyTangents(VisualScriptTimeline& timeline, VisualScriptTimelineKeyRef key,
	                                                              std::optional<glm::vec4> in, std::optional<glm::vec4> out);
	// Makes the tangents of the curve keys among keys flat (0) or auto; fails when none is a curve key.
	[[nodiscard]] Result<void> FlattenVisualScriptTimelineTangents(VisualScriptTimeline& timeline,
	                                                               std::span<VisualScriptTimelineKeyRef const> keys);
	[[nodiscard]] Result<void> ResetVisualScriptTimelineTangents(VisualScriptTimeline& timeline,
	                                                             std::span<VisualScriptTimelineKeyRef const> keys);

	// Adds a track without keys at the end; returns its index.
	[[nodiscard]] Result<uint32_t> AddVisualScriptTimelineTrack(VisualScriptTimeline& timeline, std::string name, std::string type);
	[[nodiscard]] Result<void> RemoveVisualScriptTimelineTrack(VisualScriptTimeline& timeline, uint32_t track);
	[[nodiscard]] Result<void> RenameVisualScriptTimelineTrack(VisualScriptTimeline& timeline, uint32_t track, std::string name);
	// Changes a track's type: its keys keep their times; curve keys keep the components both types have (the others 0, white's
	// for a colour's alpha), their interpolation and the shared tangent components; keys becoming event keys keep only their
	// times, and event keys becoming curve keys take the type's default value.
	[[nodiscard]] Result<void> SetVisualScriptTimelineTrackType(VisualScriptTimeline& timeline, uint32_t track, std::string type);
}
