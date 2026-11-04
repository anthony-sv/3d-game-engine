#include "Editor/VisualScripting/VisualScriptDebugControls.h"

#include "Editor/GameInputRouter.h"
#include "Editor/PlayMode.h"
#include "Editor/UI/EditorUI.h"
#include "Editor/VisualScripting/VisualScriptDebugSession.h"
#include "Editor/VisualScripting/VisualScriptShortcuts.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <spdlog/fmt/fmt.h>

namespace Strada
{
	namespace
	{
		struct DebugButton
		{
			char const* Id;
			UI::ToolbarIcon Icon;
			std::string_view Label;
			// The keys that do the same (the second Step Into chord is for macOS, whose F11 shows the desktop).
			ImGuiKeyChord Chord;
			ImGuiKeyChord SecondChord;
			VisualScriptDebugAction Action;
		};

		constexpr std::array<DebugButton, 4> ResumeButtons = {{
			{"##Continue", UI::ToolbarIcon::Continue, VisualScriptDebugControls::Texts::Continue, ImGuiKey_F5, ImGuiKey_None,
		     VisualScriptDebugAction::Continue},
			{"##StepOver", UI::ToolbarIcon::StepOver, VisualScriptDebugControls::Texts::StepOver, ImGuiKey_F10, ImGuiKey_None,
		     VisualScriptDebugAction::StepOver},
			{"##StepInto", UI::ToolbarIcon::StepInto, VisualScriptDebugControls::Texts::StepInto, ImGuiKey_F11,
		     ImGuiMod_Ctrl | ImGuiKey_F11, VisualScriptDebugAction::StepInto},
			{"##StepOut", UI::ToolbarIcon::StepOut, VisualScriptDebugControls::Texts::StepOut, ImGuiMod_Shift | ImGuiKey_F11, ImGuiKey_None,
		     VisualScriptDebugAction::StepOut},
		}};
	}

	VisualScriptDebugControls::VisualScriptDebugControls(VisualScriptDebugSession& session, PlayMode& playMode, GameInputRouter& input)
		: m_Session(session),
		  m_PlayMode(playMode),
		  m_Input(input)
	{
	}

	bool VisualScriptDebugControls::IsPlayingWithDebugger() const
	{
		// The session attaches for Play alone: never while simulating or in a test run.
		return m_PlayMode.IsPlaying() && m_Session.IsAttached();
	}

	bool VisualScriptDebugControls::CanContinue() const
	{
		VisualScriptDebugSessionState const state = m_Session.GetSessionState();
		return IsPlayingWithDebugger() &&
		       (state == VisualScriptDebugSessionState::Stopped || state == VisualScriptDebugSessionState::StepEnded ||
		        (state == VisualScriptDebugSessionState::Running && m_Session.HasPausedScene()));
	}

	bool VisualScriptDebugControls::CanStep() const
	{
		VisualScriptDebugSessionState const state = m_Session.GetSessionState();
		return IsPlayingWithDebugger() &&
		       (state == VisualScriptDebugSessionState::Stopped || state == VisualScriptDebugSessionState::StepEnded);
	}

	bool VisualScriptDebugControls::CanBreak() const
	{
		VisualScriptDebugSessionState const state = m_Session.GetSessionState();
		return IsPlayingWithDebugger() &&
		       (state == VisualScriptDebugSessionState::Running || state == VisualScriptDebugSessionState::Stepping);
	}

	std::string VisualScriptDebugControls::GetBreakTooltip() const
	{
		AssetHandle const graph = m_Host.GetBreakGraph ? m_Host.GetBreakGraph() : AssetHandle();
		if (!graph.IsValid())
		{
			return std::string(Texts::BreakAny);
		}
		std::string const name = m_Host.GetGraphName ? m_Host.GetGraphName(graph) : graph.ToString();
		return fmt::format("{}{}", Texts::BreakIn, name);
	}

	float VisualScriptDebugControls::GetToolbarButtonsWidth() const
	{
		if (!IsPlayingWithDebugger())
		{
			return 0.0f;
		}
		ImGuiStyle const& style = ImGui::GetStyle();
		return UI::GetToolbarButtonSize().x * 5.0f + style.ItemSpacing.x * 5.0f;
	}

