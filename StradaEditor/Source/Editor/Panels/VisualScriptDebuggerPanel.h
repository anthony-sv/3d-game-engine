#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/UUID.h"
#include "Strada/Script/VisualScriptDebugger.h"

#include <imgui.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	class EditorContext;
	class VisualScriptDebugSession;

	// The Debugger panel's tabs (graph.debug-panel's tab), in their order.
	enum class VisualScriptDebuggerTab : uint8_t
	{
		CallStack = 0,
		Values,
		Breakpoints,
		Instances
	};

	// "CallStack", "Values", "Breakpoints" or "Instances".
	[[nodiscard]] std::string_view GetVisualScriptDebuggerTabName(VisualScriptDebuggerTab tab);
	[[nodiscard]] std::optional<VisualScriptDebuggerTab> ParseVisualScriptDebuggerTab(std::string_view name);

	// "Visual Script Debugger" (spec-slice4 §14.1): the debug session's stops and their call stacks, the selected frame's values
	// with a one-shot Evaluate of a formula against it, every breakpoint of the project, and the running instances. Docked with
	// the Console; it comes to the front at a play session's first stop, and graph.debug-panel opens it, picks its tab and
	// selects a stop and frame. Values are read-only (spec-slice4 §24 D12); resumes are queued with the session
	// (RequestResume), never run while the panel draws. Constructed without ImGui (automation reads and sets its state
	// headless); only OnImGuiRender calls ImGui. Main thread only.
	class VisualScriptDebuggerPanel
	{
	public:
		static constexpr char const* WindowName = "Visual Script Debugger";
		// The Instances tab's page, and how often it reads the instances while shown (seconds).
		static constexpr uint32_t InstancesPage = 100;
		static constexpr double InstancesInterval = 0.25;

		// The panel's fixed texts, all listed in All for the glyph test (the editor's UI font must have each character).
		struct Texts
		{
			static constexpr std::string_view CallStack = "Call Stack";
			static constexpr std::string_view Values = "Values";
			static constexpr std::string_view Breakpoints = "Breakpoints";
			static constexpr std::string_view Instances = "Instances";
			static constexpr std::string_view NothingStopped = "Nothing is stopped. Breakpoints stop the game's graphs while it plays.";
			static constexpr std::string_view NotPlaying = "Play the game to debug its graphs.";
			static constexpr std::string_view StoppedExecutions = "Stopped executions: ";
			static constexpr std::string_view Passed = " passed";
			static constexpr std::string_view Continue = "Continue";
			static constexpr std::string_view EngineCode = "\xE2\x80\x94 engine code (";
			static constexpr std::string_view EngineCodeEnd = ") \xE2\x80\x94";
			static constexpr std::string_view StartedBy = "Started by ";
			static constexpr std::string_view Resumed = " (resumed) \xC2\xB7 started by ";
			static constexpr std::string_view EventGraph = "Event Graph";
			static constexpr std::string_view Name = "Name";
			static constexpr std::string_view Type = "Type";
			static constexpr std::string_view Value = "Value";
			static constexpr std::string_view Inputs = "Inputs";
			static constexpr std::string_view Outputs = "Outputs";
			static constexpr std::string_view Event = "Event";
			static constexpr std::string_view Parameters = "Parameters";
			static constexpr std::string_view Locals = "Locals";
			static constexpr std::string_view Variables = "Variables";
			static constexpr std::string_view Loops = "Loops";
			static constexpr std::string_view Computed = "Computed by this execution";
			static constexpr std::string_view Select = "Select";
			static constexpr std::string_view Evaluate = "Evaluate";
			static constexpr std::string_view EvaluateHint = "A formula read in the stopped frame, such as Health * 2";
			static constexpr std::string_view NoValues = "The frame's values cannot be read.";
			static constexpr std::string_view DisableAll = "Disable All";
			static constexpr std::string_view RemoveAll = "Remove All";
			static constexpr std::string_view RemoveAllQuestion = "Remove every breakpoint of the project?";
			static constexpr std::string_view Remove = "Remove";
			static constexpr std::string_view Cancel = "Cancel";
			static constexpr std::string_view BreakOnErrors = "Break on errors";
			static constexpr std::string_view NoBreakpoints = "No breakpoints yet: F9 on a node, or click its top-left corner.";
			static constexpr std::string_view Reached = "Reached";
			static constexpr std::string_view Hits = "Hits";
			static constexpr std::string_view Filter = "Filter by name or graph";
			static constexpr std::string_view Previous = "Previous";
			static constexpr std::string_view Next = "Next";
			static constexpr std::string_view Idle = "idle";
			static constexpr std::string_view Waiting = "waiting";
			static constexpr std::string_view Stopped = "stopped";
			static constexpr std::string_view SecondsLeft = " s left";
			static constexpr std::string_view FramesLeft = " frames left";

			static constexpr std::array<std::string_view, 49> All = {CallStack,
			                                                         Values,
			                                                         Breakpoints,
			                                                         Instances,
			                                                         NothingStopped,
			                                                         NotPlaying,
			                                                         StoppedExecutions,
			                                                         Passed,
			                                                         Continue,
			                                                         EngineCode,
			                                                         EngineCodeEnd,
			                                                         StartedBy,
			                                                         Resumed,
			                                                         EventGraph,
			                                                         Name,
			                                                         Type,
			                                                         Value,
			                                                         Inputs,
			                                                         Outputs,
			                                                         Event,
			                                                         Parameters,
			                                                         Locals,
			                                                         Variables,
			                                                         Loops,
			                                                         Computed,
			                                                         Select,
			                                                         Evaluate,
			                                                         EvaluateHint,
			                                                         NoValues,
			                                                         DisableAll,
			                                                         RemoveAll,
			                                                         RemoveAllQuestion,
			                                                         Remove,
			                                                         Cancel,
			                                                         BreakOnErrors,
			                                                         NoBreakpoints,
			                                                         Reached,
			                                                         Hits,
			                                                         Filter,
			                                                         Previous,
			                                                         Next,
			                                                         Idle,
			                                                         Waiting,
			                                                         Stopped,
			                                                         SecondsLeft,
			                                                         FramesLeft,
			                                                         "#0 \xC2\xB7 \xE2\x80\xBA",
			                                                         "\xC3\x97",
			                                                         "frame "};
		};

		// What a call-stack row says (the Call Stack tab, tests).
		struct CallStackRow
		{
			uint32_t Frame = 0;
			std::string Text;
			bool Selected = false;
		};

		VisualScriptDebuggerPanel(VisualScriptDebugSession& session, EditorContext& context);

		// A frame's window (a call-stack row, an instance's graph): the window manager's FollowFrame and Open. Unset opens nothing.
		void SetFollowFrame(std::function<void(VisualScriptStop const& stop, uint32_t frame)> follow) { m_FollowFrame = std::move(follow); }
		void SetOpenNode(std::function<void(AssetHandle graph, uint32_t node)> open) { m_OpenNode = std::move(open); }
		void SetOpenGraph(std::function<void(AssetHandle graph, UUID entity)> open) { m_OpenGraph = std::move(open); }
		// The font type names are drawn in (the editor's monospace font); null draws them with the current font.
		void SetMonospaceFont(ImFont* font) { m_MonospaceFont = font; }

		// Draws the panel while it is open; a panel that was never placed docks into dockTarget (the Console's node) at first
		// use.
		void OnImGuiRender(std::optional<ImGuiID> dockTarget);

		VisualScriptDebugSession& GetSession() { return m_Session; }
		bool IsOpen() const { return m_Open; }
		void SetOpen(bool open) { m_Open = open; }
		// Opens the panel on a tab (none keeps the tab) and brings it to the front at its next draw.
		void Show(std::optional<VisualScriptDebuggerTab> tab);
		// The last draw found its tab in front.
		bool IsVisible() const { return m_Visible; }
		VisualScriptDebuggerTab GetTab() const { return m_Tab; }
		// Picks the tab at the next draw (keeping whether it is open).
		void SetTab(VisualScriptDebuggerTab tab);
		// Selects a frame of the selected stop as a call-stack click does: the session's frame, and its window.
		void SelectFrame(uint32_t frame);
		// The session's stop listener: the first stop of a play session brings the panel forward on its Call Stack.
		void OnStop(VisualScriptStop const& stop);

		// What the last draw showed: the call stack's rows and the Evaluate row's last result.
		std::vector<CallStackRow> const& GetCallStackRows() const { return m_CallStackRows; }
		std::string const& GetEvaluateResult() const { return m_EvaluateResult; }
		// The Evaluate row's formula; Evaluate runs it against the selected frame now (read-only, one-shot).
		void SetEvaluateText(std::string text) { m_EvaluateText = std::move(text); }
		std::string const& GetEvaluateText() const { return m_EvaluateText; }
		void Evaluate();
		// Whether the Remove All confirmation is open.
		bool IsRemoveAllOpen() const { return m_RemoveAllOpen; }

		// The call stack's rows of a stop, innermost first, then how its execution started (pure, for the panel and tests).
		static std::vector<std::string> DescribeCallStack(VisualScriptStop const& stop, EditorContext& context);

	private:
		void DrawCallStack();
		void DrawValues();
		void DrawBreakpoints();
		void DrawInstances();
		VisualScriptStop const* GetSelectedStop() const;

		VisualScriptDebugSession& m_Session;
		EditorContext& m_Context;
		std::function<void(VisualScriptStop const&, uint32_t)> m_FollowFrame;
		std::function<void(AssetHandle, uint32_t)> m_OpenNode;
		std::function<void(AssetHandle, UUID)> m_OpenGraph;
		ImFont* m_MonospaceFont = nullptr;

		bool m_Open = true;
		bool m_Visible = false;
		bool m_FocusRequested = false;
		VisualScriptDebuggerTab m_Tab = VisualScriptDebuggerTab::CallStack;
		std::optional<VisualScriptDebuggerTab> m_TabRequest;
		// A stop was made since the session attached (the first one brings the panel forward).
		bool m_StoppedThisSession = false;

		std::vector<CallStackRow> m_CallStackRows;
		std::string m_EvaluateText;
		std::string m_EvaluateResult;
		bool m_RemoveAllOpen = false;
		bool m_OpenRemoveAll = false;

		// The Instances tab: its filter, page, and what it read last.
		std::string m_InstanceFilter;
		uint32_t m_InstanceOffset = 0;
		std::optional<VisualScriptInstanceList> m_Instances;
		double m_InstancesRead = -1.0e30;
	};
}
