#include "stpch.h"
#include "Strada/Asset/MeshImporter.h"

#include "Strada/Asset/MeshProcessing.h"
#include "Strada/Core/Base64.h"
#include "Strada/Core/FileSystem.h"

#include <cgltf.h>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <ufbx.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace Strada
{
	namespace
	{
		std::string ToLower(std::string_view text)
		{
			std::string lower(text);
			for (char& character : lower)
			{
				character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
			}
			return lower;
		}

		Error MakeTooLargeError()
		{
			return MakeError("the model is too large: it has more than {} vertices or {} triangles with every node instance baked in",
			                 MeshImporter::MaxVertices, MeshImporter::MaxTriangles);
		}

		// Adds `count` to `total` (at most `limit`) unless the sum would exceed `limit`.
		[[nodiscard]] bool AddWithinLimit(uint64_t& total, uint64_t count, uint64_t limit)
		{
			if (count > limit - total)
			{
				return false;
			}
			total += count;
			return true;
		}

		// Applies a node's world transform to imported geometry.
		class GeometryTransform
		{
		public:
			explicit GeometryTransform(glm::mat4 const& matrix)
				: m_Matrix(matrix),
				  m_Linear(matrix)
			{
				float const determinant = glm::determinant(m_Linear);
				m_IsValid = std::isfinite(determinant) && std::abs(determinant) > 1e-30f;
				m_IsMirrored = determinant < 0.0f;
				m_NormalMatrix = m_IsValid ? glm::inverseTranspose(m_Linear) : glm::mat3(1.0f);
			}

			bool IsValid() const { return m_IsValid; }
			bool IsMirrored() const { return m_IsMirrored; }

			void Apply(Vertex& vertex, bool transformTangent) const
			{
				vertex.Position = glm::vec3(m_Matrix * glm::vec4(vertex.Position, 1.0f));
				vertex.Normal = SafeNormalize(m_NormalMatrix * vertex.Normal, glm::vec3(0.0f, 1.0f, 0.0f));
				if (transformTangent)
				{
					glm::vec3 const tangent = SafeNormalize(m_Linear * glm::vec3(vertex.Tangent), glm::vec3(1.0f, 0.0f, 0.0f));
					// Mirroring flips the handedness of the tangent frame.
					float const sign = (vertex.Tangent.w < 0.0f) != m_IsMirrored ? -1.0f : 1.0f;
					vertex.Tangent = glm::vec4(tangent, sign);
				}
			}

		private:
			static glm::vec3 SafeNormalize(glm::vec3 const& value, glm::vec3 const& fallback)
			{
				float const length = glm::length(value);
				return length > 1e-20f && std::isfinite(length) ? value / length : fallback;
			}

			glm::mat4 m_Matrix;
			glm::mat3 m_Linear;
			glm::mat3 m_NormalMatrix = glm::mat3(1.0f);
			bool m_IsValid = false;
			bool m_IsMirrored = false;
		};

		class ModelBuilder
		{
		public:
			ImportedModel& GetModel() { return m_Model; }

			void Warn(std::string message)
			{
				// Repeated problems (one per primitive or material) are reported once.
				if (m_ReportedWarnings.insert(message).second)
				{
					m_Model.Warnings.push_back(std::move(message));
				}
			}

			// Adds a triangle-list submesh given in node space.
			Result<void> AddSubmesh(std::string name, std::vector<Vertex> vertices, std::vector<uint32_t> indices, bool hasNormals,
			                        bool hasTangents, uint32_t materialIndex, GeometryTransform const& transform)
			{
				for (Vertex& vertex : vertices)
				{
					transform.Apply(vertex, hasTangents);
					if (!std::isfinite(vertex.Position.x) || !std::isfinite(vertex.Position.y) || !std::isfinite(vertex.Position.z))
					{
						return MakeError("submesh '{}' has non-finite vertex positions", name);
					}
				}
				if (transform.IsMirrored())
				{
					for (size_t i = 0; i + 2 < indices.size(); i += 3)
					{
						std::swap(indices[i + 1], indices[i + 2]);
					}
				}

				MeshProcessing::IndexedGeometry geometry =
					MeshProcessing::Finalize(std::move(vertices), std::move(indices), hasNormals, hasTangents);
				if (geometry.Indices.empty())
				{
					return {};
				}

				// The importers check the limits before building anything; welding can still split vertices (flat normals).
				if (m_Model.Vertices.size() + geometry.Vertices.size() > MeshImporter::MaxVertices ||
				    m_Model.Indices.size() + geometry.Indices.size() > MeshImporter::MaxTriangles * 3)
				{
					return MakeTooLargeError();
				}

				Submesh submesh;
				submesh.BaseVertex = static_cast<uint32_t>(m_Model.Vertices.size());
				submesh.VertexCount = static_cast<uint32_t>(geometry.Vertices.size());
				submesh.BaseIndex = static_cast<uint32_t>(m_Model.Indices.size());
				submesh.IndexCount = static_cast<uint32_t>(geometry.Indices.size());
				submesh.MaterialIndex = materialIndex;
				submesh.Name = std::move(name);

				m_Model.Vertices.insert(m_Model.Vertices.end(), geometry.Vertices.begin(), geometry.Vertices.end());
				m_Model.Indices.insert(m_Model.Indices.end(), geometry.Indices.begin(), geometry.Indices.end());
				m_Model.Submeshes.push_back(std::move(submesh));
				return {};
			}

			uint32_t AddMaterial(ImportedMaterial material)
			{
				m_Model.Materials.push_back(std::move(material));
				return static_cast<uint32_t>(m_Model.Materials.size() - 1);
			}

			uint32_t GetDefaultMaterialSlot()
			{
				if (!m_DefaultMaterialSlot)
				{
					ImportedMaterial material;
					material.Name = "Default";
					m_DefaultMaterialSlot = AddMaterial(std::move(material));
				}
				return *m_DefaultMaterialSlot;
			}

			int32_t AddTexture(ImportedTexture texture)
			{
				m_Model.Textures.push_back(std::move(texture));
				return static_cast<int32_t>(m_Model.Textures.size() - 1);
			}

		private:
			ImportedModel m_Model;
			std::unordered_set<std::string> m_ReportedWarnings;
			std::optional<uint32_t> m_DefaultMaterialSlot;
		};

		// --- glTF 2.0 (cgltf) ------------------------------------------------------------------------------------------

		void* GltfAllocate(void*, cgltf_size size)
		{
			return std::malloc(size == 0 ? 1 : size);
		}

		void GltfFree(void*, void* pointer)
		{
			std::free(pointer);
		}

		// Reads referenced files (.bin buffers) through FileSystem so UTF-8 paths work everywhere. Like cgltf's default reader,
		// a non-zero *size requests exactly that many bytes.
		cgltf_result GltfReadFile(cgltf_memory_options const* memory, cgltf_file_options const*, char const* path, cgltf_size* size,
		                          void** data)
		{
			Result<Buffer> file = FileSystem::ReadBinaryFile(FileSystem::PathFromUtf8(path));
			if (!file)
			{
				return cgltf_result_file_not_found;
			}

			cgltf_size const fileSize = file.GetValue().GetSize();
			cgltf_size const requested = size != nullptr && *size != 0 ? *size : fileSize;
			if (requested > fileSize)
			{
				return cgltf_result_data_too_short;
			}

			void* block = memory->alloc_func != nullptr ? memory->alloc_func(memory->user_data, requested == 0 ? 1 : requested)
			                                            : GltfAllocate(nullptr, requested);
			if (block == nullptr)
			{
				return cgltf_result_out_of_memory;
			}
			if (requested > 0)
			{
				std::memcpy(block, file.GetValue().GetData(), requested);
			}
			if (size != nullptr)
			{
				*size = requested;
			}
			*data = block;
			return cgltf_result_success;
		}

		void GltfReleaseFile(cgltf_memory_options const* memory, cgltf_file_options const*, void* data)
		{
			if (memory->free_func != nullptr)
			{
				memory->free_func(memory->user_data, data);
			}
			else
			{
				GltfFree(nullptr, data);
			}
		}

		char const* GltfResultToString(cgltf_result result)
		{
			switch (result)
			{
				case cgltf_result_success:
					return "success";
				case cgltf_result_data_too_short:
					return "the file is truncated";
				case cgltf_result_unknown_format:
					return "unknown file format";
				case cgltf_result_invalid_json:
					return "invalid JSON";
				case cgltf_result_invalid_gltf:
					return "invalid glTF data";
				case cgltf_result_invalid_options:
					return "invalid options";
				case cgltf_result_file_not_found:
					return "a referenced file was not found";
				case cgltf_result_io_error:
					return "I/O error";
				case cgltf_result_out_of_memory:
					return "out of memory";
				case cgltf_result_legacy_gltf:
					return "glTF 1.0 files are not supported";
				case cgltf_result_max_enum:
					break;
			}
			return "unknown error";
		}

		struct GltfDataDeleter
		{
			void operator()(cgltf_data* data) const { cgltf_free(data); }
		};

		// Whether `count` elements of `size` bytes, `stride` bytes apart and starting at `offset`, fit in `limit` bytes;
		// computed without overflow, since every value comes from the file.
		bool FitsElements(cgltf_size offset, cgltf_size stride, cgltf_size count, cgltf_size size, cgltf_size limit)
		{
			if (offset > limit)
			{
				return false;
			}
			if (count == 0)
			{
				return true;
			}
			if (size > limit - offset)
			{
				return false;
			}
			return stride == 0 || count - 1 <= (limit - offset - size) / stride;
		}

		// Runs before cgltf_validate: its range arithmetic is not overflow-safe, and it reads sparse indices before it checks
		// the buffer views they lie in.
		Result<void> CheckGltfRanges(cgltf_data const& data)
		{
			for (size_t i = 0; i < data.buffer_views_count; i++)
			{
				cgltf_buffer_view const& view = data.buffer_views[i];
				if (view.buffer == nullptr || !FitsElements(view.offset, 1, view.size, 1, view.buffer->size))
				{
					return MakeError("buffer view {} lies outside its buffer", i);
				}
			}
			for (size_t i = 0; i < data.accessors_count; i++)
			{
				cgltf_accessor const& accessor = data.accessors[i];
				cgltf_size const elementSize = cgltf_calc_size(accessor.type, accessor.component_type);
				if (elementSize == 0)
				{
					// An invalid type, which cgltf_validate refuses.
					continue;
				}
				if (accessor.buffer_view != nullptr &&
				    !FitsElements(accessor.offset, accessor.stride, accessor.count, elementSize, accessor.buffer_view->size))
				{
					return MakeError("accessor {} reaches beyond its buffer view", i);
				}
				if (accessor.is_sparse)
				{
					// glTF packs sparse values tightly, but cgltf steps through them with the accessor's stride.
					if (accessor.stride != elementSize)
					{
						return MakeError("accessor {} is sparse with interleaved data, which is not supported", i);
					}
					cgltf_accessor_sparse const& sparse = accessor.sparse;
					cgltf_size const indexSize = cgltf_component_size(sparse.indices_component_type);
					bool const fits =
						sparse.count <= accessor.count && sparse.indices_buffer_view != nullptr && sparse.values_buffer_view != nullptr &&
						FitsElements(sparse.indices_byte_offset, indexSize, sparse.count, indexSize, sparse.indices_buffer_view->size) &&
						FitsElements(sparse.values_byte_offset, elementSize, sparse.count, elementSize, sparse.values_buffer_view->size);
					if (!fits)
					{
						return MakeError("the sparse data of accessor {} reaches beyond its buffer views", i);
					}
				}
			}
			return {};
		}

		bool IsTriangleMode(cgltf_primitive_type type)
		{
			return type == cgltf_primitive_type_triangles || type == cgltf_primitive_type_triangle_strip ||
			       type == cgltf_primitive_type_triangle_fan;
		}

		cgltf_accessor const* FindPositions(cgltf_primitive const& primitive)
		{
			for (size_t i = 0; i < primitive.attributes_count; i++)
			{
				if (primitive.attributes[i].type == cgltf_attribute_type_position)
				{
					return primitive.attributes[i].data;
				}
			}
			return nullptr;
		}

		std::vector<float> ReadGltfFloats(cgltf_accessor const* accessor, size_t components)
		{
			if (accessor == nullptr || cgltf_num_components(accessor->type) != components)
			{
				return {};
			}
			std::vector<float> values(accessor->count * components);
			if (cgltf_accessor_unpack_floats(accessor, values.data(), values.size()) != values.size())
			{
				return {};
			}
			return values;
		}

		class GltfImporter
		{
		public:
			GltfImporter(std::filesystem::path const& path, cgltf_data const& data, ModelBuilder& builder)
				: m_Directory(path.parent_path()),
				  m_Data(data),
				  m_Builder(builder)
			{
			}

			Result<void> Import()
			{
				std::vector<cgltf_node const*> const nodes = CollectMeshNodes();
				if (Result<void> size = CheckSize(nodes); !size)
				{
					return size;
				}
				for (cgltf_node const* node : nodes)
				{
					if (Result<void> result = ImportNode(*node); !result)
					{
						return result;
					}
				}
				return {};
			}

		private:
			// The nodes with meshes, in import order.
			std::vector<cgltf_node const*> CollectMeshNodes() const
			{
				std::vector<cgltf_node const*> roots;
				cgltf_scene const* scene = m_Data.scene != nullptr ? m_Data.scene : (m_Data.scenes_count > 0 ? &m_Data.scenes[0] : nullptr);
				if (scene != nullptr)
				{
					for (size_t i = 0; i < scene->nodes_count; i++)
					{
						roots.push_back(scene->nodes[i]);
					}
				}
				else
				{
					for (size_t i = 0; i < m_Data.nodes_count; i++)
					{
						if (m_Data.nodes[i].parent == nullptr)
						{
							roots.push_back(&m_Data.nodes[i]);
						}
					}
				}

				// Iterative traversal: hierarchies can be deep, and a visited set guards against malformed graphs.
				std::vector<cgltf_node const*> nodes;
				std::vector<cgltf_node const*> stack(roots.rbegin(), roots.rend());
				std::unordered_set<cgltf_node const*> visited;
				while (!stack.empty())
				{
					cgltf_node const* node = stack.back();
					stack.pop_back();
					if (node == nullptr || !visited.insert(node).second)
					{
						continue;
					}
					if (node->mesh != nullptr)
					{
						nodes.push_back(node);
					}
					for (size_t i = node->children_count; i > 0; i--)
					{
						stack.push_back(node->children[i - 1]);
					}
				}
				return nodes;
			}

			// Counts the geometry of every mesh instance before any of it is read: the counts come from the file, and a node
			// can instance a large mesh many times.
			static Result<void> CheckSize(std::vector<cgltf_node const*> const& nodes)
			{
				uint64_t vertices = 0;
				uint64_t triangles = 0;
				for (cgltf_node const* node : nodes)
				{
					for (size_t i = 0; i < node->mesh->primitives_count; i++)
					{
						cgltf_primitive const& primitive = node->mesh->primitives[i];
						cgltf_accessor const* positions = FindPositions(primitive);
						if (!IsTriangleMode(primitive.type) || positions == nullptr)
						{
							continue;
						}
						uint64_t const elements = primitive.indices != nullptr ? primitive.indices->count : positions->count;
						uint64_t const primitiveTriangles = primitive.type == cgltf_primitive_type_triangles ? elements / 3
						                                    : elements >= 3                                  ? elements - 2
						                                                                                     : 0;
						if (!AddWithinLimit(vertices, positions->count, MeshImporter::MaxVertices) ||
						    !AddWithinLimit(triangles, primitiveTriangles, MeshImporter::MaxTriangles))
						{
							return MakeTooLargeError();
						}
					}
				}
				return {};
			}

			Result<void> ImportNode(cgltf_node const& node)
			{
				if (node.mesh == nullptr)
				{
					return {};
				}

				glm::mat4 world(1.0f);
				if (node.skin != nullptr)
				{
					// The node transform of skinned meshes is ignored by the glTF specification; without skinning support the
					// mesh is imported in its bind pose.
					m_Builder.Warn("skinning is not supported; skinned meshes are imported in their bind pose");
				}
				else
				{
					float matrix[16];
					cgltf_node_transform_world(&node, matrix);
					world = glm::make_mat4(matrix);
				}

				GeometryTransform const transform(world);
				if (!transform.IsValid())
				{
					m_Builder.Warn(
						fmt::format("node '{}' has a degenerate transform and was skipped", node.name != nullptr ? node.name : ""));
					return {};
				}

				cgltf_mesh const& mesh = *node.mesh;
				for (size_t i = 0; i < mesh.primitives_count; i++)
				{
					if (Result<void> result = ImportPrimitive(node, mesh, i, transform); !result)
					{
						return result;
					}
				}
				return {};
			}

			Result<void> ImportPrimitive(cgltf_node const& node, cgltf_mesh const& mesh, size_t primitiveIndex,
			                             GeometryTransform const& transform)
			{
				cgltf_primitive const& primitive = mesh.primitives[primitiveIndex];
				std::string const name =
					fmt::format("{}#{}", mesh.name != nullptr ? mesh.name : (node.name != nullptr ? node.name : "Mesh"), primitiveIndex);

				if (!IsTriangleMode(primitive.type))
				{
					m_Builder.Warn("point and line primitives are not supported and were skipped");
					return {};
				}
				if (primitive.targets_count > 0)
				{
					m_Builder.Warn("morph targets are not supported; the base geometry was imported");
				}

				// The same accessor CheckSize counted.
				cgltf_accessor const* const positions = FindPositions(primitive);
				cgltf_accessor const* normals = nullptr;
				cgltf_accessor const* tangents = nullptr;
				cgltf_accessor const* texCoords = nullptr;
				for (size_t i = 0; i < primitive.attributes_count; i++)
				{
					cgltf_attribute const& attribute = primitive.attributes[i];
					switch (attribute.type)
					{
						case cgltf_attribute_type_normal:
							normals = attribute.data;
							break;
						case cgltf_attribute_type_tangent:
							tangents = attribute.data;
							break;
						case cgltf_attribute_type_texcoord:
							if (attribute.index == 0)
							{
								texCoords = attribute.data;
							}
							break;
						default:
							break;
					}
				}

				if (positions == nullptr || positions->count == 0)
				{
					m_Builder.Warn(fmt::format("primitive '{}' has no positions and was skipped", name));
					return {};
				}
				size_t const vertexCount = positions->count;
				std::vector<float> const positionData = ReadGltfFloats(positions, 3);
				if (positionData.empty())
				{
					return MakeError("primitive '{}' has unreadable positions", name);
				}

				auto const readOptional = [&](cgltf_accessor const* accessor, size_t components, char const* attributeName)
				{
					if (accessor == nullptr)
					{
						return std::vector<float>();
					}
					std::vector<float> values =
						accessor->count == vertexCount ? ReadGltfFloats(accessor, components) : std::vector<float>();
					if (values.empty())
					{
						m_Builder.Warn(fmt::format("primitive '{}' has invalid {} data; it was ignored", name, attributeName));
					}
					return values;
				};
				std::vector<float> const normalData = readOptional(normals, 3, "NORMAL");
				// Tangents are only meaningful together with the authored normals.
				std::vector<float> const tangentData = normalData.empty() ? std::vector<float>() : readOptional(tangents, 4, "TANGENT");
				std::vector<float> const texCoordData = readOptional(texCoords, 2, "TEXCOORD_0");

				std::vector<Vertex> vertices(vertexCount);
				for (size_t i = 0; i < vertexCount; i++)
				{
					Vertex& vertex = vertices[i];
					vertex.Position = glm::vec3(positionData[i * 3], positionData[i * 3 + 1], positionData[i * 3 + 2]);
					if (!normalData.empty())
					{
						vertex.Normal = glm::vec3(normalData[i * 3], normalData[i * 3 + 1], normalData[i * 3 + 2]);
					}
					if (!tangentData.empty())
					{
						vertex.Tangent =
							glm::vec4(tangentData[i * 4], tangentData[i * 4 + 1], tangentData[i * 4 + 2], tangentData[i * 4 + 3]);
					}
					if (!texCoordData.empty())
					{
						vertex.TexCoord = glm::vec2(texCoordData[i * 2], texCoordData[i * 2 + 1]);
					}
				}

				std::vector<uint32_t> elements;
				if (primitive.indices != nullptr)
				{
					elements.reserve(primitive.indices->count);
					for (size_t i = 0; i < primitive.indices->count; i++)
					{
						size_t const index = cgltf_accessor_read_index(primitive.indices, i);
						if (index >= vertexCount)
						{
							return MakeError("primitive '{}' has an out-of-range index {}", name, index);
						}
						elements.push_back(static_cast<uint32_t>(index));
					}
				}
				else
				{
					elements.resize(vertexCount);
					for (size_t i = 0; i < vertexCount; i++)
					{
						elements[i] = static_cast<uint32_t>(i);
					}
				}

				if (primitive.type == cgltf_primitive_type_triangles && elements.size() % 3 != 0)
				{
					m_Builder.Warn(fmt::format("primitive '{}' has an incomplete last triangle", name));
				}
				std::vector<uint32_t> indices = ToTriangleList(primitive.type, elements);
				if (indices.empty())
				{
					return {};
				}

				uint32_t const materialSlot = GetMaterialSlot(primitive.material);
				return m_Builder.AddSubmesh(name, std::move(vertices), std::move(indices), !normalData.empty(), !tangentData.empty(),
				                            materialSlot, transform);
			}

			static std::vector<uint32_t> ToTriangleList(cgltf_primitive_type type, std::vector<uint32_t> const& elements)
			{
				std::vector<uint32_t> indices;
				if (type == cgltf_primitive_type_triangles)
				{
					indices.assign(elements.begin(), elements.begin() + static_cast<std::ptrdiff_t>((elements.size() / 3) * 3));
				}
				else if (elements.size() >= 3)
				{
					indices.reserve((elements.size() - 2) * 3);
					for (size_t i = 0; i + 2 < elements.size(); i++)
					{
						if (type == cgltf_primitive_type_triangle_strip)
						{
							// glTF: triangle i is {i, i + 1 + i % 2, i + 2 - i % 2}.
							indices.push_back(elements[i]);
							indices.push_back(elements[i + 1 + i % 2]);
							indices.push_back(elements[i + 2 - i % 2]);
						}
						else
						{
							// glTF: triangle i of a fan is {i + 1, i + 2, 0}.
							indices.push_back(elements[i + 1]);
							indices.push_back(elements[i + 2]);
							indices.push_back(elements[0]);
						}
					}
				}
				return indices;
			}

			uint32_t GetMaterialSlot(cgltf_material const* material)
			{
				if (material == nullptr)
				{
					return m_Builder.GetDefaultMaterialSlot();
				}
				if (auto const it = m_MaterialSlots.find(material); it != m_MaterialSlots.end())
				{
					return it->second;
				}

				ImportedMaterial imported;
				size_t const materialIndex = static_cast<size_t>(material - m_Data.materials);
				imported.Name = material->name != nullptr ? material->name : fmt::format("Material{}", materialIndex);
				MaterialData& data = imported.Data;

				if (material->has_pbr_specular_glossiness && !material->has_pbr_metallic_roughness)
				{
					cgltf_pbr_specular_glossiness const& specularGlossiness = material->pbr_specular_glossiness;
					m_Builder.Warn("specular-glossiness materials are approximated with metallic-roughness");
					data.BaseColor = glm::make_vec4(specularGlossiness.diffuse_factor);
					data.Metallic = 0.0f;
					data.Roughness = 1.0f - specularGlossiness.glossiness_factor;
					imported.BaseColorTexture = GetTexture(specularGlossiness.diffuse_texture, data, true);
				}
				else
				{
					// cgltf fills the glTF defaults (metallic 1, roughness 1) when pbrMetallicRoughness is absent.
					cgltf_pbr_metallic_roughness const& pbr = material->pbr_metallic_roughness;
					data.BaseColor = glm::make_vec4(pbr.base_color_factor);
					data.Metallic = pbr.metallic_factor;
					data.Roughness = pbr.roughness_factor;
					imported.BaseColorTexture = GetTexture(pbr.base_color_texture, data, true);
					imported.MetallicRoughnessTexture = GetTexture(pbr.metallic_roughness_texture, data, false);
				}

				imported.NormalTexture = GetTexture(material->normal_texture, data, false);
				if (material->normal_texture.texture != nullptr)
				{
					data.NormalStrength = material->normal_texture.scale;
				}
				imported.OcclusionTexture = GetTexture(material->occlusion_texture, data, false);
				if (material->occlusion_texture.texture != nullptr)
				{
					data.OcclusionStrength = material->occlusion_texture.scale;
				}
				imported.EmissiveTexture = GetTexture(material->emissive_texture, data, false);
				data.EmissiveColor = glm::make_vec3(material->emissive_factor);
				data.EmissiveIntensity = material->has_emissive_strength ? material->emissive_strength.emissive_strength : 1.0f;

				switch (material->alpha_mode)
				{
					case cgltf_alpha_mode_mask:
						data.AlphaMode = MaterialAlphaMode::Mask;
						break;
					case cgltf_alpha_mode_blend:
						data.AlphaMode = MaterialAlphaMode::Blend;
						break;
					default:
						data.AlphaMode = MaterialAlphaMode::Opaque;
						break;
				}
				data.AlphaCutoff = material->alpha_cutoff;
				data.DoubleSided = material->double_sided != 0;
				if (material->unlit)
				{
					m_Builder.Warn("unlit materials (KHR_materials_unlit) are imported as lit materials");
				}

				uint32_t const slot = m_Builder.AddMaterial(std::move(imported));
				m_MaterialSlots.emplace(material, slot);
				return slot;
			}

			// Imports the image of a texture slot. The base color slot also provides the material's UV transform.
			int32_t GetTexture(cgltf_texture_view const& view, MaterialData& data, bool isBaseColor)
			{
				if (view.texture == nullptr)
				{
					return -1;
				}
				if (view.texcoord != 0)
				{
					m_Builder.Warn("only TEXCOORD_0 is supported; textures using other UV sets use TEXCOORD_0");
				}
				if (view.has_transform)
				{
					if (isBaseColor)
					{
						data.UVOffset = glm::make_vec2(view.transform.offset);
						data.UVTiling = glm::make_vec2(view.transform.scale);
					}
					if (view.transform.rotation != 0.0f)
					{
						m_Builder.Warn("texture rotation (KHR_texture_transform) is not supported");
					}
				}

				cgltf_image const* image = view.texture->image;
				if (image == nullptr)
				{
					m_Builder.Warn("a texture uses an unsupported image format (only PNG, JPEG and other stb_image formats are supported)");
					return -1;
				}
				if (auto const it = m_Textures.find(image); it != m_Textures.end())
				{
					return it->second;
				}

				Result<ImportedTexture> texture = LoadImage(*image);
				int32_t index = -1;
				if (texture)
				{
					index = m_Builder.AddTexture(std::move(texture.GetValue()));
				}
				else
				{
					m_Builder.Warn(texture.GetError());
				}
				m_Textures.emplace(image, index);
				return index;
			}

			Result<ImportedTexture> LoadImage(cgltf_image const& image)
			{
				size_t const imageIndex = static_cast<size_t>(&image - m_Data.images);
				ImportedTexture texture;
				texture.Name = image.name != nullptr ? image.name : fmt::format("Image{}", imageIndex);

				if (image.buffer_view != nullptr)
				{
					uint8_t const* bytes = cgltf_buffer_view_data(image.buffer_view);
					if (bytes == nullptr || image.buffer_view->size == 0)
					{
						return MakeError("image '{}' has no data", texture.Name);
					}
					texture.EncodedData = Buffer::Copy(bytes, image.buffer_view->size);
					return texture;
				}
				if (image.uri == nullptr)
				{
					return MakeError("image '{}' has no data", texture.Name);
				}

				std::string_view const uri = image.uri;
				if (uri.starts_with("data:"))
				{
					size_t const comma = uri.find(',');
					if (comma == std::string_view::npos || uri.substr(0, comma).find(";base64") == std::string_view::npos)
					{
						return MakeError("image '{}' uses an unsupported data URI", texture.Name);
					}
					Result<std::vector<uint8_t>> decoded = Base64Decode(uri.substr(comma + 1));
					if (!decoded || decoded.GetValue().empty())
					{
						return MakeError("image '{}' has an invalid data URI", texture.Name);
					}
					texture.EncodedData = Buffer::Copy(decoded.GetValue().data(), decoded.GetValue().size());
					return texture;
				}
				if (uri.find("://") != std::string_view::npos)
				{
					return MakeError("image '{}' references a remote URI, which is not supported", texture.Name);
				}

				std::string decodedUri(uri);
				decodedUri.resize(cgltf_decode_uri(decodedUri.data()));
				texture.FilePath = (m_Directory / FileSystem::PathFromUtf8(decodedUri)).lexically_normal();
				if (image.name == nullptr)
				{
					texture.Name = FileSystem::PathToUtf8(texture.FilePath.filename());
				}
				return texture;
			}

			std::filesystem::path m_Directory;
			cgltf_data const& m_Data;
			ModelBuilder& m_Builder;
			std::unordered_map<cgltf_material const*, uint32_t> m_MaterialSlots;
			std::unordered_map<cgltf_image const*, int32_t> m_Textures;
		};

		Result<void> ImportGltf(std::filesystem::path const& path, ModelBuilder& builder)
		{
			// The file data must outlive the parsed document: GLB binary chunks are referenced in place.
			Result<Buffer> file = FileSystem::ReadBinaryFile(path);
			if (!file)
			{
				return Error{file.GetError()};
			}

			cgltf_options options = {};
			options.memory.alloc_func = GltfAllocate;
			options.memory.free_func = GltfFree;
			options.file.read = GltfReadFile;
			options.file.release = GltfReleaseFile;

			cgltf_data* parsed = nullptr;
			cgltf_result result = cgltf_parse(&options, file.GetValue().GetData(), file.GetValue().GetSize(), &parsed);
			std::unique_ptr<cgltf_data, GltfDataDeleter> data(parsed);
			if (result != cgltf_result_success)
			{
				return Error{GltfResultToString(result)};
			}

			static constexpr std::string_view SupportedRequiredExtensions[] = {"KHR_mesh_quantization", "KHR_texture_transform",
			                                                                   "KHR_materials_emissive_strength"};
			std::string unsupported;
			for (size_t i = 0; i < data->extensions_required_count; i++)
			{
				std::string_view const extension = data->extensions_required[i];
				if (std::find(std::begin(SupportedRequiredExtensions), std::end(SupportedRequiredExtensions), extension) ==
				    std::end(SupportedRequiredExtensions))
				{
					unsupported += unsupported.empty() ? "" : ", ";
					unsupported += extension;
				}
			}
			if (!unsupported.empty())
			{
				return MakeError("the file requires unsupported glTF extensions: {}", unsupported);
			}

			std::string const pathUtf8 = FileSystem::PathToUtf8(path);
			result = cgltf_load_buffers(&options, data.get(), pathUtf8.c_str());
			if (result != cgltf_result_success)
			{
				return MakeError("failed to load buffers: {}", GltfResultToString(result));
			}
			if (Result<void> ranges = CheckGltfRanges(*data); !ranges)
			{
				return MakeError("validation failed: {}", ranges.GetError());
			}
			result = cgltf_validate(data.get());
			if (result != cgltf_result_success)
			{
				// cgltf reports every range violation (accessors, buffer views, indices) as "data too short".
				return MakeError("validation failed: {}", result == cgltf_result_data_too_short
				                                              ? "an accessor, buffer view or index refers to data out of range"
				                                              : GltfResultToString(result));
			}

			GltfImporter importer(path, *data, builder);
			return importer.Import();
		}

		// --- FBX and OBJ (ufbx) ----------------------------------------------------------------------------------------

		struct UfbxSceneDeleter
		{
			void operator()(ufbx_scene* scene) const { ufbx_free_scene(scene); }
		};

		std::string ToString(ufbx_string const& text)
		{
			return std::string(text.data, text.length);
		}

		glm::vec3 ToGlm(ufbx_vec3 const& value)
		{
			return {static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z)};
		}

		glm::mat4 ToGlm(ufbx_matrix const& matrix)
		{
			glm::mat4 result(1.0f);
			for (int column = 0; column < 4; column++)
			{
				result[column] = glm::vec4(ToGlm(matrix.cols[column]), column == 3 ? 1.0f : 0.0f);
			}
			return result;
		}

		float GetReal(ufbx_material_map const& map, float fallback)
		{
			return map.has_value ? static_cast<float>(map.value_real) : fallback;
		}

		class UfbxImporter
		{
		public:
			UfbxImporter(std::filesystem::path const& path, ufbx_scene const& scene, ModelBuilder& builder)
				: m_Directory(path.parent_path()),
				  m_Scene(scene),
				  m_Builder(builder)
			{
			}

			Result<void> Import()
			{
				// Every face corner becomes a vertex before welding, so the triangles bound what the instances build.
				uint64_t triangles = 0;
				for (size_t i = 0; i < m_Scene.nodes.count; i++)
				{
					ufbx_node const* node = m_Scene.nodes.data[i];
					if (node->mesh != nullptr && !AddWithinLimit(triangles, node->mesh->num_triangles, MeshImporter::MaxTriangles))
					{
						return MakeTooLargeError();
					}
				}

				for (size_t i = 0; i < m_Scene.nodes.count; i++)
				{
					ufbx_node const* node = m_Scene.nodes.data[i];
					if (node->mesh == nullptr)
					{
						continue;
					}
					if (Result<void> result = ImportNode(*node); !result)
					{
						return result;
					}
				}
				return {};
			}

		private:
			Result<void> ImportNode(ufbx_node const& node)
			{
				ufbx_mesh const& mesh = *node.mesh;
				GeometryTransform const transform(ToGlm(node.geometry_to_world));
				std::string const nodeName = node.name.length > 0 ? ToString(node.name) : "Mesh";
				if (!transform.IsValid())
				{
					m_Builder.Warn(fmt::format("node '{}' has a degenerate transform and was skipped", nodeName));
					return {};
				}
				if (mesh.max_face_triangles == 0)
				{
					return {};
				}

				std::vector<uint32_t> triangleIndices(mesh.max_face_triangles * 3);
				auto const importFaces = [&](uint32_t const* faceIndices, size_t faceCount, uint32_t materialIndex) -> Result<void>
				{
					ufbx_material const* material = nullptr;
					if (materialIndex < node.materials.count)
					{
						material = node.materials.data[materialIndex];
					}
					else if (materialIndex < mesh.materials.count)
					{
						material = mesh.materials.data[materialIndex];
					}

					std::vector<Vertex> vertices;
					std::vector<uint32_t> indices;
					for (size_t f = 0; f < faceCount; f++)
					{
						size_t const faceIndex = faceIndices != nullptr ? faceIndices[f] : f;
						ufbx_face const face = mesh.faces.data[faceIndex];
						if (face.num_indices < 3)
						{
							continue;
						}
						uint32_t const triangles = ufbx_triangulate_face(triangleIndices.data(), triangleIndices.size(), &mesh, face);
						for (uint32_t k = 0; k < triangles * 3; k++)
						{
							uint32_t const corner = triangleIndices[k];
							Vertex vertex;
							vertex.Position = ToGlm(ufbx_get_vertex_vec3(&mesh.vertex_position, corner));
							if (mesh.vertex_normal.exists)
							{
								vertex.Normal = ToGlm(ufbx_get_vertex_vec3(&mesh.vertex_normal, corner));
							}
							if (mesh.vertex_uv.exists)
							{
								ufbx_vec2 const uv = ufbx_get_vertex_vec2(&mesh.vertex_uv, corner);
								// FBX and OBJ put the UV origin at the bottom-left.
								vertex.TexCoord = glm::vec2(static_cast<float>(uv.x), 1.0f - static_cast<float>(uv.y));
							}
							indices.push_back(static_cast<uint32_t>(vertices.size()));
							vertices.push_back(vertex);
						}
					}
					if (indices.empty())
					{
						return {};
					}

					uint32_t const slot = GetMaterialSlot(material);
					std::string const name = material != nullptr && material->name.length > 0
					                             ? fmt::format("{}#{}", nodeName, ToString(material->name))
					                             : nodeName;
					return m_Builder.AddSubmesh(name, std::move(vertices), std::move(indices), mesh.vertex_normal.exists, false, slot,
					                            transform);
				};

				if (mesh.material_parts.count == 0)
				{
					return importFaces(nullptr, mesh.num_faces, 0);
				}
				for (size_t p = 0; p < mesh.material_parts.count; p++)
				{
					ufbx_mesh_part const& part = mesh.material_parts.data[p];
					if (part.num_triangles == 0)
					{
						continue;
					}
					if (Result<void> result = importFaces(part.face_indices.data, part.face_indices.count, part.index); !result)
					{
						return result;
					}
				}
				return {};
			}

			uint32_t GetMaterialSlot(ufbx_material const* material)
			{
				if (material == nullptr)
				{
					return m_Builder.GetDefaultMaterialSlot();
				}
				if (auto const it = m_MaterialSlots.find(material); it != m_MaterialSlots.end())
				{
					return it->second;
				}

				ImportedMaterial imported;
				imported.Name = material->name.length > 0 ? ToString(material->name) : "Material";
				MaterialData& data = imported.Data;
				ufbx_material_pbr_maps const& pbr = material->pbr;

				glm::vec4 baseColor(1.0f);
				if (pbr.base_color.has_value)
				{
					baseColor = glm::vec4(static_cast<float>(pbr.base_color.value_vec4.x), static_cast<float>(pbr.base_color.value_vec4.y),
					                      static_cast<float>(pbr.base_color.value_vec4.z),
					                      pbr.base_color.value_components >= 4 ? static_cast<float>(pbr.base_color.value_vec4.w) : 1.0f);
				}
				float const baseFactor = GetReal(pbr.base_factor, 1.0f);
				baseColor = glm::vec4(glm::vec3(baseColor) * baseFactor, baseColor.a * GetReal(pbr.opacity, 1.0f));
				data.BaseColor = glm::clamp(baseColor, glm::vec4(0.0f), glm::vec4(std::numeric_limits<float>::max()));
				if (data.BaseColor.a < 1.0f)
				{
					data.AlphaMode = MaterialAlphaMode::Blend;
				}
				data.Roughness = std::clamp(GetReal(pbr.roughness, 0.5f), 0.0f, 1.0f);
				data.Metallic = std::clamp(GetReal(pbr.metalness, 0.0f), 0.0f, 1.0f);
				if (pbr.emission_color.has_value)
				{
					data.EmissiveColor = ToGlm(pbr.emission_color.value_vec3);
					data.EmissiveIntensity = GetReal(pbr.emission_factor, 1.0f);
				}
				data.DoubleSided = material->features.double_sided.enabled;

				imported.BaseColorTexture = GetTexture(pbr.base_color.texture);
				imported.NormalTexture = GetTexture(pbr.normal_map.texture);
				imported.OcclusionTexture = GetTexture(pbr.ambient_occlusion.texture);
				imported.EmissiveTexture = GetTexture(pbr.emission_color.texture);
				if (pbr.roughness.texture != nullptr && pbr.roughness.texture == pbr.metalness.texture)
				{
					imported.MetallicRoughnessTexture = GetTexture(pbr.roughness.texture);
				}
				else if (pbr.roughness.texture != nullptr || pbr.metalness.texture != nullptr)
				{
					m_Builder.Warn("separate roughness/metalness textures are not supported (use a packed metallic-roughness texture)");
				}

				uint32_t const slot = m_Builder.AddMaterial(std::move(imported));
				m_MaterialSlots.emplace(material, slot);
				return slot;
			}

			int32_t GetTexture(ufbx_texture const* texture)
			{
				if (texture == nullptr)
				{
					return -1;
				}
				if (texture->type == UFBX_TEXTURE_LAYERED && texture->file_textures.count > 0)
				{
					m_Builder.Warn("layered textures are not supported; the first layer is used");
					texture = texture->file_textures.data[0];
				}
				if (texture->type != UFBX_TEXTURE_FILE)
				{
					m_Builder.Warn("procedural and shader textures are not supported");
					return -1;
				}
				if (auto const it = m_Textures.find(texture); it != m_Textures.end())
				{
					return it->second;
				}

				ImportedTexture imported;
				imported.Name = texture->name.length > 0 ? ToString(texture->name) : "Texture";
				int32_t index = -1;
				if (texture->content.size > 0)
				{
					imported.EncodedData = Buffer::Copy(texture->content.data, texture->content.size);
					index = m_Builder.AddTexture(std::move(imported));
				}
				else if (std::optional<std::filesystem::path> const file = FindTextureFile(*texture))
				{
					imported.FilePath = *file;
					index = m_Builder.AddTexture(std::move(imported));
				}
				else
				{
					m_Builder.Warn(fmt::format("texture file '{}' was not found", ToString(texture->relative_filename)));
				}
				m_Textures.emplace(texture, index);
				return index;
			}

			// Exporters store absolute paths of the authoring machine; fall back to the model's directory.
			std::optional<std::filesystem::path> FindTextureFile(ufbx_texture const& texture) const
			{
				std::vector<std::filesystem::path> candidates;
				for (ufbx_string const* name : {&texture.filename, &texture.relative_filename, &texture.absolute_filename})
				{
					if (name->length == 0)
					{
						continue;
					}
					std::filesystem::path const path = FileSystem::PathFromUtf8(ToString(*name));
					candidates.push_back(path.is_absolute() ? path : m_Directory / path);
					candidates.push_back(m_Directory / path.filename());
				}
				for (std::filesystem::path const& candidate : candidates)
				{
					std::filesystem::path const normalized = candidate.lexically_normal();
					if (FileSystem::IsRegularFile(normalized))
					{
						return normalized;
					}
				}
				return std::nullopt;
			}

			std::filesystem::path m_Directory;
			ufbx_scene const& m_Scene;
			ModelBuilder& m_Builder;
			std::unordered_map<ufbx_material const*, uint32_t> m_MaterialSlots;
			std::unordered_map<ufbx_texture const*, int32_t> m_Textures;
		};

		// Opens OBJ material libraries through FileSystem (UTF-8 paths); nothing else (the model is loaded from memory, and
		// FBX geometry caches and other external references are never followed).
		bool OpenUfbxFile(void*, ufbx_stream* stream, char const* path, size_t pathLength, ufbx_open_file_info const* info)
		{
			if (info->type != UFBX_OPEN_FILE_OBJ_MTL)
			{
				return false;
			}
			Result<Buffer> file = FileSystem::ReadBinaryFile(FileSystem::PathFromUtf8(std::string_view(path, pathLength)));
			if (!file)
			{
				return false;
			}
			// The memory stream copies the data, so the buffer may be released afterwards. An empty file has no data pointer,
			// which ufbx does not accept even for zero bytes.
			static constexpr uint8_t EmptyFile = 0;
			Buffer const& data = file.GetValue();
			void const* bytes = data.GetSize() > 0 ? data.GetData() : &EmptyFile;
			ufbx_open_memory_opts memoryOptions = {};
			ufbx_error error = {};
			return ufbx_open_memory(stream, bytes, data.GetSize(), &memoryOptions, &error);
		}

		Result<void> ImportUfbx(std::filesystem::path const& path, bool isObj, ModelBuilder& builder)
		{
			Result<Buffer> file = FileSystem::ReadBinaryFile(path);
			if (!file)
			{
				return Error{file.GetError()};
			}

			ufbx_load_opts options = {};
			// ufbx needs a few times a model's size (up to 23 times for the stress models of its own test suite); the limit
			// bounds what a damaged file can make it allocate.
			constexpr uint64_t BaseMemoryLimit = 256ull << 20;
			constexpr uint64_t MemoryLimitPerFileByte = 64;
			size_t const memoryLimit = static_cast<size_t>(BaseMemoryLimit + file.GetValue().GetSize() * MemoryLimitPerFileByte);
			options.temp_allocator.memory_limit = memoryLimit;
			options.result_allocator.memory_limit = memoryLimit;
			options.target_axes = ufbx_axes_right_handed_y_up;
			options.target_unit_meters = 1.0f;
			options.generate_missing_normals = true;
			options.obj_search_mtl_by_filename = true;
			options.ignore_animation = true;
			options.open_file_cb.fn = OpenUfbxFile;
			// OBJ materials live in external .mtl files; a missing library only produces a warning.
			options.load_external_files = isObj;
			options.ignore_missing_external_files = true;

			// Material libraries resolve relative to the model.
			std::string const pathUtf8 = FileSystem::PathToUtf8(path);
			options.filename = ufbx_string{pathUtf8.data(), pathUtf8.size()};
			ufbx_error error = {};
			std::unique_ptr<ufbx_scene, UfbxSceneDeleter> scene(
				ufbx_load_memory(file.GetValue().GetData(), file.GetValue().GetSize(), &options, &error));
			if (scene == nullptr)
			{
				return Error{error.description.length > 0 ? ToString(error.description) : "failed to load the file"};
			}
			for (size_t i = 0; i < scene->metadata.warnings.count; i++)
			{
				builder.Warn(ToString(scene->metadata.warnings.data[i].description));
			}

			UfbxImporter importer(path, *scene, builder);
			return importer.Import();
		}
	}

	bool MeshImporter::IsSupportedExtension(std::string_view extension)
	{
		std::string const lower = ToLower(extension);
		return lower == ".gltf" || lower == ".glb" || lower == ".fbx" || lower == ".obj";
	}

	Result<ImportedModel> MeshImporter::Import(std::filesystem::path const& path)
	{
		std::string const extension = ToLower(FileSystem::PathToUtf8(path.extension()));
		ModelBuilder builder;
		Result<void> result;
		if (extension == ".gltf" || extension == ".glb")
		{
			result = ImportGltf(path, builder);
		}
		else if (extension == ".fbx" || extension == ".obj")
		{
			result = ImportUfbx(path, extension == ".obj", builder);
		}
		else
		{
			return MakeError("'{}': unsupported model format", FileSystem::PathToUtf8(path));
		}

		if (!result)
		{
			return MakeError("'{}': {}", FileSystem::PathToUtf8(path), result.GetError());
		}
		ImportedModel& model = builder.GetModel();
		if (model.Submeshes.empty())
		{
			return MakeError("'{}' contains no triangle meshes", FileSystem::PathToUtf8(path));
		}
		return std::move(model);
	}
}
