#pragma once

#include "Editor/AssetThumbnails.h"
#include "Editor/EditorOperations.h"
#include "Editor/ProjectImport.h"

#include "Strada/Asset/Asset.h"
#include "Strada/Core/Result.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct ImVec2;

namespace Strada
{
	class EditorScripts;

	// The two trees of files the content browser shows: the project's asset directory and the C# sources of its scripts
	// (ScriptProject::GetSourceDirectory).
	enum class ContentRoot : uint8_t
	{
		Assets = 0,
		Scripts
	};

	// Browses the open project's files: the asset directory and, under a second root, the C# sources of its scripts. A folder
	// tree, breadcrumbs, folders and files as tiles labeled with their file names (textures, materials, meshes, prefabs and
	// environments show rendered thumbnails, fonts "Aa" in their own glyphs, audio clips their waveforms, other files icons of
	// their type; a slider sizes them), and a search field. Click selects (the inspector shows assets), double-click opens
	// folders, scenes and scripts (in the desktop's editor for C#); assets drag into the scene view, the hierarchy and asset
	// fields, scripts onto entities (hierarchy rows, the inspector), and assets, folders and scripts onto folders to move them.
	// Files and folders dropped onto the editor from the OS are imported into the folder under the cursor, else the displayed
	// one (ProjectImport). Context menus create folders, materials and scripts, import files, rename (F2), delete (with
	// confirmation) and copy references or paths. File operations are not part of the undo history. Main thread only.
	class ContentBrowserPanel
	{
	public:
		// Opens a scene file (the editor asks about unsaved changes first).
		using OpenSceneCallback = std::function<void(std::filesystem::path const&)>;
		// Opens a file with the desktop's application for it.
		using OpenFileCallback = std::function<Result<void>(std::filesystem::path const&)>;

		// Files open with Desktop::Open until SetOpenFileCallback says otherwise.
		ContentBrowserPanel();

		void SetOpenSceneCallback(OpenSceneCallback callback) { m_OpenScene = std::move(callback); }
		void SetOpenFileCallback(OpenFileCallback callback) { m_OpenFile = std::move(callback); }
		// Asks for a new script class in a folder of the scripts (the editor's New Script dialog): the displayed one while the
		// browser shows the scripts, else their directory itself. Without it the browser offers no "C# Script".
		using NewScriptCallback = std::function<void(std::string const& folder)>;
		void SetNewScriptCallback(NewScriptCallback callback) { m_NewScript = std::move(callback); }
		// The project's scripts, which receive the C# sources imported into the browser. Not owned; without them C# sources
		// are refused.
		void SetScripts(EditorScripts* scripts) { m_Scripts = scripts; }

		void OnImGuiRender(EditorOperations& operations, bool& open);

		ContentRoot GetRoot() const { return m_Root; }
		// The displayed folder of the asset directory, relative to it with forward slashes ("" is the directory itself). A
		// folder that does not exist shows the asset directory instead. Setting it shows the Assets root.
		std::string const& GetFolder() const { return m_Folder; }
		void SetFolder(std::string folder);
		// The displayed folder of the scripts, relative to their directory. Setting it shows the Scripts root.
		std::string const& GetScriptFolder() const { return m_ScriptFolder; }
		void SetScriptFolder(std::string folder);
		// Tile size as a multiple of the default.
		float GetTileScale() const { return m_TileScale; }
		void SetTileScale(float scale);
		// Brings the browser to the front (its tab, when docked) the next time it is drawn.
		void RequestFocus() { m_FocusRequested = true; }
		// Files and folders dropped onto the editor from the OS. The browser comes to the front and imports them the next
		// time it is drawn: into the folder under the cursor (a folder tile, a tree node or a breadcrumb), else the displayed
		// folders, assets into the asset folder and C# sources into the scripts folder.
		void DropFiles(std::vector<std::filesystem::path> paths);
		// Shows the folder of a C# source of the open project's scripts (absolute path) with the source selected in place of
		// the context's selection.
		void RevealScript(EditorContext& context, std::filesystem::path const& file);

		static constexpr float MinTileScale = 0.6f;
		static constexpr float MaxTileScale = 2.5f;

	private:
		enum class ItemKind : uint8_t
		{
			Folder = 0,
			Asset,
			Script,
			// A file of the asset directory that is not an asset (a glTF scene's buffers, notes).
			File
		};

		// A folder (or a script file) of one of the roots.
		struct Location
		{
			ContentRoot Root = ContentRoot::Assets;
			std::string Path;

			bool operator==(Location const& other) const = default;
		};

		// A folder, an asset or a script of the listing.
		struct Item
		{
			ItemKind Kind = ItemKind::Folder;
			ContentRoot Root = ContentRoot::Assets;
			// Relative to the root's directory with forward slashes.
			std::string Path;
			// The file or folder name, shown under the tile.
			std::string Name;
			// Invalid for folders and scripts.
			AssetHandle Asset;
			AssetType Type = AssetType::None;
			bool Missing = false;
			// The name laid out in two lines for a tile of LinesWidth (computed when drawn at another width).
			std::array<std::string, 2> Lines;
			float LinesWidth = 0.0f;

			bool IsFolder() const { return Kind == ItemKind::Folder; }
			Location GetLocation() const { return {Root, Path}; }
		};

