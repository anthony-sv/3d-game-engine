#pragma once

#include "Strada/Core/Base64.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace Strada::Testing
{
	// Builds small glTF 2.0 files for importer tests.
	class GltfTestBuilder
	{
	public:
		struct Primitive
		{
			std::vector<glm::vec3> Positions;
			std::vector<glm::vec3> Normals;
			std::vector<glm::vec4> Tangents;
			std::vector<glm::vec2> TexCoords;
			// Empty: non-indexed.
			std::vector<uint32_t> Indices;
			int Material = -1;
			// 4 = triangles, 5 = triangle strip, 6 = triangle fan, 0 = points.
			int Mode = 4;
		};

		GltfTestBuilder()
		{
			m_Document["asset"] = Json::object({{"version", "2.0"}});
			for (char const* key : {"buffers", "bufferViews", "accessors", "meshes", "nodes", "materials", "textures", "images"})
			{
				m_Document[key] = Json::array();
			}
		}

		int AddMesh(std::string const& name, std::vector<Primitive> const& primitives)
		{
			Json primitivesJson = Json::array();
			for (Primitive const& primitive : primitives)
			{
				Json attributes = Json::object();
				attributes["POSITION"] = AddAccessor(primitive.Positions, true);
				if (!primitive.Normals.empty())
				{
					attributes["NORMAL"] = AddAccessor(primitive.Normals, false);
				}
				if (!primitive.Tangents.empty())
				{
					attributes["TANGENT"] = AddAccessor(primitive.Tangents);
				}
				if (!primitive.TexCoords.empty())
				{
					attributes["TEXCOORD_0"] = AddAccessor(primitive.TexCoords);
				}
				Json primitiveJson = Json::object();
				primitiveJson["attributes"] = std::move(attributes);
				if (!primitive.Indices.empty())
				{
					primitiveJson["indices"] = AddIndexAccessor(primitive.Indices);
				}
				if (primitive.Material >= 0)
				{
					primitiveJson["material"] = primitive.Material;
				}
				primitiveJson["mode"] = primitive.Mode;
				primitivesJson.push_back(std::move(primitiveJson));
			}
			m_Document["meshes"].push_back(Json::object({{"name", name}, {"primitives", std::move(primitivesJson)}}));
			return static_cast<int>(m_Document["meshes"].size() - 1);
		}

		int AddNode(std::string const& name, int mesh, glm::vec3 const& translation = glm::vec3(0.0f),
		            glm::quat const& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3 const& scale = glm::vec3(1.0f),
		            std::vector<int> const& children = {})
		{
			Json node = Json::object();
			node["name"] = name;
			if (mesh >= 0)
			{
				node["mesh"] = mesh;
			}
			node["translation"] = Json::array({translation.x, translation.y, translation.z});
			node["rotation"] = Json::array({rotation.x, rotation.y, rotation.z, rotation.w});
			node["scale"] = Json::array({scale.x, scale.y, scale.z});
			if (!children.empty())
			{
				node["children"] = children;
			}
			m_Document["nodes"].push_back(std::move(node));
			return static_cast<int>(m_Document["nodes"].size() - 1);
		}

		// An image stored in the binary buffer (GLB-style embedding).
		int AddEmbeddedImage(std::vector<uint8_t> const& png)
		{
			int const view = AddBufferView(png.data(), png.size());
			m_Document["images"].push_back(Json::object({{"bufferView", view}, {"mimeType", "image/png"}}));
			return static_cast<int>(m_Document["images"].size() - 1);
		}

		// An image referenced by URI (relative file path or data URI).
		int AddImageUri(std::string const& uri)
		{
			m_Document["images"].push_back(Json::object({{"uri", uri}}));
			return static_cast<int>(m_Document["images"].size() - 1);
		}

		int AddTexture(int image)
		{
			m_Document["textures"].push_back(Json::object({{"source", image}}));
			return static_cast<int>(m_Document["textures"].size() - 1);
		}

		int AddMaterial(Json material)
		{
			m_Document["materials"].push_back(std::move(material));
			return static_cast<int>(m_Document["materials"].size() - 1);
		}

		void SetScene(std::vector<int> const& rootNodes)
		{
			m_Document["scenes"] = Json::array({Json::object({{"nodes", rootNodes}})});
			m_Document["scene"] = 0;
		}

		Json& GetDocument() { return m_Document; }

		// .gltf with the buffer as a base64 data URI (embedBuffer) or as an external .bin next to it.
		Result<void> WriteGltf(std::filesystem::path const& path, bool embedBuffer)
		{
			Json document = m_Document;
			Json buffer = Json::object();
			buffer["byteLength"] = m_Buffer.size();
			if (embedBuffer)
			{
				buffer["uri"] = "data:application/octet-stream;base64," + Base64Encode(m_Buffer);
			}
			else
			{
				std::filesystem::path binPath = path;
				binPath.replace_extension(".bin");
				if (Result<void> written = FileSystem::WriteBinaryFile(binPath, m_Buffer); !written)
				{
					return written;
				}
				buffer["uri"] = FileSystem::PathToUtf8(binPath.filename());
			}
			document["buffers"] = Json::array({std::move(buffer)});
			return FileSystem::WriteTextFile(path, document.dump());
		}

		Result<void> WriteGlb(std::filesystem::path const& path)
		{
			Json document = m_Document;
			document["buffers"] = Json::array({Json::object({{"byteLength", m_Buffer.size()}})});
			std::string json = document.dump();
			while (json.size() % 4 != 0)
			{
				json.push_back(' ');
			}
			std::vector<uint8_t> binary = m_Buffer;
			while (binary.size() % 4 != 0)
			{
				binary.push_back(0);
			}

			std::vector<uint8_t> glb;
			auto const write32 = [&glb](uint32_t value)
			{
				for (int i = 0; i < 4; i++)
				{
					glb.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
				}
			};
			write32(0x46546C67); // "glTF"
			write32(2);
			write32(static_cast<uint32_t>(12 + 8 + json.size() + 8 + binary.size()));
			write32(static_cast<uint32_t>(json.size()));
			write32(0x4E4F534A); // "JSON"
			glb.insert(glb.end(), json.begin(), json.end());
			write32(static_cast<uint32_t>(binary.size()));
			write32(0x004E4942); // "BIN\0"
			glb.insert(glb.end(), binary.begin(), binary.end());
			return FileSystem::WriteBinaryFile(path, glb);
		}

	private:
		int AddBufferView(void const* data, size_t size)
		{
			while (m_Buffer.size() % 4 != 0)
			{
				m_Buffer.push_back(0);
			}
			size_t const offset = m_Buffer.size();
			m_Buffer.resize(offset + size);
			std::memcpy(m_Buffer.data() + offset, data, size);
			m_Document["bufferViews"].push_back(Json::object({{"buffer", 0}, {"byteOffset", offset}, {"byteLength", size}}));
			return static_cast<int>(m_Document["bufferViews"].size() - 1);
		}

		template<typename TVector>
		int AddAccessor(std::vector<TVector> const& values, bool withBounds = false)
		{
			constexpr int Components = TVector::length();
			int const view = AddBufferView(values.data(), values.size() * sizeof(TVector));
			Json accessor = Json::object();
			accessor["bufferView"] = view;
			accessor["componentType"] = 5126;
			accessor["count"] = values.size();
			accessor["type"] = Components == 2 ? "VEC2" : (Components == 3 ? "VEC3" : "VEC4");
			if (withBounds && !values.empty())
			{
				TVector minimum = values[0];
				TVector maximum = values[0];
				for (TVector const& value : values)
				{
					minimum = glm::min(minimum, value);
					maximum = glm::max(maximum, value);
				}
				Json minimumJson = Json::array();
				Json maximumJson = Json::array();
				for (int i = 0; i < Components; i++)
				{
					minimumJson.push_back(minimum[i]);
					maximumJson.push_back(maximum[i]);
				}
				accessor["min"] = std::move(minimumJson);
				accessor["max"] = std::move(maximumJson);
			}
			m_Document["accessors"].push_back(std::move(accessor));
			return static_cast<int>(m_Document["accessors"].size() - 1);
		}

		int AddIndexAccessor(std::vector<uint32_t> const& indices)
		{
			int const view = AddBufferView(indices.data(), indices.size() * sizeof(uint32_t));
			m_Document["accessors"].push_back(
				Json::object({{"bufferView", view}, {"componentType", 5125}, {"count", indices.size()}, {"type", "SCALAR"}}));
			return static_cast<int>(m_Document["accessors"].size() - 1);
		}

		Json m_Document = Json::object();
		std::vector<uint8_t> m_Buffer;
	};
}
