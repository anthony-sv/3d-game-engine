#include "Editor/Panels/ContentBrowserPanel.h"

#include "Editor/AssetBrowsing.h"
#include "Editor/FileDialogs.h"
#include "Editor/UI/EditorUI.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <utility>

namespace Strada
{
	namespace
	{
		struct TypeStyle
		{
			char const* Label;
			ImU32 Color;
		};

		TypeStyle GetTypeStyle(AssetType type)
		{
			switch (type)
			{
				case AssetType::Scene:
					return {"SCENE", IM_COL32(70, 140, 85, 255)};
				case AssetType::Prefab:
					return {"PREFAB", IM_COL32(65, 115, 190, 255)};
				case AssetType::Mesh:
					return {"MESH", IM_COL32(190, 115, 55, 255)};
				case AssetType::Material:
					return {"MAT", IM_COL32(140, 85, 170, 255)};
				case AssetType::Texture:
					return {"TEX", IM_COL32(55, 140, 150, 255)};
				case AssetType::Environment:
					return {"HDR", IM_COL32(175, 155, 55, 255)};
				case AssetType::AudioClip:
					return {"AUDIO", IM_COL32(175, 75, 115, 255)};
				case AssetType::Font:
					return {"FONT", IM_COL32(110, 110, 110, 255)};
				case AssetType::None:
					break;
			}
			return {"?", IM_COL32(90, 90, 90, 255)};
		}

		constexpr ImU32 FolderColor = IM_COL32(200, 160, 70, 255);
		constexpr ImVec4 MissingColor = ImVec4(0.95f, 0.40f, 0.35f, 1.0f);
		// Tiles scale with the font (and so with the display scale).
		constexpr float TileSizeInFontSizes = 5.5f;
		// Folders changed outside the editor show up after at most this many seconds.
		constexpr double RescanInterval = 1.0;
		// Frames a new rename field may take to receive keyboard focus before the rename is dropped.
		constexpr int RenameFocusFrames = 3;

		float TextWidth(std::string_view text)
		{
			return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
		}

		// Bytes of the UTF-8 code point starting at `index`.
		size_t CodePointLength(std::string_view text, size_t index)
		{
			size_t length = 1;
			while (index + length < text.size() && (static_cast<unsigned char>(text[index + length]) & 0xC0) == 0x80)
			{
				length++;
			}
			return length;
		}

		// The text shortened with "..." to fit the width, cut between code points.
		std::string FitText(std::string_view text, float width)
		{
			if (TextWidth(text) <= width)
			{
				return std::string(text);
			}
			float const ellipsis = TextWidth("...");
			size_t end = 0;
			while (end < text.size())
			{
				size_t const next = end + CodePointLength(text, end);
				if (TextWidth(text.substr(0, next)) + ellipsis > width)
				{
					break;
				}
				end = next;
			}
			return std::string(text.substr(0, end)) + "...";
		}

		// A tile label in two lines of the given width: the first breaks after the last space, '_', '-' or '.' that fits (or
		// inside a long word), the second is shortened with "..." when the rest does not fit. The second line is empty for
		// short names.
		std::array<std::string, 2> SplitLabel(std::string_view name, float width)
		{
			if (TextWidth(name) <= width)
			{
				return {std::string(name), std::string()};
			}
			size_t end = 0;
			size_t afterSeparator = 0;
			while (end < name.size())
			{
				size_t const next = end + CodePointLength(name, end);
				if (TextWidth(name.substr(0, next)) > width)
				{
					break;
				}
				if (std::string_view(" _-.").find(name[end]) != std::string_view::npos)
				{
					afterSeparator = next;
				}
				end = next;
			}
			if (afterSeparator > 0)
			{
				end = afterSeparator;
			}
			// Every line shows at least one character, however narrow the tile.
			end = std::max(end, CodePointLength(name, 0));
			std::string_view first = name.substr(0, end);
			while (first.ends_with(' '))
			{
				first.remove_suffix(1);
			}
			return {std::string(first), FitText(name.substr(end), width)};
		}

