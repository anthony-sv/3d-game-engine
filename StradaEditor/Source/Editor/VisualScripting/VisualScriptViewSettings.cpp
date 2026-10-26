#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Editor/RecentProjects.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Utf8.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>

namespace Strada
{
	template<>
	struct EnumTraits<VisualScriptScrollMode>
	{
		static constexpr std::array<std::pair<VisualScriptScrollMode, std::string_view>, 2> Values = {{
			{VisualScriptScrollMode::Zoom, "Zoom"},
			{VisualScriptScrollMode::Pan, "Pan"},
		}};
	};

	// A view's centre may reach every point a document can place something at, and no further.
	static_assert(VisualScriptViewSettings::MaxCenter == static_cast<float>(VisualScriptSerializer::MaxCoordinate));

	namespace
	{
		using Settings = VisualScriptViewSettings;

		constexpr std::string_view FileType = "VisualScriptViews";
		// Project paths are UTF-8 text of at most this many bytes.
		constexpr size_t MaxPathBytes = 4096;

		bool IsControlCharacter(char character)
		{
			unsigned char const code = static_cast<unsigned char>(character);
			return code < 0x20 || code == 0x7F;
		}

		// 1 to maxBytes bytes of UTF-8 without control characters: type IDs, node types, hint IDs and paths.
		bool IsValidText(std::string_view text, size_t maxBytes)
		{
			return !text.empty() && text.size() <= maxBytes && std::none_of(text.begin(), text.end(), IsControlCharacter) &&
			       Utf8::IsValid(text);
		}

		// The longest start of valid UTF-8 text that has at most maxBytes and ends at a character boundary.
		std::string_view CutText(std::string_view text, size_t maxBytes)
		{
			if (text.size() <= maxBytes)
			{
				return text;
			}
			size_t end = maxBytes;
			while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
			{
				end--;
			}
			return text.substr(0, end);
		}

		// A bookmark's name as it is kept: its first line (up to a control character), invalid bytes replaced, at most
		// MaxBookmarkNameBytes; "Bookmark <slot>" when nothing is left.
		std::string MakeBookmarkName(std::string_view name, uint8_t slot)
		{
			auto const control = std::find_if(name.begin(), name.end(), IsControlCharacter);
			std::string const line = Utf8::Sanitize(name.substr(0, static_cast<size_t>(control - name.begin())));
			std::string_view const cut = CutText(line, Settings::MaxBookmarkNameBytes);
			return cut.empty() ? fmt::format("Bookmark {}", slot) : std::string(cut);
		}

		// The view with its centre and zoom moved into their ranges; nothing when a number is not finite.
		std::optional<VisualScriptViewState> ClampView(VisualScriptViewState view)
		{
			if (!std::isfinite(view.Center.x) || !std::isfinite(view.Center.y) || !std::isfinite(view.Zoom))
			{
				return std::nullopt;
			}
			view.Center = glm::clamp(view.Center, glm::vec2(-Settings::MaxCenter), glm::vec2(Settings::MaxCenter));
			view.Zoom = std::clamp(view.Zoom, VisualScriptMinZoom, VisualScriptMaxZoom);
			return view;
		}

		bool IsValidCommentColor(glm::vec4 color)
		{
			for (glm::length_t channel = 0; channel < 4; channel++)
			{
				// Also false for NaN.
				if (!(color[channel] >= 0.0f && color[channel] <= 1.0f))
				{
					return false;
				}
			}
			return true;
		}

		// Moves the element at index to the front, keeping the order of the others.
		template<typename T>
		void MoveToFront(std::vector<T>& values, size_t index)
		{
			auto const element = values.begin() + static_cast<std::ptrdiff_t>(index);
			std::rotate(values.begin(), element, element + 1);
		}

		Json const* FindMember(Json const& object, char const* key)
		{
			auto const found = object.find(key);
			return found != object.end() ? &*found : nullptr;
		}

		std::string GetMemberPath(std::string_view objectPath, std::string_view key)
		{
			return objectPath.empty() ? std::string(key) : fmt::format("{}.{}", objectPath, key);
		}

