#pragma once

#include "Editor/VisualScripting/VisualScriptView.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	// The panes beside the canvas, shared by every graph window. Widths are in font sizes (ImGui::GetFontSize()).
	struct VisualScriptPaneSettings
	{
		float Sidebar = 16.0f;
		float Details = 20.0f;
		bool ShowSidebar = true;
		bool ShowDetails = true;
		bool ShowMinimap = true;

		bool operator==(VisualScriptPaneSettings const& other) const = default;
	};

	// A node added from the palette, as it was added.
	struct VisualScriptRecentNode
	{
		std::string Type;
		// The settings it was added with: the preset of a typed entry ("Literal" with { "Type": "float" }), else empty.
		Json Settings = Json::object();

		bool operator==(VisualScriptRecentNode const& other) const = default;
	};

	// A view kept under a slot, which Ctrl+<slot> goes back to.
	struct VisualScriptBookmark
	{
		// 1 to 9.
		uint8_t Slot = 1;
		std::string Name;
		VisualScriptViewState View;

		bool operator==(VisualScriptBookmark const& other) const = default;
	};

	// What the views file keeps of a graph's window.
	struct VisualScriptGraphViews
	{
		AssetHandle Graph;
		// The sub-graph shown last: 0 for the event graph, else a function's ID.
		uint32_t Shown = 0;
		// The entity whose instance the window shows; invalid when none.
		UUID ContextEntity = UUID::Invalid();
		// The view of each sub-graph shown, one per sub-graph, the most recently shown first.
		std::vector<VisualScriptViewState> Views;
		// One per slot, in slot order.
		std::vector<VisualScriptBookmark> Bookmarks;

		// Null when the sub-graph has no view kept.
		VisualScriptViewState const* FindView(uint32_t function) const;
		// Null when the slot is free.
		VisualScriptBookmark const* FindBookmark(uint8_t slot) const;

		bool operator==(VisualScriptGraphViews const& other) const = default;
	};

	// What the views file keeps of a project.
	struct VisualScriptProjectViews
	{
		// The project file, as RecentProjects::NormalizePath gives it.
		std::filesystem::path Project;
		// The graphs whose windows were open, in tab order.
		std::vector<AssetHandle> Open;
		// The graphs whose windows were used, the most recently used first.
		std::vector<VisualScriptGraphViews> Graphs;

		// Null when the project keeps nothing of the graph.
		VisualScriptGraphViews const* FindGraph(AssetHandle graph) const;

		bool operator==(VisualScriptProjectViews const& other) const = default;
	};

	// Everything the views file holds.
	struct VisualScriptViews
	{
		VisualScriptPaneSettings Panes;
		// The palette opened by a wire dropped on the canvas lists only the nodes that connect to it.
		bool ContextPalette = true;
		// Framing, bookmarks and other moves of the view jump instead of animating.
		bool ReduceMotion = false;
		// None: the platform's default (GetDefaultVisualScriptScrollMode).
		std::optional<VisualScriptScrollMode> Scroll;
		// Moved nodes snap to the grid (Alt inverts it for one drag).
		bool SnapToGrid = true;
		// Moving a comment box moves what lies inside it (Alt inverts it for one drag).
		bool MoveCommentContents = true;
		// Nodes added from the palette, the most recent first.
		std::vector<VisualScriptRecentNode> Recents;
		// Type IDs chosen in type pickers, the most recent first.
		std::vector<std::string> RecentTypes;
		// The colour (linear RGBA) chosen last for a comment box; none: the format's default comment colour.
		std::optional<glm::vec4> CommentColor;
		// The type given to the last variable added.
		std::string LastVariableType = "float";
		// The one-time hints dismissed, the most recent first.
		std::vector<std::string> HintsSeen;
		// The most recently used first.
		std::vector<VisualScriptProjectViews> Projects;

		bool operator==(VisualScriptViews const& other) const = default;
	};

	// The visual script editor's per-user state, in <UserData>/Editor/VisualScriptViews.json: the panes and toggles, the
	// palette's recent nodes, recent types, the last comment colour and variable type, the hints seen, and per project the
	// graph windows that were open with each graph's views (the sub-graph shown, a view per sub-graph, the context entity,
	// bookmarks). It is never part of documents or of the undo history, and never exported. The editor reads it at start and
	// writes it when graph windows close, when the project changes and at exit, never per frame; in between, the windows and
	// panes update it here.
	//
	// Everything kept is valid: setters clamp numbers into their ranges and ignore what cannot be kept, and the per-project
	// lists keep the most recently used entries within their limits. Main thread only.
	class VisualScriptViewSettings
	{
	public:
		static constexpr int FormatVersion = 1;
		static constexpr size_t MaxProjects = 32;
		static constexpr size_t MaxGraphsPerProject = 256;
		// The sub-graphs most recently shown keep their views.
		static constexpr size_t MaxViewsPerGraph = 64;
		static constexpr uint8_t MaxBookmarkSlot = 9;
		static constexpr size_t MaxBookmarkNameBytes = 64;
		static constexpr size_t MaxRecents = 20;
		static constexpr size_t MaxRecentTypes = 10;
		static constexpr size_t MaxHints = 16;
		static constexpr size_t MaxHintBytes = 64;
		// The centres of views lie within +-MaxCenter, the documents' coordinate limit; zooms within VisualScriptMinZoom and
		// VisualScriptMaxZoom.
		static constexpr float MaxCenter = VisualScriptMaxCenter;
		// Pane widths, in font sizes.
		static constexpr float MinSidebarWidth = 10.0f;
		static constexpr float MinDetailsWidth = 14.0f;
		static constexpr float MaxPaneWidth = 1000.0f;

		// Starts with the defaults and no project; Load reads the file.
		explicit VisualScriptViewSettings(std::filesystem::path file);

		std::filesystem::path const& GetFile() const { return m_File; }

		// Replaces what this holds with the file's content; the current project stays current. A missing file holds the
		// defaults. A file that cannot be read or parsed, is not a views file or has a newer version is ignored (the
		// defaults) with one warning; parts of it that are not valid are skipped or clamped into their ranges, each with a
		// warning, and members this version does not know are ignored. Returns the warnings, which the caller logs: a file
		// this class wrote has none.
		[[nodiscard]] std::vector<std::string> Load();
		// Writes the file atomically, creating its directory.
		[[nodiscard]] Result<void> Save() const;

		VisualScriptViews const& GetViews() const { return m_Views; }

		// The file's form: { "Strada": { "Version": 1, "Type": "VisualScriptViews" }, "VisualScriptViews": { ... } }, members at
		// their defaults included, except Scroll, CommentColor and a graph's ContextEntity when they are none.
		static Json Serialize(VisualScriptViews const& views);
		// Reads the file's form as Load does: what is valid within the limits, the warnings appended.
		static VisualScriptViews Deserialize(Json const& file, std::vector<std::string>& warnings);

		// --- Shared by every graph window ---

		VisualScriptPaneSettings const& GetPanes() const { return m_Views.Panes; }
		// Widths are clamped to [the pane's minimum, MaxPaneWidth]; a width that is not finite keeps the current one.
		void SetPanes(VisualScriptPaneSettings const& panes);
		bool IsContextPalette() const { return m_Views.ContextPalette; }
		void SetContextPalette(bool contextPalette) { m_Views.ContextPalette = contextPalette; }
		bool IsReduceMotion() const { return m_Views.ReduceMotion; }
		void SetReduceMotion(bool reduceMotion) { m_Views.ReduceMotion = reduceMotion; }
		// The mode chosen, else the platform's default.
		VisualScriptScrollMode GetScrollMode() const;
		void SetScrollMode(VisualScriptScrollMode mode) { m_Views.Scroll = mode; }
		bool IsSnapToGrid() const { return m_Views.SnapToGrid; }
		void SetSnapToGrid(bool snapToGrid) { m_Views.SnapToGrid = snapToGrid; }
		bool IsMoveCommentContents() const { return m_Views.MoveCommentContents; }
		void SetMoveCommentContents(bool moveCommentContents) { m_Views.MoveCommentContents = moveCommentContents; }
		// The colour chosen last, else the format's default comment colour (linear RGBA).
		glm::vec4 GetCommentColor() const;
		// False (nothing kept) unless every channel lies within [0, 1], as in documents.
		bool SetCommentColor(glm::vec4 linearColor);
		std::string const& GetLastVariableType() const { return m_Views.LastVariableType; }
		// False (nothing kept) for a text that is no type ID: empty, longer than a document's type, or with control
		// characters or invalid UTF-8.
		bool SetLastVariableType(std::string_view type);

		// The most recent first.
		std::span<VisualScriptRecentNode const> GetRecents() const { return m_Views.Recents; }
		// Makes the node the most recent (an equal entry moves to the front), dropping the oldest beyond MaxRecents. False
		// (nothing kept) for a type that is no node type or settings that are not an object.
		bool AddRecent(VisualScriptRecentNode node);
		// The most recent first.
		std::span<std::string const> GetRecentTypes() const { return m_Views.RecentTypes; }
		// As AddRecent, within MaxRecentTypes; false for a text that is no type ID.
		bool AddRecentType(std::string_view type);
		bool IsHintSeen(std::string_view hint) const;
		// Remembers that the hint was dismissed, forgetting the oldest beyond MaxHints. False (nothing kept) for an ID that
		// is empty, longer than MaxHintBytes, or has control characters or invalid UTF-8.
		bool SetHintSeen(std::string_view hint);

		// --- The current project's graph windows ---

		// Makes the project of the file the current one and its entry the most recently used, dropping the least recently
		// used beyond MaxProjects. Projects are keyed by RecentProjects::NormalizePath of their file, so every spelling of a
		// path names one project. An empty path means no project: the members below then keep and find nothing.
		void SetProject(std::filesystem::path const& projectFile);
		// Normalized; empty when none.
		std::filesystem::path const& GetProject() const { return m_Project; }
		// The graphs whose windows were open in the current project, in tab order; empty without a project.
		std::span<AssetHandle const> GetOpenGraphs() const;
		// Invalid and repeated handles are left out, and handles beyond MaxGraphsPerProject.
		void SetOpenGraphs(std::span<AssetHandle const> graphs);
		// Null when the current project keeps nothing of the graph, and without a project.
		VisualScriptGraphViews const* FindGraph(AssetHandle graph) const;
		// Drops what the current project keeps of the graph, its open window included (its asset was deleted).
		void ForgetGraph(AssetHandle graph);

		// Each setter below keeps its value for the graph (a valid handle) in the current project and makes the graph the
		// project's most recently used, dropping the least recently used beyond MaxGraphsPerProject; without a project they
		// keep nothing.
		void SetShown(AssetHandle graph, uint32_t function);
		void SetContextEntity(AssetHandle graph, UUID entity);
		// The view of the sub-graph view.Function, which becomes the graph's most recently shown (the views of the least
		// recently shown beyond MaxViewsPerGraph are dropped). The centre and the zoom are clamped into their ranges. False
		// when nothing is kept: a view that is not finite, an invalid handle, no project.
		bool SetView(AssetHandle graph, VisualScriptViewState view);
		// Keeps the bookmark in its slot, replacing the slot's bookmark. Its name is cut at its first control character and to
		// MaxBookmarkNameBytes (at a character boundary), invalid UTF-8 replaced, and is "Bookmark <slot>" when nothing is
		// left; the view is clamped as SetView clamps it. False when nothing is kept: a slot outside 1 to 9, a view that is not
		// finite, an invalid handle, no project.
		bool SetBookmark(AssetHandle graph, VisualScriptBookmark bookmark);
		void RemoveBookmark(AssetHandle graph, uint8_t slot);

	private:
		VisualScriptProjectViews* GetCurrentProject();
		VisualScriptProjectViews const* GetCurrentProject() const;
		// The graph's entry in the current project, created when missing and made the most recently used; null without a
		// project or for an invalid handle.
		VisualScriptGraphViews* UseGraph(AssetHandle graph);
		// Makes the project the front entry of the list (it is m_Project, normalized).
		void UseProject();

		std::filesystem::path m_File;
		VisualScriptViews m_Views;
		// The current project: the front entry of m_Views.Projects while not empty.
		std::filesystem::path m_Project;
	};
}
