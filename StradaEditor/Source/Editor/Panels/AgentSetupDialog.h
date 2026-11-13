#pragma once

#include "Editor/Agents/AgentKit.h"

#include "Strada/Core/EngineLayout.h"

#include <optional>
#include <string>

struct ImFont;

namespace Strada
{
	// File > Set Up for Coding Agents: what setting the open project up for coding agents would do (AgentKit::Plan), file by
	// file with its action and, for the selected file, the diff of its managed part; Apply writes the files (AgentKit::Apply)
	// and shows what it did, or the file that stopped it; Cancel writes nothing. The plan is made when the dialog opens and
	// again when the project changes under it (automation runs while the dialog is open), never per frame.
	class AgentSetupDialog
	{
	public:
		// The File menu's item, which the agent kit's texts name ("File > Set Up for Coding Agents...").
		static constexpr char const* MenuItem = "Set Up for Coding Agents...";
		// The modal popup's name.
		static constexpr char const* PopupName = "Set Up for Coding Agents";

		// Opens the dialog in the next frame.
		void Open();
		// Draws the dialog while it is open, for the open project (none: the dialog says so). The layout gives the engine's
		// agent kit and guides; findTool is the editor's lookup of strada on PATH.
		void OnImGuiRender(std::optional<AgentProject> const& project, EngineLayout const& layout, AgentToolLookup const& findTool);
		// The fixed-width font of the diffs (none: the default font).
		void SetMonospaceFont(ImFont* font) { m_MonospaceFont = font; }

		// The selected file (relative, forward slashes), whose diff or state the dialog shows; empty when none is.
		std::string const& GetSelectedFile() const { return m_Selected; }
		// What the last Apply did, until the dialog opens again or its project changes.
		std::optional<AgentApplyResult> const& GetLastResult() const { return m_Result; }

	private:
		void MakePlan(AgentProject const& project, EngineLayout const& layout, AgentToolLookup const& findTool);
		void DrawFiles(AgentPlan const& plan, bool applied);
		void DrawSelection(AgentPlan const& plan, bool applied);
		void DrawResult(AgentApplyResult const& result);

		bool m_OpenRequested = false;
		// The frames in which the dialog is centered again.
		int m_RecenterFrames = 0;
		ImFont* m_MonospaceFont = nullptr;
		// The project the plan was made for; none before the first plan.
		std::optional<AgentProject> m_Project;
		// The plan, or why there is none (the engine's agent kit is incomplete).
		std::optional<AgentPlan> m_Plan;
		std::string m_PlanError;
		std::optional<AgentApplyResult> m_Result;
		std::string m_Selected;
	};
}