		std::optional<uint32_t> ReadUInt32(Json const& value)
		{
			if (value.is_number_unsigned())
			{
				uint64_t const number = value.get<uint64_t>();
				if (number <= std::numeric_limits<uint32_t>::max())
				{
					return static_cast<uint32_t>(number);
				}
			}
			else if (value.is_number_integer())
			{
				int64_t const number = value.get<int64_t>();
				if (number >= 0 && number <= static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
				{
					return static_cast<uint32_t>(number);
				}
			}
			return std::nullopt;
		}

		// The float a number of the file reads as (the nearest one), moved into [minimum, maximum]; clamped tells whether it
		// had to move. Numbers beyond the range of float move to the nearest bound too. Nothing for what is not a number.
		std::optional<float> ReadClamped(Json const& value, float minimum, float maximum, bool& clamped)
		{
			double const number = value.is_number() ? value.get<double>() : std::numeric_limits<double>::quiet_NaN();
			if (std::isnan(number))
			{
				return std::nullopt;
			}
			if (std::abs(number) > static_cast<double>(std::numeric_limits<float>::max()))
			{
				clamped = true;
				return number < 0.0 ? minimum : maximum;
			}
			float const nearest = static_cast<float>(number);
			float const bounded = std::clamp(nearest, minimum, maximum);
			clamped = bounded != nearest;
			return bounded;
		}

		// A handle or entity ID: a decimal string of a valid (non-zero) ID.
		std::optional<UUID> ReadID(Json const& value)
		{
			if (!value.is_string())
			{
				return std::nullopt;
			}
			std::optional<UUID> const id = UUID::FromString(value.get_ref<std::string const&>());
			return id && id->IsValid() ? id : std::nullopt;
		}

		// Reads the views file's content object, reporting every part it skips or changes.
		class ViewsReader
		{
		public:
			explicit ViewsReader(std::vector<std::string>& warnings)
				: m_Warnings(warnings)
			{
			}

			VisualScriptViews Read(Json const& content)
			{
				VisualScriptViews views;
				ReadPanes(content, views.Panes);
				ReadBool(content, "ContextPalette", views.ContextPalette);
				ReadBool(content, "ReduceMotion", views.ReduceMotion);
				if (Json const* scroll = FindMember(content, "Scroll"))
				{
					std::optional<VisualScriptScrollMode> const mode =
						scroll->is_string() ? EnumFromString<VisualScriptScrollMode>(scroll->get_ref<std::string const&>()) : std::nullopt;
					if (mode)
					{
						views.Scroll = mode;
					}
					else
					{
						Warn("Scroll", "expected \"Zoom\" or \"Pan\"; the platform's default is used");
					}
				}
				ReadBool(content, "SnapToGrid", views.SnapToGrid);
				ReadBool(content, "MoveCommentContents", views.MoveCommentContents);
				ReadRecents(content, views.Recents);
				ReadTexts(content, "RecentTypes", VisualScriptSerializer::MaxTypeBytes, Settings::MaxRecentTypes, views.RecentTypes);
				if (Json const* color = FindMember(content, "CommentColor"))
				{
					glm::vec4 value{0.0f};
					if (JsonTraits<glm::vec4>::FromJson(*color, value, DeserializationContext()).IsOk() && IsValidCommentColor(value))
					{
						views.CommentColor = value;
					}
					else
					{
						Warn("CommentColor", "expected four numbers from 0 to 1; the default comment colour is used");
					}
				}
				if (Json const* type = FindMember(content, "LastVariableType"))
				{
					if (type->is_string() && IsValidText(type->get_ref<std::string const&>(), VisualScriptSerializer::MaxTypeBytes))
					{
						views.LastVariableType = type->get<std::string>();
					}
					else
					{
						Warn("LastVariableType", fmt::format("expected a type ID; \"{}\" is used", views.LastVariableType));
					}
				}
				ReadTexts(content, "HintsSeen", Settings::MaxHintBytes, Settings::MaxHints, views.HintsSeen);
				ReadProjects(content, views.Projects);
				return views;
			}

		private:
			void Warn(std::string_view path, std::string_view message) { m_Warnings.push_back(fmt::format("{}: {}", path, message)); }

			// Reads the elements of an array member with read, which returns the element to keep (or nothing, having
			// warned), until limit elements are kept.
			template<typename T, typename Read>
			void ReadList(Json const& object, std::string_view objectPath, char const* key, size_t limit, std::vector<T>& values,
			              Read&& read)
			{
				Json const* list = FindMember(object, key);
				if (list == nullptr)
				{
					return;
				}
				std::string const path = GetMemberPath(objectPath, key);
				if (!list->is_array())
				{
					Warn(path, "expected an array; ignored");
					return;
				}
				for (size_t index = 0; index < list->size(); index++)
				{
					if (values.size() == limit)
					{
						Warn(path, fmt::format("more than {} entries; the rest are dropped", limit));
						return;
					}
					if (std::optional<T> value = read((*list)[index], fmt::format("{}[{}]", path, index)))
					{
						values.push_back(std::move(*value));
					}
				}
			}

			void ReadBool(Json const& object, char const* key, bool& value, std::string_view objectPath = {})
			{
				Json const* member = FindMember(object, key);
				if (member == nullptr)
				{
					return;
				}
				if (member->is_boolean())
				{
					value = member->get<bool>();
					return;
				}
				Warn(GetMemberPath(objectPath, key), fmt::format("expected true or false; {} is used", value));
			}

			void ReadWidth(Json const& panes, char const* key, float minimum, float& width)
			{
				Json const* member = FindMember(panes, key);
				if (member == nullptr)
				{
					return;
				}
				std::string const path = GetMemberPath("Panes", key);
				bool clamped = false;
				std::optional<float> const value = ReadClamped(*member, minimum, Settings::MaxPaneWidth, clamped);
				if (!value)
				{
					Warn(path, fmt::format("expected a width in font sizes; {} is used", width));
					return;
				}
				width = *value;
				if (clamped)
				{
					Warn(path, fmt::format("outside {} to {}; clamped", minimum, Settings::MaxPaneWidth));
				}
			}

			void ReadPanes(Json const& content, VisualScriptPaneSettings& panes)
			{
				Json const* member = FindMember(content, "Panes");
				if (member == nullptr)
				{
					return;
				}
				if (!member->is_object())
				{
					Warn("Panes", "expected an object; the defaults are used");
					return;
				}
				ReadWidth(*member, "Sidebar", Settings::MinSidebarWidth, panes.Sidebar);
				ReadWidth(*member, "Details", Settings::MinDetailsWidth, panes.Details);
				ReadBool(*member, "ShowSidebar", panes.ShowSidebar, "Panes");
				ReadBool(*member, "ShowDetails", panes.ShowDetails, "Panes");
				ReadBool(*member, "ShowMinimap", panes.ShowMinimap, "Panes");
			}

			void ReadRecents(Json const& content, std::vector<VisualScriptRecentNode>& recents)
			{
				ReadList(content, {}, "Recents", Settings::MaxRecents, recents,
				         [&](Json const& entry, std::string const& path) -> std::optional<VisualScriptRecentNode>
				         {
							 Json const* type = entry.is_object() ? FindMember(entry, "Type") : nullptr;
							 if (type == nullptr || !type->is_string() ||
					             !IsValidText(type->get_ref<std::string const&>(), VisualScriptSerializer::MaxNodeTypeBytes))
							 {
								 Warn(path, "expected an object with a node Type; skipped");
								 return std::nullopt;
							 }
							 VisualScriptRecentNode node;
							 node.Type = type->get<std::string>();
							 if (Json const* settings = FindMember(entry, "Settings"))
							 {
								 if (!settings->is_object())
								 {
									 Warn(GetMemberPath(path, "Settings"), "expected an object; the entry is skipped");
									 return std::nullopt;
								 }
								 node.Settings = *settings;
							 }
							 if (std::find(recents.begin(), recents.end(), node) != recents.end())
							 {
								 Warn(path, "repeats an earlier entry; skipped");
								 return std::nullopt;
							 }
							 return node;
						 });
			}

			void ReadTexts(Json const& object, char const* key, size_t maxBytes, size_t limit, std::vector<std::string>& texts)
			{
				ReadList(object, {}, key, limit, texts,
				         [&](Json const& entry, std::string const& path) -> std::optional<std::string>
				         {
							 if (!entry.is_string() || !IsValidText(entry.get_ref<std::string const&>(), maxBytes))
							 {
								 Warn(path, fmt::format("expected text of 1 to {} bytes without control characters; skipped", maxBytes));
								 return std::nullopt;
							 }
							 if (std::find(texts.begin(), texts.end(), entry.get_ref<std::string const&>()) != texts.end())
							 {
								 Warn(path, "repeats an earlier entry; skipped");
								 return std::nullopt;
							 }
							 return entry.get<std::string>();
						 });
			}

			// The Function, Center and Zoom members of a view or bookmark: nothing (having warned) when one is missing or not
			// valid; the centre and zoom are clamped into their ranges.
			std::optional<VisualScriptViewState> ReadView(Json const& entry, std::string const& path, std::string_view what)
			{
				VisualScriptViewState view;
				Json const* function = FindMember(entry, "Function");
				std::optional<uint32_t> const functionID = function != nullptr ? ReadUInt32(*function) : std::nullopt;
				if (!functionID)
				{
					Warn(GetMemberPath(path, "Function"), fmt::format("expected 0 or a function's ID; the {} is skipped", what));
					return std::nullopt;
				}
				view.Function = *functionID;
				Json const* center = FindMember(entry, "Center");
				bool clamped = false;
				bool centerValid = center != nullptr && center->is_array() && center->size() == 2;
				for (glm::length_t axis = 0; centerValid && axis < 2; axis++)
				{
					bool axisClamped = false;
					std::optional<float> const coordinate =
						ReadClamped((*center)[static_cast<size_t>(axis)], -Settings::MaxCenter, Settings::MaxCenter, axisClamped);
					centerValid = coordinate.has_value();
					view.Center[axis] = coordinate.value_or(0.0f);
					clamped = clamped || axisClamped;
				}
				if (!centerValid)
				{
					Warn(GetMemberPath(path, "Center"), fmt::format("expected two numbers; the {} is skipped", what));
					return std::nullopt;
				}
				if (clamped)
				{
					Warn(GetMemberPath(path, "Center"),
					     fmt::format("outside -{0} to {0}; clamped", static_cast<int64_t>(Settings::MaxCenter)));
				}
				Json const* zoom = FindMember(entry, "Zoom");
				bool zoomClamped = false;
				std::optional<float> const zoomValue =
					zoom != nullptr ? ReadClamped(*zoom, VisualScriptMinZoom, VisualScriptMaxZoom, zoomClamped) : std::nullopt;
				if (!zoomValue)
				{
					Warn(GetMemberPath(path, "Zoom"), fmt::format("expected a number; the {} is skipped", what));
					return std::nullopt;
				}
				view.Zoom = *zoomValue;
				if (zoomClamped)
				{
					Warn(GetMemberPath(path, "Zoom"), fmt::format("outside {} to {}; clamped", VisualScriptMinZoom, VisualScriptMaxZoom));
				}
				return view;
			}

			std::optional<VisualScriptBookmark> ReadBookmark(Json const& entry, std::string const& path,
			                                                 std::vector<VisualScriptBookmark> const& bookmarks)
			{
				if (!entry.is_object())
				{
					Warn(path, "expected an object; skipped");
					return std::nullopt;
				}
				Json const* slot = FindMember(entry, "Slot");
				std::optional<uint32_t> const slotNumber = slot != nullptr ? ReadUInt32(*slot) : std::nullopt;
				if (!slotNumber || *slotNumber < 1 || *slotNumber > Settings::MaxBookmarkSlot)
				{
					Warn(GetMemberPath(path, "Slot"), fmt::format("expected a slot from 1 to {}; skipped", Settings::MaxBookmarkSlot));
					return std::nullopt;
				}
				VisualScriptBookmark bookmark;
				bookmark.Slot = static_cast<uint8_t>(*slotNumber);
				if (std::any_of(bookmarks.begin(), bookmarks.end(),
				                [&bookmark](VisualScriptBookmark const& earlier)
				                {
									return earlier.Slot == bookmark.Slot;
								}))
				{
					Warn(path, fmt::format("slot {} has an earlier bookmark; skipped", bookmark.Slot));
					return std::nullopt;
				}
				Json const* name = FindMember(entry, "Name");
				if (name == nullptr || !name->is_string())
				{
					Warn(GetMemberPath(path, "Name"), "expected a name; skipped");
					return std::nullopt;
				}
				std::optional<VisualScriptViewState> const view = ReadView(entry, path, "bookmark");
				if (!view)
				{
					return std::nullopt;
				}
				bookmark.View = *view;
				bookmark.Name = MakeBookmarkName(name->get_ref<std::string const&>(), bookmark.Slot);
				if (bookmark.Name != name->get_ref<std::string const&>())
				{
					Warn(GetMemberPath(path, "Name"), fmt::format("not a name of 1 to {} bytes on one line; \"{}\" is used",
					                                              Settings::MaxBookmarkNameBytes, bookmark.Name));
				}
				return bookmark;
			}

			std::optional<VisualScriptGraphViews> ReadGraph(Json const& entry, std::string const& path,
			                                                std::vector<VisualScriptGraphViews> const& graphs)
			{
				Json const* handle = entry.is_object() ? FindMember(entry, "Graph") : nullptr;
				std::optional<UUID> const id = handle != nullptr ? ReadID(*handle) : std::nullopt;
				if (!id)
				{
					Warn(path, "expected an object with the Graph's handle as a decimal string; skipped");
					return std::nullopt;
				}
				VisualScriptGraphViews graph;
				graph.Graph = AssetHandle(*id);
				if (std::any_of(graphs.begin(), graphs.end(),
				                [&graph](VisualScriptGraphViews const& earlier)
				                {
									return earlier.Graph == graph.Graph;
								}))
				{
					Warn(path, "repeats an earlier graph; skipped");
					return std::nullopt;
				}
				if (Json const* shown = FindMember(entry, "Shown"))
				{
					if (std::optional<uint32_t> const function = ReadUInt32(*shown))
					{
						graph.Shown = *function;
					}
					else
					{
						Warn(GetMemberPath(path, "Shown"), "expected 0 or a function's ID; the event graph is used");
					}
				}
				if (Json const* entity = FindMember(entry, "ContextEntity"))
				{
					// "0" is no entity, as in every other file.
					bool const none = entity->is_string() && entity->get_ref<std::string const&>() == "0";
					if (std::optional<UUID> const contextEntity = ReadID(*entity))
					{
						graph.ContextEntity = *contextEntity;
					}
					else if (!none)
					{
						Warn(GetMemberPath(path, "ContextEntity"), "expected an entity ID as a decimal string; none is used");
					}
				}
				ReadList(entry, path, "Views", Settings::MaxViewsPerGraph, graph.Views,
				         [&](Json const& viewEntry, std::string const& viewPath) -> std::optional<VisualScriptViewState>
				         {
							 if (!viewEntry.is_object())
							 {
								 Warn(viewPath, "expected an object; skipped");
								 return std::nullopt;
							 }
							 Json const* function = FindMember(viewEntry, "Function");
							 std::optional<uint32_t> const functionID = function != nullptr ? ReadUInt32(*function) : std::nullopt;
							 if (functionID && graph.FindView(*functionID) != nullptr)
							 {
								 Warn(viewPath, fmt::format("sub-graph {} has an earlier view; skipped", *functionID));
								 return std::nullopt;
							 }
							 return ReadView(viewEntry, viewPath, "view");
						 });
				ReadList(entry, path, "Bookmarks", Settings::MaxBookmarkSlot, graph.Bookmarks,
				         [&](Json const& bookmarkEntry, std::string const& bookmarkPath)
				         {
							 return ReadBookmark(bookmarkEntry, bookmarkPath, graph.Bookmarks);
						 });
				std::sort(graph.Bookmarks.begin(), graph.Bookmarks.end(),
				          [](VisualScriptBookmark const& a, VisualScriptBookmark const& b)
				          {
							  return a.Slot < b.Slot;
						  });
				return graph;
			}

			std::optional<VisualScriptProjectViews> ReadProject(Json const& entry, std::string const& path,
			                                                    std::vector<VisualScriptProjectViews> const& projects)
			{
				Json const* file = entry.is_object() ? FindMember(entry, "Project") : nullptr;
				if (file == nullptr || !file->is_string() || !IsValidText(file->get_ref<std::string const&>(), MaxPathBytes))
				{
					Warn(path, "expected an object with the Project's file path; skipped");
					return std::nullopt;
				}
				VisualScriptProjectViews project;
				project.Project = FileSystem::PathFromUtf8(file->get_ref<std::string const&>());
				if (std::any_of(projects.begin(), projects.end(),
				                [&project](VisualScriptProjectViews const& earlier)
				                {
									return earlier.Project == project.Project;
								}))
				{
					Warn(path, "repeats an earlier project; skipped");
					return std::nullopt;
				}
				ReadList(entry, path, "Open", Settings::MaxGraphsPerProject, project.Open,
				         [&](Json const& handle, std::string const& handlePath) -> std::optional<AssetHandle>
				         {
							 std::optional<UUID> const id = ReadID(handle);
							 if (!id)
							 {
								 Warn(handlePath, "expected a graph's handle as a decimal string; skipped");
								 return std::nullopt;
							 }
							 if (std::find(project.Open.begin(), project.Open.end(), AssetHandle(*id)) != project.Open.end())
							 {
								 Warn(handlePath, "repeats an earlier graph; skipped");
								 return std::nullopt;
							 }
							 return AssetHandle(*id);
						 });
				ReadList(entry, path, "Graphs", Settings::MaxGraphsPerProject, project.Graphs,
				         [&](Json const& graph, std::string const& graphPath)
				         {
							 return ReadGraph(graph, graphPath, project.Graphs);
						 });
				return project;
			}

			void ReadProjects(Json const& content, std::vector<VisualScriptProjectViews>& projects)
			{
				ReadList(content, {}, "Projects", Settings::MaxProjects, projects,
				         [&](Json const& entry, std::string const& path)
				         {
							 return ReadProject(entry, path, projects);
						 });
			}

			std::vector<std::string>& m_Warnings;
		};

		Json SerializeView(VisualScriptViewState const& view)
		{
			Json json = Json::object();
			json["Function"] = view.Function;
			json["Center"] = JsonTraits<glm::vec2>::ToJson(view.Center);
			json["Zoom"] = view.Zoom;
			return json;
		}

		Json SerializeGraph(VisualScriptGraphViews const& graph)
		{
			Json views = Json::array();
			for (VisualScriptViewState const& view : graph.Views)
			{
				views.push_back(SerializeView(view));
			}
			Json bookmarks = Json::array();
			for (VisualScriptBookmark const& bookmark : graph.Bookmarks)
			{
				Json json = Json::object();
				json["Slot"] = bookmark.Slot;
				json["Name"] = bookmark.Name;
				json.update(SerializeView(bookmark.View));
				bookmarks.push_back(std::move(json));
			}
			Json json = Json::object();
			json["Graph"] = graph.Graph.ToString();
			json["Shown"] = graph.Shown;
			if (graph.ContextEntity.IsValid())
			{
				json["ContextEntity"] = graph.ContextEntity.ToString();
			}
			json["Views"] = std::move(views);
			json["Bookmarks"] = std::move(bookmarks);
			return json;
		}
	}