		// The items of the displayed folder or search. Rebuilt when the root, the folder, the search or the registered assets
		// change, after the panel's own file operations, and periodically for folders changed outside the editor.
		struct Listing
		{
			bool Valid = false;
			ContentRoot Root = ContentRoot::Assets;
			std::string Folder;
			std::string Filter;
			uint64_t AssetVersion = 0;
			double ScanTime = 0.0;
			std::vector<Item> Items;
		};

		static Item MakeFolderItem(ContentRoot root, std::string const& path);
		static Item MakeAssetItem(AssetMetadata const& asset);
		static Item MakeScriptItem(std::string const& path);
		static Item MakeFileItem(std::string const& path);

		// The displayed folder of the displayed root.
		std::string const& GetDisplayedFolder() const { return m_Root == ContentRoot::Assets ? m_Folder : m_ScriptFolder; }
		void Navigate(ContentRoot root, std::string folder);
		// The directory of a root: the asset directory or the scripts' sources; empty without a project.
		std::filesystem::path GetRootDirectory(ContentRoot root) const;
		std::filesystem::path GetAbsolutePath(Item const& item) const;

		void UpdateListing();
		// The panel's own file operations changed folders: listings and the tree are made again.
		void InvalidateListings();
		void DrawToolbar(EditorOperations& operations);
		// "Folder", "Material" and "C# Script..." items, prefixed with "New " in context menus.
		void DrawCreateMenuItems(EditorOperations& operations, bool prefixNew);
		void DrawFolderTree(EditorOperations& operations);
		void DrawFolderTreeNode(EditorOperations& operations, ContentRoot root, std::string const& folder);
		// The folders inside a folder, listed once per rescan interval.
		std::vector<std::string> const& GetSubfolders(ContentRoot root, std::string const& folder);
		void DrawItems(EditorOperations& operations);
		void DrawItem(EditorOperations& operations, Item& item, float tileSize);
		void DrawTile(Item const& item, ImVec2 tileMin, float tileSize);
		void DrawItemTooltip(Item const& item);
		void DrawItemContextMenu(EditorOperations& operations, Item const& item);
		void DrawBackground(EditorOperations& operations);
		void DrawRenameField(EditorOperations& operations, Item const& item, float width);
		void DrawDeletePopup(EditorOperations& operations);
		void HandleShortcuts(EditorContext const& context);
		// Accepts what may be dropped onto a folder: assets, folders and entities (as prefabs) in the asset directory, C#
		// sources and folders in the scripts. Files dropped from the OS go to the folder under the cursor.
		void AcceptDrop(EditorOperations& operations, Location const& folder);
		bool IsSelected(EditorContext const& context, Item const& item) const;
		Item const* FindSelectedItem(EditorContext const& context) const;
		void Select(EditorOperations& operations, Item const& item);
		void Open(EditorOperations& operations, Item const& item);
		void BeginRename(Item item);
		void Rename(EditorOperations& operations, Item const& item, std::string const& newName);
		void Delete(EditorOperations& operations, Item const& item);
		void CreateFolder(EditorOperations& operations);
		void CreateMaterial(EditorOperations& operations);
		void ImportFiles(EditorOperations& operations);
		void ImportDroppedFiles(EditorOperations& operations);
		// Imports into the given folders and shows what came in: selected, in the root where it went when it all went to
		// the other one.
		void Import(EditorOperations& operations, std::vector<std::filesystem::path> const& paths,
		            ProjectImport::Destination const& destination);
		// Runs file operations after the items are drawn: they change the listing.
		void Defer(std::function<void()> action) { m_Deferred.push_back(std::move(action)); }

		OpenSceneCallback m_OpenScene;
		OpenFileCallback m_OpenFile;
		NewScriptCallback m_NewScript;
		EditorScripts* m_Scripts = nullptr;
		std::filesystem::path m_ProjectFile;
		// The open project's ScriptProject::GetSourceDirectory.
		std::filesystem::path m_ScriptsDirectory;
		ContentRoot m_Root = ContentRoot::Assets;
		std::string m_Folder;
		std::string m_ScriptFolder;
		std::string m_Filter;
		Listing m_Listing;
		float m_TileScale = 1.0f;
		AssetThumbnails m_Thumbnails;
		// The folder tree's listings (root and folder -> its folders) and when they were made.
		std::unordered_map<std::string, std::vector<std::string>> m_Subfolders;
		double m_SubfoldersTime = 0.0;
		uint64_t m_SubfoldersAssetVersion = 0;
		// The tree opens the displayed folder's ancestors when the folder changes.
		Location m_TreeRevealedFolder;
		bool m_ScrollToTop = false;
		bool m_FocusRequested = false;
		// A selected folder or script; selected assets are the EditorContext's (shared with the inspector and automation).
		std::optional<Location> m_Selected;

		std::optional<Item> m_Renaming;
		std::string m_RenameBuffer;
		bool m_FocusRenameField = false;
		bool m_RenameFieldActivated = false;
		int m_RenameFieldFrame = -1;

		std::optional<Item> m_PendingDelete;
		bool m_OpenDeletePopup = false;

		// Files dropped from the OS, imported in the next frame, and the folder under the cursor while they were.
		std::vector<std::filesystem::path> m_DroppedFiles;
		std::optional<Location> m_FileDropTarget;

		std::vector<std::function<void()>> m_Deferred;
	};
}