		// A file or folder name typed by the user, trimmed. Empty names cancel the rename and are not errors.
		Result<std::string> CheckNewName(std::string_view text)
		{
			size_t const first = text.find_first_not_of(" \t");
			if (first == std::string_view::npos)
			{
				return std::string();
			}
			std::string_view const name = text.substr(first, text.find_last_not_of(" \t") - first + 1);
			if (name == "." || name == ".." || name.find_first_of("/\\") != std::string_view::npos)
			{
				return MakeError("'{}' is not a valid file or folder name", name);
			}
			return std::string(name);
		}

		std::string GetImportFilterExtensions()
		{
			std::string extensions;
			for (std::string_view const extension : GetSupportedAssetExtensions())
			{
				extensions += extensions.empty() ? "" : ",";
				extensions += extension.substr(1);
			}
			return extensions;
		}
	}

	ContentBrowserPanel::Item ContentBrowserPanel::MakeFolderItem(std::string const& path)
	{
		Item item;
		item.Path = path;
		item.Name = AssetBrowsing::GetFileName(path);
		item.Label = item.Name;
		return item;
	}

	ContentBrowserPanel::Item ContentBrowserPanel::MakeAssetItem(AssetMetadata const& asset)
	{
		Item item;
		item.Path = asset.Path;
		item.Name = AssetBrowsing::GetFileName(asset.Path);
		item.Label = FileSystem::PathToUtf8(FileSystem::PathFromUtf8(item.Name).stem());
		item.Asset = asset.Handle;
		item.Type = asset.Type;
		item.Missing = AssetManager::IsMissing(asset.Handle);
		return item;
	}

	void ContentBrowserPanel::OnImGuiRender(EditorOperations& operations, bool& open)
	{
		if (!ImGui::Begin("Content Browser", &open))
		{
			ImGui::End();
			return;
		}

		EditorContext& context = operations.GetContext();
		Project const* project = context.GetProject();
		std::filesystem::path const projectFile = project != nullptr ? project->GetFilePath() : std::filesystem::path();
		if (projectFile != m_ProjectFile)
		{
			// Leaving a project starts the next one at its asset directory; a folder set before any project was shown stays.
			if (!m_ProjectFile.empty())
			{
				m_Folder.clear();
			}
			m_ProjectFile = projectFile;
			m_Filter.clear();
			m_SelectedFolder.clear();
			m_Renaming.reset();
			m_PendingDelete.reset();
			m_Listing.Valid = false;
		}
		if (project == nullptr || !AssetManager::IsInitialized() || !AssetManager::HasAssetDirectory())
		{
			ImGui::TextDisabled("No project is open.");
			ImGui::TextWrapped("Create or open a project from the File menu to manage its assets.");
			ImGui::End();
			return;
		}
		// Entities selected elsewhere replace a selected folder, as they replace a selected asset.
		if (!context.GetSelection().IsEmpty())
		{
			m_SelectedFolder.clear();
		}

		DrawToolbar(operations);
		ImGui::Separator();
		// After the toolbar, which may navigate, search or create.
		UpdateListing();
		DrawItems(operations);
		HandleShortcuts(context);
		DrawDeletePopup(operations);

		std::vector<std::function<void()>> deferred;
		deferred.swap(m_Deferred);
		for (std::function<void()> const& action : deferred)
		{
			action();
		}
		ImGui::End();
	}

