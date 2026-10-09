#include "stpch.h"
#include "Strada/Asset/MeshFactory.h"

#include "Strada/Asset/MeshProcessing.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace Strada::MeshFactory
{
	namespace
	{
		struct Builder
		{
			std::vector<Vertex> Vertices;
			std::vector<uint32_t> Indices;

			uint32_t AddVertex(glm::vec3 const& position, glm::vec3 const& normal, glm::vec2 const& texCoord)
			{
				Vertex vertex;
				vertex.Position = position;
				vertex.Normal = glm::normalize(normal);
				vertex.TexCoord = texCoord;
				Vertices.push_back(vertex);
				return static_cast<uint32_t>(Vertices.size() - 1);
			}

			void AddTriangle(uint32_t a, uint32_t b, uint32_t c)
			{
				Indices.push_back(a);
				Indices.push_back(b);
				Indices.push_back(c);
			}

			// Corners in the order top-left, bottom-left, bottom-right, top-right as seen from the front.
			void AddQuad(uint32_t topLeft, uint32_t bottomLeft, uint32_t bottomRight, uint32_t topRight)
			{
				AddTriangle(topLeft, bottomLeft, bottomRight);
				AddTriangle(topLeft, bottomRight, topRight);
			}

			// A planar face with normal = cross(right, up), centered at center, of size 2 * halfSize.
			void AddFace(glm::vec3 const& center, glm::vec3 const& right, glm::vec3 const& up, float halfSize)
			{
				glm::vec3 const normal = glm::cross(right, up);
				uint32_t const topLeft = AddVertex(center + (-right + up) * halfSize, normal, {0.0f, 0.0f});
				uint32_t const bottomLeft = AddVertex(center + (-right - up) * halfSize, normal, {0.0f, 1.0f});
				uint32_t const bottomRight = AddVertex(center + (right - up) * halfSize, normal, {1.0f, 1.0f});
				uint32_t const topRight = AddVertex(center + (right + up) * halfSize, normal, {1.0f, 0.0f});
				AddQuad(topLeft, bottomLeft, bottomRight, topRight);
			}

			// Disc cap at height y facing +Y (or -Y), UVs projected from above.
			void AddCap(float y, float radius, uint32_t segments, bool facingUp)
			{
				glm::vec3 const normal(0.0f, facingUp ? 1.0f : -1.0f, 0.0f);
				uint32_t const center = AddVertex({0.0f, y, 0.0f}, normal, {0.5f, 0.5f});
				uint32_t const first = static_cast<uint32_t>(Vertices.size());
				for (uint32_t i = 0; i <= segments; i++)
				{
					float const angle = glm::two_pi<float>() * static_cast<float>(i) / static_cast<float>(segments);
					glm::vec3 const position(std::sin(angle) * radius, y, std::cos(angle) * radius);
					// -Z is "up" in the image from both sides; seen from below, the image is mirrored horizontally.
					float const u = 0.5f + (facingUp ? position.x : -position.x) / (2.0f * radius);
					float const v = 0.5f + position.z / (2.0f * radius);
					AddVertex(position, normal, {u, v});
				}
				for (uint32_t i = 0; i < segments; i++)
				{
					if (facingUp)
					{
						AddTriangle(center, first + i, first + i + 1);
					}
					else
					{
						AddTriangle(center, first + i + 1, first + i);
					}
				}
			}

			Ref<MeshSource> Build(AssetHandle material, char const* name)
			{
				std::vector<Vertex> corners = MeshProcessing::Unweld(Vertices, Indices);
				MeshProcessing::GenerateTangents(corners);
				MeshProcessing::IndexedGeometry geometry = MeshProcessing::Weld(corners);

				Submesh submesh;
				submesh.VertexCount = static_cast<uint32_t>(geometry.Vertices.size());
				submesh.IndexCount = static_cast<uint32_t>(geometry.Indices.size());
				submesh.Name = name;

				Result<Ref<MeshSource>> mesh =
					MeshSource::Create(std::move(geometry.Vertices), std::move(geometry.Indices), {std::move(submesh)}, {material});
				ST_CORE_ASSERT(mesh.IsOk(), "Built-in mesh generation produced invalid data");
				return mesh.GetValue();
			}
		};

		// Direction on the unit sphere for polar angle theta (0 = +Y) and azimuth phi (0 = +Z, increasing towards +X).
		glm::vec3 SphereDirection(float theta, float phi)
		{
			return {std::sin(theta) * std::sin(phi), std::cos(theta), std::sin(theta) * std::cos(phi)};
		}
	}

	Ref<MeshSource> CreateCube(AssetHandle material)
	{
		Builder builder;
		builder.AddFace({0.5f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, 0.5f);
		builder.AddFace({-0.5f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, 0.5f);
		builder.AddFace({0.0f, 0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 0.5f);
		builder.AddFace({0.0f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, 0.5f);
		builder.AddFace({0.0f, 0.0f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 0.5f);
		builder.AddFace({0.0f, 0.0f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 0.5f);
		return builder.Build(material, "Cube");
	}

	Ref<MeshSource> CreateSphere(AssetHandle material, uint32_t segments, uint32_t rings)
	{
		segments = std::max(segments, 3u);
		rings = std::max(rings, 2u);
		constexpr float Radius = 0.5f;

		Builder builder;
		for (uint32_t ring = 0; ring <= rings; ring++)
		{
			float const v = static_cast<float>(ring) / static_cast<float>(rings);
			for (uint32_t segment = 0; segment <= segments; segment++)
			{
				float const u = static_cast<float>(segment) / static_cast<float>(segments);
				glm::vec3 const direction = SphereDirection(v * glm::pi<float>(), u * glm::two_pi<float>());
				builder.AddVertex(direction * Radius, direction, {u, v});
			}
		}

		uint32_t const stride = segments + 1;
		for (uint32_t ring = 0; ring < rings; ring++)
		{
			for (uint32_t segment = 0; segment < segments; segment++)
			{
				uint32_t const topLeft = ring * stride + segment;
				uint32_t const bottomLeft = topLeft + stride;
				uint32_t const bottomRight = bottomLeft + 1;
				uint32_t const topRight = topLeft + 1;
				// The pole rows would produce zero-area triangles.
				if (ring != 0)
				{
					builder.AddTriangle(topLeft, bottomRight, topRight);
				}
				if (ring != rings - 1)
				{
					builder.AddTriangle(topLeft, bottomLeft, bottomRight);
				}
			}
		}
		return builder.Build(material, "Sphere");
	}

	Ref<MeshSource> CreatePlane(AssetHandle material)
	{
		Builder builder;
		builder.AddFace({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 0.5f);
		return builder.Build(material, "Plane");
	}

	Ref<MeshSource> CreateCylinder(AssetHandle material, uint32_t segments)
	{
		segments = std::max(segments, 3u);
		constexpr float Radius = 0.5f;
		constexpr float HalfHeight = 0.5f;

		Builder builder;
		uint32_t const first = static_cast<uint32_t>(builder.Vertices.size());
		for (uint32_t segment = 0; segment <= segments; segment++)
		{
			float const u = static_cast<float>(segment) / static_cast<float>(segments);
			float const angle = u * glm::two_pi<float>();
			glm::vec3 const normal(std::sin(angle), 0.0f, std::cos(angle));
			builder.AddVertex(normal * Radius + glm::vec3(0.0f, HalfHeight, 0.0f), normal, {u, 0.0f});
			builder.AddVertex(normal * Radius - glm::vec3(0.0f, HalfHeight, 0.0f), normal, {u, 1.0f});
		}
		for (uint32_t segment = 0; segment < segments; segment++)
		{
			uint32_t const topLeft = first + segment * 2;
			builder.AddQuad(topLeft, topLeft + 1, topLeft + 3, topLeft + 2);
		}

		builder.AddCap(HalfHeight, Radius, segments, true);
		builder.AddCap(-HalfHeight, Radius, segments, false);
		return builder.Build(material, "Cylinder");
	}

	Ref<MeshSource> CreateCapsule(AssetHandle material, uint32_t segments, uint32_t hemisphereRings)
	{
		segments = std::max(segments, 3u);
		hemisphereRings = std::max(hemisphereRings, 1u);
		constexpr float Radius = 0.5f;
		constexpr float HalfHeight = 0.5f;

		// Profile rows from the top pole to the bottom pole; the equator is duplicated (top and bottom of the cylinder).
		struct Row
		{
			float Theta;
			float OffsetY;
		};
		std::vector<Row> rows;
		for (uint32_t ring = 0; ring <= hemisphereRings; ring++)
		{
			rows.push_back({glm::half_pi<float>() * static_cast<float>(ring) / static_cast<float>(hemisphereRings), HalfHeight});
		}
		for (uint32_t ring = 0; ring <= hemisphereRings; ring++)
		{
			float const theta =
				glm::half_pi<float>() + glm::half_pi<float>() * static_cast<float>(ring) / static_cast<float>(hemisphereRings);
			rows.push_back({theta, -HalfHeight});
		}

		// V follows the arc length of the profile so textures are not stretched on the cylinder part.
		float const totalLength = glm::pi<float>() * Radius + 2.0f * HalfHeight;
		Builder builder;
		for (size_t row = 0; row < rows.size(); row++)
		{
			float const theta = rows[row].Theta;
			float const length = row <= hemisphereRings ? theta * Radius : theta * Radius + 2.0f * HalfHeight;
			float const v = length / totalLength;
			for (uint32_t segment = 0; segment <= segments; segment++)
			{
				float const u = static_cast<float>(segment) / static_cast<float>(segments);
				glm::vec3 const direction = SphereDirection(theta, u * glm::two_pi<float>());
				builder.AddVertex(direction * Radius + glm::vec3(0.0f, rows[row].OffsetY, 0.0f), direction, {u, v});
			}
		}

		uint32_t const stride = segments + 1;
		uint32_t const rowCount = static_cast<uint32_t>(rows.size());
		for (uint32_t row = 0; row + 1 < rowCount; row++)
		{
			for (uint32_t segment = 0; segment < segments; segment++)
			{
				uint32_t const topLeft = row * stride + segment;
				uint32_t const bottomLeft = topLeft + stride;
				uint32_t const bottomRight = bottomLeft + 1;
				uint32_t const topRight = topLeft + 1;
				if (row != 0)
				{
					builder.AddTriangle(topLeft, bottomRight, topRight);
				}
				if (row != rowCount - 2)
				{
					builder.AddTriangle(topLeft, bottomLeft, bottomRight);
				}
			}
		}
		return builder.Build(material, "Capsule");
	}

	Ref<MeshSource> CreateCone(AssetHandle material, uint32_t segments)
	{
		segments = std::max(segments, 3u);
		constexpr float Radius = 0.5f;
		constexpr float Height = 1.0f;
		constexpr float HalfHeight = Height * 0.5f;

		Builder builder;
		for (uint32_t segment = 0; segment < segments; segment++)
		{
			float const u0 = static_cast<float>(segment) / static_cast<float>(segments);
			float const u1 = static_cast<float>(segment + 1) / static_cast<float>(segments);
			float const uMid = (u0 + u1) * 0.5f;
			auto const sideNormal = [](float angle)
			{
				// The side surface tilts up by atan(radius / height).
				return glm::normalize(glm::vec3(std::sin(angle) * Height, Radius, std::cos(angle) * Height));
			};
			float const angle0 = u0 * glm::two_pi<float>();
			float const angle1 = u1 * glm::two_pi<float>();
			float const angleMid = uMid * glm::two_pi<float>();

			uint32_t const apex = builder.AddVertex({0.0f, HalfHeight, 0.0f}, sideNormal(angleMid), {uMid, 0.0f});
			uint32_t const left =
				builder.AddVertex({std::sin(angle0) * Radius, -HalfHeight, std::cos(angle0) * Radius}, sideNormal(angle0), {u0, 1.0f});
			uint32_t const right =
				builder.AddVertex({std::sin(angle1) * Radius, -HalfHeight, std::cos(angle1) * Radius}, sideNormal(angle1), {u1, 1.0f});
			builder.AddTriangle(apex, left, right);
		}
		builder.AddCap(-HalfHeight, Radius, segments, false);
		return builder.Build(material, "Cone");
	}

	Ref<MeshSource> CreateQuad(AssetHandle material)
	{
		Builder builder;
		builder.AddFace({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 0.5f);
		return builder.Build(material, "Quad");
	}
}