	VisualScriptViewState const* VisualScriptGraphViews::FindView(uint32_t function) const
	{
		auto const found = std::find_if(Views.begin(), Views.end(),
		                                [function](VisualScriptViewState const& view)
		                                {
											return view.Function == function;
										});
		return found != Views.end() ? &*found : nullptr;
	}

	VisualScriptBookmark const* VisualScriptGraphViews::FindBookmark(uint8_t slot) const
	{
		auto const found = std::find_if(Bookmarks.begin(), Bookmarks.end(),
		                                [slot](VisualScriptBookmark const& bookmark)
		                                {
											return bookmark.Slot == slot;
										});
		return found != Bookmarks.end() ? &*found : nullptr;
	}

	VisualScriptGraphViews const* VisualScriptProjectViews::FindGraph(AssetHandle graph) const
	{
		auto const found = std::find_if(Graphs.begin(), Graphs.end(),
		                                [graph](VisualScriptGraphViews const& views)
		                                {
											return views.Graph == graph;
										});
		return found != Graphs.end() ? &*found : nullptr;
	}

	VisualScriptViewSettings::VisualScriptViewSettings(std::filesystem::path file)
		: m_File(std::move(file))
	{
	}

	std::vector<std::string> VisualScriptViewSettings::Load()
	{
		std::vector<std::string> warnings;
		m_Views = VisualScriptViews();
		if (FileSystem::Exists(m_File))
		{
			if (Result<std::string> text = FileSystem::ReadTextFile(m_File); !text)
			{
				warnings.push_back(fmt::format("the file cannot be read: {}; it is ignored", text.GetError()));
			}
			else if (Result<Json> json = ParseJson(text.GetValue()); !json)
			{
				warnings.push_back(fmt::format("not valid JSON: {}; the file is ignored", json.GetError()));
			}
			else
			{
				m_Views = Deserialize(json.GetValue(), warnings);
			}
		}
		// Projects are keyed by their normalized path, as SetProject normalizes the current one.
		std::vector<VisualScriptProjectViews>& projects = m_Views.Projects;
		for (size_t index = 0; index < projects.size();)
		{
			std::filesystem::path normalized = RecentProjects::NormalizePath(projects[index].Project);
			bool const repeated = std::any_of(projects.begin(), projects.begin() + static_cast<std::ptrdiff_t>(index),
			                                  [&normalized](VisualScriptProjectViews const& earlier)
			                                  {
												  return earlier.Project == normalized;
											  });
			if (repeated)
			{
				warnings.push_back(fmt::format("VisualScriptViews.Projects: '{}' is the file of an earlier project; skipped",
				                               FileSystem::PathToUtf8(projects[index].Project)));
				projects.erase(projects.begin() + static_cast<std::ptrdiff_t>(index));
				continue;
			}
			projects[index].Project = std::move(normalized);
			index++;
		}
		UseProject();
		std::string const file = FileSystem::PathToUtf8(m_File);
		for (std::string& warning : warnings)
		{
			warning = fmt::format("'{}': {}", file, warning);
		}
		return warnings;
	}

