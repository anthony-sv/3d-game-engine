#include "Editor/AssetAutoRefresh.h"

#include "Editor/EditorOperations.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/Log.h"

namespace Strada
{
	bool AssetAutoRefresh::Update(bool editorFocused, bool canScan, EditorOperations& operations)
	{
		m_ScanPending = m_ScanPending || (editorFocused && !m_Focused);
		m_Focused = editorFocused;
		if (!m_ScanPending || !canScan)
		{
			return false;
		}
		m_ScanPending = false;
		if (!AssetManager::IsInitialized() || !AssetManager::HasAssetDirectory())
		{
			return false;
		}

		Result<AssetRefreshResult> refreshed = operations.RefreshAssets();
		if (!refreshed)
		{
			ST_WARN("Could not look for changed assets: {}", refreshed.GetError());
			return true;
		}
		// Missing files and scan warnings stay the same from one scan to the next (the content browser shows missing files):
		// only what changed since the last scan is worth a line.
		AssetRefreshResult const& changes = refreshed.GetValue();
		if (!changes.Added.empty() || !changes.Modified.empty())
		{
			ST_INFO("Assets changed outside the editor: {} new, {} modified", changes.Added.size(), changes.Modified.size());
		}
		return true;
	}
}
