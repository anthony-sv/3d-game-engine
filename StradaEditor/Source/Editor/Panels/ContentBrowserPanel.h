#pragma once

#include "Editor/AssetThumbnails.h"
#include "Editor/EditorOperations.h"

#include "Strada/Asset/Asset.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Strada
{
	// Browses the open project's asset directory: a folder tree, breadcrumbs, folders and assets as tiles (with rendered
	// thumbnails of textures, materials, meshes, prefabs and environments; a slider sizes them), and a search field. Click
	// selects (the inspector shows the asset), double-click opens folders and scenes; assets drag into the scene view, the
	// hierarchy and asset fields, and assets and folders drag onto folders (tiles, breadcrumbs, tree) to move them. Context menus create
	// folders and materials, import files, rename (F2), delete (with confirmation) and copy asset references. File operations are not part
	// of the undo history. Main thread only.
	class ContentBrowserPanel
	{
	public:
		// Opens a scene file (the editor asks about unsaved changes first).
		using OpenSceneCallback = std::function<void(std::filesystem::path const&)>;

		void SetOpenSceneCallback(OpenSceneCallback callback) { m_OpenScene = std::move(callback); }

		void OnImGuiRender(EditorOperations& operations, bool& open);

		// The displayed folder, relative to the asset directory with forward slashes ("" is the directory itself). A folder
		// that does not exist shows the asset directory instead.
		std::string const& GetFolder() const { return m_Folder; }
		void SetFolder(std::string folder) { m_Folder = std::move(folder); }
		// Tile size as a multiple of the default.
		float GetTileScale() const { return m_TileScale; }
		void SetTileScale(float scale);

		static constexpr float MinTileScale = 0.6f;
		static constexpr float MaxTileScale = 2.5f;

	private:
		// A folder or an asset of the listing.
		struct Item
		{
			std::string Path;
			// The file or folder name.
			std::string Name;
			// Shown under the tile: the name without its extension (the tile shows the type).
			std::string Label;
			// Invalid for folders.
			AssetHandle Asset;
			AssetType Type = AssetType::None;
			bool Missing = false;
			// The label laid out in two lines for a tile of LinesWidth (computed when drawn at another width).
			std::array<std::string, 2> Lines;
			float LinesWidth = 0.0f;

			bool IsFolder() const { return !Asset.IsValid(); }
		};

		// The items of the displayed folder or search. Rebuilt when the folder, the search or the registered assets change,
		// after the panel's own file operations, and periodically for folders changed outside the editor.
		struct Listing
		{
			bool Valid = false;
			std::string Folder;
			std::string Filter;
			uint64_t AssetVersion = 0;
			double ScanTime = 0.0;
			std::vector<Item> Items;
		};

		static Item MakeFolderItem(std::string const& path);
		static Item MakeAssetItem(AssetMetadata const& asset);

		void UpdateListing();
		void DrawToolbar(EditorOperations& operations);
		void DrawFolderTree(EditorOperations& operations);
		void DrawFolderTreeNode(EditorOperations& operations, std::string const& folder);
		// The folders inside a folder, listed once per rescan interval.
		std::vector<std::string> const& GetSubfolders(std::string const& folder);
		void DrawItems(EditorOperations& operations);
		void DrawItem(EditorOperations& operations, Item& item, float tileSize);
		void DrawItemContextMenu(EditorOperations& operations, Item const& item);
		void DrawBackground(EditorOperations& operations);
		void DrawRenameField(EditorOperations& operations, Item const& item, float width);
		void DrawDeletePopup(EditorOperations& operations);
		void HandleShortcuts(EditorContext const& context);
		// Accepts assets and folders dropped onto a folder (moving them into it).
		void AcceptDrop(EditorOperations& operations, std::string const& folder);
		bool IsSelected(EditorContext const& context, Item const& item) const;
		Item const* FindSelectedItem(EditorContext const& context) const;
		void Select(EditorOperations& operations, Item const& item);
		void Open(EditorOperations& operations, Item const& item);
		void BeginRename(Item item);
		void CreateFolder(EditorOperations& operations);
		void CreateMaterial(EditorOperations& operations);
		void ImportFiles(EditorOperations& operations);
		// Runs file operations after the items are drawn: they change the listing.
		void Defer(std::function<void()> action) { m_Deferred.push_back(std::move(action)); }

		OpenSceneCallback m_OpenScene;
		std::filesystem::path m_ProjectFile;
		std::string m_Folder;
		std::string m_Filter;
		Listing m_Listing;
		float m_TileScale = 1.0f;
		AssetThumbnails m_Thumbnails;
		// The folder tree's listings (folder -> its folders) and when they were made.
		std::unordered_map<std::string, std::vector<std::string>> m_Subfolders;
		double m_SubfoldersTime = 0.0;
		uint64_t m_SubfoldersAssetVersion = 0;
		// The tree opens the displayed folder's ancestors when the folder changes.
		std::string m_TreeRevealedFolder;
		bool m_ScrollToTop = false;
		// A selected folder; selected assets are the EditorContext's (shared with the inspector and automation).
		std::string m_SelectedFolder;

		std::optional<Item> m_Renaming;
		std::string m_RenameBuffer;
		bool m_FocusRenameField = false;
		bool m_RenameFieldActivated = false;
		int m_RenameFieldFrame = -1;

		std::optional<Item> m_PendingDelete;
		bool m_OpenDeletePopup = false;

		std::vector<std::function<void()>> m_Deferred;
	};
}