	void VisualScriptDebugControls::DrawToolbarButtons()
	{
		if (!IsPlayingWithDebugger())
		{
			return;
		}
		bool const macOS = ImGui::GetIO().ConfigMacOSXBehaviors;
		VisualScriptDebugSessionState const state = m_Session.GetSessionState();
		for (DebugButton const& button : ResumeButtons)
		{
			bool const enabled = button.Action == VisualScriptDebugAction::Continue ? CanContinue()
			                     : button.Action == VisualScriptDebugAction::StepOut
			                         ? CanStep() && state != VisualScriptDebugSessionState::StepEnded
			                         : CanStep();
			ImGui::SameLine();
			ImGui::BeginDisabled(!enabled);
			if (UI::ToolbarButton(button.Id, button.Icon, false))
			{
				// Continue resumes every stop (F5); the steps step the stop the windows show.
				bool const all = button.Action == VisualScriptDebugAction::Continue;
				m_Session.RequestResume(all ? std::nullopt : m_Session.GetSelectedStop(), button.Action, all);
			}
			std::string keys = FormatVisualScriptChord(button.Chord, macOS);
			if (button.SecondChord != ImGuiKey_None)
			{
				keys += ", " + FormatVisualScriptChord(button.SecondChord, macOS);
			}
			ImGui::SetItemTooltip("%s", fmt::format("{} ({})", button.Label, keys).c_str());
			ImGui::EndDisabled();
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!CanBreak());
		if (UI::ToolbarButton("##Break", UI::ToolbarIcon::Break, false))
		{
			// Arming a break runs no graph code: it is the session's configuration, made at once.
			AssetHandle const graph = m_Host.GetBreakGraph ? m_Host.GetBreakGraph() : AssetHandle();
			Result<void> const armed = m_Session.Break(false, graph.IsValid() ? std::optional<AssetHandle>(graph) : std::nullopt);
			UI::ReportFailure(armed, "Breaking");
		}
		ImGui::SetItemTooltip("%s", GetBreakTooltip().c_str());
		ImGui::EndDisabled();
	}

	void VisualScriptDebugControls::TogglePause()
	{
		if (!m_PlayMode.IsPlaying())
		{
			return;
		}
		// While an execution is stopped, resuming play is Continue (spec-slice4 §4.9): queued for the next frame, never run
		// while the panels draw.
		if (m_Session.GetFrameStepRefusal())
		{
			m_Session.RequestResume(std::nullopt, VisualScriptDebugAction::Continue, true, true);
			return;
		}
		// Refused only while an execution is stopped, which is handled above.
		static_cast<void>(m_PlayMode.SetPaused(!m_PlayMode.IsPaused()));
	}

	std::string VisualScriptDebugControls::GetPauseTooltip() const
	{
		if (m_Session.GetFrameStepRefusal())
		{
			return std::string(Texts::PauseWhileStopped);
		}
		return std::string(m_PlayMode.IsPaused() ? Texts::Resume : Texts::Pause);
	}

	bool VisualScriptDebugControls::CanStepFrame() const
	{
		return m_PlayMode.IsPaused() && !m_Session.GetFrameStepRefusal();
	}

	std::optional<std::string> VisualScriptDebugControls::GetStepRefusal() const
	{
		return m_Session.GetFrameStepRefusalForUI();
	}

	void VisualScriptDebugControls::StepFrame()
	{
		if (CanStepFrame())
		{
			m_PlayMode.Step();
		}
	}

	void VisualScriptDebugControls::HandleKeys()
	{
		// The global routes ignore modals: the keys do nothing while one is open (a confirmation acts on what is shown).
		if (ImGui::GetTopMostPopupModal() != nullptr || !IsPlayingWithDebugger())
		{
			return;
		}
		ImGuiInputFlags const route = ImGuiInputFlags_RouteGlobal;
		bool const stepped = CanStep();
		if (ImGui::Shortcut(ImGuiKey_F5, route) && CanContinue())
		{
			m_Session.RequestResume(std::nullopt, VisualScriptDebugAction::Continue, true);
		}
		if (ImGui::Shortcut(ImGuiKey_F10, route) && stepped)
		{
			m_Session.RequestResume(m_Session.GetSelectedStop(), VisualScriptDebugAction::StepOver);
		}
		bool const into = ImGui::Shortcut(ImGuiKey_F11, route);
		bool const intoSecond = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F11, route);
		if ((into || intoSecond) && stepped)
		{
			m_Session.RequestResume(m_Session.GetSelectedStop(), VisualScriptDebugAction::StepInto);
		}
		if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_F11, route) && stepped &&
		    m_Session.GetSessionState() != VisualScriptDebugSessionState::StepEnded)
		{
			m_Session.RequestResume(m_Session.GetSelectedStop(), VisualScriptDebugAction::StepOut);
		}
	}

	bool VisualScriptDebugControls::IsWindowShownWithoutTabChange(char const* name)
	{
		ImGuiWindow const* window = ImGui::FindWindowByName(name);
		return window != nullptr && window->WasActive && !window->Hidden && (window->DockNode == nullptr || window->DockTabIsVisible);
	}

	void VisualScriptDebugControls::OnStop(VisualScriptStop const&)
	{
		// While the game has the input only Shift+Esc and the play chords reach the editor: the debug keys need it back.
		// The listener is told after the scene was paused for the stop.
		if (m_PlayMode.IsPaused() && m_Input.HasInput())
		{
			m_Input.Release();
			m_TookInput = true;
		}
	}

	void VisualScriptDebugControls::OnResume()
	{
		if (!m_TookInput)
		{
			return;
		}
		m_TookInput = false;
		// Granting brings the Game View to the front: only where it shows without a tab change, so the graph window in front of
		// it in a shared dock node stays there (its preview offers the game instead).
		if (m_Host.IsGameViewVisible && m_Host.IsGameViewVisible())
		{
			m_Input.Grant();
		}
	}
}
