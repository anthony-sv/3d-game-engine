#include "Editor/Commands/SetDataAssetCommand.h"

#include "Editor/EditorContext.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Core/FileSystem.h"

#include <filesystem>
#include <optional>
#include <utility>

namespace Strada
{
	namespace
	{
		// The loaded asset of a data asset file the step edits, and its file.
		struct DataAssetFile
		{
			Ref<DataAsset> Asset;
			std::filesystem::path Path;
		};

		Result<DataAssetFile> FindDataAssetFile(AssetHandle handle)
		{
			if (Result<void> editable = SetDataAssetCommand::CheckEditable(handle); !editable)
			{
				return Error{editable.GetError()};
			}
			Result<Ref<Asset>> loaded = AssetManager::LoadAsset(handle);
			if (!loaded)
			{
				return Error{loaded.GetError()};
			}
			Ref<DataAsset> asset = std::dynamic_pointer_cast<DataAsset>(loaded.GetValue());
			if (asset == nullptr)
			{
				return MakeError("asset {} is not a data asset", handle);
			}
			return DataAssetFile{std::move(asset), AssetManager::GetAbsolutePath(handle)};
		}

		// A new asset of the file's type with the fields: first, then the patch over them (when given). The loaded asset does
		// not change, so a refused value or a failed write leaves it as it was.
		Result<Ref<DataAsset>> MakeState(DataAssetFile const& file, Json const& fields, Json const* patch,
		                                 DeserializationContext const& context)
		{
			Ref<DataAsset> state = file.Asset->GetDataAssetType().CreateAsset();
			if (Result<void> applied = state->ApplyFields(fields, context); !applied)
			{
				return Error{applied.GetError()};
			}
			if (patch != nullptr)
			{
				if (Result<void> applied = state->ApplyFields(*patch, context); !applied)
				{
					return Error{applied.GetError()};
				}
			}
			return state;
		}

		// Writes the file from the state, then gives the loaded asset the state's fields (its version moves when they differ)
		// and records the write, so the next refresh keeps the asset object.
		Result<void> Write(AssetHandle handle, DataAssetFile const& file, DataAsset const& state, DeserializationContext const& context)
		{
			if (Result<void> saved = DataAssetSerializer::SaveToFile(state, file.Path); !saved)
			{
				return saved;
			}
			if (Result<void> applied = file.Asset->ApplyFields(state.SerializeFields(), context); !applied)
			{
				// The state's own fields: the asset takes them as the state did, so this only fails on a broken type.
				AssetManager::MarkFileChanged(handle);
				return applied;
			}
			AssetManager::RecordFileWrite(handle);
			return {};
		}

		// Applies every field (a complete state recorded earlier) to the asset and its file.
		Result<void> WriteFields(AssetHandle handle, Json const& fields, DeserializationContext const& context)
		{
			Result<DataAssetFile> file = FindDataAssetFile(handle);
			if (!file)
			{
				return Error{file.GetError()};
			}
			Result<Ref<DataAsset>> state = MakeState(file.GetValue(), fields, nullptr, context);
			if (!state)
			{
				return Error{state.GetError()};
			}
			return Write(handle, file.GetValue(), *state.GetValue(), context);
		}
	}

	SetDataAssetCommand::SetDataAssetCommand(AssetHandle asset, Json patch, std::string description, uint64_t mergeKey)
		: m_Asset(asset),
		  m_Patch(std::move(patch)),
		  m_MergeKey(mergeKey),
		  m_Description(std::move(description))
	{
	}

	Result<void> SetDataAssetCommand::CheckEditable(AssetHandle asset)
	{
		if (!AssetManager::IsInitialized())
		{
			return Error{"no assets are available"};
		}
		std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(asset);
		if (!metadata)
		{
			return MakeError("no asset {} is registered", asset);
		}
		DataAssetType const* const type = DataAssetRegistry::Find(metadata->Type);
		if (type == nullptr)
		{
			return MakeError("asset '{}' is a {}, not a data asset", metadata->GetDisplayName(), AssetTypeToString(metadata->Type));
		}
		// A data asset made in memory has nowhere to keep the change.
		if (!metadata->IsFileAsset())
		{
			return MakeError("{} '{}' has no file of its own and cannot be edited", type->GetName(), metadata->GetDisplayName());
		}
		if (AssetManager::IsMissing(asset))
		{
			return MakeError("asset file '{}' is missing", metadata->Path);
		}
		// Windows refuses to replace a read-only file while a rename on other systems replaces it anyway: edits must behave
		// alike everywhere.
		if (FileSystem::IsReadOnly(AssetManager::GetAbsolutePath(asset)))
		{
			return MakeError("the file of {} '{}' is read-only", type->GetName(), metadata->Path);
		}
		return {};
	}

	Result<void> SetDataAssetCommand::Execute(EditorContext& context)
	{
		DeserializationContext const deserialization = context.CreateDeserializationContext();
		if (!m_After.is_null())
		{
			return WriteFields(m_Asset, m_After, deserialization);
		}

		Result<DataAssetFile> file = FindDataAssetFile(m_Asset);
		if (!file)
		{
			return Error{file.GetError()};
		}
		Json before = file.GetValue().Asset->SerializeFields();
		Result<Ref<DataAsset>> state = MakeState(file.GetValue(), before, &m_Patch, deserialization);
		if (!state)
		{
			return Error{state.GetError()};
		}
		Json after = state.GetValue()->SerializeFields();
		if (after != before)
		{
			if (Result<void> written = Write(m_Asset, file.GetValue(), *state.GetValue(), deserialization); !written)
			{
				return written;
			}
		}
		if (m_Description.empty())
		{
			m_Description = "Edit " + file.GetValue().Asset->GetDataAssetType().GetName();
		}
		m_Before = std::move(before);
		m_After = std::move(after);
		return {};
	}

	Result<void> SetDataAssetCommand::Undo(EditorContext& context)
	{
		return WriteFields(m_Asset, m_Before, context.CreateDeserializationContext());
	}

	std::string SetDataAssetCommand::GetDescription() const
	{
		return m_Description.empty() ? std::string("Edit Data Asset") : m_Description;
	}

	bool SetDataAssetCommand::HasEffect() const
	{
		return m_Before != m_After;
	}

	bool SetDataAssetCommand::ModifiesScene() const
	{
		return false;
	}

	bool SetDataAssetCommand::CanMergeWith(EditorCommand const& next) const
	{
		auto const* other = dynamic_cast<SetDataAssetCommand const*>(&next);
		return m_MergeKey != 0 && other != nullptr && other->m_MergeKey == m_MergeKey && other->m_Asset == m_Asset;
	}

	void SetDataAssetCommand::MergeWith(EditorCommand& next)
	{
		auto& other = static_cast<SetDataAssetCommand&>(next);
		m_After = std::move(other.m_After);
		m_Description = std::move(other.m_Description);
	}

	AssetHandle SetDataAssetCommand::GetEditedAsset() const
	{
		return m_Asset;
	}
}
