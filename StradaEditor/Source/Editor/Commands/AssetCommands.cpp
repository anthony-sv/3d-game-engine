#include "Editor/Commands/AssetCommands.h"

#include "Editor/EditorContext.h"
#include "Editor/VisualScripting/VisualScriptCommit.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/Buffer.h"
#include "Strada/Core/FileSystem.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
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

		// Writes the material's file and gives the loaded asset the data: what the editor writes is not reloaded by the next
		// refresh.
		Result<void> SaveMaterial(AssetHandle handle, MaterialFile const& file, MaterialData data)
		{
			if (Result<void> saved = MaterialSerializer::SaveToFile(data, file.Path); !saved)
			{
				return saved;
			}
			file.Material->SetData(std::move(data));
			AssetManager::RecordFileWrite(handle);
			return {};
		}

		// Applies every field (a complete state recorded earlier) to the material and its file.
		Result<void> Write(AssetHandle handle, MaterialFile const& file, Json const& state, DeserializationContext const& context)
		{
			MaterialData data;
			if (Result<void> read = DeserializeFields<StructTraits<MaterialData>>(state, data, context, "material"); !read)
			{
				return read;
			}
			return SaveMaterial(handle, file, std::move(data));
		}

		// The file of a visual script that edits write.
		struct GraphFile
		{
			std::filesystem::path Path;
			// The path relative to the asset directory, for messages.
			std::string Name;
		};

		// A file without any write permission, which is how the standard library reports the read-only attribute on Windows.
		bool IsReadOnly(std::filesystem::path const& path)
		{
			std::error_code error;
			std::filesystem::file_status const status = std::filesystem::status(path, error);
			if (error || !std::filesystem::exists(status))
			{
				return false;
			}
			constexpr std::filesystem::perms Writable =
				std::filesystem::perms::owner_write | std::filesystem::perms::group_write | std::filesystem::perms::others_write;
			return (status.permissions() & Writable) == std::filesystem::perms::none;
		}

		// Only visual script files can be edited: a graph without a file of its own has nowhere to keep the change, and a
		// file the asset directory misses is written again only once it is back (asset.refresh finds it). A read-only file
		// is refused: Windows refuses to replace it while a rename on other systems replaces it anyway, and edits must
		// behave alike everywhere.
		Result<GraphFile> FindGraphFile(AssetHandle handle)
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
			std::filesystem::path path = AssetManager::GetAbsolutePath(handle);
			if (IsReadOnly(path))
			{
				return MakeError("the file of visual script '{}' is read-only", metadata->Path);
			}
			return GraphFile{std::move(path), metadata->Path};
		}

		// Null when the asset is not loaded (its file did not load, or a refresh unloaded it).
		Ref<VisualScriptAsset> FindLoadedGraph(AssetHandle graph)
		{
			return AssetManager::IsLoaded(graph) ? AssetManager::GetAsset<VisualScriptAsset>(graph) : nullptr;
		}

		bool HoldsText(std::span<uint8_t const> bytes, std::string_view text)
		{
			return std::equal(bytes.begin(), bytes.end(), text.begin(), text.end(),
			                  [](uint8_t byte, char character)
			                  {
								  return byte == static_cast<uint8_t>(character);
							  });
		}

		// Whether the bytes of a visual script file load as the document, read as the asset loader reads them: a UTF-8 byte
		// order mark skipped (as FileSystem::ReadTextFile does), damage repaired and unknown keys skipped.
		bool LoadsAs(std::span<uint8_t const> bytes, VisualScriptDocument const& document)
		{
			std::string_view text(reinterpret_cast<char const*>(bytes.data()), bytes.size());
			if (text.starts_with("\xEF\xBB\xBF"))
			{
				text.remove_prefix(3);
			}
			Result<Json> const json = ParseJson(text);
			if (!json)
			{
				return false;
			}
			DeserializationContext context;
			context.UnknownFields = UnknownFieldPolicy::Warn;
			Result<VisualScriptDocument> const loaded = VisualScriptSerializer::Deserialize(json.GetValue(), context);
			return loaded && IsSameVisualScriptDocument(loaded.GetValue(), document);
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
			return Write(m_Material, file.GetValue(), m_After, deserialization);
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
			if (Result<void> saved = SaveMaterial(m_Material, file.GetValue(), std::move(updated)); !saved)
			{
				return saved;
			}
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
		return Write(m_Material, file.GetValue(), m_Before, context.CreateDeserializationContext());
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

	SetVisualScriptCommand::SetVisualScriptCommand(AssetHandle graph, Ref<VisualScriptDocument const> document, std::string description,
	                                               uint64_t mergeKey)
		: m_Graph(graph),
		  m_After(std::move(document)),
		  m_Description(std::move(description)),
		  m_MergeKey(mergeKey)
	{
		ST_CORE_ASSERT(m_After != nullptr, "A visual script edit needs a document");
	}

	Result<void> SetVisualScriptCommand::Execute(EditorContext& context)
	{
		if (m_After == nullptr)
		{
			return Error{"there is no visual script document to write"};
		}
		Result<GraphFile> found = FindGraphFile(m_Graph);
		if (!found)
		{
			return Error{found.GetError()};
		}
		GraphFile const& file = found.GetValue();
		bool const first = !m_Recorded;
		if (first)
		{
			if (Result<void> recorded = RecordBefore(context, file.Path, file.Name); !recorded)
			{
				return recorded;
			}
			if (!HasEffect())
			{
				// The file has this document already: nothing is written, and the history drops the step.
				return {};
			}
		}

		std::string text = DumpJson(VisualScriptSerializer::Serialize(*m_After));
		if (Result<void> written = FileSystem::WriteTextFile(file.Path, text); !written)
		{
			// A first execution that failed reads the file again when it runs again.
			m_Recorded = !first;
			return written;
		}
		m_Wrote = true;
		m_AfterTextSize = text.size();
		if (Ref<VisualScriptAsset> const asset = FindLoadedGraph(m_Graph); asset != nullptr)
		{
			asset->SetDocument(m_After);
			context.RememberVisualScriptText(m_Graph, m_After, std::move(text));
		}
		else
		{
			// A load that failed is forgotten with the file it read: the asset loads what was written at its next use.
			AssetManager::UnloadAsset(m_Graph);
			context.ForgetVisualScriptText(m_Graph);
		}
		AssetManager::RecordFileWrite(m_Graph);
		return {};
	}

	Result<void> SetVisualScriptCommand::Undo(EditorContext& context)
	{
		Result<GraphFile> found = FindGraphFile(m_Graph);
		if (!found)
		{
			return Error{found.GetError()};
		}
		std::filesystem::path const& path = found.GetValue().Path;
		// The kept bytes, or the canonical text of the document the file had.
		std::string text = KeepsBytes() ? std::string() : DumpJson(VisualScriptSerializer::Serialize(*m_Before));
		Result<void> const written =
			KeepsBytes() ? FileSystem::WriteBinaryFile(path, m_BeforeBytes) : FileSystem::WriteTextFile(path, text);
		if (!written)
		{
			return written;
		}
		Ref<VisualScriptAsset> const asset = m_Before != nullptr ? FindLoadedGraph(m_Graph) : nullptr;
		if (asset != nullptr)
		{
			asset->SetDocument(m_Before);
		}
		else
		{
			// A file that did not load fails to load again at the asset's next use, as it did.
			AssetManager::UnloadAsset(m_Graph);
		}
		if (asset != nullptr && !KeepsBytes())
		{
			context.RememberVisualScriptText(m_Graph, m_Before, std::move(text));
		}
		else
		{
			context.ForgetVisualScriptText(m_Graph);
		}
		if (m_BytesLoadAsBefore)
		{
			AssetManager::RecordFileWrite(m_Graph);
		}
		return {};
	}

	bool SetVisualScriptCommand::HasEffect() const
	{
		bool const same = m_Before != nullptr && m_After != nullptr && IsSameVisualScriptDocument(*m_Before, *m_After);
		return !same || (m_Wrote && KeepsBytes());
	}

	bool SetVisualScriptCommand::CanMergeWith(EditorCommand const& next) const
	{
		auto const* other = dynamic_cast<SetVisualScriptCommand const*>(&next);
		return m_MergeKey != 0 && other != nullptr && other->m_MergeKey == m_MergeKey && other->m_Graph == m_Graph;
	}

	void SetVisualScriptCommand::MergeWith(EditorCommand& next)
	{
		auto& other = static_cast<SetVisualScriptCommand&>(next);
		m_After = std::move(other.m_After);
		m_AfterTextSize = other.m_AfterTextSize;
	}

	Result<void> SetVisualScriptCommand::RecordBefore(EditorContext const& context, std::filesystem::path const& path,
	                                                  std::string const& name)
	{
		Result<Buffer> const bytes = FileSystem::ReadBinaryFile(path);
		if (!bytes)
		{
			// Gone since the asset directory was last scanned.
			if (!FileSystem::Exists(path))
			{
				return MakeError("asset file '{}' is missing", name);
			}
			return Error{bytes.GetError()};
		}
		std::span<uint8_t const> const read = bytes.GetValue().GetSpan();
		Result<Ref<VisualScriptAsset>> const asset = AssetManager::TryGetAsset<VisualScriptAsset>(m_Graph);
		m_Before = asset ? asset.GetValue()->GetSharedDocument() : nullptr;
		m_BeforeBytes.clear();
		m_BytesLoadAsBefore = true;
		bool canonical = false;
		if (m_Before != nullptr)
		{
			// The text the editor wrote last for this very document, else the document serialized again.
			std::string const* const written = context.FindVisualScriptText(m_Graph, m_Before.get());
			canonical =
				written != nullptr ? HoldsText(read, *written) : HoldsText(read, DumpJson(VisualScriptSerializer::Serialize(*m_Before)));
		}
		if (!canonical)
		{
			m_BeforeBytes.assign(read.begin(), read.end());
			m_BytesLoadAsBefore = m_Before == nullptr || LoadsAs(read, *m_Before);
		}
		m_Recorded = true;
		return {};
	}

	SetVisualScriptAndComponentsCommand::SetVisualScriptAndComponentsCommand(AssetHandle graph, Ref<VisualScriptDocument const> document,
	                                                                         std::string description, std::vector<ComponentEdit> edits)
		: m_Graph(CreateScope<SetVisualScriptCommand>(graph, std::move(document), description)),
		  m_Components(std::move(edits), 0, description),
		  m_Description(std::move(description))
	{
	}

	Result<void> SetVisualScriptAndComponentsCommand::Execute(EditorContext& context)
	{
		// The first execution finds out whether the graph changes; a redo writes the graph only when it does.
		bool const executeGraph = m_Graph != nullptr && (!m_Recorded || m_Graph->HasEffect());
		if (executeGraph)
		{
			if (Result<void> result = m_Graph->Execute(context); !result)
			{
				return result;
			}
		}
		if (Result<void> result = m_Components.Execute(context); !result)
		{
			if (executeGraph && m_Graph->HasEffect())
			{
				if (Result<void> undone = m_Graph->Undo(context); !undone)
				{
					return MakeError("{} (the visual script keeps the edit: {})", result.GetError(), undone.GetError());
				}
			}
			return result;
		}
		m_Recorded = true;
		return {};
	}

	Result<void> SetVisualScriptAndComponentsCommand::Undo(EditorContext& context)
	{
		if (Result<void> result = m_Components.Undo(context); !result)
		{
			return result;
		}
		if (ChangesGraph())
		{
			if (Result<void> result = m_Graph->Undo(context); !result)
			{
				// A failed undo leaves the step as it was: the components take the edit again.
				if (Result<void> redone = m_Components.Execute(context); !redone)
				{
					return MakeError("{} (the components keep the undo: {})", result.GetError(), redone.GetError());
				}
				return result;
			}
		}
		return {};
	}

	AssetHandle SetVisualScriptAndComponentsCommand::GetEditedAsset() const
	{
		return m_Graph != nullptr ? m_Graph->GetEditedAsset() : AssetHandle();
	}

	size_t SetVisualScriptAndComponentsCommand::GetMemoryUse() const
	{
		return m_Graph != nullptr ? m_Graph->GetMemoryUse() : 0;
	}

	void SetVisualScriptAndComponentsCommand::ForgetEditedAsset()
	{
		m_Graph.reset();
	}
}
