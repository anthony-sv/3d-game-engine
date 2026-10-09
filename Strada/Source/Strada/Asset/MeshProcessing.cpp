#include "stpch.h"
#include "Strada/Asset/MeshProcessing.h"

#include <mikktspace.h>

#include <cmath>
#include <cstring>
#include <unordered_map>

namespace Strada::MeshProcessing
{
	namespace
	{
		int GetNumFaces(SMikkTSpaceContext const* context)
		{
			auto const* corners = static_cast<std::vector<Vertex> const*>(context->m_pUserData);
			return static_cast<int>(corners->size() / 3);
		}

		int GetNumVerticesOfFace(SMikkTSpaceContext const*, int const)
		{
			return 3;
		}

		Vertex const& GetCorner(SMikkTSpaceContext const* context, int const face, int const vertex)
		{
			auto const* corners = static_cast<std::vector<Vertex> const*>(context->m_pUserData);
			return (*corners)[static_cast<size_t>(face) * 3 + static_cast<size_t>(vertex)];
		}

		void GetPosition(SMikkTSpaceContext const* context, float output[], int const face, int const vertex)
		{
			glm::vec3 const& position = GetCorner(context, face, vertex).Position;
			output[0] = position.x;
			output[1] = position.y;
			output[2] = position.z;
		}

		void GetNormal(SMikkTSpaceContext const* context, float output[], int const face, int const vertex)
		{
			glm::vec3 const& normal = GetCorner(context, face, vertex).Normal;
			output[0] = normal.x;
			output[1] = normal.y;
			output[2] = normal.z;
		}

		void GetTexCoord(SMikkTSpaceContext const* context, float output[], int const face, int const vertex)
		{
			// UVs have their origin at the top-left while normal maps encode +Y as "up" in the image: flipping V makes
			// MikkTSpace's bitangent point towards the top of the image (the glTF tangent convention).
			glm::vec2 const& texCoord = GetCorner(context, face, vertex).TexCoord;
			output[0] = texCoord.x;
			output[1] = 1.0f - texCoord.y;
		}

		void SetTangentSpace(SMikkTSpaceContext const* context, float const tangent[], float const sign, int const face, int const vertex)
		{
			auto* corners = static_cast<std::vector<Vertex>*>(context->m_pUserData);
			Vertex& corner = (*corners)[static_cast<size_t>(face) * 3 + static_cast<size_t>(vertex)];
			corner.Tangent = glm::vec4(tangent[0], tangent[1], tangent[2], sign < 0.0f ? -1.0f : 1.0f);
		}

		struct BitwiseVertexHash
		{
			size_t operator()(Vertex const& vertex) const noexcept
			{
				// FNV-1a over the raw bytes; matches BitwiseVertexEqual.
				uint8_t bytes[sizeof(Vertex)];
				std::memcpy(bytes, &vertex, sizeof(Vertex));
				uint64_t hash = 0xCBF29CE484222325ull;
				for (uint8_t const byte : bytes)
				{
					hash = (hash ^ byte) * 0x100000001B3ull;
				}
				return static_cast<size_t>(hash);
			}
		};

		struct BitwiseVertexEqual
		{
			bool operator()(Vertex const& a, Vertex const& b) const noexcept { return std::memcmp(&a, &b, sizeof(Vertex)) == 0; }
		};

		bool IsUsableTangent(glm::vec3 const& tangent, glm::vec3 const& normal)
		{
			float const lengthSquared = glm::dot(tangent, tangent);
			if (!std::isfinite(lengthSquared) || lengthSquared < 1e-12f)
			{
				return false;
			}
			// Nearly parallel to the normal: no usable direction left after orthogonalization.
			float const alignment = glm::dot(tangent, normal);
			return alignment * alignment < 0.9999f * lengthSquared;
		}
	}

