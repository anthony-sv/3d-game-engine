#pragma once

#include "Editor/VisualScripting/VisualScriptEditorTypes.h"

#include "Strada/Asset/AssetHandle.h"

#include <glm/glm.hpp>
#include <imgui.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace Strada
{
	// What a Game View Beside request came to (spec-slice4 §15.6): queued for the next frame, docked, nothing to do (the game
	// shows already), or refused with a reason.
	enum class VisualScriptGameBesideOutcome : uint8_t
	{
		Queued = 0,
		Applied,
		AlreadyVisible,
		Failed
	};

	// The outcome's automation name ("Applied").
	[[nodiscard]] std::string_view GetVisualScriptGameBesideOutcomeName(VisualScriptGameBesideOutcome outcome);

	// The last Game View Beside request: the graph whose window it was for, its outcome, and Failed's reason.
	struct VisualScriptGameBesideState
	{
		AssetHandle Graph;
		VisualScriptGameBesideOutcome Outcome = VisualScriptGameBesideOutcome::Queued;
		std::string Reason;

		bool operator==(VisualScriptGameBesideState const& other) const = default;
	};

	// Where the Game View goes in the node it shares with a graph window: above the graph or to its left.
	enum class VisualScriptBesideDirection : uint8_t
	{
		Up = 0,
		Left
	};

	// A split of a node between the Game View and a graph window.
	struct VisualScriptBesideSplit
	{
		VisualScriptBesideDirection Direction = VisualScriptBesideDirection::Up;
		// The Game View's share of the node's height (Up) or width (Left).
		float Share = 0.0f;
		// The game's image in the Game View after the split (pixels), letterboxed to the game's aspect ratio.
		glm::vec2 Image{0.0f};

		bool operator==(VisualScriptBesideSplit const& other) const = default;
	};

	// The split of a node of the given size (pixels, before the split) between the Game View and a graph window, for a game of
	// the aspect ratio (width / height; DefaultAspect when not positive and finite), a Game View toolbar of toolbar pixels, a
	// font size and the dock splitter's thickness: the candidate (Up or Left) whose game image has the larger area, Up on a
	// tie; Left alone when Up would leave the graph less than MinGraphHeight. Pure.
	[[nodiscard]] VisualScriptBesideSplit ComputeVisualScriptBesideSplit(glm::vec2 node, float aspect, float toolbar, float fontSize,
	                                                                     float splitter);

	// What PlaceGameViewBeside docks: the windows by their ImGui names, the dock space they dock into, and the game's image.
	struct VisualScriptBesideRequest
	{
		// The graph window's ImGui name (VisualScriptEditorPanel::GetWindowName).
		std::string GraphWindow;
		std::string GameWindow = "Game View";
		// The window whose node a floating graph window joins when the Game View is closed or floats (the default dock
		// target, the scene view's).
		std::string FallbackWindow = "Scene View";
		ImGuiID Dockspace = 0;
		// The Game View is open (drawn when its tab is in front); a closed one is docked by the split and the caller opens it.
		bool GameViewOpen = false;
		// The game's aspect ratio (width / height) and the Game View's toolbar height in pixels.
		float Aspect = VisualScriptBeside::DefaultAspect;
		float Toolbar = 0.0f;
	};

	// Docks the Game View beside a graph window (spec-slice4 §15.6), from the windows as the last frame drew them: with the
	// graph window docked, its node is split by ComputeVisualScriptBesideSplit (the node's windows stay on the graph's side)
	// and the Game View docks into the other side; a floating graph window, or one in a dock node outside the dock space,
	// first joins the Game View's node (with the Game View closed or floating, the fallback window's node, else the dock
	// space's central node), and the split docks a floating Game View too. Nothing is docked (AlreadyVisible) when a docked
	// graph window's Game View shows already outside its node: floating, or the tab in front of another node. Failed when the
	// graph window was not drawn in the last frame (VisualScriptPanelTexts::BesideNotOpen) or the dock space has no node to
	// split (BesideNoDockSpace). Inside an ImGui frame, after the dock space is submitted and before the windows of its
	// nodes, which show the split as they are drawn.
	[[nodiscard]] VisualScriptGameBesideState PlaceGameViewBeside(VisualScriptBesideRequest const& request);
}
