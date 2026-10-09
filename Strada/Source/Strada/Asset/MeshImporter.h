#pragma once

#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/MeshSource.h"
#include "Strada/Core/Buffer.h"
#include "Strada/Core/Result.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	// An image referenced by an imported model: embedded data (GLB, data URIs, FBX content) or an external file.
	struct ImportedTexture
	{
		std::string Name;
		// Encoded image file data; empty for external files.
		Buffer EncodedData;
		// Absolute, lexically normal path of an external image file; empty for embedded images.
		std::filesystem::path FilePath;
	};

	struct ImportedMaterial
	{
		std::string Name;
		// Parameters; the texture handles are unset (see the texture indices below).
		MaterialData Data;
		// Indices into ImportedModel::Textures, -1 for none.
		int32_t BaseColorTexture = -1;
		int32_t NormalTexture = -1;
		int32_t MetallicRoughnessTexture = -1;
		int32_t OcclusionTexture = -1;
		int32_t EmissiveTexture = -1;
	};

	// A model flattened into one mesh: node transforms are baked into the vertices, every primitive/material part becomes a
	// submesh, and every material becomes a material slot (at least one).
	struct ImportedModel
	{
		std::vector<Vertex> Vertices;
		std::vector<uint32_t> Indices;
		std::vector<Submesh> Submeshes;
		std::vector<ImportedMaterial> Materials;
		std::vector<ImportedTexture> Textures;
		// Non-fatal problems (unsupported features that were skipped).
		std::vector<std::string> Warnings;
	};

	class MeshImporter
	{
	public:
		// .gltf, .glb (glTF 2.0), .fbx, .obj (case-insensitive).
		static bool IsSupportedExtension(std::string_view extension);

		// Imports the default scene of a model file. Output is Y-up, in meters, with counter-clockwise front faces, UV origin
		// at the top-left, and MikkTSpace tangents. Fails for unreadable or invalid files and files without triangles.
		[[nodiscard]] static Result<ImportedModel> Import(std::filesystem::path const& path);
	};
}