	Result<void> VisualScriptViewSettings::Save() const
	{
		return FileSystem::WriteTextFile(m_File, DumpJson(Serialize(m_Views)));
	}

	Json VisualScriptViewSettings::Serialize(VisualScriptViews const& views)
	{
		Json panes = Json::object();
		panes["Sidebar"] = views.Panes.Sidebar;
		panes["Details"] = views.Panes.Details;
		panes["ShowSidebar"] = views.Panes.ShowSidebar;
		panes["ShowDetails"] = views.Panes.ShowDetails;
		panes["ShowMinimap"] = views.Panes.ShowMinimap;
		Json recents = Json::array();
		for (VisualScriptRecentNode const& recent : views.Recents)
		{
			Json json = Json::object();
			json["Type"] = recent.Type;
			json["Settings"] = recent.Settings;
			recents.push_back(std::move(json));
		}
		Json projects = Json::array();
		for (VisualScriptProjectViews const& project : views.Projects)
		{
			Json open = Json::array();
			for (AssetHandle const graph : project.Open)
			{
				open.push_back(graph.ToString());
			}
			Json graphs = Json::array();
			for (VisualScriptGraphViews const& graph : project.Graphs)
			{
				graphs.push_back(SerializeGraph(graph));
			}
			Json json = Json::object();
			json["Project"] = FileSystem::PathToUtf8(project.Project);
			json["Open"] = std::move(open);
			json["Graphs"] = std::move(graphs);
			projects.push_back(std::move(json));
		}

		Json content = Json::object();
		content["Panes"] = std::move(panes);
		content["ContextPalette"] = views.ContextPalette;
		content["ReduceMotion"] = views.ReduceMotion;
		if (views.Scroll)
		{
			content["Scroll"] = JsonTraits<VisualScriptScrollMode>::ToJson(*views.Scroll);
		}
		content["SnapToGrid"] = views.SnapToGrid;
		content["MoveCommentContents"] = views.MoveCommentContents;
		content["Recents"] = std::move(recents);
		content["RecentTypes"] = views.RecentTypes;
		if (views.CommentColor)
		{
			content["CommentColor"] = JsonTraits<glm::vec4>::ToJson(*views.CommentColor);
		}
		content["LastVariableType"] = views.LastVariableType;
		content["HintsSeen"] = views.HintsSeen;
		content["Projects"] = std::move(projects);

		Json file = Json::object();
		file["Strada"] = MakeFileHeader(FileType, FormatVersion);
		file["VisualScriptViews"] = std::move(content);
		return file;
	}

