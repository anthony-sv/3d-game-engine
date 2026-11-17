#include "Editor/Panels/InputActionsPanel.h"

#include "Editor/Commands/SetDataAssetCommand.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/InputActionsAsset.h"
#include "Strada/Asset/InputBindingProfile.h"
#include "Strada/Project/Project.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Serialization/JsonSerialization.h"
#include "Strada/Serialization/StructSerialization.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <filesystem>
#include <string_view>
#include <utility>

namespace Strada
{
	namespace
	{
		constexpr ImVec4 ErrorColor = ImVec4(0.95f, 0.40f, 0.35f, 1.0f);
		constexpr ImVec4 WarningColor = ImVec4(0.93f, 0.68f, 0.30f, 1.0f);
		constexpr ImVec4 DownColor = ImVec4(0.45f, 0.80f, 0.45f, 1.0f);
		constexpr ImU32 PromptColor = IM_COL32(90, 157, 255, 56);
		constexpr ImU32 PromptBorderColor = IM_COL32(90, 157, 255, 255);

		// The parts of a composite, as their buttons name them: an axis (negative, positive) and a vector (up, down, left,
		// right).
		constexpr std::array<char const*, 2> AxisPartLabels = {"-", "+"};
		constexpr std::array<char const*, 4> VectorPartLabels = {"Up", "Down", "Left", "Right"};

		// The table's columns, by index.
		constexpr int ActionColumn = 0;
		constexpr int TypeColumn = 1;
		constexpr int ControlsColumn = 2;
		constexpr int BindColumn = 3;
		constexpr int ConflictsColumn = 4;
		constexpr int ValueColumn = 5;
		constexpr int RemoveColumn = 6;
		constexpr int ColumnCount = 7;

		InputActionsPanel::DrawRecord::Point GetItemCenter()
		{
			ImVec2 const min = ImGui::GetItemRectMin();
			ImVec2 const max = ImGui::GetItemRectMax();
			return {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f};
		}

		// The asset's name as step descriptions end with it: its file's name without the extension.
		std::string GetAssetTitle(AssetHandle asset)
		{
			std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(asset);
			if (!metadata)
			{
				return "Input Actions";
			}
			if (!metadata->Path.empty())
			{
				return std::filesystem::path(metadata->Path).stem().generic_string();
			}
			return metadata->Name.empty() ? std::string("Input Actions") : metadata->Name;
		}

		std::string_view GetTypeName(InputActionType type)
		{
			for (auto const& [value, name] : EnumTraits<InputActionType>::Values)
			{
				if (value == type)
				{
					return name;
				}
			}
			return "Button";
		}

		bool EqualsIgnoringCase(std::string_view first, std::string_view second)
		{
			return std::ranges::equal(first, second,
			                          [](char a, char b)
			                          {
										  return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
									  });
		}

		// "Action", else "Action2", "Action3" and so on: the first name no action has in any letter case.
		std::string MakeActionName(InputActionMap const& map)
		{
			for (uint32_t number = 1;; number++)
			{
				std::string const name = number == 1 ? std::string("Action") : fmt::format("Action{}", number);
				bool const used = std::ranges::any_of(map.Actions,
				                                      [&name](InputAction const& action)
				                                      {
														  return EqualsIgnoringCase(action.Name, name);
													  });
				if (!used)
				{
					return name;
				}
			}
		}

		std::optional<uint32_t> FindAction(InputActionMap const& map, std::string_view name)
		{
			for (size_t index = 0; index < map.Actions.size(); index++)
			{
				if (map.Actions[index].Name == name)
				{
					return static_cast<uint32_t>(index);
				}
			}
			return std::nullopt;
		}

		// The number of parts a binding's controls have as a composite (2 or 4), else 0.
		uint32_t GetPartCount(std::string const& controls)
		{
			Result<InputControlList> const parsed = ParseInputControls(controls);
			return parsed && parsed.GetValue().Count >= 2 ? parsed.GetValue().Count : 0;
		}

		std::string FormatValue(InputActionType type, glm::vec2 value)
		{
			return type == InputActionType::Vector2 ? fmt::format("{:.2f}, {:.2f}", value.x, value.y) : fmt::format("{:.2f}", value.x);
		}

		std::string JoinNames(std::vector<std::string> const& names)
		{
			std::string text;
			for (std::string const& name : names)
			{
				text += text.empty() ? name : ", " + name;
			}
			return text;
		}
	}