	std::vector<Vertex> Unweld(std::vector<Vertex> const& vertices, std::vector<uint32_t> const& indices)
	{
		std::vector<Vertex> corners;
		corners.reserve(indices.size());
		for (uint32_t const index : indices)
		{
			corners.push_back(vertices[index]);
		}
		return corners;
	}

	void ComputeFlatNormals(std::vector<Vertex>& corners)
	{
		for (size_t i = 0; i + 2 < corners.size(); i += 3)
		{
			glm::vec3 const edge1 = corners[i + 1].Position - corners[i].Position;
			glm::vec3 const edge2 = corners[i + 2].Position - corners[i].Position;
			glm::vec3 normal = glm::cross(edge1, edge2);
			float const length = glm::length(normal);
			normal = length > 1e-20f && std::isfinite(length) ? normal / length : glm::vec3(0.0f, 1.0f, 0.0f);
			corners[i].Normal = normal;
			corners[i + 1].Normal = normal;
			corners[i + 2].Normal = normal;
		}
	}

	void GenerateTangents(std::vector<Vertex>& corners)
	{
		if (corners.size() >= 3)
		{
			SMikkTSpaceInterface callbacks = {};
			callbacks.m_getNumFaces = GetNumFaces;
			callbacks.m_getNumVerticesOfFace = GetNumVerticesOfFace;
			callbacks.m_getPosition = GetPosition;
			callbacks.m_getNormal = GetNormal;
			callbacks.m_getTexCoord = GetTexCoord;
			callbacks.m_setTSpaceBasic = SetTangentSpace;

			SMikkTSpaceContext context = {};
			context.m_pInterface = &callbacks;
			context.m_pUserData = &corners;
			genTangSpaceDefault(&context);
		}

		// Orthonormalize against the normal and repair degenerate results (missing or collapsed UVs).
		for (Vertex& corner : corners)
		{
			glm::vec3 tangent(corner.Tangent);
			if (IsUsableTangent(tangent, corner.Normal))
			{
				tangent = glm::normalize(tangent - corner.Normal * glm::dot(corner.Normal, tangent));
				corner.Tangent = glm::vec4(tangent, corner.Tangent.w < 0.0f ? -1.0f : 1.0f);
			}
			else
			{
				corner.Tangent = glm::vec4(ComputeOrthogonalTangent(corner.Normal), 1.0f);
			}
		}
	}

	IndexedGeometry Weld(std::vector<Vertex> const& corners)
	{
		IndexedGeometry geometry;
		geometry.Indices.reserve(corners.size());
		std::unordered_map<Vertex, uint32_t, BitwiseVertexHash, BitwiseVertexEqual> lookup;
		lookup.reserve(corners.size());
		for (Vertex const& corner : corners)
		{
			auto const [it, inserted] = lookup.try_emplace(corner, static_cast<uint32_t>(geometry.Vertices.size()));
			if (inserted)
			{
				geometry.Vertices.push_back(corner);
			}
			geometry.Indices.push_back(it->second);
		}
		return geometry;
	}

	IndexedGeometry Finalize(std::vector<Vertex> vertices, std::vector<uint32_t> indices, bool hasNormals, bool hasTangents)
	{
		if (hasNormals && hasTangents)
		{
			return {std::move(vertices), std::move(indices)};
		}

		std::vector<Vertex> corners = Unweld(vertices, indices);
		if (!hasNormals)
		{
			ComputeFlatNormals(corners);
		}
		GenerateTangents(corners);
		return Weld(corners);
	}

	glm::vec3 ComputeOrthogonalTangent(glm::vec3 const& normal)
	{
		// Duff et al. 2017, "Building an Orthonormal Basis, Revisited".
		float const sign = std::copysign(1.0f, normal.z);
		float const a = -1.0f / (sign + normal.z);
		float const b = normal.x * normal.y * a;
		return glm::vec3(1.0f + sign * normal.x * normal.x * a, sign * b, -sign * normal.x);
	}
}
