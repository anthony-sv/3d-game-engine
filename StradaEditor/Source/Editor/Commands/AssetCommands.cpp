#include "Editor/Commands/AssetCommands.h"

#include "Editor/EditorContext.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/Buffer.h"
#include "Strada/Core/FileSystem.h"

#include <filesystem>
#include <optional>
#include <span>
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

		// Only visual script files can be edited: a graph without a file of its own has nowhere to keep the change, and a
		// file the asset directory misses is written again only once it is back (asset.refresh finds it).
		Result<std::filesystem::path> FindGraphFile(AssetHandle handle)
		{
			if (!AssetManager::IsInitialized())
			{
				return Error{"no assets are available"};
			}
			std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(handle);
			if (!metadata || metadata->Type != AssetType::VisualScript)
			{
				return MakeError("asset {} is not a visual script", handle);
			}
			if (!metadata->IsFileAsset())
			{
				return MakeError("visual script '{}' has no file of its own and cannot be edited", metadata->GetDisplayName());
			}
			if (AssetManager::IsMissing(handle))
			{
				return MakeError("asset file '{}' is missing", metadata->Path);
			}
			return AssetManager::GetAbsolutePath(handle);
		}

		// Writes a document to a graph's file. The loaded asset takes it, so running graphs take the edit at their next update;
		// an asset that is not loaded (its file did not load, or a refresh unloaded it) loads it from the file at its next use.
		Result<void> WriteDocument(AssetHandle graph, std::filesystem::path const& path, VisualScriptDocument const& document)
		{
			if (Result<void> saved = VisualScriptSerializer::SaveToFile(document, path); !saved)
			{
				return saved;
			}
			Ref<VisualScriptAsset> const asset = AssetManager::IsLoaded(graph) ? AssetManager::GetAsset<VisualScriptAsset>(graph) : nullptr;
			if (asset != nullptr)
			{
				asset->SetDocument(document);
			}
			else
			{
				// A load that failed is forgotten with the file it read.
				AssetManager::UnloadAsset(graph);
			}
			return {};
		}

		// Writes back the bytes of a file that did not load: the asset fails to load them again at its next use, as it did.
		Result<void> WriteBytes(AssetHandle graph, std::filesystem::path const& path, std::span<uint8_t const> bytes)
		{
			if (Result<void> written = FileSystem::WriteBinaryFile(path, bytes); !written)
			{
				return written;
			}
			AssetManager::UnloadAsset(graph);
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

	SetVisualScriptCommand::SetVisualScriptCommand(AssetHandle graph, VisualScriptDocument document, uint64_t mergeKey)
		: m_Graph(graph),
		  m_After(std::move(document)),
		  m_MergeKey(mergeKey)
	{
	}

	Result<void> SetVisualScriptCommand::Execute(EditorContext&)
	{
		Result<std::filesystem::path> file = FindGraphFile(m_Graph);
		if (!file)
		{
			return Error{file.GetError()};
		}
		if (!m_Recorded)
		{
			Result<Ref<VisualScriptAsset>> asset = AssetManager::TryGetAsset<VisualScriptAsset>(m_Graph);
			if (asset)
			{
				m_Before = asset.GetValue()->GetDocument();
			}
			else
			{
				// A file that does not load is replaced: its bytes are kept to give it back.
				Result<Buffer> bytes = FileSystem::ReadBinaryFile(file.GetValue());
				if (!bytes)
				{
					return Error{asset.GetError()};
				}
				std::span<uint8_t const> const read = bytes.GetValue().GetSpan();
				m_BeforeBytes.assign(read.begin(), read.end());
			}
			m_Recorded = true;
			if (!HasEffect())
			{
				return {};
			}
		}
		return WriteDocument(m_Graph, file.GetValue(), m_After);
	}

	Result<void> SetVisualScriptCommand::Undo(EditorContext&)
	{
		Result<std::filesystem::path> file = FindGraphFile(m_Graph);
		if (!file)
		{
			return Error{file.GetError()};
		}
		if (m_Before)
		{
			return WriteDocument(m_Graph, file.GetValue(), *m_Before);
		}
		return WriteBytes(m_Graph, file.GetValue(), m_BeforeBytes);
	}

	bool SetVisualScriptCommand::CanMergeWith(EditorCommand const& next) const
	{
		auto const* other = dynamic_cast<SetVisualScriptCommand const*>(&next);
		return m_MergeKey != 0 && other != nullptr && other->m_MergeKey == m_MergeKey && other->m_Graph == m_Graph;
	}

	void SetVisualScriptCommand::MergeWith(EditorCommand& next)
	{
		m_After = std::move(static_cast<SetVisualScriptCommand&>(next).m_After);
	}
}