	void ContentBrowserPanel::UpdateListing()
	{
		double const now = ImGui::GetTime();
		if (m_Listing.Valid && m_Listing.Folder == m_Folder && m_Listing.Filter == m_Filter &&
		    m_Listing.AssetVersion == AssetManager::GetVersion() && now - m_Listing.ScanTime < RescanInterval)
		{
			return;
		}
		// The folder may have been moved or deleted (by automation, or outside the editor).
		if (!m_Folder.empty() && !FileSystem::IsDirectory(AssetManager::GetAssetDirectory() / FileSystem::PathFromUtf8(m_Folder)))
		{
			m_Folder.clear();
		}
		if (m_Listing.Folder != m_Folder || m_Listing.Filter != m_Filter)
		{
			m_ScrollToTop = true;
		}

		std::vector<Item> items;
		if (!m_Filter.empty())
		{
			for (AssetMetadata const& asset : AssetBrowsing::SearchAssets(m_Filter))
			{
				items.push_back(MakeAssetItem(asset));
			}
		}
		else
		{
			for (std::string const& folder : AssetBrowsing::ListFolders(m_Folder))
			{
				items.push_back(MakeFolderItem(folder));
			}
			for (AssetMetadata const& asset : AssetBrowsing::ListAssets(m_Folder))
			{
				items.push_back(MakeAssetItem(asset));
			}
		}
		m_Listing.Valid = true;
		m_Listing.Folder = m_Folder;
		m_Listing.Filter = m_Filter;
		m_Listing.AssetVersion = AssetManager::GetVersion();
		m_Listing.ScanTime = now;
		m_Listing.Items = std::move(items);

		// A rename or deletion whose item went away (moved or deleted elsewhere) ends.
		auto const isListed = [this](Item const& item)
		{
			return std::any_of(m_Listing.Items.begin(), m_Listing.Items.end(),
			                   [&item](Item const& listed)
			                   {
								   return listed.Path == item.Path;
							   });
		};
		if (m_Renaming && !isListed(*m_Renaming))
		{
			m_Renaming.reset();
		}
		if (m_PendingDelete && !isListed(*m_PendingDelete))
		{
			m_PendingDelete.reset();
		}
	}

