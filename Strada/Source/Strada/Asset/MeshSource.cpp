#include "stpch.h"
#include "Strada/Asset/MeshSource.h"

#include <cmath>

namespace Strada
{
	Result<Ref<MeshSource>> MeshSource::Create(std::vector<Vertex> vertices, std::vector<uint32_t> indices, std::vector<Submesh> submeshes,
	                                           std::vector<AssetHandle> materials)
	{
		if (submeshes.empty())
		{
			return Error{"a mesh needs at least one submesh"};
		}
		if (materials.empty())
		{
			return Error{"a mesh needs at least one material slot"};
		}

		for (Vertex const& vertex : vertices)
		{
			if (!std::isfinite(vertex.Position.x) || !std::isfinite(vertex.Position.y) || !std::isfinite(vertex.Position.z))
			{
				return Error{"mesh contains non-finite vertex positions"};
			}
		}

		for (size_t i = 0; i < submeshes.size(); i++)
		{
			Submesh& submesh = submeshes[i];
			if (submesh.IndexCount == 0 || submesh.IndexCount % 3 != 0)
			{
				return MakeError("submesh {} has {} indices (expected a non-empty triangle list)", i, submesh.IndexCount);
			}
			if (static_cast<uint64_t>(submesh.BaseIndex) + submesh.IndexCount > indices.size())
			{
				return MakeError("submesh {} index range exceeds the index buffer", i);
			}
			if (submesh.VertexCount == 0 || static_cast<uint64_t>(submesh.BaseVertex) + submesh.VertexCount > vertices.size())
			{
				return MakeError("submesh {} vertex range exceeds the vertex buffer", i);
			}
			if (submesh.MaterialIndex >= materials.size())
			{
				return MakeError("submesh {} uses material slot {} but the mesh has {}", i, submesh.MaterialIndex, materials.size());
			}

			AABB bounds;
			for (uint32_t j = 0; j < submesh.IndexCount; j++)
			{
				uint32_t const index = indices[submesh.BaseIndex + j];
				if (index >= submesh.VertexCount)
				{
					return MakeError("submesh {} index {} is out of range", i, index);
				}
				bounds.Expand(vertices[submesh.BaseVertex + index].Position);
			}
			submesh.Bounds = bounds;
		}

		return CreateRef<MeshSource>(PrivateTag{}, std::move(vertices), std::move(indices), std::move(submeshes), std::move(materials));
	}

	MeshSource::MeshSource(PrivateTag, std::vector<Vertex> vertices, std::vector<uint32_t> indices, std::vector<Submesh> submeshes,
	                       std::vector<AssetHandle> materials)
		: m_Vertices(std::move(vertices)),
		  m_Indices(std::move(indices)),
		  m_Submeshes(std::move(submeshes)),
		  m_Materials(std::move(materials))
	{
		for (Submesh const& submesh : m_Submeshes)
		{
			m_Bounds.Expand(submesh.Bounds);
		}
	}
}
