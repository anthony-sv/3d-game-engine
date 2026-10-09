#include "stpch.h"
#include "Strada/Asset/AssetManager.h"

#include "Strada/Asset/AssetRegistry.h"
#include "Strada/Asset/AudioClipAsset.h"
#include "Strada/Asset/EnvironmentAsset.h"
#include "Strada/Asset/FontAsset.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/MeshFactory.h"
#include "Strada/Asset/MeshImporter.h"
#include "Strada/Asset/MeshSource.h"
#include "Strada/Asset/PrefabAsset.h"
#include "Strada/Asset/TextureAsset.h"
#include "Strada/Core/FileSystem.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace Strada
{
	namespace
	{
		// Sub-asset kinds for DeriveAssetHandle.
		constexpr uint32_t SubAssetTexture = 1;
		constexpr uint32_t SubAssetMaterial = 2;

		struct AssetManagerData
		{
			std::filesystem::path AssetDirectory;
			AssetRegistry Registry;
			// Built-in, memory and sub-assets (never persisted).
			std::unordered_map<AssetHandle, AssetMetadata> RuntimeAssets;
			std::unordered_map<std::string, AssetHandle> BuiltInNames;
			std::unordered_map<AssetHandle, Ref<Asset>> LoadedAssets;
			std::unordered_map<AssetHandle, std::string> FailedLoads;
			std::unordered_set<AssetHandle> MissingFiles;
			// File write times of loaded file assets, to detect modifications on refresh.
			std::unordered_map<AssetHandle, std::filesystem::file_time_type> LoadedWriteTimes;
			// Sub-assets created by each loaded parent.
			std::unordered_map<AssetHandle, std::vector<AssetHandle>> SubAssets;
		};

		Scope<AssetManagerData> s_Data;

		AssetManagerData& GetData()
		{
			ST_CORE_ASSERT(s_Data, "AssetManager is not initialized");
			return *s_Data;
		}

		std::filesystem::path const s_EmptyPath;

		void RegisterRuntimeAsset(AssetMetadata metadata, Ref<Asset> asset)
		{
			AssetManagerData& data = GetData();
			asset->Handle = metadata.Handle;
			data.LoadedAssets[metadata.Handle] = std::move(asset);
			data.RuntimeAssets[metadata.Handle] = std::move(metadata);
		}

		void RemoveSubAssets(AssetHandle parent)
		{
			AssetManagerData& data = GetData();
			auto const it = data.SubAssets.find(parent);
			if (it == data.SubAssets.end())
			{
				return;
			}
			for (AssetHandle const subAsset : it->second)
			{
				data.LoadedAssets.erase(subAsset);
				data.RuntimeAssets.erase(subAsset);
			}
			data.SubAssets.erase(it);
		}

		Image MakeSolidImage(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
		{
			Image image(1, 1, 4);
			uint8_t* pixel = image.GetPixel(0, 0);
			pixel[0] = r;
			pixel[1] = g;
			pixel[2] = b;
			pixel[3] = a;
			return image;
		}

		// Equirectangular sky: a gradient from the horizon to the zenith above, a darker ground below.
		HdrImage MakeSkyImage()
		{
			constexpr uint32_t Width = 256;
			constexpr uint32_t Height = 128;
			glm::vec3 const zenith(0.22f, 0.42f, 0.85f);
			glm::vec3 const horizon(0.75f, 0.82f, 0.92f);
			glm::vec3 const ground(0.18f, 0.17f, 0.15f);
			HdrImage image(Width, Height, 4);
			for (uint32_t y = 0; y < Height; y++)
			{
				// Elevation from +1 (up) to -1 (down).
				float const elevation = std::cos((static_cast<float>(y) + 0.5f) / static_cast<float>(Height) * glm::pi<float>());
				glm::vec3 color = elevation >= 0.0f ? glm::mix(horizon, zenith, std::pow(elevation, 0.6f))
				                                    : glm::mix(horizon * 0.6f, ground, std::min(-elevation * 4.0f, 1.0f));
				for (uint32_t x = 0; x < Width; x++)
				{
					float* pixel = image.GetPixel(x, y);
					pixel[0] = color.r;
					pixel[1] = color.g;
					pixel[2] = color.b;
					pixel[3] = 1.0f;
				}
			}
			return image;
		}

		Ref<Asset> CreateDefaultBuiltInAsset(BuiltInAsset asset)
		{
			AssetHandle const material = GetBuiltInHandle(BuiltInAsset::DefaultMaterial);
			switch (asset)
			{
				case BuiltInAsset::CubeMesh:
					return MeshFactory::CreateCube(material);
				case BuiltInAsset::SphereMesh:
					return MeshFactory::CreateSphere(material);
				case BuiltInAsset::PlaneMesh:
					return MeshFactory::CreatePlane(material);
				case BuiltInAsset::CylinderMesh:
					return MeshFactory::CreateCylinder(material);
				case BuiltInAsset::CapsuleMesh:
					return MeshFactory::CreateCapsule(material);
				case BuiltInAsset::ConeMesh:
					return MeshFactory::CreateCone(material);
				case BuiltInAsset::QuadMesh:
					return MeshFactory::CreateQuad(material);
				case BuiltInAsset::DefaultMaterial:
					return CreateRef<MaterialAsset>();
				case BuiltInAsset::WhiteTexture:
					return TextureAsset::CreateFromImage(MakeSolidImage(255, 255, 255, 255)).GetValue();
				case BuiltInAsset::BlackTexture:
					return TextureAsset::CreateFromImage(MakeSolidImage(0, 0, 0, 255)).GetValue();
				case BuiltInAsset::FlatNormalTexture:
					return TextureAsset::CreateFromImage(MakeSolidImage(128, 128, 255, 255)).GetValue();
				case BuiltInAsset::DefaultSky:
					return EnvironmentAsset::CreateFromImage(MakeSkyImage()).GetValue();
			}
			return nullptr;
		}

		Result<Buffer> ReadAssetFile(std::filesystem::path const& path)
		{
			return FileSystem::ReadBinaryFile(path);
		}

		// Reads a file and creates an asset from its bytes with TAsset's factory.
		template<typename TAsset>
		Result<Ref<Asset>> LoadBinaryAsset(std::filesystem::path const& path, Result<Ref<TAsset>> (*factory)(Buffer))
		{
			Result<Buffer> file = ReadAssetFile(path);
			if (!file)
			{
				return Error{file.GetError()};
			}
			Result<Ref<TAsset>> asset = factory(std::move(file.GetValue()));
			if (!asset)
			{
				return Error{asset.GetError()};
			}
			return Ref<Asset>(asset.GetValue());
		}

		Result<Ref<Asset>> LoadMesh(AssetMetadata const& metadata, std::filesystem::path const& path)
		{
			Result<ImportedModel> imported = MeshImporter::Import(path);
			if (!imported)
			{
				return Error{imported.GetError()};
			}
			ImportedModel& model = imported.GetValue();
			for (std::string const& warning : model.Warnings)
			{
				ST_CORE_WARN("'{}': {}", metadata.Path, warning);
			}

			AssetManagerData& data = GetData();
			std::string const displayName = metadata.GetDisplayName();
			std::vector<AssetHandle> subAssets;

			std::vector<AssetHandle> textures(model.Textures.size());
			for (size_t i = 0; i < model.Textures.size(); i++)
			{
				ImportedTexture& texture = model.Textures[i];
				if (!texture.FilePath.empty())
				{
					// Textures inside the asset directory are shared through their own registered asset.
					if (FileSystem::IsInside(texture.FilePath, data.AssetDirectory))
					{
						Result<std::string> relative =
							NormalizeAssetPath(FileSystem::PathToUtf8(FileSystem::GetRelativePath(texture.FilePath, data.AssetDirectory)));
						if (relative)
						{
							if (AssetHandle const registered = data.Registry.FindByPath(relative.GetValue()); registered.IsValid())
							{
								textures[i] = registered;
								continue;
							}
						}
					}
					Result<Buffer> file = ReadAssetFile(texture.FilePath);
					if (!file)
					{
						ST_CORE_WARN("'{}': texture '{}' could not be read: {}", metadata.Path, texture.Name, file.GetError());
						continue;
					}
					texture.EncodedData = std::move(file.GetValue());
				}

				Result<Ref<TextureAsset>> asset = TextureAsset::CreateFromEncoded(std::move(texture.EncodedData));
				if (!asset)
				{
					ST_CORE_WARN("'{}': texture '{}' is invalid: {}", metadata.Path, texture.Name, asset.GetError());
					continue;
				}

				AssetMetadata subAsset;
				subAsset.Handle = DeriveAssetHandle(metadata.Handle, SubAssetTexture, static_cast<uint32_t>(i));
				subAsset.Type = AssetType::Texture;
				subAsset.Name = fmt::format("{}/Textures/{}", displayName, texture.Name);
				subAsset.Parent = metadata.Handle;
				textures[i] = subAsset.Handle;
				subAssets.push_back(subAsset.Handle);
				RegisterRuntimeAsset(std::move(subAsset), asset.GetValue());
			}

			auto const textureHandle = [&textures](int32_t index)
			{
				return index >= 0 && static_cast<size_t>(index) < textures.size() ? textures[static_cast<size_t>(index)] : AssetHandle();
			};

			std::vector<AssetHandle> materials;
			for (size_t i = 0; i < model.Materials.size(); i++)
			{
				ImportedMaterial const& material = model.Materials[i];
				MaterialData materialData = material.Data;
				materialData.BaseColorTexture = textureHandle(material.BaseColorTexture);
				materialData.NormalTexture = textureHandle(material.NormalTexture);
				materialData.MetallicRoughnessTexture = textureHandle(material.MetallicRoughnessTexture);
				materialData.OcclusionTexture = textureHandle(material.OcclusionTexture);
				materialData.EmissiveTexture = textureHandle(material.EmissiveTexture);

				AssetMetadata subAsset;
				subAsset.Handle = DeriveAssetHandle(metadata.Handle, SubAssetMaterial, static_cast<uint32_t>(i));
				subAsset.Type = AssetType::Material;
				subAsset.Name = fmt::format("{}/Materials/{}", displayName, material.Name);
				subAsset.Parent = metadata.Handle;
				materials.push_back(subAsset.Handle);
				subAssets.push_back(subAsset.Handle);
				RegisterRuntimeAsset(std::move(subAsset), CreateRef<MaterialAsset>(std::move(materialData)));
			}

			Result<Ref<MeshSource>> mesh =
				MeshSource::Create(std::move(model.Vertices), std::move(model.Indices), std::move(model.Submeshes), std::move(materials));
			if (!mesh)
			{
				for (AssetHandle const subAsset : subAssets)
				{
					data.LoadedAssets.erase(subAsset);
					data.RuntimeAssets.erase(subAsset);
				}
				return Error{mesh.GetError()};
			}
			data.SubAssets[metadata.Handle] = std::move(subAssets);
			return Ref<Asset>(mesh.GetValue());
		}

		Result<Ref<Asset>> LoadFileAsset(AssetMetadata const& metadata, std::filesystem::path const& path)
		{
			switch (metadata.Type)
			{
				case AssetType::Texture:
					return LoadBinaryAsset<TextureAsset>(path, &TextureAsset::CreateFromEncoded);
				case AssetType::Environment:
					return LoadBinaryAsset<EnvironmentAsset>(path, &EnvironmentAsset::CreateFromEncoded);
				case AssetType::Font:
					return LoadBinaryAsset<FontAsset>(path, &FontAsset::Create);
				case AssetType::AudioClip:
					return LoadBinaryAsset<AudioClipAsset>(path, &AudioClipAsset::Create);
				case AssetType::Material:
				{
					// Unknown fields only warn so files written by newer engine versions still load.
					std::vector<std::string> warnings;
					Result<MaterialData> material = MaterialSerializer::LoadFromFile(
						path, AssetManager::CreateDeserializationContext(UnknownFieldPolicy::Warn, &warnings));
					for (std::string const& warning : warnings)
					{
						ST_CORE_WARN("'{}': {}", metadata.Path, warning);
					}
					if (!material)
					{
						return Error{material.GetError()};
					}
					return Ref<Asset>(CreateRef<MaterialAsset>(std::move(material.GetValue())));
				}
				case AssetType::Prefab:
				{
					Result<std::string> text = FileSystem::ReadTextFile(path);
					if (!text)
					{
						return Error{text.GetError()};
					}
					Result<Json> json = ParseJson(text.GetValue());
					if (!json)
					{
						return Error{json.GetError()};
					}
					Result<Ref<PrefabAsset>> prefab = PrefabAsset::Create(std::move(json.GetValue()));
					if (!prefab)
					{
						return Error{prefab.GetError()};
					}
					return Ref<Asset>(prefab.GetValue());
				}
				case AssetType::Mesh:
					return LoadMesh(metadata, path);
				case AssetType::Scene:
					return Error{"scenes are opened with the scene serializer, not loaded as assets"};
				case AssetType::None:
					break;
			}
			return Error{"the asset has no type"};
		}
	}

	void AssetManager::Init()
	{
		ST_CORE_ASSERT(!s_Data, "AssetManager is already initialized");
		s_Data = CreateScope<AssetManagerData>();

		for (BuiltInAsset const asset : GetDefaultBuiltInAssets())
		{
			Result<void> result =
				RegisterBuiltInAsset(GetBuiltInAssetName(asset), GetBuiltInHandle(asset), CreateDefaultBuiltInAsset(asset));
			ST_CORE_ASSERT(result.IsOk(), "Failed to register a default built-in asset");
		}
	}

	void AssetManager::Shutdown()
	{
		ST_CORE_ASSERT(s_Data, "AssetManager is not initialized");
		s_Data.reset();
	}

	bool AssetManager::IsInitialized()
	{
		return s_Data != nullptr;
	}

	Result<AssetRefreshResult> AssetManager::OpenAssetDirectory(std::filesystem::path const& directory)
	{
		AssetManagerData& data = GetData();
		if (!FileSystem::IsDirectory(directory))
		{
			return MakeError("asset directory '{}' does not exist", FileSystem::PathToUtf8(directory));
		}

		std::error_code errorCode;
		std::filesystem::path const absolute = std::filesystem::absolute(directory, errorCode).lexically_normal();
		if (errorCode)
		{
			return MakeError("invalid asset directory '{}': {}", FileSystem::PathToUtf8(directory), errorCode.message());
		}

		AssetRegistry registry;
		std::vector<std::string> warnings;
		std::filesystem::path const registryPath = absolute / AssetRegistry::FileName;
		if (FileSystem::IsRegularFile(registryPath))
		{
			DeserializationContext context;
			context.Warnings = &warnings;
			Result<AssetRegistry> loaded = AssetRegistry::LoadFromFile(registryPath, context);
			if (!loaded)
			{
				return MakeError("the asset registry is unreadable (fix or delete it to re-register every file): {}", loaded.GetError());
			}
			registry = std::move(loaded.GetValue());
		}

		CloseAssetDirectory();
		data.AssetDirectory = absolute.has_filename() ? absolute : absolute.parent_path();
		data.Registry = std::move(registry);

		Result<AssetRefreshResult> result = Refresh();
		if (!result)
		{
			CloseAssetDirectory();
			return result;
		}
		result.GetValue().Warnings.insert(result.GetValue().Warnings.begin(), warnings.begin(), warnings.end());
		for (std::string const& warning : result.GetValue().Warnings)
		{
			ST_CORE_WARN("Assets: {}", warning);
		}
		ST_CORE_INFO("Opened asset directory '{}' ({} assets, {} new, {} missing)", FileSystem::PathToUtf8(data.AssetDirectory),
		             data.Registry.GetCount(), result.GetValue().Added.size(), result.GetValue().Missing.size());
		return result;
	}

	void AssetManager::CloseAssetDirectory()
	{
		AssetManagerData& data = GetData();
		// Keep built-ins; drop file assets, their sub-assets and memory assets.
		for (auto it = data.RuntimeAssets.begin(); it != data.RuntimeAssets.end();)
		{
			if (it->second.IsBuiltIn())
			{
				++it;
				continue;
			}
			data.LoadedAssets.erase(it->first);
			it = data.RuntimeAssets.erase(it);
		}
		for (AssetMetadata const& metadata : data.Registry.GetAll())
		{
			data.LoadedAssets.erase(metadata.Handle);
		}
		data.Registry.Clear();
		data.SubAssets.clear();
		data.FailedLoads.clear();
		data.MissingFiles.clear();
		data.LoadedWriteTimes.clear();
		data.AssetDirectory.clear();
	}

	bool AssetManager::HasAssetDirectory()
	{
		return !GetData().AssetDirectory.empty();
	}

	std::filesystem::path const& AssetManager::GetAssetDirectory()
	{
		return s_Data ? s_Data->AssetDirectory : s_EmptyPath;
	}

	Result<AssetRefreshResult> AssetManager::Refresh()
	{
		AssetManagerData& data = GetData();
		if (data.AssetDirectory.empty())
		{
			return Error{"no asset directory is open"};
		}

		AssetRefreshResult result;
		std::unordered_set<std::string> found;
		bool changed = false;

		std::error_code errorCode;
		std::filesystem::recursive_directory_iterator it(data.AssetDirectory, std::filesystem::directory_options::skip_permission_denied,
		                                                 errorCode);
		if (errorCode)
		{
			return MakeError("cannot scan '{}': {}", FileSystem::PathToUtf8(data.AssetDirectory), errorCode.message());
		}

		for (std::filesystem::recursive_directory_iterator const end; it != end; it.increment(errorCode))
		{
			if (errorCode)
			{
				result.Warnings.push_back(fmt::format("scan stopped early: {}", errorCode.message()));
				break;
			}

			std::filesystem::directory_entry const& entry = *it;
			std::string const fileName = FileSystem::PathToUtf8(entry.path().filename());
			// Hidden files and folders (.git, editor state) are not assets.
			if (fileName.starts_with('.'))
			{
				if (entry.is_directory(errorCode))
				{
					it.disable_recursion_pending();
				}
				continue;
			}
			if (!entry.is_regular_file(errorCode))
			{
				continue;
			}

			AssetType const type = GetAssetTypeForExtension(FileSystem::PathToUtf8(entry.path().extension()));
			if (type == AssetType::None)
			{
				continue;
			}

			Result<std::string> path = NormalizeAssetPath(FileSystem::PathToUtf8(entry.path().lexically_relative(data.AssetDirectory)));
			if (!path)
			{
				result.Warnings.push_back(
					fmt::format("'{}' was not registered: {}", FileSystem::PathToUtf8(entry.path()), path.GetError()));
				continue;
			}
			found.insert(path.GetValue());

			if (AssetHandle const existing = data.Registry.FindByPath(path.GetValue()); existing.IsValid())
			{
				if (data.Registry.Find(existing)->Type != type)
				{
					result.Warnings.push_back(fmt::format("'{}' changed type to {}", path.GetValue(), AssetTypeToString(type)));
					(void)data.Registry.SetType(existing, type);
					UnloadAsset(existing);
					changed = true;
				}
				continue;
			}

			AssetMetadata metadata;
			metadata.Handle = AssetHandle::Generate();
			metadata.Type = type;
			metadata.Path = path.GetValue();
			if (Result<void> added = data.Registry.Add(metadata); !added)
			{
				result.Warnings.push_back(added.GetError());
				continue;
			}
			result.Added.push_back(metadata.Handle);
			changed = true;
		}

		data.MissingFiles.clear();
		for (AssetMetadata const& metadata : data.Registry.GetAll())
		{
			if (!found.contains(metadata.Path))
			{
				data.MissingFiles.insert(metadata.Handle);
				result.Missing.push_back(metadata.Handle);
				UnloadAsset(metadata.Handle);
				continue;
			}

			auto const writeTime = data.LoadedWriteTimes.find(metadata.Handle);
			if (writeTime != data.LoadedWriteTimes.end())
			{
				std::optional<std::filesystem::file_time_type> const current =
					FileSystem::GetLastWriteTime(GetAbsolutePath(metadata.Handle));
				if (!current || *current != writeTime->second)
				{
					UnloadAsset(metadata.Handle);
					result.Modified.push_back(metadata.Handle);
				}
			}
		}

		// Files may have been fixed: failed loads are retried.
		data.FailedLoads.clear();

		if (changed)
		{
			if (Result<void> saved = SaveRegistry(); !saved)
			{
				result.Warnings.push_back(saved.GetError());
			}
		}
		return result;
	}

	Result<void> AssetManager::SaveRegistry()
	{
		AssetManagerData& data = GetData();
		if (data.AssetDirectory.empty())
		{
			return Error{"no asset directory is open"};
		}
		return data.Registry.SaveToFile(data.AssetDirectory / AssetRegistry::FileName);
	}

	Result<AssetHandle> AssetManager::ImportFile(std::filesystem::path const& path)
	{
		AssetManagerData& data = GetData();
		if (data.AssetDirectory.empty())
		{
			return Error{"no asset directory is open"};
		}

		std::filesystem::path const absolute = (path.is_absolute() ? path : data.AssetDirectory / path).lexically_normal();
		if (!FileSystem::IsInside(absolute, data.AssetDirectory))
		{
			return MakeError("'{}' is outside the asset directory", FileSystem::PathToUtf8(path));
		}
		if (!FileSystem::IsRegularFile(absolute))
		{
			return MakeError("'{}' does not exist", FileSystem::PathToUtf8(path));
		}
		AssetType const type = GetAssetTypeForExtension(FileSystem::PathToUtf8(absolute.extension()));
		if (type == AssetType::None)
		{
			return MakeError("'{}' is not a supported asset type", FileSystem::PathToUtf8(path));
		}

		Result<std::string> relative =
			NormalizeAssetPath(FileSystem::PathToUtf8(FileSystem::GetRelativePath(absolute, data.AssetDirectory)));
		if (!relative)
		{
			return Error{relative.GetError()};
		}
		if (AssetHandle const existing = data.Registry.FindByPath(relative.GetValue()); existing.IsValid())
		{
			data.MissingFiles.erase(existing);
			return existing;
		}

		AssetMetadata metadata;
		metadata.Handle = AssetHandle::Generate();
		metadata.Type = type;
		metadata.Path = relative.GetValue();
		if (Result<void> added = data.Registry.Add(metadata); !added)
		{
			return Error{added.GetError()};
		}
		if (Result<void> saved = SaveRegistry(); !saved)
		{
			data.Registry.Remove(metadata.Handle);
			return Error{saved.GetError()};
		}
		return metadata.Handle;
	}

	Result<void> AssetManager::MoveAsset(AssetHandle handle, std::string_view newPath)
	{
		AssetManagerData& data = GetData();
		AssetMetadata const* metadata = data.Registry.Find(handle);
		if (metadata == nullptr)
		{
			return MakeError("asset {} is not a file asset", handle);
		}
		Result<std::string> normalized = NormalizeAssetPath(newPath);
		if (!normalized)
		{
			return Error{normalized.GetError()};
		}
		std::string const oldPath = metadata->Path;
		std::string const& path = normalized.GetValue();
		if (path == oldPath)
		{
			return {};
		}
		if (data.Registry.FindByPath(path).IsValid())
		{
			return MakeError("'{}' is already registered", path);
		}
		AssetType const newType = GetAssetTypeForExtension(FileSystem::PathToUtf8(FileSystem::PathFromUtf8(path).extension()));
		if (newType != metadata->Type)
		{
			return MakeError("moving '{}' to '{}' would change its type", oldPath, path);
		}

		std::filesystem::path const from = data.AssetDirectory / FileSystem::PathFromUtf8(oldPath);
		std::filesystem::path const to = data.AssetDirectory / FileSystem::PathFromUtf8(path);
		if (FileSystem::Exists(to))
		{
			return MakeError("'{}' already exists", path);
		}
		if (Result<void> created = FileSystem::CreateDirectories(to.parent_path()); !created)
		{
			return created;
		}
		if (Result<void> moved = FileSystem::Move(from, to); !moved)
		{
			return moved;
		}
		if (Result<void> updated = data.Registry.SetPath(handle, path); !updated)
		{
			// Keep disk and registry consistent.
			(void)FileSystem::Move(to, from);
			return updated;
		}
		data.MissingFiles.erase(handle);
		if (Result<void> saved = SaveRegistry(); !saved)
		{
			return MakeError("moved '{}' but the registry could not be saved: {}", oldPath, saved.GetError());
		}
		return {};
	}

	Result<void> AssetManager::DeleteAsset(AssetHandle handle)
	{
		AssetManagerData& data = GetData();
		AssetMetadata const* metadata = data.Registry.Find(handle);
		if (metadata == nullptr)
		{
			return MakeError("asset {} is not a file asset", handle);
		}
		std::filesystem::path const path = data.AssetDirectory / FileSystem::PathFromUtf8(metadata->Path);
		if (FileSystem::Exists(path))
		{
			if (Result<void> removed = FileSystem::Remove(path); !removed)
			{
				return removed;
			}
		}
		UnloadAsset(handle);
		data.Registry.Remove(handle);
		data.MissingFiles.erase(handle);
		return SaveRegistry();
	}

	Result<void> AssetManager::MoveFolder(std::string_view folder, std::string_view newFolder)
	{
		AssetManagerData& data = GetData();
		if (data.AssetDirectory.empty())
		{
			return Error{"no asset directory is open"};
		}
		Result<std::string> from = NormalizeAssetPath(folder);
		if (!from)
		{
			return Error{from.GetError()};
		}
		Result<std::string> to = NormalizeAssetPath(newFolder);
		if (!to)
		{
			return Error{to.GetError()};
		}
		std::string const& source = from.GetValue();
		std::string const& destination = to.GetValue();
		if (source == destination)
		{
			return {};
		}
		if (destination.starts_with(source + "/"))
		{
			return MakeError("'{}' cannot be moved into itself", source);
		}
		std::filesystem::path const sourcePath = data.AssetDirectory / FileSystem::PathFromUtf8(source);
		std::filesystem::path const destinationPath = data.AssetDirectory / FileSystem::PathFromUtf8(destination);
		if (!FileSystem::IsDirectory(sourcePath))
		{
			return MakeError("'{}' is not a folder", source);
		}
		if (FileSystem::Exists(destinationPath))
		{
			return MakeError("'{}' already exists", destination);
		}

		// Every registered path under the folder moves with it; none may collide with a registered (missing) file.
		std::vector<std::pair<AssetHandle, std::string>> moved;
		for (AssetMetadata const& asset : data.Registry.GetAll())
		{
			if (asset.Path.starts_with(source + "/"))
			{
				std::string path = destination + asset.Path.substr(source.size());
				if (data.Registry.FindByPath(path).IsValid())
				{
					return MakeError("'{}' is already registered", path);
				}
				moved.emplace_back(asset.Handle, std::move(path));
			}
		}

		if (Result<void> created = FileSystem::CreateDirectories(destinationPath.parent_path()); !created)
		{
			return created;
		}
		if (Result<void> renamed = FileSystem::Move(sourcePath, destinationPath); !renamed)
		{
			return renamed;
		}
		std::vector<std::pair<AssetHandle, std::string>> previous;
		for (auto const& [handle, path] : moved)
		{
			std::string oldPath = data.Registry.Find(handle)->Path;
			if (Result<void> updated = data.Registry.SetPath(handle, path); !updated)
			{
				// Keep disk and registry consistent.
				for (auto const& [restored, restoredPath] : previous)
				{
					(void)data.Registry.SetPath(restored, restoredPath);
				}
				(void)FileSystem::Move(destinationPath, sourcePath);
				return updated;
			}
			previous.emplace_back(handle, std::move(oldPath));
		}
		if (Result<void> saved = SaveRegistry(); !saved)
		{
			return MakeError("moved '{}' but the registry could not be saved: {}", source, saved.GetError());
		}
		return {};
	}

	Result<void> AssetManager::DeleteFolder(std::string_view folder)
	{
		AssetManagerData& data = GetData();
		if (data.AssetDirectory.empty())
		{
			return Error{"no asset directory is open"};
		}
		Result<std::string> normalized = NormalizeAssetPath(folder);
		if (!normalized)
		{
			return Error{normalized.GetError()};
		}
		std::string const& path = normalized.GetValue();
		std::filesystem::path const directory = data.AssetDirectory / FileSystem::PathFromUtf8(path);
		if (!FileSystem::IsDirectory(directory))
		{
			return MakeError("'{}' is not a folder", path);
		}
		if (Result<void> removed = FileSystem::RemoveAll(directory); !removed)
		{
			// Files that were removed before the failure are reported as missing by the next refresh.
			return removed;
		}
		for (AssetMetadata const& asset : data.Registry.GetAll())
		{
			if (asset.Path.starts_with(path + "/"))
			{
				UnloadAsset(asset.Handle);
				data.Registry.Remove(asset.Handle);
				data.MissingFiles.erase(asset.Handle);
			}
		}
		return SaveRegistry();
	}

	AssetHandle AssetManager::AddMemoryAsset(Ref<Asset> asset, std::string name)
	{
		ST_CORE_ASSERT(asset, "Memory assets cannot be null");
		AssetMetadata metadata;
		metadata.Handle = AssetHandle::Generate();
		metadata.Type = asset->GetAssetType();
		metadata.Name = std::move(name);
		AssetHandle const handle = metadata.Handle;
		RegisterRuntimeAsset(std::move(metadata), std::move(asset));
		return handle;
	}

	void AssetManager::RemoveMemoryAsset(AssetHandle handle)
	{
		AssetManagerData& data = GetData();
		auto const it = data.RuntimeAssets.find(handle);
		if (it != data.RuntimeAssets.end() && it->second.IsMemoryAsset())
		{
			data.RuntimeAssets.erase(it);
			data.LoadedAssets.erase(handle);
		}
	}

	Result<void> AssetManager::RegisterBuiltInAsset(std::string_view name, AssetHandle handle, Ref<Asset> asset)
	{
		AssetManagerData& data = GetData();
		if (!asset)
		{
			return Error{"built-in assets cannot be null"};
		}
		if (!handle.IsReserved())
		{
			return MakeError("built-in asset '{}' needs a reserved handle (below {})", name, AssetHandle::ReservedCount);
		}
		if (name.empty() || data.RuntimeAssets.contains(handle) || data.BuiltInNames.contains(std::string(name)))
		{
			return MakeError("built-in asset '{}' ({}) is already registered", name, handle);
		}

		AssetMetadata metadata;
		metadata.Handle = handle;
		metadata.Type = asset->GetAssetType();
		metadata.Path = fmt::format("builtin://{}", name);
		data.BuiltInNames.emplace(std::string(name), handle);
		RegisterRuntimeAsset(std::move(metadata), std::move(asset));
		return {};
	}

	bool AssetManager::IsValid(AssetHandle handle)
	{
		AssetManagerData const& data = GetData();
		return handle.IsValid() && (data.Registry.Contains(handle) || data.RuntimeAssets.contains(handle));
	}

	bool AssetManager::IsLoaded(AssetHandle handle)
	{
		return GetData().LoadedAssets.contains(handle);
	}

	bool AssetManager::IsMissing(AssetHandle handle)
	{
		return GetData().MissingFiles.contains(handle);
	}

	std::optional<AssetMetadata> AssetManager::GetMetadata(AssetHandle handle)
	{
		AssetManagerData const& data = GetData();
		if (AssetMetadata const* metadata = data.Registry.Find(handle))
		{
			return *metadata;
		}
		if (auto const it = data.RuntimeAssets.find(handle); it != data.RuntimeAssets.end())
		{
			return it->second;
		}
		return std::nullopt;
	}

	AssetType AssetManager::GetAssetType(AssetHandle handle)
	{
		std::optional<AssetMetadata> const metadata = GetMetadata(handle);
		return metadata ? metadata->Type : AssetType::None;
	}

	AssetHandle AssetManager::FindByPath(std::string_view path)
	{
		Result<std::string> normalized = NormalizeAssetPath(path);
		return normalized ? GetData().Registry.FindByPath(normalized.GetValue()) : AssetHandle();
	}

	AssetHandle AssetManager::FindBuiltIn(std::string_view name)
	{
		AssetManagerData const& data = GetData();
		auto const it = data.BuiltInNames.find(std::string(name));
		return it != data.BuiltInNames.end() ? it->second : AssetHandle();
	}

	std::vector<AssetMetadata> AssetManager::GetAssets(AssetType type)
	{
		AssetManagerData const& data = GetData();
		auto const matches = [type](AssetMetadata const& metadata)
		{
			return type == AssetType::None || metadata.Type == type;
		};

		std::vector<AssetMetadata> builtIn;
		std::vector<AssetMetadata> runtime;
		for (auto const& [handle, metadata] : data.RuntimeAssets)
		{
			if (matches(metadata))
			{
				(metadata.IsBuiltIn() ? builtIn : runtime).push_back(metadata);
			}
		}
		std::sort(builtIn.begin(), builtIn.end(),
		          [](AssetMetadata const& a, AssetMetadata const& b)
		          {
					  return a.Handle.GetUUID().GetValue() < b.Handle.GetUUID().GetValue();
				  });
		std::sort(runtime.begin(), runtime.end(),
		          [](AssetMetadata const& a, AssetMetadata const& b)
		          {
					  return a.Name != b.Name ? a.Name < b.Name : a.Handle.GetUUID().GetValue() < b.Handle.GetUUID().GetValue();
				  });

		std::vector<AssetMetadata> assets = std::move(builtIn);
		for (AssetMetadata& metadata : data.Registry.GetAll())
		{
			if (matches(metadata))
			{
				assets.push_back(std::move(metadata));
			}
		}
		assets.insert(assets.end(), std::make_move_iterator(runtime.begin()), std::make_move_iterator(runtime.end()));
		return assets;
	}

	Result<AssetHandle> AssetManager::ResolveReference(std::string_view reference)
	{
		AssetManagerData const& data = GetData();
		if (reference.starts_with("asset://"))
		{
			std::string_view const path = reference.substr(std::string_view("asset://").size());
			Result<std::string> normalized = NormalizeAssetPath(path);
			if (!normalized)
			{
				return Error{normalized.GetError()};
			}
			AssetHandle const handle = data.Registry.FindByPath(normalized.GetValue());
			if (!handle.IsValid())
			{
				return MakeError("no asset is registered at '{}'", normalized.GetValue());
			}
			return handle;
		}
		if (reference.starts_with("builtin://"))
		{
			std::string_view const name = reference.substr(std::string_view("builtin://").size());
			AssetHandle const handle = FindBuiltIn(name);
			if (!handle.IsValid())
			{
				return MakeError("unknown built-in asset '{}'", name);
			}
			return handle;
		}
		if (std::optional<UUID> const id = UUID::FromString(reference); id && IsValid(AssetHandle(*id)))
		{
			return AssetHandle(*id);
		}
		return MakeError("'{}' is not a registered asset handle or reference", reference);
	}

	std::string AssetManager::GetReference(AssetHandle handle)
	{
		std::optional<AssetMetadata> const metadata = GetMetadata(handle);
		if (metadata && metadata->IsFileAsset())
		{
			return "asset://" + metadata->Path;
		}
		if (metadata && metadata->IsBuiltIn())
		{
			return metadata->Path;
		}
		return handle.ToString();
	}

	std::filesystem::path AssetManager::GetAbsolutePath(AssetHandle handle)
	{
		AssetManagerData const& data = GetData();
		AssetMetadata const* metadata = data.Registry.Find(handle);
		return metadata != nullptr ? data.AssetDirectory / FileSystem::PathFromUtf8(metadata->Path) : std::filesystem::path();
	}

	DeserializationContext AssetManager::CreateDeserializationContext(UnknownFieldPolicy unknownFields, std::vector<std::string>* warnings)
	{
		DeserializationContext context;
		context.UnknownFields = unknownFields;
		context.Warnings = warnings;
		context.ResolveAssetReference = [](std::string_view reference)
		{
			return ResolveReference(reference);
		};
		return context;
	}

	Result<Ref<Asset>> AssetManager::LoadAsset(AssetHandle handle)
	{
		AssetManagerData& data = GetData();
		if (!handle.IsValid())
		{
			return Error{"no asset"};
		}
		if (auto const loaded = data.LoadedAssets.find(handle); loaded != data.LoadedAssets.end())
		{
			return loaded->second;
		}
		if (auto const failed = data.FailedLoads.find(handle); failed != data.FailedLoads.end())
		{
			return Error{failed->second};
		}

		AssetMetadata const* registered = data.Registry.Find(handle);
		if (registered == nullptr)
		{
			return MakeError("asset {} is not registered", handle);
		}
		AssetMetadata const metadata = *registered;
		if (data.MissingFiles.contains(handle))
		{
			return MakeError("asset file '{}' is missing", metadata.Path);
		}

		std::filesystem::path const path = data.AssetDirectory / FileSystem::PathFromUtf8(metadata.Path);
		std::optional<std::filesystem::file_time_type> const writeTime = FileSystem::GetLastWriteTime(path);
		Result<Ref<Asset>> asset = LoadFileAsset(metadata, path);
		if (!asset)
		{
			std::string error = fmt::format("failed to load '{}': {}", metadata.Path, asset.GetError());
			ST_CORE_ERROR("{}", error);
			data.FailedLoads.emplace(handle, error);
			return Error{std::move(error)};
		}

		asset.GetValue()->Handle = handle;
		data.LoadedAssets[handle] = asset.GetValue();
		if (writeTime)
		{
			data.LoadedWriteTimes[handle] = *writeTime;
		}
		return asset;
	}

	Result<void> AssetManager::ReloadAsset(AssetHandle handle)
	{
		AssetManagerData& data = GetData();
		if (auto const runtime = data.RuntimeAssets.find(handle); runtime != data.RuntimeAssets.end())
		{
			if (!runtime->second.IsSubAsset())
			{
				return MakeError("asset {} does not come from a file and cannot be reloaded", handle);
			}
			handle = runtime->second.Parent;
		}
		if (!data.Registry.Contains(handle))
		{
			return MakeError("asset {} is not registered", handle);
		}

		UnloadAsset(handle);
		Result<Ref<Asset>> asset = LoadAsset(handle);
		if (!asset)
		{
			return Error{asset.GetError()};
		}
		return {};
	}

	void AssetManager::UnloadAsset(AssetHandle handle)
	{
		AssetManagerData& data = GetData();
		if (!data.Registry.Contains(handle))
		{
			return;
		}
		RemoveSubAssets(handle);
		data.LoadedAssets.erase(handle);
		data.LoadedWriteTimes.erase(handle);
		data.FailedLoads.erase(handle);
	}
}