	VisualScriptViews VisualScriptViewSettings::Deserialize(Json const& file, std::vector<std::string>& warnings)
	{
		if (Result<int> header = ReadFileHeader(file, FileType, FormatVersion); !header)
		{
			warnings.push_back(fmt::format("{}; the file is ignored", header.GetError()));
			return {};
		}
		Json const* content = FindMember(file, "VisualScriptViews");
		if (content == nullptr || !content->is_object())
		{
			warnings.push_back("expected a \"VisualScriptViews\" object; the file is ignored");
			return {};
		}
		return ViewsReader(warnings).Read(*content);
	}

	void VisualScriptViewSettings::SetPanes(VisualScriptPaneSettings const& panes)
	{
		auto const width = [](float value, float minimum, float current)
		{
			return std::isfinite(value) ? std::clamp(value, minimum, MaxPaneWidth) : current;
		};
		m_Views.Panes.Sidebar = width(panes.Sidebar, MinSidebarWidth, m_Views.Panes.Sidebar);
		m_Views.Panes.Details = width(panes.Details, MinDetailsWidth, m_Views.Panes.Details);
		m_Views.Panes.ShowSidebar = panes.ShowSidebar;
		m_Views.Panes.ShowDetails = panes.ShowDetails;
		m_Views.Panes.ShowMinimap = panes.ShowMinimap;
	}

