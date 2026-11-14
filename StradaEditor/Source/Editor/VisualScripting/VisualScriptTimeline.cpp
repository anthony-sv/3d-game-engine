#include "Editor/VisualScripting/VisualScriptTimeline.h"

#include "Editor/VisualScripting/VisualScriptEditing.h"

#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Script/ScriptField.h"
#include "Strada/Script/VisualScriptLiterals.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <unordered_set>

namespace Strada
{
	namespace
	{
		using Format = VisualScriptSerializer;

		constexpr std::string_view TrackTypes = "float, Strada.Vector2, Strada.Vector3, Strada.Vector4, Strada.Color or exec";
		constexpr std::string_view ColorType = "Strada.Color";
		constexpr std::string_view ReservedList = "Execute, Then, Target, Self, Default";
		// The runtime's messages quote values in at most this many UTF-16 units (JsonText.Shorten).
		constexpr size_t ExcerptUnits = 48;

		// A text shortened as the runtime shortens what its messages quote: at most ExcerptUnits UTF-16 units, else the first
		// ExcerptUnits - 3 of them (never half a surrogate pair) and "...".
		std::string Shorten(std::string_view text)
		{
			auto const sequenceLength = [](unsigned char lead) -> size_t
			{
				return lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
			};
			size_t units = 0;
			for (size_t index = 0; index < text.size(); index += sequenceLength(static_cast<unsigned char>(text[index])))
			{
				units += sequenceLength(static_cast<unsigned char>(text[index])) == 4 ? 2 : 1;
			}
			if (units <= ExcerptUnits)
			{
				return std::string(text);
			}
			size_t kept = 0;
			size_t end = 0;
			while (end < text.size())
			{
				size_t const length = sequenceLength(static_cast<unsigned char>(text[end]));
				size_t const width = length == 4 ? 2 : 1;
				if (kept + width > ExcerptUnits - 3)
				{
					break;
				}
				kept += width;
				end += length;
			}
			return std::string(text.substr(0, end)) + "...";
		}

		// A value as the runtime's messages quote it: its JSON text as the runtime receives documents, shortened.
		std::string Excerpt(Json const& value)
		{
			return Shorten(value.dump(-1, ' ', false, Json::error_handler_t::replace));
		}

		std::string Quote(std::string_view text)
		{
			return "\"" + Shorten(text) + "\"";
		}

		// A number as the runtime's messages write a float (.NET's shortest round-trip text: fixed notation for decimal
		// exponents from -5 to 8, else d.ddddE+XX).
		std::string FormatNumber(float value)
		{
			if (value == 0.0f)
			{
				return std::signbit(value) ? "-0" : "0";
			}
			std::array<char, 64> buffer{};
			auto const [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::scientific);
			if (error != std::errc())
			{
				return "0";
			}
			std::string_view const text(buffer.data(), static_cast<size_t>(end - buffer.data()));
			bool const negative = text.front() == '-';
			size_t const mark = text.find('e');
			std::string_view const mantissa = text.substr(negative ? 1 : 0, mark - (negative ? 1 : 0));
			int exponent = 0;
			std::from_chars(text.data() + mark + 1 + (text[mark + 1] == '+' ? 1 : 0), text.data() + text.size(), exponent);
			std::string digits;
			for (char const character : mantissa)
			{
				if (character != '.')
				{
					digits.push_back(character);
				}
			}
			while (digits.size() > 1 && digits.back() == '0')
			{
				digits.pop_back();
			}
			std::string result = negative ? "-" : "";
			// The position of the decimal point after the first digit.
			int const point = exponent + 1;
			if (point > 9 || point < -3)
			{
				result += digits.substr(0, 1);
				if (digits.size() > 1)
				{
					result += "." + digits.substr(1);
				}
				result += fmt::format("E{}{:02}", exponent < 0 ? '-' : '+', std::abs(exponent));
				return result;
			}
			if (point <= 0)
			{
				result += "0." + std::string(static_cast<size_t>(-point), '0') + digits;
			}
			else if (static_cast<size_t>(point) >= digits.size())
			{
				result += digits + std::string(static_cast<size_t>(point) - digits.size(), '0');
			}
			else
			{
				result += digits.substr(0, static_cast<size_t>(point)) + "." + digits.substr(static_cast<size_t>(point));
			}
			return result;
		}

		// The runtime's name rule (Names.Check): null when the name is valid.
		std::optional<std::string> CheckName(std::string_view name)
		{
			if (name.empty())
			{
				return "a name cannot be empty";
			}
			if (name.size() > Format::MaxNameBytes)
			{
				return fmt::format("a name has at most {} bytes", Format::MaxNameBytes);
			}
			if (std::any_of(name.begin(), name.end(),
			                [](char character)
			                {
								auto const byte = static_cast<unsigned char>(character);
								return byte < 0x20 || byte == 0x7F;
							}))
			{
				return "a name cannot contain control characters";
			}
			if (name.front() == ' ' || name.back() == ' ')
			{
				return "a name cannot start or end with a space";
			}
			if (name.find('.') != std::string_view::npos)
			{
				return "a name cannot contain '.'";
			}
			return std::nullopt;
		}