	void ContentBrowserPanel::DrawToolbar(EditorOperations& operations)
	{
		float const rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
		ImGui::BeginDisabled(m_Folder.empty());
		if (ImGui::ArrowButton("##up", ImGuiDir_Up))
		{
			m_Folder = AssetBrowsing::GetParentFolder(m_Folder);
			m_Filter.clear();
		}
		ImGui::EndDisabled();
		ImGui::SetItemTooltip("Parent folder");

		// Breadcrumbs; each one also takes dropped assets and folders.
		ImGui::SameLine();
		if (ImGui::Button("Assets"))
		{
			m_Folder.clear();
			m_Filter.clear();
		}
		AcceptDrop(operations, "");
		size_t start = 0;
		while (start < m_Folder.size())
		{
			size_t const end = std::min(m_Folder.find('/', start), m_Folder.size());
			std::string const partial = m_Folder.substr(0, end);
			ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
			ImGui::TextDisabled(">");
			ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
			ImGui::PushID(partial.c_str());
			bool const clicked = ImGui::Button(m_Folder.substr(start, end - start).c_str());
			AcceptDrop(operations, partial);
			ImGui::PopID();
			if (clicked)
			{
				m_Folder = partial;
				m_Filter.clear();
			}
			start = end + 1;
		}

		// The search field and the buttons sit at the right edge unless the breadcrumbs reach it.
		float const searchWidth = ImGui::GetFontSize() * 14.0f;
		float const buttonsWidth = ImGui::CalcTextSize("+").x + ImGui::CalcTextSize("Import").x + ImGui::CalcTextSize("Refresh").x +
		                           ImGui::GetStyle().FramePadding.x * 6.0f + ImGui::GetStyle().ItemSpacing.x * 3.0f;
		ImGui::SameLine();
		ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rightEdge - searchWidth - buttonsWidth));
		ImGui::SetNextItemWidth(searchWidth);
		ImGui::InputTextWithHint("##search", "Search", &m_Filter);
		ImGui::SameLine();
		if (ImGui::Button("+"))
		{
			ImGui::OpenPopup("##create");
		}
		ImGui::SetItemTooltip("Create");
		if (ImGui::BeginPopup("##create"))
		{
			if (ImGui::MenuItem("Folder"))
			{
				CreateFolder(operations);
			}
			if (ImGui::MenuItem("Material"))
			{
				CreateMaterial(operations);
			}
			ImGui::EndPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Import"))
		{
			ImportFiles(operations);
		}
		ImGui::SetItemTooltip("Copy files into this folder");
		ImGui::SameLine();
		if (ImGui::Button("Refresh"))
		{
			UI::ReportFailure(operations.RefreshAssets(), "Refreshing the assets");
			m_Listing.Valid = false;
		}
		ImGui::SetItemTooltip("Find new, missing and modified files");
	}

	void ContentBrowserPanel::DrawItems(EditorOperations& operations)
	{
		if (ImGui::BeginChild("##items"))
		{
			if (std::exchange(m_ScrollToTop, false))
			{
				ImGui::SetScrollY(0.0f);
			}
			std::vector<Item>& items = m_Listing.Items;
			ImGuiStyle const& style = ImGui::GetStyle();
			float const tileSize = std::round(ImGui::GetFontSize() * TileSizeInFontSizes);
			size_t const columns = static_cast<size_t>(
				std::max(1.0f, std::floor((ImGui::GetContentRegionAvail().x + style.ItemSpacing.x) / (tileSize + style.ItemSpacing.x))));
			size_t const rows = (items.size() + columns - 1) / columns;

			// Only visible rows are drawn. A row is a tile, two label lines and the spacing between them and to the next row.
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(rows), tileSize + ImGui::GetTextLineHeight() * 2.0f + style.ItemSpacing.y * 3.0f);
			if (m_Renaming)
			{
				// The renamed item is drawn even when scrolled away, so its field can take focus (which scrolls to it).
				auto const renamed = std::find_if(items.begin(), items.end(),
				                                  [this](Item const& item)
				                                  {
													  return item.Path == m_Renaming->Path;
												  });
				if (renamed != items.end())
				{
					clipper.IncludeItemByIndex(static_cast<int>(static_cast<size_t>(renamed - items.begin()) / columns));
				}
			}
			while (clipper.Step())
			{
				for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
				{
					for (size_t column = 0; column < columns; column++)
					{
						size_t const index = static_cast<size_t>(row) * columns + column;
						if (index >= items.size())
						{
							break;
						}
						if (column > 0)
						{
							ImGui::SameLine();
						}
						DrawItem(operations, items[index], tileSize);
					}
				}
			}
			clipper.End();

			if (items.empty())
			{
				ImGui::TextDisabled(m_Filter.empty() ? "This folder is empty." : "No assets match the search.");
			}
			DrawBackground(operations);
		}
		ImGui::EndChild();
	}

	void ContentBrowserPanel::DrawItem(EditorOperations& operations, Item& item, float tileSize)
	{
		EditorContext& context = operations.GetContext();
		ImGui::PushID(item.Path.c_str());
		ImGui::BeginGroup();

		ImVec2 const tileMin = ImGui::GetCursorScreenPos();
		ImVec2 const tileMax(tileMin.x + tileSize, tileMin.y + tileSize);
		ImGui::InvisibleButton("##tile", ImVec2(tileSize, tileSize));
		bool const hovered = ImGui::IsItemHovered();
		bool const selected = IsSelected(context, item);

		if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right))
		{
			Select(operations, item);
		}
		if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		{
			Open(operations, item);
		}
		if (item.IsFolder())
		{
			if (ImGui::BeginDragDropSource())
			{
				ImGui::SetDragDropPayload(DragDropPayload::AssetFolder, item.Path.data(), item.Path.size());
				ImGui::TextUnformatted(item.Name.c_str());
				ImGui::EndDragDropSource();
			}
			AcceptDrop(operations, item.Path);
		}
		else
		{
			UI::AssetDragSource(item.Asset, item.Name);
		}
		if (hovered && ImGui::BeginItemTooltip())
		{
			ImGui::TextUnformatted(item.Path.c_str());
			if (item.Missing)
			{
				ImGui::TextColored(MissingColor, "The file is missing.");
			}
			ImGui::EndTooltip();
		}
		DrawItemContextMenu(operations, item);

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		if (selected || hovered)
		{
			drawList->AddRectFilled(tileMin, tileMax, ImGui::GetColorU32(selected ? ImGuiCol_Header : ImGuiCol_HeaderHovered), 4.0f);
		}
		float const inset = tileSize * 0.16f;
		ImVec2 const iconMin(tileMin.x + inset, tileMin.y + inset);
		ImVec2 const iconMax(tileMax.x - inset, tileMax.y - inset);
		if (item.IsFolder())
		{
			float const tabHeight = (iconMax.y - iconMin.y) * 0.18f;
			drawList->AddRectFilled(ImVec2(iconMin.x, iconMin.y + tabHeight * 0.4f),
			                        ImVec2(iconMin.x + (iconMax.x - iconMin.x) * 0.45f, iconMin.y + tabHeight * 1.4f), FolderColor, 3.0f);
			drawList->AddRectFilled(ImVec2(iconMin.x, iconMin.y + tabHeight), iconMax, FolderColor, 4.0f);
		}
		else
		{
			TypeStyle const style = GetTypeStyle(item.Type);
			drawList->AddRectFilled(iconMin, iconMax, style.Color, 6.0f);
			ImVec2 const labelSize = ImGui::CalcTextSize(style.Label);
			drawList->AddText(ImVec2((iconMin.x + iconMax.x - labelSize.x) * 0.5f, (iconMin.y + iconMax.y - labelSize.y) * 0.5f),
			                  IM_COL32(255, 255, 255, 230), style.Label);
		}

		if (m_Renaming && m_Renaming->Path == item.Path)
		{
			DrawRenameField(operations, item, tileSize);
		}
		else
		{
			if (item.LinesWidth != tileSize)
			{
				item.Lines = SplitLabel(item.Label, tileSize);
				item.LinesWidth = tileSize;
			}
			// Always two lines (the second may be empty) so the tiles of a row line up.
			ImGui::PushStyleColor(ImGuiCol_Text, item.Missing ? MissingColor : ImGui::GetStyleColorVec4(ImGuiCol_Text));
			for (std::string const& line : item.Lines)
			{
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max((tileSize - TextWidth(line)) * 0.5f, 0.0f));
				ImGui::TextUnformatted(line.c_str());
			}
			ImGui::PopStyleColor();
		}
		ImGui::EndGroup();
		ImGui::PopID();
	}

	void ContentBrowserPanel::DrawItemContextMenu(EditorOperations& operations, Item const& item)
	{
		if (!ImGui::BeginPopupContextItem("##itemMenu"))
		{
			return;
		}
		if ((item.IsFolder() || item.Type == AssetType::Scene) && ImGui::MenuItem("Open"))
		{
			Open(operations, item);
		}
		if (ImGui::MenuItem("Rename", "F2"))
		{
			BeginRename(item);
		}
		if (ImGui::MenuItem("Delete", "Delete"))
		{
			m_PendingDelete = item;
			m_OpenDeletePopup = true;
		}
		ImGui::Separator();
		if (!item.IsFolder() && ImGui::MenuItem("Copy Reference"))
		{
			ImGui::SetClipboardText(AssetManager::GetReference(item.Asset).c_str());
		}
		if (ImGui::MenuItem("Copy Path"))
		{
			ImGui::SetClipboardText(
				FileSystem::PathToUtf8(AssetManager::GetAssetDirectory() / FileSystem::PathFromUtf8(item.Path)).c_str());
		}
		ImGui::EndPopup();
	}

	void ContentBrowserPanel::DrawBackground(EditorOperations& operations)
	{
		// The space after the tiles: clicking clears the browser's selection, dropping moves into this folder.
		ImVec2 size = ImGui::GetContentRegionAvail();
		size.x = std::max(size.x, 1.0f);
		size.y = std::max(size.y, ImGui::GetFrameHeight());
		ImGui::InvisibleButton("##background", size);
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
		{
			m_SelectedFolder.clear();
			operations.GetContext().SelectAsset(AssetHandle());
		}
		AcceptDrop(operations, m_Folder);
		if (ImGui::BeginPopupContextItem("##backgroundMenu"))
		{
			if (ImGui::MenuItem("New Folder"))
			{
				CreateFolder(operations);
			}
			if (ImGui::MenuItem("New Material"))
			{
				CreateMaterial(operations);
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Import..."))
			{
				ImportFiles(operations);
			}
			if (ImGui::MenuItem("Refresh"))
			{
				UI::ReportFailure(operations.RefreshAssets(), "Refreshing the assets");
				m_Listing.Valid = false;
			}
			ImGui::EndPopup();
		}
	}

	void ContentBrowserPanel::DrawRenameField(EditorOperations& operations, Item const& item, float width)
	{
		if (std::exchange(m_FocusRenameField, false))
		{
			ImGui::SetKeyboardFocusHere();
			m_RenameFieldFrame = ImGui::GetFrameCount();
		}
		ImGui::SetNextItemWidth(width);
		ImGui::InputText("##rename", &m_RenameBuffer, ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue);
		bool const active = ImGui::IsItemActive();
		// As tall as the two label lines it replaces, so the row keeps its height.
		float const padding = ImGui::GetTextLineHeight() * 2.0f - ImGui::GetFrameHeight();
		if (padding > 0.0f)
		{
			ImGui::Dummy(ImVec2(width, padding));
		}
		if (active)
		{
			m_RenameFieldActivated = true;
			return;
		}
		if (!m_RenameFieldActivated)
		{
			// Focus requests take effect within a frame or two; a field that never got focus is dropped.
			if (m_RenameFieldFrame >= 0 && ImGui::GetFrameCount() - m_RenameFieldFrame >= RenameFocusFrames)
			{
				m_Renaming.reset();
			}
			return;
		}

		// The field lost focus: Enter or a click elsewhere commits; Escape restored the original text.
		m_Renaming.reset();
		Result<std::string> const name = CheckNewName(m_RenameBuffer);
		if (!name)
		{
			ST_ERROR("Renaming failed: {}", name.GetError());
			return;
		}
		std::string const extension =
			item.IsFolder() ? std::string() : FileSystem::PathToUtf8(FileSystem::PathFromUtf8(item.Name).extension());
		std::string const newName = name.GetValue() + extension;
		if (name.GetValue().empty() || newName == item.Name)
		{
			return;
		}
		std::string const newPath = AssetBrowsing::JoinPath(AssetBrowsing::GetParentFolder(item.Path), newName);
		Defer(
			[this, &operations, item, newPath]
			{
				Result<void> const renamed =
					item.IsFolder() ? operations.MoveAssetFolder(item.Path, newPath) : operations.MoveAsset(item.Asset, newPath);
				UI::ReportFailure(renamed, "Renaming");
				if (renamed && m_SelectedFolder == item.Path)
				{
					m_SelectedFolder = newPath;
				}
			});
	}

	void ContentBrowserPanel::DrawDeletePopup(EditorOperations& operations)
	{
		if (std::exchange(m_OpenDeletePopup, false))
		{
			ImGui::OpenPopup("Delete Asset");
		}
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		if (!ImGui::BeginPopupModal("Delete Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
		{
			return;
		}
		if (!m_PendingDelete)
		{
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}
		Item const item = *m_PendingDelete;
		ImGui::Text("Delete '%s'?", item.Path.c_str());
		ImGui::TextDisabled(item.IsFolder() ? "The folder and every file inside are deleted from disk. This cannot be undone."
		                                    : "The file is deleted from disk. This cannot be undone.");
		ImGui::Spacing();
		if (ImGui::Button("Delete"))
		{
			ImGui::CloseCurrentPopup();
			m_PendingDelete.reset();
			Defer(
				[this, &operations, item]
				{
					Result<void> const deleted =
						item.IsFolder() ? operations.DeleteAssetFolder(item.Path) : operations.DeleteAsset(item.Asset);
					UI::ReportFailure(deleted, "Deleting");
					if (deleted && item.IsFolder() && (m_SelectedFolder == item.Path || m_SelectedFolder.starts_with(item.Path + "/")))
					{
						m_SelectedFolder.clear();
					}
				});
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			ImGui::CloseCurrentPopup();
			m_PendingDelete.reset();
		}
		ImGui::EndPopup();
	}

	void ContentBrowserPanel::HandleShortcuts(EditorContext const& context)
	{
		// Registered on the panel (its item area is a child window, part of the same focus route) every frame, even with
		// nothing selected: while the browser is focused these keys never reach the editor's global shortcuts, which act
		// on selected entities. Cmd+Backspace on macOS keyboards.
		bool const renamePressed = ImGui::Shortcut(ImGuiKey_F2, ImGuiInputFlags_RouteFocused);
		bool const deletePressed = ImGui::Shortcut(ImGuiKey_Delete, ImGuiInputFlags_RouteFocused) ||
		                           ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Backspace, ImGuiInputFlags_RouteFocused);
		Item const* selected = FindSelectedItem(context);
		if (selected == nullptr || m_Renaming)
		{
			return;
		}
		if (renamePressed)
		{
			BeginRename(*selected);
		}
		else if (deletePressed)
		{
			m_PendingDelete = *selected;
			m_OpenDeletePopup = true;
		}
	}

	void ContentBrowserPanel::AcceptDrop(EditorOperations& operations, std::string const& folder)
	{
		if (!ImGui::BeginDragDropTarget())
		{
			return;
		}
		// Assets move into the folder unless they are already there.
		AssetHandle const asset = UI::AcceptAssetDrop(
			[&folder](AssetMetadata const& metadata)
			{
				return metadata.IsFileAsset() && AssetBrowsing::GetParentFolder(metadata.Path) != folder;
			});
		if (std::optional<AssetMetadata> const metadata = asset.IsValid() ? AssetManager::GetMetadata(asset) : std::nullopt)
		{
			std::string const newPath = AssetBrowsing::JoinPath(folder, AssetBrowsing::GetFileName(metadata->Path));
			Defer(
				[&operations, asset, newPath]
				{
					UI::ReportFailure(operations.MoveAsset(asset, newPath), "Moving the asset");
				});
		}
		// Folders move unless dropped onto themselves, into their own subfolders or onto the folder they are in.
		if (ImGuiPayload const* payload = ImGui::AcceptDragDropPayload(DragDropPayload::AssetFolder, ImGuiDragDropFlags_AcceptPeekOnly))
		{
			std::string const source(static_cast<char const*>(payload->Data), static_cast<size_t>(payload->DataSize));
			std::string const newFolder = AssetBrowsing::JoinPath(folder, AssetBrowsing::GetFileName(source));
			bool const movable = !source.empty() && folder != source && !folder.starts_with(source + "/") && newFolder != source;
			if (movable && ImGui::AcceptDragDropPayload(DragDropPayload::AssetFolder) != nullptr)
			{
				Defer(
					[this, &operations, source, newFolder]
					{
						Result<void> const moved = operations.MoveAssetFolder(source, newFolder);
						UI::ReportFailure(moved, "Moving the folder");
						if (moved && m_SelectedFolder == source)
						{
							m_SelectedFolder = newFolder;
						}
					});
			}
		}
		ImGui::EndDragDropTarget();
	}

	bool ContentBrowserPanel::IsSelected(EditorContext const& context, Item const& item) const
	{
		AssetHandle const asset = context.GetSelectedAsset();
		if (item.IsFolder())
		{
			return !asset.IsValid() && !m_SelectedFolder.empty() && item.Path == m_SelectedFolder;
		}
		return asset.IsValid() && item.Asset == asset;
	}

	ContentBrowserPanel::Item const* ContentBrowserPanel::FindSelectedItem(EditorContext const& context) const
	{
		auto const it = std::find_if(m_Listing.Items.begin(), m_Listing.Items.end(),
		                             [this, &context](Item const& item)
		                             {
										 return IsSelected(context, item);
									 });
		return it != m_Listing.Items.end() ? &*it : nullptr;
	}

	void ContentBrowserPanel::Select(EditorOperations& operations, Item const& item)
	{
		EditorContext& context = operations.GetContext();
		if (item.IsFolder())
		{
			// What was selected last is shown: a folder replaces selected entities and assets.
			context.ClearSelection();
			m_SelectedFolder = item.Path;
		}
		else
		{
			m_SelectedFolder.clear();
			context.SelectAsset(item.Asset);
		}
	}

	void ContentBrowserPanel::Open(EditorOperations& operations, Item const& item)
	{
		if (item.IsFolder())
		{
			m_Folder = item.Path;
			m_Filter.clear();
			m_SelectedFolder.clear();
			return;
		}
		Select(operations, item);
		if (item.Type == AssetType::Scene && m_OpenScene)
		{
			m_OpenScene(AssetManager::GetAbsolutePath(item.Asset));
		}
	}

	void ContentBrowserPanel::BeginRename(Item item)
	{
		m_RenameBuffer = item.IsFolder() ? item.Name : FileSystem::PathToUtf8(FileSystem::PathFromUtf8(item.Name).stem());
		m_Renaming = std::move(item);
		m_FocusRenameField = true;
		m_RenameFieldActivated = false;
		m_RenameFieldFrame = -1;
	}

	void ContentBrowserPanel::CreateFolder(EditorOperations& operations)
	{
		std::string const name = AssetBrowsing::MakeUniqueName(m_Folder, "New Folder", "");
		std::string const path = AssetBrowsing::JoinPath(m_Folder, name);
		Result<void> const created = operations.CreateAssetFolder(path);
		UI::ReportFailure(created, "Creating the folder");
		if (!created)
		{
			return;
		}
		// Shown right away (outside any search) so it can be named.
		m_Filter.clear();
		m_Listing.Valid = false;
		Item const item = MakeFolderItem(path);
		Select(operations, item);
		BeginRename(item);
	}

	void ContentBrowserPanel::CreateMaterial(EditorOperations& operations)
	{
		std::string const name = AssetBrowsing::MakeUniqueName(m_Folder, "New Material", ".smat");
		Result<AssetHandle> const created = operations.CreateMaterial(AssetBrowsing::JoinPath(m_Folder, name));
		UI::ReportFailure(created, "Creating the material");
		std::optional<AssetMetadata> const metadata = created ? AssetManager::GetMetadata(created.GetValue()) : std::nullopt;
		if (!metadata)
		{
			return;
		}
		m_Filter.clear();
		Item const item = MakeAssetItem(*metadata);
		Select(operations, item);
		BeginRename(item);
	}

	void ContentBrowserPanel::ImportFiles(EditorOperations& operations)
	{
		std::array<FileDialogFilter, 1> const filters = {{{"Assets", GetImportFilterExtensions()}}};
		Result<std::vector<std::filesystem::path>> chosen = FileDialogs::OpenFiles(filters);
		if (!chosen)
		{
			ST_ERROR("{}", chosen.GetError());
			return;
		}
		if (chosen.GetValue().empty())
		{
			return;
		}
		Result<std::vector<AssetHandle>> imported = operations.ImportAssets(chosen.GetValue(), m_Folder);
		UI::ReportFailure(imported, "Importing");
		if (imported && !imported.GetValue().empty())
		{
			if (std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(imported.GetValue().front()))
			{
				m_Filter.clear();
				Select(operations, MakeAssetItem(*metadata));
			}
		}
	}
}
