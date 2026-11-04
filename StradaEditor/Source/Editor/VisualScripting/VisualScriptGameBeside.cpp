#include "Editor/VisualScripting/VisualScriptGameBeside.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

namespace Strada
{
	namespace
	{
		// The largest image of the aspect ratio inside an area.
		glm::vec2 Fit(glm::vec2 area, float aspect)
		{
			area = glm::max(area, glm::vec2(0.0f));
			if (area.x <= 0.0f || area.y <= 0.0f)
			{
				return glm::vec2(0.0f);
			}
			return area.x / area.y > aspect ? glm::vec2(area.y * aspect, area.y) : glm::vec2(area.x, area.x / aspect);
		}

		// The dock space a node belongs to.
		ImGuiID GetRootID(ImGuiDockNode const* node)
		{
			while (node != nullptr && node->ParentNode != nullptr)
			{
				node = node->ParentNode;
			}
			return node != nullptr ? node->ID : 0;
		}

		// The node of a window that ImGui drew docked in the last frame, when it lies in the dock space.
		ImGuiDockNode* FindDockedNode(ImGuiWindow const* window, ImGuiID dockspace)
		{
			if (window == nullptr || window->DockNode == nullptr || GetRootID(window->DockNode) != dockspace)
			{
				return nullptr;
			}
			return window->DockNode;
		}
	}

	std::string_view GetVisualScriptGameBesideOutcomeName(VisualScriptGameBesideOutcome outcome)
	{
		switch (outcome)
		{
			case VisualScriptGameBesideOutcome::Queued:
				return "Queued";
			case VisualScriptGameBesideOutcome::Applied:
				return "Applied";
			case VisualScriptGameBesideOutcome::AlreadyVisible:
				return "AlreadyVisible";
			case VisualScriptGameBesideOutcome::Failed:
				return "Failed";
		}
		return "Failed";
	}

	VisualScriptBesideSplit ComputeVisualScriptBesideSplit(glm::vec2 node, float aspect, float toolbar, float fontSize, float splitter)
	{
		aspect = aspect > 0.0f && std::isfinite(aspect) ? aspect : VisualScriptBeside::DefaultAspect;
		node = glm::max(node, glm::vec2(0.0f));
		toolbar = std::max(toolbar, 0.0f);
		splitter = std::max(splitter, 0.0f);

		VisualScriptBesideSplit up;
		up.Direction = VisualScriptBesideDirection::Up;
		up.Share = VisualScriptBeside::UpShare;
		up.Image = Fit(glm::vec2(node.x, VisualScriptBeside::UpShare * node.y - toolbar), aspect);
		bool const upKeepsGraph = (1.0f - VisualScriptBeside::UpShare) * node.y - splitter >= VisualScriptBeside::MinGraphHeight * fontSize;

		VisualScriptBesideSplit left;
		left.Direction = VisualScriptBesideDirection::Left;
		float const keeping = node.x > 0.0f ? (node.x - VisualScriptBeside::MinGraphWidth * fontSize - splitter) / node.x : 0.0f;
		left.Share = std::min(keeping, VisualScriptBeside::MaxLeftShare);
		if (left.Share < VisualScriptBeside::MinLeftShare)
		{
			left.Share = VisualScriptBeside::FallbackLeftShare;
		}
		left.Image = Fit(glm::vec2(left.Share * node.x, node.y - toolbar), aspect);

		if (!upKeepsGraph)
		{
			return left;
		}
		return up.Image.x * up.Image.y >= left.Image.x * left.Image.y ? up : left;
	}

	VisualScriptGameBesideState PlaceGameViewBeside(VisualScriptBesideRequest const& request)
	{
		VisualScriptGameBesideState state;
		state.Outcome = VisualScriptGameBesideOutcome::Failed;
		// WasActive: drawn in the last frame (NewFrame moves Active there, imgui.cpp:5979).
		ImGuiWindow const* const graph = ImGui::FindWindowByName(request.GraphWindow.c_str());
		if (graph == nullptr || !graph->WasActive)
		{
			state.Reason = "the graph window is not open";
			return state;
		}
		if (ImGui::DockBuilderGetNode(request.Dockspace) == nullptr)
		{
			state.Reason = "the dock space is not available";
			return state;
		}

		ImGuiWindow const* const game = ImGui::FindWindowByName(request.GameWindow.c_str());
		bool const gameDrawn = request.GameViewOpen && game != nullptr && game->WasActive;
		ImGuiDockNode* const graphNode = FindDockedNode(graph, request.Dockspace);
		// Shown already: floating (in the editor's window or in one of its own), or the tab in front of another node.
		bool const gameFloating = gameDrawn && game->DockNode == nullptr;
		bool const gameInFront = gameDrawn && game->DockNode != nullptr && game->DockTabIsVisible;
		if (gameFloating || (gameInFront && game->DockNode != graphNode))
		{
			state.Outcome = VisualScriptGameBesideOutcome::AlreadyVisible;
			return state;
		}

		ImGuiDockNode* target = graphNode;
		if (target == nullptr && gameDrawn)
		{
			target = FindDockedNode(game, request.Dockspace);
		}
		if (target == nullptr)
		{
			target = FindDockedNode(ImGui::FindWindowByName(request.FallbackWindow.c_str()), request.Dockspace);
		}
		if (target == nullptr)
		{
			target = ImGui::DockBuilderGetCentralNode(request.Dockspace);
		}
		if (target == nullptr || !target->IsLeafNode())
		{
			state.Reason = "the dock space is not available";
			return state;
		}

		VisualScriptBesideSplit const split =
			ComputeVisualScriptBesideSplit(glm::vec2(target->Size.x, target->Size.y), request.Aspect, request.Toolbar, ImGui::GetFontSize(),
		                                   ImGui::GetStyle().DockingSeparatorSize);
		ImGuiID gameSide = 0;
		ImGuiID graphSide = 0;
		// The split moves the node's windows to the side opposite the direction (imgui.cpp:21168-21197, :18605-18614).
		ImGui::DockBuilderSplitNode(target->ID, split.Direction == VisualScriptBesideDirection::Up ? ImGuiDir_Up : ImGuiDir_Left,
		                            split.Share, &gameSide, &graphSide);
		ImGui::DockBuilderDockWindow(request.GameWindow.c_str(), gameSide);
		// A floating graph window joins the graph's side; a docked one is there already. Neither docks into the split node itself,
		// which holds no windows.
		ImGui::DockBuilderDockWindow(request.GraphWindow.c_str(), graphSide);
		ImGui::DockBuilderFinish(request.Dockspace);
		state.Outcome = VisualScriptGameBesideOutcome::Applied;
		return state;
	}
}