		bool IsFixedPin(std::string_view name)
		{
			return std::find(VisualScriptTimelineFixedPins.begin(), VisualScriptTimelineFixedPins.end(), name) !=
			       VisualScriptTimelineFixedPins.end();
		}

		std::optional<VisualScriptKeyInterpolation> ReadInterpolation(Json const& value)
		{
			if (!value.is_string())
			{
				return std::nullopt;
			}
			std::string const& name = value.get_ref<std::string const&>();
			if (name == "Cubic")
			{
				return VisualScriptKeyInterpolation::Cubic;
			}
			if (name == "Linear")
			{
				return VisualScriptKeyInterpolation::Linear;
			}
			if (name == "Constant")
			{
				return VisualScriptKeyInterpolation::Constant;
			}
			return std::nullopt;
		}

		char const* GetInterpolationName(VisualScriptKeyInterpolation interpolation)
		{
			switch (interpolation)
			{
				case VisualScriptKeyInterpolation::Cubic:
					return "Cubic";
				case VisualScriptKeyInterpolation::Linear:
					return "Linear";
				case VisualScriptKeyInterpolation::Constant:
					return "Constant";
			}
			return "Cubic";
		}

		// The numbers of a curve value literal: a number for float, an array of Components numbers otherwise. The runtime's
		// message when it is none.
		Result<glm::vec4> DecodeValue(Json const& literal, uint32_t components)
		{
			glm::vec4 value(0.0f);
			if (literal.is_null())
			{
				return Error{"null is not a value: leave the value out to use the default"};
			}
			if (components == 1)
			{
				std::optional<float> const number = Format::ReadFloat(literal);
				if (!number)
				{
					return MakeError("expected a finite number of at most 3.4028235e38, not {}", Excerpt(literal));
				}
				value.x = *number;
				return value;
			}
			if (!literal.is_array() || literal.size() != components)
			{
				return MakeError("expected an array of {} finite numbers, not {}", components, Excerpt(literal));
			}
			for (uint32_t component = 0; component < components; component++)
			{
				std::optional<float> const number = Format::ReadFloat(literal[component]);
				if (!number)
				{
					return MakeError("expected an array of {} finite numbers, not {}", components, Excerpt(literal));
				}
				value[static_cast<glm::length_t>(component)] = *number;
			}
			return value;
		}

		// A tangent: a number for a float track, an array of one number per component otherwise.
		Result<glm::vec4> DecodeTangent(Json const& tangent, std::string const& path, uint32_t components)
		{
			glm::vec4 value(0.0f);
			if (components == 1)
			{
				if (std::optional<float> const number = Format::ReadFloat(tangent))
				{
					value.x = *number;
					return value;
				}
				return MakeError("{}: expected a number, not {}", path, Excerpt(tangent));
			}
			if (tangent.is_array() && tangent.size() == components)
			{
				bool read = true;
				for (uint32_t component = 0; component < components && read; component++)
				{
					std::optional<float> const number = Format::ReadFloat(tangent[component]);
					read = number.has_value();
					value[static_cast<glm::length_t>(component)] = number.value_or(0.0f);
				}
				if (read)
				{
					return value;
				}
			}
			return MakeError("{}: expected an array of {} numbers, not {}", path, components, Excerpt(tangent));
		}

		// A float as the literal codec writes it; null for one that is not finite (which reads back as no number).
		Json EncodeNumber(float value)
		{
			std::optional<Json> encoded = EncodeVisualScriptLiteral(ScriptFieldValue::FromFloat(value), "float", {});
			return encoded ? std::move(*encoded) : Json();
		}

		Json EncodeComponents(glm::vec4 value, uint32_t components)
		{
			if (components == 1)
			{
				return EncodeNumber(value.x);
			}
			Json numbers = Json::array();
			for (uint32_t component = 0; component < components; component++)
			{
				numbers.push_back(EncodeNumber(value[static_cast<glm::length_t>(component)]));
			}
			return numbers;
		}

		glm::vec4 GetDefaultValue(std::string_view type)
		{
			return type == ColorType ? glm::vec4(1.0f) : glm::vec4(0.0f);
		}

