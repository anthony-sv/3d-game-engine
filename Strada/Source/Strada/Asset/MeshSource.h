#pragma once

#include "Strada/Asset/Asset.h"
#include "Strada/Core/Result.h"
#include "Strada/Math/AABB.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace Strada
{
	// Vertex layout shared by every mesh (48 bytes): position, normal, tangent (xyz + bitangent sign in w), UV0 with (0, 0)
	// at the top-left of the texture.
	struct Vertex
	{
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec3 Normal = glm::vec3(0.0f, 1.0f, 0.0f);
		glm::vec4 Tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
		glm::vec2 TexCoord = glm::vec2(0.0f);

		bool operator==(Vertex const& other) const = default;
	};
	static_assert(sizeof(Vertex) == 48, "Vertex must be tightly packed (it is uploaded to the GPU as is)");

	// A range of a mesh drawn with one material. Indices are relative to BaseVertex.
	struct Submesh
	{
		uint32_t BaseVertex = 0;
		uint32_t VertexCount = 0;
		uint32_t BaseIndex = 0;
		uint32_t IndexCount = 0;
		// Index into MeshSource::GetMaterials() (the material slot).
		uint32_t MaterialIndex = 0;
		AABB Bounds;
		std::string Name;
	};

	// Triangle-list geometry with submeshes and one default material per material slot. Immutable after creation.
	class MeshSource final : public Asset
	{
		struct PrivateTag
		{
		};

	public:
		// Validates the data (index and submesh ranges, triangle lists, material slots) and computes bounds.
		[[nodiscard]] static Result<Ref<MeshSource>> Create(std::vector<Vertex> vertices, std::vector<uint32_t> indices,
		                                                    std::vector<Submesh> submeshes, std::vector<AssetHandle> materials);

		MeshSource(PrivateTag, std::vector<Vertex> vertices, std::vector<uint32_t> indices, std::vector<Submesh> submeshes,
		           std::vector<AssetHandle> materials);

		static AssetType GetStaticType() { return AssetType::Mesh; }
		AssetType GetAssetType() const override { return GetStaticType(); }

		std::vector<Vertex> const& GetVertices() const { return m_Vertices; }
		std::vector<uint32_t> const& GetIndices() const { return m_Indices; }
		std::vector<Submesh> const& GetSubmeshes() const { return m_Submeshes; }
		// Default material of each slot (invalid handles mean "engine default material").
		std::vector<AssetHandle> const& GetMaterials() const { return m_Materials; }
		AABB const& GetBounds() const { return m_Bounds; }
		uint32_t GetTriangleCount() const { return static_cast<uint32_t>(m_Indices.size() / 3); }

	private:
		std::vector<Vertex> m_Vertices;
		std::vector<uint32_t> m_Indices;
		std::vector<Submesh> m_Submeshes;
		std::vector<AssetHandle> m_Materials;
		AABB m_Bounds;
	};
}