	VisualScriptScrollMode VisualScriptViewSettings::GetScrollMode() const
	{
		return m_Views.Scroll.value_or(GetDefaultVisualScriptScrollMode());
	}

	glm::vec4 VisualScriptViewSettings::GetCommentColor() const
	{
		return m_Views.CommentColor.value_or(VisualScriptComment().Color);
	}

	bool VisualScriptViewSettings::SetCommentColor(glm::vec4 linearColor)
	{
		if (!IsValidCommentColor(linearColor))
		{
			return false;
		}
		m_Views.CommentColor = linearColor;
		return true;
	}

	bool VisualScriptViewSettings::SetLastVariableType(std::string_view type)
	{
		if (!IsValidText(type, VisualScriptSerializer::MaxTypeBytes))
		{
			return false;
		}
		m_Views.LastVariableType = std::string(type);
		return true;
	}

	bool VisualScriptViewSettings::AddRecent(VisualScriptRecentNode node)
	{
		if (!IsValidText(node.Type, VisualScriptSerializer::MaxNodeTypeBytes) || !node.Settings.is_object())
		{
			return false;
		}
		std::vector<VisualScriptRecentNode>& recents = m_Views.Recents;
		std::erase(recents, node);
		recents.insert(recents.begin(), std::move(node));
		if (recents.size() > MaxRecents)
		{
			recents.resize(MaxRecents);
		}
		return true;
	}