		// Reads one key of a track (ReadKey of the runtime's TimelineReader, stopping at the first problem).
		Result<VisualScriptTimelineKey> ReadKey(Json const& key, std::string const& path, float length,
		                                        VisualScriptTimelineTrack const& track, std::optional<float> previous)
		{
			bool const event = track.Components == 0;
			if (!key.is_object())
			{
				return MakeError("{}: expected an object with {}, not {}", path, event ? "Time" : "Time and Value", Excerpt(key));
			}
			VisualScriptTimelineKey read;
			std::optional<float> time;
			Json const* value = nullptr;
			Json const* in = nullptr;
			Json const* out = nullptr;
			for (auto const& [name, member] : key.items())
			{
				if (event && name != "Time")
				{
					return MakeError("{}: an event key has only Time, not {}", path, Quote(name));
				}
				if (name == "Time")
				{
					std::optional<float> const seconds = Format::ReadFloat(member);
					if (!seconds || !(*seconds >= 0.0f) || *seconds > length)
					{
						return MakeError("{}.Time: expected a number from 0 to {}, not {}", path, FormatNumber(length), Excerpt(member));
					}
					time = *seconds;
				}
				else if (name == "Value")
				{
					value = &member;
				}
				else if (name == "Interpolation")
				{
					std::optional<VisualScriptKeyInterpolation> const interpolation = ReadInterpolation(member);
					if (!interpolation)
					{
						return MakeError("{}.Interpolation: expected Cubic, Linear or Constant, not {}", path, Excerpt(member));
					}
					read.Interpolation = *interpolation;
				}
				else if (name == "In")
				{
					in = &member;
				}
				else if (name == "Out")
				{
					out = &member;
				}
				else
				{
					return MakeError("{}: a key has only Time, Value, Interpolation, In and Out, not {}", path, Quote(name));
				}
			}
			if (!time)
			{
				return MakeError("{}: a key needs a Time", path);
			}
			if (previous && *time < *previous)
			{
				return MakeError("{}.Time: keys are in time order: {} comes after {}", path, FormatNumber(*time), FormatNumber(*previous));
			}
			read.Time = *time;
			if (event)
			{
				return read;
			}
			if (value == nullptr)
			{
				return MakeError("{}: a key of a {} track needs a Value", path, track.Type);
			}
			Result<glm::vec4> decoded = DecodeValue(*value, track.Components);
			if (!decoded)
			{
				return MakeError("{}.Value: {}", path, decoded.GetError());
			}
			read.Value = decoded.GetValue();
			if (in != nullptr)
			{
				Result<glm::vec4> tangent = DecodeTangent(*in, path + ".In", track.Components);
				if (!tangent)
				{
					return Error{tangent.GetError()};
				}
				read.In = tangent.GetValue();
			}
			if (out != nullptr)
			{
				Result<glm::vec4> tangent = DecodeTangent(*out, path + ".Out", track.Components);
				if (!tangent)
				{
					return Error{tangent.GetError()};
				}
				read.Out = tangent.GetValue();
			}
			return read;
		}

		// A track's members (ReadTrack of the runtime): unknown members first, then the name, then the type, then the keys.
		Result<VisualScriptTimelineTrack> ReadTrack(Json const& track, std::string const& path, float length,
		                                            std::unordered_set<std::string>& names)
		{
			if (!track.is_object())
			{
				return MakeError("{}: expected an object with Name, Type and Keys, not {}", path, Excerpt(track));
			}
			Json const* name = nullptr;
			Json const* type = nullptr;
			Json const* keys = nullptr;
			for (auto const& [member, value] : track.items())
			{
				if (member == "Name")
				{
					name = &value;
				}
				else if (member == "Type")
				{
					type = &value;
				}
				else if (member == "Keys")
				{
					keys = &value;
				}
				else
				{
					return MakeError("{}: a track has only Name, Type and Keys, not {}", path, Quote(member));
				}
			}
			VisualScriptTimelineTrack read;
			if (name == nullptr)
			{
				return MakeError("{}: a track needs a Name", path);
			}
			if (!name->is_string())
			{
				return MakeError("{}.Name: expected a name, not {}", path, Excerpt(*name));
			}
			read.Name = name->get<std::string>();
			if (std::optional<std::string> const problem = CheckName(read.Name))
			{
				return MakeError("{}.Name {}: {}", path, Quote(read.Name), *problem);
			}
			if (VisualScriptEditing::IsReservedPinName(read.Name))
			{
				return MakeError("{}.Name {}: {} is a reserved pin name ({})", path, Quote(read.Name), read.Name, ReservedList);
			}
			if (IsFixedPin(read.Name))
			{
				return MakeError("{}.Name: {} is a pin of the timeline: name the track otherwise", path, read.Name);
			}
			if (!names.insert(read.Name).second)
			{
				return MakeError("{}.Name: a track named {} comes earlier", path, read.Name);
			}
			std::optional<uint32_t> const components = type != nullptr && type->is_string()
			                                               ? GetVisualScriptTimelineComponents(type->get_ref<std::string const&>())
			                                               : std::nullopt;
			if (!components)
			{
				return MakeError("{}.Type: expected {}", path, TrackTypes);
			}
			read.Type = type->get<std::string>();
			read.Components = *components;
			if (keys == nullptr)
			{
				return read;
			}
			if (!keys->is_array())
			{
				return MakeError("{}.Keys: expected an array of keys, not {}", path, Excerpt(*keys));
			}
			if (keys->size() > MaxVisualScriptTimelineKeys)
			{
				return MakeError("{}.Keys: a track has at most {} keys", path, MaxVisualScriptTimelineKeys);
			}
			for (size_t index = 0; index < keys->size(); index++)
			{
				std::optional<float> const previous = read.Keys.empty() ? std::nullopt : std::optional<float>(read.Keys.back().Time);
				Result<VisualScriptTimelineKey> key =
					ReadKey((*keys)[index], fmt::format("{}.Keys[{}]", path, index), length, read, previous);
				if (!key)
				{
					return Error{key.GetError()};
				}
				read.Keys.push_back(key.TakeValue());
			}
			return read;
		}

