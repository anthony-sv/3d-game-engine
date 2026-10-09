#include "Editor/Commands/AssetCommands.h"

#include "Editor/EditorContext.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/MaterialAsset.h"

#include <filesystem>
#include <optional>
#include <utility>

namespace Strada
{
	namespace
	{
		struct MaterialFile
		{
			Ref<MaterialAsset> Material;
			std::filesystem::path Path;
		};

		// Only material files can be edited: built-in and mesh-embedded materials have no file to keep the change.
		Result<MaterialFile> FindMaterialFile(AssetHandle handle)
		{
			if (!AssetManager::IsInitialized())
			{
				return Error{"no assets are available"};
			}
			std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(handle);
			if (!metadata || metadata->Type != AssetType::Material)
			{
				return MakeError("asset {} is not a material", handle);
			}
			if (!metadata->IsFileAsset())
			{
				return MakeError("material '{}' has no file of its own and cannot be edited", metadata->GetDisplayName());
			}
			Result<Ref<MaterialAsset>> material = AssetManager::TryGetAsset<MaterialAsset>(handle);
			if (!material)
			{
				return Error{material.GetError()};
			}
			return MaterialFile{material.TakeValue(), AssetManager::GetAbsolutePath(handle)};
		}

		// Applies every field (a complete state recorded earlier) to the material and its file.
		Result<void> Write(MaterialFile const& file, Json const& state, DeserializationContext const& context)
		{
			MaterialData data;
			if (Result<void> read = DeserializeFields<StructTraits<MaterialData>>(state, data, context, "material"); !read)
			{
				return read;
			}
			if (Result<void> saved = MaterialSerializer::SaveToFile(data, file.Path); !saved)
			{
				return saved;
			}
			file.Material->SetData(std::move(data));
			return {};
		}
	}

	SetMaterialCommand::SetMaterialCommand(AssetHandle material, Json patch, uint64_t mergeKey)
		: m_Material(material),
		  m_Patch(std::move(patch)),
		  m_MergeKey(mergeKey)
	{
	}

	Result<void> SetMaterialCommand::Execute(EditorContext& context)
	{
		Result<MaterialFile> file = FindMaterialFile(m_Material);
		if (!file)
		{
			return Error{file.GetError()};
		}
		DeserializationContext const deserialization = context.CreateDeserializationContext();
		if (!m_After.is_null())
		{
			return Write(file.GetValue(), m_After, deserialization);
		}

		MaterialData updated = file.GetValue().Material->GetData();
		Json before = SerializeFields<StructTraits<MaterialData>>(updated);
		if (Result<void> result = DeserializeFields<StructTraits<MaterialData>>(m_Patch, updated, deserialization, "material"); !result)
		{
			return result;
		}
		Json after = SerializeFields<StructTraits<MaterialData>>(updated);
		if (after != before)
		{
			if (Result<void> saved = MaterialSerializer::SaveToFile(updated, file.GetValue().Path); !saved)
			{
				return saved;
			}
			file.GetValue().Material->SetData(std::move(updated));
		}
		m_Before = std::move(before);
		m_After = std::move(after);
		return {};
	}

	Result<void> SetMaterialCommand::Undo(EditorContext& context)
	{
		Result<MaterialFile> file = FindMaterialFile(m_Material);
		if (!file)
		{
			return Error{file.GetError()};
		}
		return Write(file.GetValue(), m_Before, context.CreateDeserializationContext());
	}

	bool SetMaterialCommand::CanMergeWith(EditorCommand const& next) const
	{
		auto const* other = dynamic_cast<SetMaterialCommand const*>(&next);
		return m_MergeKey != 0 && other != nullptr && other->m_MergeKey == m_MergeKey && other->m_Material == m_Material;
	}

	void SetMaterialCommand::MergeWith(EditorCommand& next)
	{
		m_After = std::move(static_cast<SetMaterialCommand&>(next).m_After);
	}
}