	InputActionsPanel::InputActionsPanel()
		: m_EditSession(MergeKeyPrefix),
		  m_ActionFields(GetFieldDescriptors<StructTraits<InputAction>, InputAction>())
	{
	}

	void InputActionsPanel::OnHidden()
	{
		m_Capture.Cancel();
		m_Record.Drawn = false;
	}

	void InputActionsPanel::OpenAsset(AssetHandle asset)
	{
		m_Opened = asset;
	}

	bool InputActionsPanel::TakeCaptureEvent(Event const& event, ImGuiLayer::InputContext const& context)
	{
		if (!m_Capture.IsTakingInput())
		{
			return false;
		}
		// A capture listens only while its panel draws: one left behind (the panel closed or hidden without OnHidden) ends.
		if (m_Capture.IsListening() && (ImGui::GetCurrentContext() == nullptr || m_DrawnFrame < ImGui::GetFrameCount() - 1))
		{
			m_Capture.Cancel();
			return false;
		}
		return m_Capture.TakeEvent(event, context);
	}

	void InputActionsPanel::OnImGuiRender(EditorOperations& operations, bool& open)
	{
		m_Record = DrawRecord{};
		if (!ImGui::Begin(WindowName, &open))
		{
			m_Capture.Cancel();
			ImGui::End();
			return;
		}
		m_DrawnFrame = ImGui::GetFrameCount();
		m_Record.Drawn = true;

		EditorContext& context = operations.GetContext();
		m_EditSession.Update(ImGui::IsAnyItemActive(), context.GetHistory());

		Project const* const project = context.GetProject();
		std::filesystem::path const projectFile = project != nullptr ? project->GetFilePath() : std::filesystem::path();
		if (projectFile != m_ProjectFile)
		{
			m_ProjectFile = projectFile;
			m_Opened = AssetHandle();
		}
		AssetHandle const projectActions = project != nullptr ? project->GetSettings().InputActions : AssetHandle();
		AssetHandle const asset = m_Opened.IsValid() ? m_Opened : projectActions;
		if (asset != m_Shown)
		{
			m_Shown = asset;
			m_Selected.reset();
			m_Error.clear();
			m_CaptureMessage.clear();
			m_TextId = 0;
			m_Capture.Cancel();
			// The capture's end belongs to the asset left.
			static_cast<void>(m_Capture.Update(0.0f));
		}
		m_Record.Asset = asset;
		m_Record.FollowsProject = !m_Opened.IsValid();

		if (project == nullptr)
		{
			m_Record.Status = "Open a project to edit its input actions.";
			ImGui::TextDisabled("%s", m_Record.Status.c_str());
			ImGui::End();
			return;
		}

		// The asset shown: the one opened here, else the project's.
		AssetReferenceOptions options;
		options.Type = TypedDataAsset<InputActionMap>::GetStaticType();
		options.AllowNone = true;
		options.CanClear = m_Opened.IsValid();
		options.ClearTooltip = "Show the project's input actions";
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Actions");
		ImGui::SameLine();
		float const noteWidth =
			m_Record.FollowsProject && asset.IsValid() ? ImGui::CalcTextSize("(the project's)").x + ImGui::GetStyle().ItemSpacing.x : 0.0f;
		ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - noteWidth, ImGui::GetFontSize() * 6.0f));
		ImGui::PushID("asset");
		if (std::optional<AssetHandle> const chosen = m_FieldEditor.DrawAssetReference(asset, options))
		{
			m_Opened = *chosen;
		}
		ImGui::PopID();
		if (noteWidth > 0.0f)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("(the project's)");
		}

		if (!asset.IsValid())
		{
			m_Record.Status = "The project names no input actions: choose an InputActions asset above, or name one in Project "
							  "Settings (InputActions).";
			ImGui::TextWrapped("%s", m_Record.Status.c_str());
			ImGui::End();
			return;
		}
		Result<Ref<Asset>> const loaded = AssetManager::LoadAsset(asset);
		Ref<TypedDataAsset<InputActionMap>> const typed =
			loaded ? std::dynamic_pointer_cast<TypedDataAsset<InputActionMap>>(loaded.GetValue()) : nullptr;
		if (typed == nullptr)
		{
			m_Record.Status = loaded ? fmt::format("{} is not an InputActions asset.", UI::DescribeAssetReference(asset))
			                         : UI::FormatSentence(loaded.GetError());
			ImGui::PushStyleColor(ImGuiCol_Text, ErrorColor);
			ImGui::TextWrapped("%s", m_Record.Status.c_str());
			ImGui::PopStyleColor();
			m_Capture.Cancel();
			ImGui::End();
			return;
		}
		RefreshMap(asset, typed->GetVersion(), typed->GetData());
		InputActionMap const& map = m_Map;
		Result<void> const editable = SetDataAssetCommand::CheckEditable(asset);
		m_Record.Editable = editable.IsOk();
		if (!editable)
		{
			m_Capture.Cancel();
		}

		if (m_Selected && *m_Selected >= map.Actions.size())
		{
			m_Selected.reset();
		}
		UpdateCapture(operations, map, ImGui::GetIO().DeltaTime);

		if (!editable)
		{
			m_Record.Status = UI::FormatSentence(editable.GetError());
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextWrapped("%s", m_Record.Status.c_str());
			ImGui::PopStyleColor();
		}
		else if (!m_Error.empty())
		{
			m_Record.Status = m_Error;
			ImGui::PushStyleColor(ImGuiCol_Text, ErrorColor);
			ImGui::TextWrapped("%s", m_Error.c_str());
			ImGui::PopStyleColor();
		}
		else if (!m_CaptureMessage.empty())
		{
			m_Record.Status = m_CaptureMessage;
			ImGui::TextDisabled("%s", m_CaptureMessage.c_str());
		}
		else
		{
			// The line is kept, so that the table does not move under the pointer when a message comes or goes.
			ImGui::TextUnformatted("");
		}

		DrawTable(operations, map, m_Record.Editable);

		ImGui::BeginDisabled(!m_Record.Editable || map.Actions.size() >= MaxInputActions);
		if (ImGui::Button("Add Action"))
		{
			InputActionMap changed = map;
			InputAction added;
			added.Name = MakeActionName(map);
			std::string description = fmt::format("Add Action {} to {}", added.Name, GetAssetTitle(asset));
			changed.Actions.push_back(std::move(added));
			Commit(operations, changed, std::move(description), 0);
			if (m_Error.empty())
			{
				m_Selected = static_cast<uint32_t>(map.Actions.size());
			}
		}
		m_Record.AddAction = GetItemCenter();
		ImGui::EndDisabled();

		DrawDetails(operations, map, m_Record.Editable);
		m_Record.Selected = m_Selected;
		ImGui::End();
	}

	std::optional<std::string> InputActionsPanel::DrawCommittedText(char const* id, std::string const& current)
	{
		ImGuiID const itemId = ImGui::GetID(id);
		std::string text = m_TextId == itemId ? m_Text : current;
		bool const entered = ImGui::InputText(id, &text, ImGuiInputTextFlags_EnterReturnsTrue);
		bool const done = entered || ImGui::IsItemDeactivatedAfterEdit();
		if (ImGui::IsItemActive() && !entered)
		{
			m_TextId = itemId;
			m_Text = text;
		}
		else if (m_TextId == itemId)
		{
			m_TextId = 0;
			m_Text.clear();
		}
		if (done && text != current)
		{
			return text;
		}
		return std::nullopt;
	}

	void InputActionsPanel::Commit(EditorOperations& operations, InputActionMap const& map, std::string description, uint64_t mergeKey)
	{
		Json patch = Json::object();
		patch["Actions"] = JsonTraits<std::vector<InputAction>>::ToJson(map.Actions);
		uint64_t const key = mergeKey != 0 ? mergeKey : StepMergeKeyBase + ++m_Steps;
		Result<void> const applied = operations.SetDataAssetFields(m_Shown, patch, std::move(description), key);
		UI::ReportFailure(applied, "Editing the input actions");
		m_Error = applied ? std::string() : UI::FormatSentence(applied.GetError());
	}

	void InputActionsPanel::StartCapture(InputActionMap const& map, uint32_t action, uint32_t binding, int32_t part)
	{
		InputAction const& target = map.Actions[action];
		InputBindingCaptureTarget capture;
		capture.Asset = m_Shown;
		capture.Action = target.Name;
		capture.Binding = binding;
		capture.Part = part;
		capture.Kind = GetInputBindingCaptureKind(target.Type, part);
		capture.Gamepad = target.Bindings[binding].Gamepad;
		capture.Controls = target.Bindings[binding].Controls;
		m_CaptureMessage.clear();
		m_Error.clear();
		m_Capture.Start(std::move(capture));
	}

	void InputActionsPanel::UpdateCapture(EditorOperations& operations, InputActionMap const& map, float seconds)
	{
		InputBindingCaptureTarget const target = m_Capture.GetTarget();
		std::optional<uint32_t> const action = FindAction(map, target.Action);
		// What the capture binds must still be there as it was: the asset shown, the action, the binding with its controls, the
		// part.
		bool const present = target.Asset == m_Shown && action && target.Binding < map.Actions[*action].Bindings.size() &&
		                     map.Actions[*action].Bindings[target.Binding].Controls == target.Controls &&
		                     (target.Part < 0 ||
		                      static_cast<uint32_t>(target.Part) < GetPartCount(map.Actions[*action].Bindings[target.Binding].Controls)) &&
		                     GetInputBindingCaptureKind(map.Actions[*action].Type, target.Part) == target.Kind;
		if (m_Capture.IsListening() && !present)
		{
			m_Capture.Cancel();
			static_cast<void>(m_Capture.Update(0.0f));
			m_CaptureMessage = "The binding changed elsewhere: nothing was bound.";
			return;
		}
		switch (m_Capture.Update(seconds))
		{
			case InputBindingCaptureOutcome::Waiting:
				return;
			case InputBindingCaptureOutcome::Cancelled:
				m_CaptureMessage = "Nothing was bound.";
				if (m_Capture.WasClickedAway())
				{
					m_ClickedAway = target;
				}
				return;
			case InputBindingCaptureOutcome::TimedOut:
				m_CaptureMessage = fmt::format("Nothing was pressed within {} seconds: nothing was bound.",
				                               static_cast<int>(InputControlCapture::Timeout));
				return;
			case InputBindingCaptureOutcome::Captured:
				break;
		}
		if (!present)
		{
			m_CaptureMessage = "The binding changed elsewhere: nothing was bound.";
			return;
		}
		InputAction const& bound = map.Actions[*action];
		Result<std::string> const controls =
			ReplaceInputBindingControl(bound.Bindings[target.Binding].Controls, target.Part, m_Capture.GetControl());
		if (!controls)
		{
			m_Error = UI::FormatSentence(controls.GetError());
			return;
		}
		if (controls.GetValue() == bound.Bindings[target.Binding].Controls)
		{
			// No step for what changes nothing.
			m_CaptureMessage = fmt::format("Binding {} of {} reads {} already.", target.Binding, bound.Name, controls.GetValue());
			return;
		}
		InputActionMap changed = map;
		changed.Actions[*action].Bindings[target.Binding].Controls = controls.GetValue();
		Commit(operations, changed, fmt::format("Bind {} to {} on {}", bound.Name, controls.GetValue(), GetAssetTitle(m_Shown)), 0);
	}

	void InputActionsPanel::RefreshMap(AssetHandle asset, uint64_t version, InputActionMap const& data)
	{
		if (m_MapAsset == asset && m_MapVersion == version)
		{
			return;
		}
		m_Map = data;
		m_MapAsset = asset;
		m_MapVersion = version;
		m_Facts.assign(m_Map.Actions.size(), {});
		for (size_t action = 0; action < m_Map.Actions.size(); action++)
		{
			for (size_t binding = 0; binding < m_Map.Actions[action].Bindings.size(); binding++)
			{
				BindingFacts facts;
				facts.Conflicts = FindInputBindingConflicts(m_Map, action, binding);
				facts.Parts = GetPartCount(m_Map.Actions[action].Bindings[binding].Controls);
				m_Facts[action].push_back(std::move(facts));
			}
		}
	}

	void InputActionsPanel::DrawTable(EditorOperations& operations, InputActionMap const& map, bool editable)
	{
		EditorContext& context = operations.GetContext();
		Project const* const project = context.GetProject();
		Scene const& scene = context.GetScene();
		// The running game's values, when it evaluates the actions shown.
		InputActionState const& state = scene.GetInputActions();
		m_Record.Live = context.GetPlayState() == EditorPlayState::Play && scene.IsRunning() && state.GetActions() != nullptr &&
		                project != nullptr && project->GetSettings().InputActions == m_Shown;

		if (map.Actions.empty())
		{
			ImGui::TextDisabled("No actions yet: Add Action makes one.");
			return;
		}
		ImGuiTableFlags const flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
		                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
		float const detailsHeight = m_Selected ? ImGui::GetFrameHeightWithSpacing() * 9.0f : 0.0f;
		float const height = std::max(ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() - detailsHeight,
		                              ImGui::GetFrameHeightWithSpacing() * 4.0f);
		if (!ImGui::BeginTable("##actions", ColumnCount, flags, ImVec2(0.0f, height)))
		{
			return;
		}
		float const fontSize = ImGui::GetFontSize();
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.4f);
		ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, fontSize * 6.0f);
		ImGui::TableSetupColumn("Controls", ImGuiTableColumnFlags_WidthStretch, 2.4f);
		ImGui::TableSetupColumn("Bind", ImGuiTableColumnFlags_WidthFixed, fontSize * 11.0f);
		ImGui::TableSetupColumn("Conflicts", ImGuiTableColumnFlags_WidthStretch, 1.4f);
		ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, fontSize * 6.0f);
		ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, ImGui::GetFrameHeight());
		ImGui::TableHeadersRow();

		std::string const title = GetAssetTitle(m_Shown);
		for (uint32_t index = 0; index < map.Actions.size(); index++)
		{
			InputAction const& action = map.Actions[index];
			DrawRecord::ActionRow row;
			row.Name = action.Name;
			row.Type = action.Type;
			ImGui::PushID(static_cast<int>(index));
			ImGui::TableNextRow();

			// The whole row selects the action; its widgets are drawn over it.
			ImGui::TableSetColumnIndex(ActionColumn);
			ImVec2 const cell = ImGui::GetCursorScreenPos();
			bool const selected = m_Selected == index;
			if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
			                      ImVec2(0.0f, ImGui::GetFrameHeight())))
			{
				m_Selected = index;
			}
			ImGui::SetCursorScreenPos(cell);
			ImGui::BeginDisabled(!editable);
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (std::optional<std::string> const name = DrawCommittedText("##name", action.Name))
			{
				InputActionMap changed = map;
				changed.Actions[index].Name = *name;
				Commit(operations, changed, fmt::format("Rename {} to {} on {}", action.Name, *name, title), 0);
			}
			row.NameField = GetItemCenter();
			if (ImGui::IsItemActivated())
			{
				m_Selected = index;
			}

			ImGui::TableSetColumnIndex(TypeColumn);
			ImGui::SetNextItemWidth(-FLT_MIN);
			std::string const typeName(GetTypeName(action.Type));
			if (ImGui::BeginCombo("##type", typeName.c_str()))
			{
				for (auto const& [value, name] : EnumTraits<InputActionType>::Values)
				{
					std::string const label(name);
					if (ImGui::Selectable(label.c_str(), value == action.Type) && value != action.Type)
					{
						InputActionMap changed = map;
						changed.Actions[index].Type = value;
						Commit(operations, changed, fmt::format("Set Type of {} to {} on {}", action.Name, label, title), 0);
					}
				}
				ImGui::EndCombo();
			}
			row.TypeCombo = GetItemCenter();

			ImGui::TableSetColumnIndex(ControlsColumn);
			// An empty cell: a click there selects the row.
			std::string const count =
				action.Bindings.size() == 1 ? std::string("1 binding") : fmt::format("{} bindings", action.Bindings.size());
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("%s", count.c_str());
			row.Row = GetItemCenter();

			ImGui::TableSetColumnIndex(BindColumn);
			ImGui::BeginDisabled(action.Bindings.size() >= MaxInputBindingsPerAction);
			if (ImGui::SmallButton("Add Binding"))
			{
				InputActionMap changed = map;
				changed.Actions[index].Bindings.emplace_back();
				Commit(operations, changed, fmt::format("Add Binding to {} on {}", action.Name, title), 0);
			}
			row.AddBinding = GetItemCenter();
			ImGui::EndDisabled();

			ImGui::TableSetColumnIndex(ValueColumn);
			if (m_Record.Live)
			{
				if (std::optional<uint32_t> const live = state.GetActions()->Find(action.Name))
				{
					row.Value = FormatValue(action.Type, state.GetVector(*live));
					row.Down = state.IsDown(*live);
					ImGui::AlignTextToFramePadding();
					if (row.Down)
					{
						ImGui::TextColored(DownColor, "%s", row.Value.c_str());
					}
					else
					{
						ImGui::TextUnformatted(row.Value.c_str());
					}
				}
			}

			ImGui::TableSetColumnIndex(RemoveColumn);
			if (ImGui::SmallButton("x"))
			{
				InputActionMap changed = map;
				changed.Actions.erase(changed.Actions.begin() + index);
				Commit(operations, changed, fmt::format("Remove Action {} from {}", action.Name, title), 0);
				if (m_Selected && *m_Selected >= index)
				{
					m_Selected = *m_Selected == index ? std::nullopt : std::optional<uint32_t>(*m_Selected - 1);
				}
			}
			ImGui::SetItemTooltip("Remove the action");
			row.Remove = GetItemCenter();
			ImGui::EndDisabled();

			for (uint32_t binding = 0; binding < action.Bindings.size(); binding++)
			{
				DrawBindingRow(operations, map, index, binding, editable, row);
			}
			m_Record.Actions.push_back(std::move(row));
			ImGui::PopID();
		}
		ImGui::EndTable();
		if (!ImGui::IsAnyMouseDown())
		{
			m_ClickedAway.reset();
		}
	}

	void InputActionsPanel::DrawBindingRow(EditorOperations& operations, InputActionMap const& map, uint32_t action, uint32_t binding,
	                                       bool editable, DrawRecord::ActionRow& row)
	{
		InputAction const& owner = map.Actions[action];
		InputActionBinding const& bound = owner.Bindings[binding];
		DrawRecord::BindingRow record;
		record.Controls = bound.Controls;
		uint32_t parts = 0;
		if (action < m_Facts.size() && binding < m_Facts[action].size())
		{
			record.Conflicts = m_Facts[action][binding].Conflicts;
			parts = m_Facts[action][binding].Parts;
		}
		InputBindingCaptureTarget const& target = m_Capture.GetTarget();
		bool const listening =
			m_Capture.IsListening() && target.Asset == m_Shown && target.Action == owner.Name && target.Binding == binding;
		record.Listening = listening;
		record.ListeningPart = listening ? target.Part : -1;

		ImGui::PushID(static_cast<int>(binding) + 1000);
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(ActionColumn);
		ImGui::Indent(ImGui::GetFontSize());
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("Binding %u", binding);
		ImGui::Unindent(ImGui::GetFontSize());

		ImGui::TableSetColumnIndex(ControlsColumn);
		ImGui::BeginDisabled(!editable);
		if (listening)
		{
			// The prompt: a press of a mouse button over it binds that button (InputBindingCapture::TakeEvent).
			ImVec2 const min = ImGui::GetCursorScreenPos();
			ImVec2 const size(std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize()), ImGui::GetFrameHeight());
			ImVec2 const max(min.x + size.x, min.y + size.y);
			ImGui::Dummy(size);
			ImDrawList* const drawList = ImGui::GetWindowDrawList();
			drawList->AddRectFilled(min, max, PromptColor, ImGui::GetStyle().FrameRounding);
			drawList->AddRect(min, max, PromptBorderColor, ImGui::GetStyle().FrameRounding);
			m_Record.Prompt = target.Kind == InputCaptureTarget::Stick
			                      ? "Move a stick (Esc cancels)"
			                      : "Press a key, a gamepad control or a mouse button here (Esc cancels)";
			if (!m_Capture.GetNote().empty())
			{
				m_Record.Prompt = m_Capture.GetNote();
			}
			ImVec2 const textSize = ImGui::CalcTextSize(m_Record.Prompt.c_str());
			drawList->PushClipRect(min, max, true);
			drawList->AddText(ImVec2(min.x + ImGui::GetStyle().FramePadding.x, min.y + (size.y - textSize.y) * 0.5f),
			                  ImGui::GetColorU32(ImGuiCol_Text), m_Record.Prompt.c_str());
			drawList->PopClipRect();
			m_Capture.SetPrompt(glm::vec2(min.x, min.y), glm::vec2(max.x, max.y), ImGui::GetWindowViewport()->ID);
			record.ControlsField = {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f};
		}
		else
		{
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (std::optional<std::string> const controls = DrawCommittedText("##controls", bound.Controls))
			{
				InputActionMap changed = map;
				changed.Actions[action].Bindings[binding].Controls = *controls;
				Commit(operations, changed, fmt::format("Set Controls of {} on {}", owner.Name, GetAssetTitle(m_Shown)), 0);
			}
			if (ImGui::IsItemActivated())
			{
				m_Selected = action;
			}
			record.ControlsField = GetItemCenter();
		}

		ImGui::TableSetColumnIndex(BindColumn);
		auto const captureButton = [this, &map, action, binding, listening, &target](char const* buttonLabel, int32_t buttonPart)
		{
			bool const capturing = listening && target.Part == buttonPart;
			if (capturing)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			}
			if (ImGui::SmallButton(buttonLabel))
			{
				// The press of this click may have ended this very capture already (InputBindingCapture::TakeEvent): the
				// click ends it rather than starting it again.
				bool const endedByThisClick = m_ClickedAway && m_ClickedAway->Asset == m_Shown &&
				                              m_ClickedAway->Action == map.Actions[action].Name && m_ClickedAway->Binding == binding &&
				                              m_ClickedAway->Part == buttonPart;
				if (capturing)
				{
					m_Capture.Cancel();
				}
				else if (!endedByThisClick)
				{
					StartCapture(map, action, binding, buttonPart);
				}
			}
			if (capturing)
			{
				ImGui::PopStyleColor();
			}
			return GetItemCenter();
		};
		record.Bind = captureButton("Bind", -1);
		ImGui::SetItemTooltip("Press the control to bind (Esc cancels)");
		for (uint32_t part = 0; part < parts; part++)
		{
			ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
			ImGui::PushID(static_cast<int>(part));
			char const* const label = parts == 2 ? AxisPartLabels[part] : VectorPartLabels[part];
			record.Parts.push_back(captureButton(label, static_cast<int32_t>(part)));
			ImGui::SetItemTooltip("Press the control for this part (Esc cancels)");
			ImGui::PopID();
		}

		ImGui::TableSetColumnIndex(ConflictsColumn);
		if (!record.Conflicts.empty())
		{
			std::string const conflicts = JoinNames(record.Conflicts);
			ImGui::AlignTextToFramePadding();
			ImGui::TextColored(WarningColor, "%s", conflicts.c_str());
			ImGui::SetItemTooltip("These actions read a control this binding reads too: %s", conflicts.c_str());
		}

		ImGui::TableSetColumnIndex(RemoveColumn);
		if (ImGui::SmallButton("x"))
		{
			InputActionMap changed = map;
			changed.Actions[action].Bindings.erase(changed.Actions[action].Bindings.begin() + binding);
			Commit(operations, changed, fmt::format("Remove Binding {} of {} on {}", binding, owner.Name, GetAssetTitle(m_Shown)), 0);
		}
		ImGui::SetItemTooltip("Remove the binding");
		record.Remove = GetItemCenter();
		ImGui::EndDisabled();
		ImGui::PopID();
		row.Bindings.push_back(std::move(record));
	}

	void InputActionsPanel::DrawDetails(EditorOperations& operations, InputActionMap const& map, bool editable)
	{
		// An action added in this frame is in the next frame's map.
		if (!m_Selected || *m_Selected >= map.Actions.size())
		{
			return;
		}
		InputAction const& action = map.Actions[*m_Selected];
		ImGui::Separator();
		ImGui::TextUnformatted(action.Name.empty() ? "(no name)" : action.Name.c_str());
		ImGui::BeginChild("##details", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
		Json const values = SerializeFields<StructTraits<InputAction>>(action);
		ImGui::BeginDisabled(!editable);
		std::optional<FieldChange> const change = m_FieldEditor.Draw(m_ActionFields, values);
		ImGui::EndDisabled();
		ImGui::EndChild();
		if (!change || !editable)
		{
			return;
		}
		Json actions = JsonTraits<std::vector<InputAction>>::ToJson(map.Actions);
		actions[*m_Selected].update(change->MakePatch(values));
		Json patch = Json::object();
		patch["Actions"] = std::move(actions);
		std::string description =
			change->Description.empty() ? fmt::format("Edit {} on {}", action.Name, GetAssetTitle(m_Shown)) : change->Description;
		uint64_t const mergeKey = change->MergeKey != 0 ? change->MergeKey : m_EditSession.GetMergeKey();
		Result<void> const applied = operations.SetDataAssetFields(m_Shown, patch, std::move(description), mergeKey);
		UI::ReportFailure(applied, "Editing the input actions");
		m_Error = applied ? std::string() : UI::FormatSentence(applied.GetError());
	}
}