		Result<VisualScriptTimelineTrack*> GetTrack(VisualScriptTimeline& timeline, uint32_t track)
		{
			if (track >= timeline.Tracks.size())
			{
				return MakeError("the timeline has no track {}", track);
			}
			return &timeline.Tracks[track];
		}

		Result<void> CheckKeys(VisualScriptTimeline const& timeline, std::span<VisualScriptTimelineKeyRef const> keys)
		{
			for (auto const& [track, key] : keys)
			{
				if (track >= timeline.Tracks.size() || key >= timeline.Tracks[track].Keys.size())
				{
					return MakeError("the timeline has no key {} of track {}", key, track);
				}
			}
			return {};
		}

		// Applies an edit to each curve key among keys; fails when there is none.
		template<typename TEdit>
		Result<void> EditCurveKeys(VisualScriptTimeline& timeline, std::span<VisualScriptTimelineKeyRef const> keys, TEdit&& edit)
		{
			if (Result<void> checked = CheckKeys(timeline, keys); !checked)
			{
				return checked;
			}
			bool any = false;
			for (auto const& [track, key] : keys)
			{
				if (timeline.Tracks[track].Components > 0)
				{
					any = true;
				}
			}
			if (!any)
			{
				return Error{"keys of event tracks have no values, interpolation or tangents"};
			}
			for (auto const& [track, key] : keys)
			{
				if (timeline.Tracks[track].Components > 0)
				{
					edit(timeline.Tracks[track].Keys[key]);
				}
			}
			return {};
		}
	}

	std::optional<uint32_t> GetVisualScriptTimelineComponents(std::string_view type)
	{
		if (type == "float")
		{
			return 1;
		}
		if (type == "Strada.Vector2")
		{
			return 2;
		}
		if (type == "Strada.Vector3")
		{
			return 3;
		}
		if (type == "Strada.Vector4" || type == ColorType)
		{
			return 4;
		}
		if (type == VisualScriptTimelineEventType)
		{
			return 0;
		}
		return std::nullopt;
	}

	std::string_view GetVisualScriptTimelineTypeLabel(std::string_view type)
	{
		for (VisualScriptTimelineTrackKind const& kind : VisualScriptTimelineTrackKinds)
		{
			if (kind.Type == type)
			{
				return kind.Label;
			}
		}
		return type;
	}

	Result<VisualScriptTimeline> ParseVisualScriptTimeline(Json const& setting)
	{
		if (!setting.is_object())
		{
			return MakeError("Timeline: expected an object with Length, Loop and Tracks, not {}", Excerpt(setting));
		}
		VisualScriptTimeline timeline;
		Json const* tracks = nullptr;
		for (auto const& [name, member] : setting.items())
		{
			if (name == "Length")
			{
				std::optional<float> const length = Format::ReadFloat(member);
				if (!length || !(*length > 0.0f) || *length > MaxVisualScriptTimelineLength)
				{
					return MakeError("Timeline.Length: expected a number greater than 0 and at most {}",
					                 FormatNumber(MaxVisualScriptTimelineLength));
				}
				timeline.Length = *length;
			}
			else if (name == "Loop")
			{
				if (!member.is_boolean())
				{
					return MakeError("Timeline.Loop: expected true or false, not {}", Excerpt(member));
				}
				timeline.Loop = member.get<bool>();
			}
			else if (name == "Tracks")
			{
				tracks = &member;
			}
			else
			{
				return MakeError("Timeline: a timeline has only Length, Loop and Tracks, not {}", Quote(name));
			}
		}
		if (tracks == nullptr)
		{
			return timeline;
		}
		if (!tracks->is_array())
		{
			return MakeError("Timeline.Tracks: expected an array of tracks, not {}", Excerpt(*tracks));
		}
		if (tracks->size() > MaxVisualScriptTimelineTracks)
		{
			return MakeError("Timeline.Tracks: a timeline has at most {} tracks", MaxVisualScriptTimelineTracks);
		}
		std::unordered_set<std::string> names;
		for (size_t index = 0; index < tracks->size(); index++)
		{
			Result<VisualScriptTimelineTrack> track =
				ReadTrack((*tracks)[index], fmt::format("Timeline.Tracks[{}]", index), timeline.Length, names);
			if (!track)
			{
				return Error{track.GetError()};
			}
			timeline.Tracks.push_back(track.TakeValue());
		}
		return timeline;
	}

