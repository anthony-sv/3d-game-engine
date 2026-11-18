#include "Editor/Panels/MixerPanel.h"

#include "Editor/Commands/SetDataAssetCommand.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/MixerEditing.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/MixerAsset.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Project/Project.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		constexpr ImVec4 ErrorColor = ImVec4(0.95f, 0.40f, 0.35f, 1.0f);
		constexpr ImVec4 MutedColor = ImVec4(0.85f, 0.45f, 0.20f, 1.0f);
		constexpr ImVec4 AuditionColor = ImVec4(0.30f, 0.55f, 0.85f, 1.0f);
		constexpr ImU32 MeterBackground = IM_COL32(28, 28, 30, 255);
		constexpr ImU32 MeterRms = IM_COL32(70, 170, 90, 255);
		constexpr ImU32 MeterPeak = IM_COL32(220, 200, 70, 255);
		constexpr ImU32 MeterClip = IM_COL32(230, 70, 60, 255);
		// The meters show -60 dBFS to 0 dBFS; a peak at or above 0 dBFS is drawn in the clip colour.
		constexpr float MeterFloorDb = -60.0f;

		MixerPanel::DrawRecord::Point GetItemCenter()
		{
			ImVec2 const first = ImGui::GetItemRectMin();
			ImVec2 const last = ImGui::GetItemRectMax();
			return {(first.x + last.x) * 0.5f, (first.y + last.y) * 0.5f};
		}

		// How far up a meter a linear level reaches, 0 at MeterFloorDb and below, 1 at 0 dBFS and above.
		float GetMeterHeight(float level)
		{
			if (!(level > 0.0f))
			{
				return 0.0f;
			}
			float const decibels = 20.0f * std::log10(level);
			return std::clamp((decibels - MeterFloorDb) / -MeterFloorDb, 0.0f, 1.0f);
		}

		// The fields of a struct but those named.
		template<typename TStruct>
		std::vector<FieldDescriptor> GetFieldsBut(std::initializer_list<std::string_view> left)
		{
			std::vector<FieldDescriptor> fields = GetFieldDescriptors<StructTraits<TStruct>, TStruct>();
			std::erase_if(fields,
			              [left](FieldDescriptor const& field)
			              {
							  return std::ranges::find(left, field.Name) != left.end();
						  });
			return fields;
		}
	}

	MixerPanel::MixerPanel()
		: m_EditSession(MergeKeyPrefix),
		  m_BusFields(GetFieldsBut<MixerBus>({"Name", "VolumeDb", "Muted"})),
		  m_SnapshotFields(GetFieldsBut<Mixer>({"Buses", "SpeedOfSound", "Impacts"})),
		  m_OtherFields(GetFieldsBut<Mixer>({"Buses", "Snapshots"}))
	{
	}

	void MixerPanel::OnHidden()
	{
		EndAudition(true);
		m_Record.Drawn = false;
	}

	void MixerPanel::OpenAsset(AssetHandle asset)
	{
		m_Opened = asset;
	}

	std::string MixerPanel::GetTitle() const
	{
		std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(m_Shown);
		if (metadata && !metadata->Path.empty())
		{
			return std::filesystem::path(metadata->Path).stem().generic_string();
		}
		return metadata && !metadata->Name.empty() ? metadata->Name : std::string("Mixer");
	}

	void MixerPanel::OnImGuiRender(EditorOperations& operations, bool& open)
	{
		m_Record = DrawRecord{};
		if (!ImGui::Begin(WindowName, &open))
		{
			EndAudition(true);
			ImGui::End();
			return;
		}
		m_Record.Drawn = true;

		EditorContext& context = operations.GetContext();
		m_EditSession.Update(ImGui::IsAnyItemActive(), context.GetHistory());

		Project const* const project = context.GetProject();
		std::filesystem::path const projectFile = project != nullptr ? project->GetFilePath() : std::filesystem::path();
		if (projectFile != m_ProjectFile)
		{
			EndAudition(true);
			m_ProjectFile = projectFile;
			m_Opened = AssetHandle();
		}
		AssetHandle const projectMixer = project != nullptr ? project->GetSettings().Mixer : AssetHandle();
		AssetHandle const asset = m_Opened.IsValid() ? m_Opened : projectMixer;
		if (asset != m_Shown)
		{
			EndAudition(true);
			m_Shown = asset;
			m_Selected.reset();
			m_Error.clear();
			m_TextId = 0;
		}
		m_Record.Asset = asset;
		m_Record.FollowsProject = !m_Opened.IsValid();

		if (project == nullptr)
		{
			m_Record.Status = "Open a project to edit its mixer.";
			ImGui::TextDisabled("%s", m_Record.Status.c_str());
			ImGui::End();
			return;
		}

		AssetReferenceOptions options;
		options.Type = TypedDataAsset<Mixer>::GetStaticType();
		options.AllowNone = true;
		options.CanClear = m_Opened.IsValid();
		options.ClearTooltip = "Show the project's mixer";
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Mixer");
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
			m_Record.Status = "The project names no mixer: choose a Mixer asset above, or name one in Project Settings (Mixer).";
			ImGui::TextWrapped("%s", m_Record.Status.c_str());
			ImGui::End();
			return;
		}
		Result<Ref<Asset>> const loaded = AssetManager::LoadAsset(asset);
		Ref<TypedDataAsset<Mixer>> const typed = loaded ? std::dynamic_pointer_cast<TypedDataAsset<Mixer>>(loaded.GetValue()) : nullptr;
		if (typed == nullptr)
		{
			EndAudition(true);
			m_Record.Status =
				loaded ? fmt::format("{} is not a Mixer asset.", UI::DescribeAssetReference(asset)) : UI::FormatSentence(loaded.GetError());
			ImGui::PushStyleColor(ImGuiCol_Text, ErrorColor);
			ImGui::TextWrapped("%s", m_Record.Status.c_str());
			ImGui::PopStyleColor();
			ImGui::End();
			return;
		}
		RefreshMixer(asset, typed->GetVersion(), typed->GetData());
		Result<void> const editable = SetDataAssetCommand::CheckEditable(asset);
		m_Record.Editable = editable.IsOk();
		if (m_Selected && *m_Selected >= m_Mixer.Buses.size())
		{
			m_Selected.reset();
		}
		if (!m_Selected && !m_Mixer.Buses.empty())
		{
			m_Selected = 0;
		}

		// Live: the game plays through this mixer (the scene runner applies the project's to the audio engine).
		m_Record.Live = context.GetPlayState() == EditorPlayState::Play && AudioEngine::IsInitialized() && asset == projectMixer;
		m_Meters.clear();
		if (m_Record.Live)
		{
			m_Record.CurrentSnapshot = AudioEngine::GetCurrentSnapshot();
			std::vector<AudioBusMeter> const meters = AudioEngine::GetBusMeters();
			for (AudioBusMeter const& meter : meters)
			{
				m_Meters.emplace_back(MakeMixerBusKey(meter.Name), std::array<float, 2>{meter.Peak, meter.Rms});
			}
			// The game moved the mix to another snapshot (or an edit removed the one auditioned): the audition is over.
			if (m_Audition && MakeMixerBusKey(m_Record.CurrentSnapshot) != MakeMixerBusKey(m_Audition->Snapshot))
			{
				m_Audition.reset();
			}
		}
		else
		{
			// Play stopped (the scene runner returned the mix to the mixer's own values) or never started.
			m_Audition.reset();
		}

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
		else if (!m_Record.Live)
		{
			m_Record.Status = asset == projectMixer
			                      ? "Play the game to see the meters and audition snapshots."
			                      : "The game mixes through the project's mixer: this one shows no meters and auditions nothing.";
			ImGui::TextDisabled("%s", m_Record.Status.c_str());
		}
		else
		{
			ImGui::TextDisabled("Playing: snapshot %s",
			                    m_Record.CurrentSnapshot.empty() ? "(the mixer's own values)" : m_Record.CurrentSnapshot.c_str());
		}

		// A dock below the scene view is wide and short: the strips stand beside the bus and the snapshots, each pane as tall
		// as the panel. A wide one gives the bus and the snapshots a column each; a narrower one, one column that holds the
		// bus and then the snapshots and scrolls. A narrow panel stacks everything and scrolls as one. A column of fields
		// keeps FieldColumnWidthEms: the strips give up their width first, and scroll.
		float const fontSize = ImGui::GetFontSize();
		ImGuiStyle const& style = ImGui::GetStyle();
		ImVec2 const available = ImGui::GetContentRegionAvail();
		float const stripsWidth =
			static_cast<float>(m_Mixer.Buses.size()) * (fontSize * StripWidthEms + style.ItemSpacing.x) + style.WindowPadding.x * 2.0f;
		if (available.x >= fontSize * ColumnsWidthEms)
		{
			bool const threeColumns = available.x >= fontSize * ThreeColumnsWidthEms;
			float const fieldColumn = fontSize * FieldColumnWidthEms;
			float const columnsWidth = threeColumns ? 2.0f * (fieldColumn + style.ItemSpacing.x) : fieldColumn;
			float const height = std::max(available.y, fontSize * 10.0f);
			float const strips =
				std::min({stripsWidth, available.x * StripsShare, std::max(available.x - columnsWidth - style.ItemSpacing.x, 0.0f)});
			DrawStrips(operations, m_Record.Editable, ImVec2(strips, height));
			ImGui::SameLine();
			if (threeColumns)
			{
				// The bus's field table is the wider of the two, and neither is narrower than a column of fields: the strips
				// left both at least that and a spacing between them.
				float const rest = available.x - strips - style.ItemSpacing.x * 2.0f;
				float const column = std::clamp(rest * 0.55f, fieldColumn, std::max(rest - fieldColumn - style.ItemSpacing.x, fieldColumn));
				ImGui::BeginChild("##bus", ImVec2(column, height), ImGuiChildFlags_None);
				m_Record.BusColumnWidth = ImGui::GetContentRegionAvail().x;
				DrawSelectedBus(operations, m_Record.Editable);
				ImGui::EndChild();
				ImGui::SameLine();
				ImGui::BeginChild("##snapshots", ImVec2(0.0f, height), ImGuiChildFlags_None);
				m_Record.SnapshotsColumnWidth = ImGui::GetContentRegionAvail().x;
				DrawSnapshots(operations, m_Record.Editable);
				if (ImGui::CollapsingHeader("Doppler and Impacts"))
				{
					DrawMixerFields(operations, m_Record.Editable);
				}
				ImGui::EndChild();
			}
			else
			{
				ImGui::BeginChild("##fields", ImVec2(0.0f, height), ImGuiChildFlags_None);
				m_Record.BusColumnWidth = ImGui::GetContentRegionAvail().x;
				m_Record.SnapshotsColumnWidth = m_Record.BusColumnWidth;
				DrawSelectedBus(operations, m_Record.Editable);
				if (ImGui::CollapsingHeader("Snapshots", ImGuiTreeNodeFlags_DefaultOpen))
				{
					DrawSnapshots(operations, m_Record.Editable);
				}
				if (ImGui::CollapsingHeader("Doppler and Impacts"))
				{
					DrawMixerFields(operations, m_Record.Editable);
				}
				ImGui::EndChild();
			}
		}
		else
		{
			DrawStrips(operations, m_Record.Editable, ImVec2(0.0f, fontSize * 13.0f));
			m_Record.BusColumnWidth = ImGui::GetContentRegionAvail().x;
			m_Record.SnapshotsColumnWidth = m_Record.BusColumnWidth;
			if (ImGui::CollapsingHeader("Bus", ImGuiTreeNodeFlags_DefaultOpen))
			{
				DrawSelectedBus(operations, m_Record.Editable);
			}
			if (ImGui::CollapsingHeader("Snapshots", ImGuiTreeNodeFlags_DefaultOpen))
			{
				DrawSnapshots(operations, m_Record.Editable);
			}
			if (ImGui::CollapsingHeader("Doppler and Impacts"))
			{
				DrawMixerFields(operations, m_Record.Editable);
			}
		}
		ImGui::End();
	}

	void MixerPanel::RefreshMixer(AssetHandle asset, uint64_t version, Mixer const& data)
	{
		if (m_MixerAsset == asset && m_MixerVersion == version)
		{
			return;
		}
		m_Mixer = data;
		m_MixerAsset = asset;
		m_MixerVersion = version;
	}

	void MixerPanel::Commit(EditorOperations& operations, Mixer const& mixer, std::string description, uint64_t mergeKey)
	{
		Json patch = SerializeFields<StructTraits<Mixer>>(mixer);
		uint64_t const key = mergeKey != 0 ? mergeKey : StepMergeKeyBase + ++m_Steps;
		Result<void> const applied = operations.SetDataAssetFields(m_Shown, patch, std::move(description), key);
		UI::ReportFailure(applied, "Editing the mixer");
		m_Error = applied ? std::string() : UI::FormatSentence(applied.GetError());
	}

	void MixerPanel::DrawStrips(EditorOperations& operations, bool editable, ImVec2 size)
	{
		float const height = size.y;
		ImGui::BeginChild("##strips", size, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
		// Every strip starts at the top: SameLine alone would line a strip up with the text baseline of the one before.
		float const top = ImGui::GetCursorPosY();
		for (uint32_t index = 0; index < m_Mixer.Buses.size(); index++)
		{
			if (index > 0)
			{
				ImGui::SameLine();
				ImGui::SetCursorPosY(top);
			}
			DrawStrip(operations, index, editable, height - ImGui::GetStyle().WindowPadding.y * 2.0f - ImGui::GetStyle().ScrollbarSize);
		}
		ImGui::EndChild();
	}

	void MixerPanel::DrawStrip(EditorOperations& operations, uint32_t index, bool editable, float height)
	{
		MixerBus const& bus = m_Mixer.Buses[index];
		DrawRecord::Strip strip;
		strip.Name = bus.Name;
		strip.VolumeDb = bus.VolumeDb;
		strip.Muted = bus.Muted;
		strip.Selected = m_Selected == index;
		std::string const key = MakeMixerBusKey(bus.Name);
		for (auto const& [meterBus, levels] : m_Meters)
		{
			if (meterBus == key)
			{
				strip.Peak = levels[0];
				strip.Rms = levels[1];
			}
		}

		float const fontSize = ImGui::GetFontSize();
		float const width = fontSize * StripWidthEms;
		ImGuiStyle const& style = ImGui::GetStyle();
		ImGui::PushID(static_cast<int>(index));
		ImGui::BeginGroup();
		// Every strip's name on the baseline of a frame's text, as a strip after the first gets from SameLine.
		ImGui::AlignTextToFramePadding();
		if (ImGui::Selectable(bus.Name.c_str(), strip.Selected, ImGuiSelectableFlags_None, ImVec2(width, 0.0f)))
		{
			m_Selected = index;
		}
		strip.Label = GetItemCenter();
		ImGui::SetItemTooltip("%s", bus.Parent.empty() ? "The root bus" : fmt::format("Feeds {}", bus.Parent).c_str());

		// The name above it and the mute below it take a row each.
		float const faderHeight =
			std::max(height - ImGui::GetFrameHeightWithSpacing() * 2.0f - ImGui::GetStyle().ItemSpacing.y, fontSize * 4.0f);
		// The meter: RMS as a bar, the peak as a line above it.
		float const meterWidth = fontSize * 0.9f;
		ImVec2 const meterTop = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(meterWidth, faderHeight));
		ImDrawList* const drawList = ImGui::GetWindowDrawList();
		ImVec2 const meterEnd(meterTop.x + meterWidth, meterTop.y + faderHeight);
		drawList->AddRectFilled(meterTop, meterEnd, MeterBackground);
		float const rmsTop = meterEnd.y - faderHeight * GetMeterHeight(strip.Rms);
		drawList->AddRectFilled(ImVec2(meterTop.x, rmsTop), meterEnd, MeterRms);
		if (strip.Peak > 0.0f)
		{
			float const peakY = std::floor(meterEnd.y - faderHeight * GetMeterHeight(strip.Peak));
			drawList->AddRectFilled(ImVec2(meterTop.x, peakY), ImVec2(meterEnd.x, peakY + 2.0f),
			                        strip.Peak >= 1.0f ? MeterClip : MeterPeak);
		}
		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);

		// The fader: the asset's volume, one undo step per drag.
		float volume = bus.VolumeDb;
		ImGui::BeginDisabled(!editable);
		float const faderWidth = width - meterWidth - style.ItemInnerSpacing.x;
		bool const moved = ImGui::VSliderFloat("##volume", ImVec2(faderWidth, faderHeight), &volume, MinMixerVolumeDb, MaxMixerVolumeDb,
		                                       "%.1f", ImGuiSliderFlags_AlwaysClamp);
		ImVec2 const faderMin = ImGui::GetItemRectMin();
		ImVec2 const faderMax = ImGui::GetItemRectMax();
		float const faderX = (faderMin.x + faderMax.x) * 0.5f;
		// The grab's centre reaches the ends only within half a grab: the points tests drag to lie past them.
		strip.FaderTop = {faderX, faderMin.y + 1.0f};
		strip.FaderBottom = {faderX, faderMax.y - 1.0f};
		ImGui::SetItemTooltip("%s volume, dB", bus.Name.c_str());
		ImGui::EndDisabled();
		if (moved && editable && volume != bus.VolumeDb)
		{
			Mixer changed = m_Mixer;
			changed.Buses[index].VolumeDb = volume;
			Commit(operations, changed, fmt::format("Set the Volume of {} in {}", bus.Name, GetTitle()), m_EditSession.GetMergeKey());
		}

		// The mute, a step of its own.
		ImGui::BeginDisabled(!editable);
		ImGui::PushStyleColor(ImGuiCol_Button, bus.Muted ? MutedColor : style.Colors[ImGuiCol_Button]);
		bool const toggled = ImGui::Button(bus.Muted ? "Muted###mute" : "Mute###mute", ImVec2(width, 0.0f));
		ImGui::PopStyleColor();
		strip.Mute = GetItemCenter();
		ImGui::EndDisabled();
		if (toggled && editable)
		{
			Mixer changed = m_Mixer;
			changed.Buses[index].Muted = !bus.Muted;
			Commit(operations, changed, fmt::format("{} {} in {}", bus.Muted ? "Unmute" : "Mute", bus.Name, GetTitle()), 0);
		}
		ImGui::EndGroup();
		ImGui::PopID();
		m_Record.Strips.push_back(std::move(strip));
	}

	std::optional<std::string> MixerPanel::DrawCommittedText(char const* id, std::string const& current)
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

	void MixerPanel::DrawSelectedBus(EditorOperations& operations, bool editable)
	{
		if (!m_Selected)
		{
			return;
		}
		uint32_t const index = *m_Selected;
		MixerBus const& bus = m_Mixer.Buses[index];
		m_Record.Selected = index;
		bool const master = bus.Name == MasterBusName;

		ImGui::BeginDisabled(!editable);
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Bus");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-FLT_MIN);
		// Master keeps its name: the mixer has its root by it.
		ImGui::BeginDisabled(master);
		std::optional<std::string> const renamed = DrawCommittedText("##name", bus.Name);
		ImGui::EndDisabled();
		m_Record.NameField = GetItemCenter();
		bool const add = ImGui::Button("Add Bus") && m_Mixer.Buses.size() < MaxMixerBuses;
		m_Record.AddBus = GetItemCenter();
		m_Record.AddBusVisible = ImGui::IsItemVisible();
		ImGui::SetItemTooltip("A new bus that feeds %s", bus.Name.c_str());
		ImGui::SameLine();
		ImGui::BeginDisabled(master);
		bool const remove = ImGui::Button("Remove Bus");
		ImGui::EndDisabled();
		m_Record.RemoveBus = GetItemCenter();
		ImGui::EndDisabled();

		if (editable && renamed && !master)
		{
			Mixer changed = m_Mixer;
			RenameMixerBus(changed, index, *renamed);
			Commit(operations, changed, fmt::format("Rename Bus {} to {} in {}", bus.Name, *renamed, GetTitle()), 0);
			return;
		}
		if (editable && add)
		{
			Mixer changed = m_Mixer;
			uint32_t const added = AddMixerBus(changed, bus.Name);
			std::string const name = changed.Buses[added].Name;
			Commit(operations, changed, fmt::format("Add Bus {} to {}", name, GetTitle()), 0);
			if (m_Error.empty())
			{
				m_Selected = added;
			}
			return;
		}
		if (editable && remove && !master)
		{
			Mixer changed = m_Mixer;
			if (RemoveMixerBus(changed, index))
			{
				Commit(operations, changed, fmt::format("Remove Bus {} from {}", bus.Name, GetTitle()), 0);
				if (m_Error.empty())
				{
					m_Selected.reset();
				}
			}
			return;
		}

		// The bus's other fields: its parent, effects and ducking. Its name, volume and mute are the strip's and the row's above.
		Json const values = SerializeFields<StructTraits<MixerBus>>(bus);
		ImGui::PushID("bus");
		ImGui::BeginDisabled(!editable);
		std::optional<FieldChange> const change = m_FieldEditor.Draw(m_BusFields, values);
		ImGui::EndDisabled();
		ImGui::PopID();
		if (change && editable)
		{
			ApplyBusChange(operations, index, *change);
		}
	}

	void MixerPanel::ApplyBusChange(EditorOperations& operations, uint32_t bus, FieldChange const& change)
	{
		if (bus >= m_Mixer.Buses.size())
		{
			return;
		}
		Mixer changed = m_Mixer;
		if (Result<void> const applied = ApplyMixerBusChange(changed, bus, change); !applied)
		{
			m_Error = UI::FormatSentence(applied.GetError());
			return;
		}
		std::string description =
			change.Description.empty() ? fmt::format("Edit Bus {} in {}", m_Mixer.Buses[bus].Name, GetTitle()) : change.Description;
		uint64_t const mergeKey = change.MergeKey != 0 ? change.MergeKey : m_EditSession.GetMergeKey();
		Commit(operations, changed, std::move(description), mergeKey);
	}

	void MixerPanel::ApplyMixerFieldChange(EditorOperations& operations, FieldChange const& change)
	{
		Mixer changed = m_Mixer;
		if (Result<void> const applied = ApplyMixerChange(changed, change); !applied)
		{
			m_Error = UI::FormatSentence(applied.GetError());
			return;
		}
		std::string description = change.Description.empty() ? fmt::format("Edit {}", GetTitle()) : change.Description;
		uint64_t const mergeKey = change.MergeKey != 0 ? change.MergeKey : m_EditSession.GetMergeKey();
		Commit(operations, changed, std::move(description), mergeKey);
	}

	void MixerPanel::ToggleAudition(std::string const& snapshot)
	{
		if (m_Audition && MakeMixerBusKey(m_Audition->Snapshot) == MakeMixerBusKey(snapshot))
		{
			EndAudition(true);
			return;
		}
		// From one audition to the next, the mix goes back to what it had before the first.
		std::string previous = m_Audition ? m_Audition->Previous : AudioEngine::GetCurrentSnapshot();
		if (AudioEngine::TransitionTo(snapshot, AuditionSeconds))
		{
			m_Audition = Audition{AudioEngine::GetCurrentSnapshot(), std::move(previous)};
		}
	}

	void MixerPanel::EndAudition(bool blend)
	{
		if (!m_Audition)
		{
			return;
		}
		Audition const ended = std::move(*m_Audition);
		m_Audition.reset();
		// Only a mix that still auditions the panel's snapshot is the panel's to move.
		if (!blend || !AudioEngine::IsInitialized() ||
		    MakeMixerBusKey(AudioEngine::GetCurrentSnapshot()) != MakeMixerBusKey(ended.Snapshot))
		{
			return;
		}
		// The snapshot it had may be gone since: then the mixer's own values.
		if (ended.Previous.empty() || FindMixerSnapshot(AudioEngine::GetMixer(), ended.Previous) == nullptr ||
		    !AudioEngine::TransitionTo(ended.Previous, AuditionSeconds))
		{
			static_cast<void>(AudioEngine::TransitionTo("", AuditionSeconds));
		}
	}

	void MixerPanel::DrawSnapshots(EditorOperations& operations, bool editable)
	{
		Mixer const& running = AudioEngine::GetMixer();
		if (!m_Mixer.Snapshots.empty() && ImGui::BeginTable("##snapshots", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			float const fontSize = ImGui::GetFontSize();
			ImGui::TableSetupColumn("Snapshot", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Values", ImGuiTableColumnFlags_WidthFixed, fontSize * 5.0f);
			ImGui::TableSetupColumn("Audition", ImGuiTableColumnFlags_WidthFixed, fontSize * 6.0f);
			for (uint32_t index = 0; index < m_Mixer.Snapshots.size(); index++)
			{
				MixerSnapshot const& snapshot = m_Mixer.Snapshots[index];
				DrawRecord::SnapshotRow row;
				row.Name = snapshot.Name;
				row.ValueCount = static_cast<uint32_t>(snapshot.Values.size());
				// The running mix may not have a snapshot added in this frame yet: the scene runner applies edits each frame.
				row.CanAudition = m_Record.Live && !snapshot.Name.empty() && FindMixerSnapshot(running, snapshot.Name) != nullptr;
				row.Auditioning = m_Audition && MakeMixerBusKey(m_Audition->Snapshot) == MakeMixerBusKey(snapshot.Name);
				ImGui::PushID(static_cast<int>(index));
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(snapshot.Name.empty() ? "(no name)" : snapshot.Name.c_str());
				ImGui::TableNextColumn();
				std::string const valueCount = row.ValueCount == 1 ? std::string("1 value") : fmt::format("{} values", row.ValueCount);
				ImGui::TextUnformatted(valueCount.c_str());
				ImGui::TableNextColumn();
				ImGui::BeginDisabled(!row.CanAudition);
				ImGui::PushStyleColor(ImGuiCol_Button, row.Auditioning ? AuditionColor : ImGui::GetStyle().Colors[ImGuiCol_Button]);
				bool const pressed = ImGui::Button(row.Auditioning ? "Stop###audition" : "Audition###audition", ImVec2(-FLT_MIN, 0.0f));
				ImGui::PopStyleColor();
				ImGui::EndDisabled();
				row.Audition = GetItemCenter();
				row.AuditionVisible = ImGui::IsItemVisible();
				ImGui::SetItemTooltip("%s", row.Auditioning ? "Blend back to the mix as it was"
				                                            : "Blend the running mix to this snapshot while you edit it");
				ImGui::PopID();
				if (pressed && row.CanAudition)
				{
					ToggleAudition(snapshot.Name);
					row.Auditioning = m_Audition && MakeMixerBusKey(m_Audition->Snapshot) == MakeMixerBusKey(snapshot.Name);
				}
				m_Record.Snapshots.push_back(std::move(row));
			}
			ImGui::EndTable();
		}

		ImGui::BeginDisabled(!editable || m_Mixer.Snapshots.size() >= MaxMixerSnapshots);
		bool const add = ImGui::Button("Add Snapshot");
		m_Record.AddSnapshot = GetItemCenter();
		ImGui::EndDisabled();
		if (add && editable)
		{
			Mixer changed = m_Mixer;
			MixerSnapshot snapshot;
			snapshot.Name = MakeMixerSnapshotName(changed);
			std::string description = fmt::format("Add Snapshot {} to {}", snapshot.Name, GetTitle());
			changed.Snapshots.push_back(std::move(snapshot));
			Commit(operations, changed, std::move(description), 0);
			return;
		}

		// Each snapshot's name and values, in the field editor.
		Json const values = SerializeFields<StructTraits<Mixer>>(m_Mixer);
		ImGui::PushID("snapshots");
		ImGui::BeginDisabled(!editable);
		std::optional<FieldChange> const change = m_FieldEditor.Draw(m_SnapshotFields, values);
		ImGui::EndDisabled();
		ImGui::PopID();
		if (change && editable)
		{
			ApplyMixerFieldChange(operations, *change);
		}
	}

	void MixerPanel::DrawMixerFields(EditorOperations& operations, bool editable)
	{
		Json const values = SerializeFields<StructTraits<Mixer>>(m_Mixer);
		ImGui::PushID("mixer");
		ImGui::BeginDisabled(!editable);
		std::optional<FieldChange> const change = m_FieldEditor.Draw(m_OtherFields, values);
		ImGui::EndDisabled();
		ImGui::PopID();
		if (change && editable)
		{
			ApplyMixerFieldChange(operations, *change);
		}
	}
}
