#include "Editor/UI/EditingChordClaims.h"

#include <imgui_internal.h>

#include <cstddef>

namespace Strada::UI
{
	namespace
	{
		// The root window's storage of a claim (ImGuiWindow::StateStorage, which also keeps its tree nodes' states under their
		// IDs): the frame it was last updated in, whether it holds, and where its chords are and how many.
		struct ClaimKeys
		{
			ImGuiID Frame = ImHashStr("Strada.EditingChordClaim.Frame");
			ImGuiID Held = ImHashStr("Strada.EditingChordClaim.Held");
			ImGuiID Chords = ImHashStr("Strada.EditingChordClaim.Chords");
			ImGuiID Count = ImHashStr("Strada.EditingChordClaim.Count");
		};

		ClaimKeys const& GetClaimKeys()
		{
			static ClaimKeys const s_Keys;
			return s_Keys;
		}

		// Whether a claim updated in that frame is still current: the widget was drawn in this frame or the last.
		bool IsCurrent(ImGuiStorage const& storage, ClaimKeys const& keys)
		{
			return storage.GetInt(keys.Frame, -1) >= ImGui::GetFrameCount() - 1;
		}
	}

	void UpdateEditingChordClaim(std::span<ImGuiKeyChord const> chords)
	{
		ImGuiWindow* const window = ImGui::GetCurrentWindow();
		ImGuiStorage& storage = window->RootWindow->StateStorage;
		ClaimKeys const& keys = GetClaimKeys();
		bool const ours = storage.GetVoidPtr(keys.Chords) == chords.data();
		bool claimed = ours && IsCurrent(storage, keys) && storage.GetBool(keys.Held);
		// From inside the widget's child window: the child or a popup it opened has the keyboard; else the root window, one of
		// its children or one of their popups has it (imgui.cpp, IsWindowFocused and IsWindowChildOf).
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
		{
			claimed = true;
		}
		else if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
		{
			claimed = false;
		}
		if (!claimed && !ours)
		{
			// Another widget of the root window holds the storage: it keeps its claim.
			return;
		}
		storage.SetInt(keys.Frame, ImGui::GetFrameCount());
		storage.SetBool(keys.Held, claimed);
		// ImGuiStorage keeps pointers as void*; the chords are only ever read through GetClaimedEditingChords.
		storage.SetVoidPtr(keys.Chords, const_cast<ImGuiKeyChord*>(chords.data()));
		storage.SetInt(keys.Count, static_cast<int>(chords.size()));
	}

	std::span<ImGuiKeyChord const> GetClaimedEditingChords(ImGuiWindow const& root)
	{
		ImGuiStorage const& storage = root.StateStorage;
		ClaimKeys const& keys = GetClaimKeys();
		if (!storage.GetBool(keys.Held) || !IsCurrent(storage, keys))
		{
			return {};
		}
		auto const* const chords = static_cast<ImGuiKeyChord const*>(storage.GetVoidPtr(keys.Chords));
		int const count = storage.GetInt(keys.Count, 0);
		if (chords == nullptr || count <= 0)
		{
			return {};
		}
		return {chords, static_cast<size_t>(count)};
	}
}
