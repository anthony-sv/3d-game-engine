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
		// The largest model an import builds, with every node instance baked in. Larger models fail before their geometry
		// is built, so the memory an import takes stays bounded, also for files that declare more data than they hold.
		static constexpr uint64_t MaxVertices = 1ull << 24;
		static constexpr uint64_t MaxTriangles = 1ull << 24;

		// .gltf, .glb (glTF 2.0), .fbx, .obj (case-insensitive).
		static bool IsSupportedExtension(std::string_view extension);

		// Imports the default scene of a model file. Output is Y-up, in meters, with counter-clockwise front faces, UV origin
		// at the top-left, and MikkTSpace tangents. Fails for unreadable or invalid files, files without triangles and
		// models above MaxVertices or MaxTriangles. Files the model references (glTF buffers and images, FBX and OBJ textures,
		// OBJ material libraries) are only read inside referenceDirectory, the model's own directory when empty: buffers
		// outside it fail the import, textures and material libraries outside it are skipped with a warning.
		[[nodiscard]] static Result<ImportedModel> Import(std::filesystem::path const& path,
		                                                  std::filesystem::path const& referenceDirectory = {});
		// The files besides itself that Import reads for a model (given as an absolute path): glTF buffers and images that are
		// not embedded, OBJ material libraries and the textures they name, FBX textures. Existing files inside
		// referenceDirectory (the model's own directory when empty), as absolute, lexically normal paths with the names below
		// referenceDirectory spelled as stored (FileSystem::FindInside), each once (also when the model names one in several
		// spellings that a file system ignoring letter case takes), in the order the model names them; files the model names
		// that are missing or lie outside are left out. Fails when the model cannot be read or parsed.
		[[nodiscard]] static Result<std::vector<std::filesystem::path>>
		FindExternalFiles(std::filesystem::path const& path, std::filesystem::path const& referenceDirectory = {});
	};
}
