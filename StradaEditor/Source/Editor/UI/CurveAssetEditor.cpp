#include "Editor/UI/CurveAssetEditor.h"

#include "Editor/UI/CurveEditorImpl.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/CurveAsset.h"

#include <imgui.h>

#include <filesystem>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		// The asset's name as descriptions end with it: its file's name without the extension.
		std::string GetCurveTitle(AssetHandle asset)
		{
			std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(asset);
			if (!metadata)
			{
				return "Curve";
			}
			if (!metadata->Path.empty())
			{
				return std::filesystem::path(metadata->Path).stem().generic_string();
			}
			return metadata->Name.empty() ? std::string("Curve") : metadata->Name;
		}

		Json MakeKeysPatch(Curve const& curve)
		{
			Json patch = Json::object();
			patch["Keys"] = JsonTraits<std::vector<CurveKey>>::ToJson(curve.Keys);
			return patch;
		}
	}

	CurveAssetEditor::CurveAssetEditor()
		: m_Editor(NudgeMergeKey)
	{
	}

	DataAssetDrawer CurveAssetEditor::MakeDrawer(Ref<CurveAssetEditor> editor)
	{
		return [shared = std::move(editor)](DataAssetDrawContext const& drawContext)
		{
			return shared->Draw(drawContext);
		};
	}

	void CurveAssetEditor::HandleKeys()
	{
		// Registered every frame the widget is drawn, as focused routes of its child window: they win over the editor's
		// global editing keys while the view has the keyboard, and decide the next frame.
		bool const typing = ImGui::GetIO().WantTextInput;
		auto const pressed = [typing](ImGuiKeyChord chord, ImGuiInputFlags flags)
		{
			return ImGui::Shortcut(chord, ImGuiInputFlags_RouteFocused | flags) && !typing;
		};
		bool const deleteKey = pressed(ImGuiKey_Delete, ImGuiInputFlags_None);
		bool const deleteBackspace = pressed(ImGuiMod_Ctrl | ImGuiKey_Backspace, ImGuiInputFlags_None);
		bool const selectAll = pressed(ImGuiMod_Ctrl | ImGuiKey_A, ImGuiInputFlags_None);
		bool const left = pressed(ImGuiKey_LeftArrow, ImGuiInputFlags_Repeat);
		bool const right = pressed(ImGuiKey_RightArrow, ImGuiInputFlags_Repeat);
		bool const coarseLeft = pressed(ImGuiMod_Shift | ImGuiKey_LeftArrow, ImGuiInputFlags_Repeat);
		bool const coarseRight = pressed(ImGuiMod_Shift | ImGuiKey_RightArrow, ImGuiInputFlags_Repeat);
		bool const frame = pressed(ImGuiKey_F, ImGuiInputFlags_None);
		// Escape is the view's only during a drag, which it cancels; otherwise it stays the window's.
		bool const escape = m_Editor.IsGestureActive() && pressed(ImGuiKey_Escape, ImGuiInputFlags_None);
		if (deleteKey || deleteBackspace)
		{
			m_Editor.QueueAction(CurveEditorAction::Delete);
		}
		if (selectAll)
		{
			m_Editor.QueueAction(CurveEditorAction::SelectAll);
		}
		if (left)
		{
			m_Editor.QueueAction(CurveEditorAction::NudgeLeft);
		}
		if (right)
		{
			m_Editor.QueueAction(CurveEditorAction::NudgeRight);
		}
		if (coarseLeft)
		{
			m_Editor.QueueAction(CurveEditorAction::NudgeLeftCoarse);
		}
		if (coarseRight)
		{
			m_Editor.QueueAction(CurveEditorAction::NudgeRightCoarse);
		}
		if (frame)
		{
			m_Editor.QueueAction(CurveEditorAction::Frame);
		}
		if (escape)
		{
			m_Editor.CancelGesture();
		}
	}

	std::optional<DataAssetEdit> CurveAssetEditor::Draw(DataAssetDrawContext const& context)
	{
		if (context.Asset != m_Asset)
		{
			m_Editor.Reset();
			m_Asset = context.Asset;
			m_DragKey = 0;
			m_DragAction.clear();
			m_Expected.reset();
		}
		m_Editor.BeginFrame();
		Curve curve;
		if (Result<void> const read = JsonTraits<Curve>::FromJson(context.Fields, curve, DeserializationContext{}); !read)
		{
			ImGui::TextDisabled("This curve cannot be shown: %s", read.GetError().c_str());
			return DataAssetEditor::DrawFields(context);
		}
		if (m_Expected && *m_Expected != curve)
		{
			// The curve changed elsewhere during the drag (an undo, curve.set): the drag ends, and that change stays (the
			// cancelled drag's intent, the curve it started from, is not committed).
			m_Editor.GetView().CancelGesture();
			m_DragKey = 0;
			m_DragAction.clear();
			m_Expected.reset();
		}

		std::string const title = GetCurveTitle(context.Asset);
		uint32_t const color = ImGui::GetColorU32(ImGuiCol_PlotLines);
		// The value field drags by a step of the values shown.
		glm::dvec2 const values = m_Editor.GetView().GetValueRange();
		auto const valueSpeed = static_cast<float>(GetNiceCurveStep((values.y - values.x) * 0.002));
		CurveEditorFrame<Curve> const frame{
			.Document = curve,
			.Title = title,
			.ReadOnly = !context.Editable,
			.Time = ImGui::GetTime(),
			.Color =
				[color](uint32_t, uint32_t)
			{
				return color;
			},
			.ValueField = [&curve, valueSpeed](uint32_t, uint32_t key) -> std::optional<glm::vec4>
			{
				float value = curve.Keys[key].Value;
				if (ImGui::DragFloat("##value", &value, valueSpeed, 0.0f, 0.0f, "%.4g"))
				{
					return glm::vec4(value, 0.0f, 0.0f, 0.0f);
				}
				return std::nullopt;
			},
			.Emphasized = std::nullopt,
			.PlayheadColor = ImGui::GetColorU32(ImGuiCol_PlotLinesHovered),
		};
		std::vector<CurveEditorChange<Curve>> changes;
		float const fontSize = ImGui::GetFontSize();
		ImGui::BeginChild("##curveEditor", ImVec2(0.0f, Height * fontSize), ImGuiChildFlags_Borders,
		                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavInputs);
		HandleKeys();
		m_Editor.DrawHeader(frame, changes);
		m_Editor.DrawCurves(frame, changes);
		m_Editor.DrawKeyMenu(frame, changes);
		m_Editor.ApplyActions(frame, changes);
		ImGui::EndChild();
		ImGui::Spacing();
		std::optional<DataAssetEdit> const fields = DataAssetEditor::DrawFields(context);

		// One change a frame reaches the asset: each is made from the curve this frame shows, so a later one supersedes an
		// earlier one.
		std::optional<DataAssetEdit> edit;
		for (CurveEditorChange<Curve>& change : changes)
		{
			switch (change.Kind)
			{
				case CurveEditorChangeKind::Drag:
					if (m_DragKey == 0)
					{
						m_DragKey = MakeMergeKey();
					}
					m_DragAction = change.Action;
					m_Expected = change.Document;
					edit = DataAssetEdit{MakeKeysPatch(change.Document), std::move(change.Action), m_DragKey};
					break;
				case CurveEditorChangeKind::EndDrag:
					m_DragKey = 0;
					m_DragAction.clear();
					m_Expected.reset();
					break;
				case CurveEditorChangeKind::CancelDrag:
					if (m_DragKey != 0 && context.Editable)
					{
						edit = DataAssetEdit{MakeKeysPatch(change.Document), m_DragAction, m_DragKey};
					}
					m_DragKey = 0;
					m_DragAction.clear();
					m_Expected.reset();
					break;
				case CurveEditorChangeKind::Change:
					// The inspector's interaction: one step while the field stays active.
					edit = DataAssetEdit{MakeKeysPatch(change.Document), std::move(change.Action), 0};
					break;
				case CurveEditorChangeKind::End:
					break;
				case CurveEditorChangeKind::Commit:
					edit = DataAssetEdit{MakeKeysPatch(change.Document), std::move(change.Action),
					                     change.MergeKey != 0 ? change.MergeKey : MakeMergeKey()};
					break;
			}
		}
		if (!context.Editable)
		{
			return std::nullopt;
		}
		return edit ? edit : fields;
	}

	template class CurveEditor<Curve>;
}