	Json WriteVisualScriptTimeline(VisualScriptTimeline const& timeline)
	{
		Json tracks = Json::array();
		for (VisualScriptTimelineTrack const& track : timeline.Tracks)
		{
			Json keys = Json::array();
			for (VisualScriptTimelineKey const& key : track.Keys)
			{
				Json written = Json::object();
				written["Time"] = EncodeNumber(key.Time);
				if (track.Components > 0)
				{
					written["Value"] = EncodeComponents(key.Value, track.Components);
					if (key.Interpolation != VisualScriptKeyInterpolation::Cubic)
					{
						written["Interpolation"] = GetInterpolationName(key.Interpolation);
					}
					if (key.In)
					{
						written["In"] = EncodeComponents(*key.In, track.Components);
					}
					if (key.Out)
					{
						written["Out"] = EncodeComponents(*key.Out, track.Components);
					}
				}
				keys.push_back(std::move(written));
			}
			Json written = Json::object();
			written["Name"] = track.Name;
			written["Type"] = track.Type;
			written["Keys"] = std::move(keys);
			tracks.push_back(std::move(written));
		}
		Json written = Json::object();
		written["Length"] = EncodeNumber(timeline.Length);
		written["Loop"] = timeline.Loop;
		written["Tracks"] = std::move(tracks);
		return written;
	}

	Result<VisualScriptTimeline> ReadVisualScriptTimeline(VisualScriptNode const& node)
	{
		if (!node.Settings.is_object())
		{
			return VisualScriptTimeline{};
		}
		auto const found = node.Settings.find(VisualScriptTimelineSetting);
		if (found == node.Settings.end())
		{
			return VisualScriptTimeline{};
		}
		return ParseVisualScriptTimeline(*found);
	}

	Result<void> CheckVisualScriptTimeline(VisualScriptTimeline const& timeline)
	{
		for (VisualScriptTimelineTrack const& track : timeline.Tracks)
		{
			if (GetVisualScriptTimelineComponents(track.Type) != track.Components)
			{
				return MakeError("track {} of type {} has {} components", track.Name, track.Type, track.Components);
			}
		}
		Result<VisualScriptTimeline> const read = ParseVisualScriptTimeline(WriteVisualScriptTimeline(timeline));
		if (!read)
		{
			return Error{read.GetError()};
		}
		return {};
	}

	std::vector<std::pair<std::string, std::string>> ListVisualScriptTimelineTrackPins(Json const& setting)
	{
		std::vector<std::pair<std::string, std::string>> pins;
		if (!setting.is_object())
		{
			return pins;
		}
		auto const tracks = setting.find("Tracks");
		if (tracks == setting.end() || !tracks->is_array() || tracks->size() > MaxVisualScriptTimelineTracks)
		{
			return pins;
		}
		std::unordered_set<std::string> names;
		for (Json const& track : *tracks)
		{
			if (!track.is_object())
			{
				continue;
			}
			auto const name = track.find("Name");
			if (name == track.end() || !name->is_string())
			{
				continue;
			}
			std::string const& text = name->get_ref<std::string const&>();
			if (CheckName(text) || VisualScriptEditing::IsReservedPinName(text) || IsFixedPin(text) || !names.insert(text).second)
			{
				continue;
			}
			auto const type = track.find("Type");
			if (type == track.end() || !type->is_string() || !GetVisualScriptTimelineComponents(type->get_ref<std::string const&>()))
			{
				continue;
			}
			pins.emplace_back(text, type->get<std::string>());
		}
		return pins;
	}

	std::optional<std::string> CheckVisualScriptTimelineTrackName(VisualScriptTimeline const& timeline, std::string_view name,
	                                                              std::optional<uint32_t> except)
	{
		if (std::optional<std::string> problem = CheckName(name))
		{
			return problem;
		}
		if (VisualScriptEditing::IsReservedPinName(name))
		{
			return fmt::format("{} is a reserved pin name ({})", name, ReservedList);
		}
		if (IsFixedPin(name))
		{
			return fmt::format("{} is a pin of the timeline: name the track otherwise", name);
		}
		for (size_t index = 0; index < timeline.Tracks.size(); index++)
		{
			if ((!except || index != *except) && timeline.Tracks[index].Name == name)
			{
				return fmt::format("the timeline has a track named {}", name);
			}
		}
		return std::nullopt;
	}

	std::string MakeVisualScriptTimelineTrackName(VisualScriptTimeline const& timeline, std::string_view base)
	{
		std::string name(base);
		for (uint32_t suffix = 2; CheckVisualScriptTimelineTrackName(timeline, name); suffix++)
		{
			name = fmt::format("{}{}", base, suffix);
		}
		return name;
	}