	bool VisualScriptViewSettings::AddRecentType(std::string_view type)
	{
		if (!IsValidText(type, VisualScriptSerializer::MaxTypeBytes))
		{
			return false;
		}
		std::vector<std::string>& types = m_Views.RecentTypes;
		std::erase(types, type);
		types.insert(types.begin(), std::string(type));
		if (types.size() > MaxRecentTypes)
		{
			types.resize(MaxRecentTypes);
		}
		return true;
	}

	bool VisualScriptViewSettings::IsHintSeen(std::string_view hint) const
	{
		return std::find(m_Views.HintsSeen.begin(), m_Views.HintsSeen.end(), hint) != m_Views.HintsSeen.end();
	}

	bool VisualScriptViewSettings::SetHintSeen(std::string_view hint)
	{
		if (!IsValidText(hint, MaxHintBytes))
		{
			return false;
		}
		if (!IsHintSeen(hint))
		{
			std::vector<std::string>& hints = m_Views.HintsSeen;
			hints.insert(hints.begin(), std::string(hint));
			if (hints.size() > MaxHints)
			{
				hints.resize(MaxHints);
			}
		}
		return true;
	}

	void VisualScriptViewSettings::SetProject(std::filesystem::path const& projectFile)
	{
		m_Project = projectFile.empty() ? std::filesystem::path() : RecentProjects::NormalizePath(projectFile);
		UseProject();
	}

	std::span<AssetHandle const> VisualScriptViewSettings::GetOpenGraphs() const
	{
		VisualScriptProjectViews const* const project = GetCurrentProject();
		return project != nullptr ? std::span<AssetHandle const>(project->Open) : std::span<AssetHandle const>();
	}

	void VisualScriptViewSettings::SetOpenGraphs(std::span<AssetHandle const> graphs)
	{
		VisualScriptProjectViews* const project = GetCurrentProject();
		if (project == nullptr)
		{
			return;
		}
		project->Open.clear();
		for (AssetHandle const graph : graphs)
		{
			if (project->Open.size() == MaxGraphsPerProject)
			{
				break;
			}
			if (graph.IsValid() && std::find(project->Open.begin(), project->Open.end(), graph) == project->Open.end())
			{
				project->Open.push_back(graph);
			}
		}
	}

	VisualScriptGraphViews const* VisualScriptViewSettings::FindGraph(AssetHandle graph) const
	{
		VisualScriptProjectViews const* const project = GetCurrentProject();
		return project != nullptr ? project->FindGraph(graph) : nullptr;
	}

	void VisualScriptViewSettings::ForgetGraph(AssetHandle graph)
	{
		VisualScriptProjectViews* const project = GetCurrentProject();
		if (project == nullptr)
		{
			return;
		}
		std::erase_if(project->Graphs,
		              [graph](VisualScriptGraphViews const& views)
		              {
						  return views.Graph == graph;
					  });
		std::erase(project->Open, graph);
	}