	glm::vec4 EvaluateVisualScriptTimelineTrack(VisualScriptTimelineTrack const& track, double time)
	{
		glm::vec4 result(0.0f);
		uint32_t const components = std::min<uint32_t>(track.Components, 4);
		if (components == 0)
		{
			return result;
		}
		if (track.Keys.empty())
		{
			glm::vec4 const fallback = GetDefaultValue(track.Type);
			for (glm::length_t component = 0; component < static_cast<glm::length_t>(components); component++)
			{
				result[component] = fallback[component];
			}
			return result;
		}
		std::array<double, 4> values{};
		EvaluateCurveKeys(std::span<VisualScriptTimelineKey const>(track.Keys), time, std::span<double>(values.data(), components));
		for (glm::length_t component = 0; component < static_cast<glm::length_t>(components); component++)
		{
			result[component] = static_cast<float>(values[static_cast<size_t>(component)]);
		}
		return result;
	}

	std::pair<glm::vec4, glm::vec4> GetVisualScriptTimelineKeyTangents(VisualScriptTimelineTrack const& track, uint32_t key)
	{
		std::pair<glm::vec4, glm::vec4> tangents(glm::vec4(0.0f), glm::vec4(0.0f));
		std::span<VisualScriptTimelineKey const> const keys(track.Keys);
		// Every component of the stored vectors, as the tangents are kept (the components a track does not have read as stored).
		for (uint32_t component = 0; component < 4; component++)
		{
			std::pair<double, double> const slopes = GetCurveKeySlopes(keys, key, component);
			tangents.first[static_cast<glm::length_t>(component)] = static_cast<float>(slopes.first);
			tangents.second[static_cast<glm::length_t>(component)] = static_cast<float>(slopes.second);
		}
		return tangents;
	}

	Json MakeVisualScriptTimelineValueLiteral(VisualScriptTimelineTrack const& track, glm::vec4 value)
	{
		return EncodeComponents(value, std::max<uint32_t>(track.Components, 1));
	}

	std::optional<glm::vec4> ReadVisualScriptTimelineValueLiteral(VisualScriptTimelineTrack const& track, Json const& literal)
	{
		if (track.Components == 0)
		{
			return std::nullopt;
		}
		Result<glm::vec4> const value = DecodeValue(literal, track.Components);
		return value ? std::optional<glm::vec4>(value.GetValue()) : std::nullopt;
	}

	Result<void> MoveVisualScriptTimelineKeys(VisualScriptTimeline& timeline, std::span<VisualScriptTimelineKeyRef const> keys,
	                                          float timeOffset, glm::vec4 valueOffset)
	{
		if (Result<void> checked = CheckKeys(timeline, keys); !checked)
		{
			return checked;
		}
		if (!std::isfinite(timeOffset) || !std::isfinite(valueOffset.x) || !std::isfinite(valueOffset.y) || !std::isfinite(valueOffset.z) ||
		    !std::isfinite(valueOffset.w))
		{
			return Error{"the offsets must be finite numbers"};
		}
		std::set<VisualScriptTimelineKeyRef> const selected(keys.begin(), keys.end());
		for (auto const& [track, key] : selected)
		{
			VisualScriptTimelineTrack const& owner = timeline.Tracks[track];
			for (glm::length_t component = 0; component < static_cast<glm::length_t>(owner.Components); component++)
			{
				if (!std::isfinite(owner.Keys[key].Value[component] + valueOffset[component]))
				{
					return Error{"a key's value is finite: the move takes it past the largest float"};
				}
			}
		}
		// The time offsets the block may take: no selected key leaves [0, Length] or passes an unselected neighbour.
		double lowest = -std::numeric_limits<double>::infinity();
		double highest = std::numeric_limits<double>::infinity();
		for (auto const& [track, key] : selected)
		{
			std::vector<VisualScriptTimelineKey> const& trackKeys = timeline.Tracks[track].Keys;
			double const time = trackKeys[key].Time;
			double lower = 0.0;
			double upper = timeline.Length;
			for (uint32_t before = key; before-- > 0;)
			{
				if (!selected.contains({track, before}))
				{
					lower = std::max(lower, static_cast<double>(trackKeys[before].Time));
					break;
				}
			}
			for (uint32_t after = key + 1; after < trackKeys.size(); after++)
			{
				if (!selected.contains({track, after}))
				{
					upper = std::min(upper, static_cast<double>(trackKeys[after].Time));
					break;
				}
			}
			lowest = std::max(lowest, lower - time);
			highest = std::min(highest, upper - time);
		}
		double const offset =
			selected.empty() ? 0.0 : std::clamp(static_cast<double>(timeOffset), std::min(lowest, 0.0), std::max(highest, 0.0));
		for (auto const& [track, key] : selected)
		{
			VisualScriptTimelineTrack& edited = timeline.Tracks[track];
			VisualScriptTimelineKey& moved = edited.Keys[key];
			// The offset keeps the sum within the float bounds of the block in double, so the float it rounds to stays within
			// them too (rounding is monotonic and the bounds are floats).
			float const time = static_cast<float>(static_cast<double>(moved.Time) + offset);
			moved.Time = time;
			if (edited.Components > 0)
			{
				for (glm::length_t component = 0; component < static_cast<glm::length_t>(edited.Components); component++)
				{
					moved.Value[component] += valueOffset[component];
				}
			}
		}
		return {};
	}