	void VisualScriptViewSettings::SetShown(AssetHandle graph, uint32_t function)
	{
		if (VisualScriptGraphViews* const views = UseGraph(graph))
		{
			views->Shown = function;
		}
	}

	void VisualScriptViewSettings::SetContextEntity(AssetHandle graph, UUID entity)
	{
		if (VisualScriptGraphViews* const views = UseGraph(graph))
		{
			views->ContextEntity = entity;
		}
	}

	bool VisualScriptViewSettings::SetView(AssetHandle graph, VisualScriptViewState view)
	{
		std::optional<VisualScriptViewState> const clamped = ClampView(view);
		VisualScriptGraphViews* const views = clamped ? UseGraph(graph) : nullptr;
		if (views == nullptr)
		{
			return false;
		}
		auto const found = std::find_if(views->Views.begin(), views->Views.end(),
		                                [function = clamped->Function](VisualScriptViewState const& stored)
		                                {
											return stored.Function == function;
										});
		if (found != views->Views.end())
		{
			MoveToFront(views->Views, static_cast<size_t>(found - views->Views.begin()));
			views->Views.front() = *clamped;
			return true;
		}
		views->Views.insert(views->Views.begin(), *clamped);
		if (views->Views.size() > MaxViewsPerGraph)
		{
			views->Views.resize(MaxViewsPerGraph);
		}
		return true;
	}

	bool VisualScriptViewSettings::SetBookmark(AssetHandle graph, VisualScriptBookmark bookmark)
	{
		std::optional<VisualScriptViewState> const clamped = ClampView(bookmark.View);
		if (bookmark.Slot < 1 || bookmark.Slot > MaxBookmarkSlot || !clamped)
		{
			return false;
		}
		VisualScriptGraphViews* const views = UseGraph(graph);
		if (views == nullptr)
		{
			return false;
		}
		bookmark.Name = MakeBookmarkName(bookmark.Name, bookmark.Slot);
		bookmark.View = *clamped;
		auto const position = std::lower_bound(views->Bookmarks.begin(), views->Bookmarks.end(), bookmark.Slot,
		                                       [](VisualScriptBookmark const& stored, uint8_t slot)
		                                       {
												   return stored.Slot < slot;
											   });
		if (position != views->Bookmarks.end() && position->Slot == bookmark.Slot)
		{
			*position = std::move(bookmark);
		}
		else
		{
			views->Bookmarks.insert(position, std::move(bookmark));
		}
		return true;
	}

	void VisualScriptViewSettings::RemoveBookmark(AssetHandle graph, uint8_t slot)
	{
		if (FindGraph(graph) == nullptr)
		{
			return;
		}
		VisualScriptGraphViews* const views = UseGraph(graph);
		std::erase_if(views->Bookmarks,
		              [slot](VisualScriptBookmark const& bookmark)
		              {
						  return bookmark.Slot == slot;
					  });
	}

	VisualScriptProjectViews* VisualScriptViewSettings::GetCurrentProject()
	{
		return m_Project.empty() ? nullptr : &m_Views.Projects.front();
	}

	VisualScriptProjectViews const* VisualScriptViewSettings::GetCurrentProject() const
	{
		return m_Project.empty() ? nullptr : &m_Views.Projects.front();
	}

	VisualScriptGraphViews* VisualScriptViewSettings::UseGraph(AssetHandle graph)
	{
		VisualScriptProjectViews* const project = GetCurrentProject();
		if (project == nullptr || !graph.IsValid())
		{
			return nullptr;
		}
		std::vector<VisualScriptGraphViews>& graphs = project->Graphs;
		auto const found = std::find_if(graphs.begin(), graphs.end(),
		                                [graph](VisualScriptGraphViews const& views)
		                                {
											return views.Graph == graph;
										});
		if (found != graphs.end())
		{
			MoveToFront(graphs, static_cast<size_t>(found - graphs.begin()));
			return &graphs.front();
		}
		VisualScriptGraphViews views;
		views.Graph = graph;
		graphs.insert(graphs.begin(), std::move(views));
		if (graphs.size() > MaxGraphsPerProject)
		{
			graphs.resize(MaxGraphsPerProject);
		}
		return &graphs.front();
	}

	void VisualScriptViewSettings::UseProject()
	{
		if (m_Project.empty())
		{
			return;
		}
		std::vector<VisualScriptProjectViews>& projects = m_Views.Projects;
		auto const found = std::find_if(projects.begin(), projects.end(),
		                                [this](VisualScriptProjectViews const& project)
		                                {
											return project.Project == m_Project;
										});
		if (found != projects.end())
		{
			MoveToFront(projects, static_cast<size_t>(found - projects.begin()));
			return;
		}
		VisualScriptProjectViews project;
		project.Project = m_Project;
		projects.insert(projects.begin(), std::move(project));
		if (projects.size() > MaxProjects)
		{
			projects.resize(MaxProjects);
		}
	}
}