	Result<void> SetVisualScriptTimelineLength(VisualScriptTimeline& timeline, float length)
	{
		if (!(length > 0.0f) || length > MaxVisualScriptTimelineLength)
		{
			return MakeError("a timeline's length is greater than 0 and at most {} seconds", FormatNumber(MaxVisualScriptTimelineLength));
		}
		timeline.Length = length;
		for (VisualScriptTimelineTrack& track : timeline.Tracks)
		{
			for (VisualScriptTimelineKey& key : track.Keys)
			{
				key.Time = std::min(key.Time, length);
			}
		}
		return {};
	}

	uint32_t CountVisualScriptTimelineKeysAfter(VisualScriptTimeline const& timeline, float time)
	{
		uint32_t count = 0;
		for (VisualScriptTimelineTrack const& track : timeline.Tracks)
		{
			count += static_cast<uint32_t>(std::count_if(track.Keys.begin(), track.Keys.end(),
			                                             [time](VisualScriptTimelineKey const& key)
			                                             {
															 return key.Time > time;
														 }));
		}
		return count;
	}

	Result<uint32_t> AddVisualScriptTimelineKey(VisualScriptTimeline& timeline, uint32_t track, float time, glm::vec4 value)
	{
		Result<VisualScriptTimelineTrack*> found = GetTrack(timeline, track);
		if (!found)
		{
			return Error{found.GetError()};
		}
		VisualScriptTimelineTrack& edited = *found.GetValue();
		if (!(time >= 0.0f) || time > timeline.Length)
		{
			return MakeError("a key's time is from 0 to the timeline's length, {} s", FormatNumber(timeline.Length));
		}
		if (edited.Keys.size() >= MaxVisualScriptTimelineKeys)
		{
			return MakeError("a track has at most {} keys", MaxVisualScriptTimelineKeys);
		}
		if (edited.Components > 0 &&
		    !(std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w)))
		{
			return Error{"a key's value is finite"};
		}
		VisualScriptTimelineKey key;
		key.Time = time;
		if (edited.Components > 0)
		{
			for (glm::length_t component = 0; component < static_cast<glm::length_t>(edited.Components); component++)
			{
				key.Value[component] = value[component];
			}
		}
		auto const position = std::upper_bound(edited.Keys.begin(), edited.Keys.end(), time,
		                                       [](float candidate, VisualScriptTimelineKey const& stored)
		                                       {
												   return candidate < stored.Time;
											   });
		auto const index = static_cast<uint32_t>(std::distance(edited.Keys.begin(), position));
		edited.Keys.insert(position, key);
		return index;
	}

	Result<void> RemoveVisualScriptTimelineKeys(VisualScriptTimeline& timeline, std::span<VisualScriptTimelineKeyRef const> keys)
	{
		if (Result<void> checked = CheckKeys(timeline, keys); !checked)
		{
			return checked;
		}
		std::set<VisualScriptTimelineKeyRef> const removed(keys.begin(), keys.end());
		for (uint32_t track = 0; track < timeline.Tracks.size(); track++)
		{
			std::vector<VisualScriptTimelineKey>& trackKeys = timeline.Tracks[track].Keys;
			// remove_if reads each element before anything is moved onto it, so its address still says its index.
			VisualScriptTimelineKey const* const first = trackKeys.data();
			std::erase_if(trackKeys,
			              [&removed, track, first](VisualScriptTimelineKey const& key)
			              {
							  return removed.contains({track, static_cast<uint32_t>(&key - first)});
						  });
		}
		return {};
	}

	Result<void> SetVisualScriptTimelineKeyValue(VisualScriptTimeline& timeline, VisualScriptTimelineKeyRef key, glm::vec4 value)
	{
		if (!(std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w)))
		{
			return Error{"a key's value is finite"};
		}
		std::array<VisualScriptTimelineKeyRef, 1> const keys = {key};
		return EditCurveKeys(timeline, keys,
		                     [value](VisualScriptTimelineKey& edited)
		                     {
								 edited.Value = value;
							 });
	}

	Result<void> SetVisualScriptTimelineKeyInterpolation(VisualScriptTimeline& timeline, std::span<VisualScriptTimelineKeyRef const> keys,
	                                                     VisualScriptKeyInterpolation interpolation)
	{
		return EditCurveKeys(timeline, keys,
		                     [interpolation](VisualScriptTimelineKey& edited)
		                     {
								 edited.Interpolation = interpolation;
							 });
	}

	Result<void> SetVisualScriptTimelineKeyTangents(VisualScriptTimeline& timeline, VisualScriptTimelineKeyRef key,
	                                                std::optional<glm::vec4> in, std::optional<glm::vec4> out)
	{
		auto const finite = [](std::optional<glm::vec4> const& tangent)
		{
			return !tangent ||
			       (std::isfinite(tangent->x) && std::isfinite(tangent->y) && std::isfinite(tangent->z) && std::isfinite(tangent->w));
		};
		if (!finite(in) || !finite(out))
		{
			return Error{"a tangent is a finite slope"};
		}
		std::array<VisualScriptTimelineKeyRef, 1> const keys = {key};
		return EditCurveKeys(timeline, keys,
		                     [&in, &out](VisualScriptTimelineKey& edited)
		                     {
								 edited.In = in;
								 edited.Out = out;
							 });
	}

	Result<void> FlattenVisualScriptTimelineTangents(VisualScriptTimeline& timeline, std::span<VisualScriptTimelineKeyRef const> keys)
	{
		return EditCurveKeys(timeline, keys,
		                     [](VisualScriptTimelineKey& edited)
		                     {
								 edited.In = glm::vec4(0.0f);
								 edited.Out = glm::vec4(0.0f);
							 });
	}

	Result<void> ResetVisualScriptTimelineTangents(VisualScriptTimeline& timeline, std::span<VisualScriptTimelineKeyRef const> keys)
	{
		return EditCurveKeys(timeline, keys,
		                     [](VisualScriptTimelineKey& edited)
		                     {
								 edited.In.reset();
								 edited.Out.reset();
							 });
	}

	Result<uint32_t> AddVisualScriptTimelineTrack(VisualScriptTimeline& timeline, std::string name, std::string type)
	{
		if (timeline.Tracks.size() >= MaxVisualScriptTimelineTracks)
		{
			return MakeError("a timeline has at most {} tracks", MaxVisualScriptTimelineTracks);
		}
		std::optional<uint32_t> const components = GetVisualScriptTimelineComponents(type);
		if (!components)
		{
			return MakeError("a track's type is {}, not {}", TrackTypes, type);
		}
		if (std::optional<std::string> const problem = CheckVisualScriptTimelineTrackName(timeline, name))
		{
			return Error{*problem};
		}
		VisualScriptTimelineTrack track;
		track.Name = std::move(name);
		track.Type = std::move(type);
		track.Components = *components;
		timeline.Tracks.push_back(std::move(track));
		return static_cast<uint32_t>(timeline.Tracks.size() - 1);
	}

	Result<void> RemoveVisualScriptTimelineTrack(VisualScriptTimeline& timeline, uint32_t track)
	{
		if (track >= timeline.Tracks.size())
		{
			return MakeError("the timeline has no track {}", track);
		}
		timeline.Tracks.erase(timeline.Tracks.begin() + track);
		return {};
	}

	Result<void> RenameVisualScriptTimelineTrack(VisualScriptTimeline& timeline, uint32_t track, std::string name)
	{
		Result<VisualScriptTimelineTrack*> found = GetTrack(timeline, track);
		if (!found)
		{
			return Error{found.GetError()};
		}
		if (std::optional<std::string> const problem = CheckVisualScriptTimelineTrackName(timeline, name, track))
		{
			return Error{*problem};
		}
		found.GetValue()->Name = std::move(name);
		return {};
	}

	Result<void> SetVisualScriptTimelineTrackType(VisualScriptTimeline& timeline, uint32_t track, std::string type)
	{
		Result<VisualScriptTimelineTrack*> found = GetTrack(timeline, track);
		if (!found)
		{
			return Error{found.GetError()};
		}
		std::optional<uint32_t> const components = GetVisualScriptTimelineComponents(type);
		if (!components)
		{
			return MakeError("a track's type is {}, not {}", TrackTypes, type);
		}
		VisualScriptTimelineTrack& edited = *found.GetValue();
		glm::vec4 const fallback = GetDefaultValue(type);
		for (VisualScriptTimelineKey& key : edited.Keys)
		{
			VisualScriptTimelineKey changed;
			changed.Time = key.Time;
			if (*components > 0)
			{
				changed.Value = fallback;
				changed.Interpolation = key.Interpolation;
				uint32_t const shared = std::min(edited.Components, *components);
				for (glm::length_t component = 0; component < static_cast<glm::length_t>(shared); component++)
				{
					changed.Value[component] = key.Value[component];
				}
				if (shared > 0 && key.In)
				{
					changed.In = glm::vec4(0.0f);
					for (glm::length_t component = 0; component < static_cast<glm::length_t>(shared); component++)
					{
						(*changed.In)[component] = (*key.In)[component];
					}
				}
				if (shared > 0 && key.Out)
				{
					changed.Out = glm::vec4(0.0f);
					for (glm::length_t component = 0; component < static_cast<glm::length_t>(shared); component++)
					{
						(*changed.Out)[component] = (*key.Out)[component];
					}
				}
			}
			key = changed;
		}
		edited.Type = std::move(type);
		edited.Components = *components;
		return {};
	}
}
