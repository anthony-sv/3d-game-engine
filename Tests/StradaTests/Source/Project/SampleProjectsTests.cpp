#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/MeshImporter.h"
#include "Strada/Asset/MeshSource.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/GameData.h"
#include "Strada/Project/Project.h"
#include "Strada/Project/ProjectCopy.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace Strada;

// The documents that name the sample projects (AGENTS.md's repository map, README.md's layout and the visual scripting
// guide's table of Prism Break's graphs) name exactly what the repository holds, Moto's collision meshes import as the
// physics needs them, its bike has the proportions the vehicle and the rider build on, its identity names its game, and a
// copy of it weighs under 20 MB.
namespace
{
	std::string ReadDocument(char const* path)
	{
		Result<std::string> text = FileSystem::ReadTextFile(FileSystem::PathFromUtf8(path));
		REQUIRE_MESSAGE(text.IsOk(), path);
		std::string content = text.TakeValue();
		std::erase(content, '\r');
		return content;
	}

	// The names of the entries of a directory that are directories (folders) or regular files with the extension.
	std::set<std::string> ListEntries(std::filesystem::path const& directory, bool folders, std::string_view extension = {})
	{
		std::set<std::string> names;
		std::error_code error;
		for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(directory, error))
		{
			bool const matches = folders ? entry.is_directory(error)
			                             : entry.is_regular_file(error) && FileSystem::PathToUtf8(entry.path().extension()) == extension;
			if (matches)
			{
				names.insert(FileSystem::PathToUtf8(entry.path().filename()));
			}
		}
		REQUIRE_MESSAGE(!error, FileSystem::PathToUtf8(directory), ": ", error.message());
		return names;
	}

	// The lines of the document that start with the prefix, in order.
	std::vector<std::string_view> FindLines(std::string_view document, std::string_view prefix)
	{
		std::vector<std::string_view> lines;
		size_t start = 0;
		while (start < document.size())
		{
			size_t const end = std::min(document.find('\n', start), document.size());
			std::string_view const line = document.substr(start, end - start);
			if (line.starts_with(prefix))
			{
				lines.push_back(line);
			}
			start = end + 1;
		}
		return lines;
	}

	// The first cells of the rows of the first table after the heading whose header starts with the header given, each
	// without its backquotes.
	std::vector<std::string> ReadFirstColumn(std::string_view document, std::string_view heading, std::string_view header)
	{
		size_t const section = document.find("\n" + std::string(heading) + "\n");
		REQUIRE_MESSAGE(section != std::string_view::npos, "no heading ", std::string(heading));
		size_t const table = document.find("\n" + std::string(header), section);
		REQUIRE_MESSAGE(table != std::string_view::npos, "no table ", std::string(header), " after ", std::string(heading));
		std::vector<std::string> cells;
		// The header and its separator row come first.
		size_t start = document.find('\n', document.find('\n', table + 1) + 1) + 1;
		while (start < document.size() && document[start] == '|')
		{
			size_t const end = std::min(document.find('\n', start), document.size());
			std::string_view const row = document.substr(start, end - start);
			size_t const close = row.find('|', 1);
			REQUIRE(close != std::string_view::npos);
			std::string cell(row.substr(1, close - 1));
			std::erase(cell, '`');
			std::erase(cell, ' ');
			cells.push_back(std::move(cell));
			start = end + 1;
		}
		return cells;
	}
}

TEST_CASE("Project: AGENTS.md's repository map and README.md's layout name every folder of Projects")
{
	std::set<std::string> const projects = ListEntries(FileSystem::PathFromUtf8(STRADA_SAMPLE_PROJECTS_DIR), true);
	REQUIRE_FALSE(projects.empty());
	std::string const agents = ReadDocument(STRADA_AGENTS_PATH);
	std::string const readme = ReadDocument(STRADA_README_PATH);
	// README.md's layout names the projects in its row of Projects/.
	std::vector<std::string_view> const layout = FindLines(readme, "| `Projects/` |");
	REQUIRE(layout.size() == 1);
	for (std::string const& project : projects)
	{
		CAPTURE(project);
		CHECK_MESSAGE(FindLines(agents, "| `Projects/" + project + "/` |").size() == 1,
		              "AGENTS.md's repository map has no row of the project");
		CHECK_MESSAGE(layout.front().find("`" + project + "`") != std::string_view::npos, "README.md's layout does not name the project");
	}
	// And the map has no row of a project that is not there.
	constexpr std::string_view RowStart = "| `Projects/";
	for (std::string_view const row : FindLines(agents, RowStart))
	{
		CAPTURE(row);
		std::string_view const path = row.substr(RowStart.size());
		size_t const end = path.find('/');
		REQUIRE(end != std::string_view::npos);
		CHECK(projects.contains(std::string(path.substr(0, end))));
	}
}

TEST_CASE("Project: the visual scripting guide's table of Prism Break's graphs names every graph of the project and nothing else")
{
	std::set<std::string> const graphs =
		ListEntries(FileSystem::PathFromUtf8(STRADA_SAMPLE_PROJECTS_DIR) / "PrismBreak" / "Assets" / "Scripts", false, ".sgraph");
	REQUIRE(graphs.size() == 13);
	std::string const guide = ReadDocument(STRADA_VISUAL_SCRIPTING_GUIDE_PATH);
	std::vector<std::string> const named = ReadFirstColumn(guide, "## A complete game: Prism Break", "| Graph |");
	std::set<std::string> const listed(named.begin(), named.end());
	CHECK_MESSAGE(listed.size() == named.size(), "the table names a graph twice");
	CHECK(listed == graphs);
}

TEST_CASE("Project: Moto's collision meshes import welded, a submesh per surface, one vertex wherever its triangles meet")
{
	// Jolt finds the edges between a mesh's triangles by their shared vertex indices: two vertices at one place would make
	// the edge between their triangles a bump a wheel hits. The generator writes each surface of the collision meshes
	// welded (normals all up, no texture coordinates), and the importer must keep them so; where two surfaces meet, the
	// physics shares their edges (it builds the shape over the mesh's distinct positions). The track's submeshes are its
	// surfaces in the order the scene gives them their physics materials (Tools/GenerateAssets.py, SURFACES).
	std::filesystem::path const meshes = FileSystem::PathFromUtf8(STRADA_SAMPLE_PROJECTS_DIR) / "Moto" / "Assets" / "Meshes";
	for (auto const& [name, surfaces] : {std::pair<char const*, size_t>{"TrackCollision.gltf", 4}, {"BarrierCollision.gltf", 1}})
	{
		CAPTURE(name);
		Result<ImportedModel> imported = MeshImporter::Import(meshes / name);
		REQUIRE_MESSAGE(imported.IsOk(), (imported ? std::string() : imported.GetError()));
		ImportedModel const& model = imported.GetValue();
		CHECK(model.Warnings.empty());
		REQUIRE(model.Submeshes.size() == surfaces);
		REQUIRE(model.Vertices.size() > 1000);
		for (Submesh const& submesh : model.Submeshes)
		{
			CAPTURE(submesh.Name);
			CHECK(submesh.IndexCount >= 3);
			std::set<std::array<float, 3>> positions;
			for (uint32_t index = 0; index < submesh.VertexCount; index++)
			{
				glm::vec3 const& position = model.Vertices[submesh.BaseVertex + index].Position;
				positions.insert({position.x, position.y, position.z});
			}
			CHECK(positions.size() == submesh.VertexCount);
		}
	}
}

TEST_CASE("Project: Moto's bike stands on both tyres with a 1,485 mm wheelbase, a 24 degree rake and a 98 mm trail")
{
	// The bike and its rider are five meshes under the Bike entity (Projects/Moto/Tools/Bike.py), which the ghost, the
	// vehicle and the rider's lean build on: each wheel's entity on its axle, the fork's at the foot of the steering axis
	// with its Y up the axis, the bike's frame with its origin on the ground between the contact patches and its front
	// along -Z. The test scene is the main scene without its props, with the same bike.
	Testing::TemporaryDirectory const directory;
	Testing::AssetManagerScope const manager;
	std::filesystem::path const assets = directory.GetPath() / "Assets";
	std::error_code error;
	// The registry's refresh may write; the test reads a copy of the sample.
	std::filesystem::copy(FileSystem::PathFromUtf8(STRADA_SAMPLE_PROJECTS_DIR) / "Moto" / "Assets", assets,
	                      std::filesystem::copy_options::recursive, error);
	REQUIRE_MESSAGE(!error, error.message());
	Result<AssetRefreshResult> opened = AssetManager::OpenAssetDirectory(assets);
	REQUIRE_MESSAGE(opened.IsOk(), (opened ? std::string() : opened.GetError()));
	Result<Ref<Scene>> loaded =
		SceneSerializer::LoadFromFile(assets / "Scenes" / "Tests.sscene", AssetManager::CreateDeserializationContext());
	REQUIRE_MESSAGE(loaded.IsOk(), (loaded ? std::string() : loaded.GetError()));
	Scene& scene = *loaded.GetValue();

	Entity const bike = scene.FindEntityByName("Bike");
	REQUIRE(bike);
	glm::mat4 const toBike = glm::inverse(scene.GetWorldTransform(bike));
	auto const local = [&scene, &toBike](Entity entity)
	{
		return toBike * scene.GetWorldTransform(entity);
	};
	for (char const* const name : {"Body", "Fork", "Front Wheel", "Rear Wheel", "Rider"})
	{
		CAPTURE(name);
		Entity const part = scene.FindEntityByName(name);
		REQUIRE(part);
		REQUIRE(part.HasComponent<MeshComponent>());
		Ref<MeshSource> const mesh = AssetManager::GetAsset<MeshSource>(part.GetComponent<MeshComponent>().Mesh);
		REQUIRE(mesh);
		CHECK_FALSE(mesh->GetSubmeshes().empty());
	}

	// The axles (each wheel's origin) and where each tyre touches the ground: its lowest vertex in the bike's frame.
	constexpr float Millimeter = 0.001f;
	auto const contact = [&scene, &local](char const* name)
	{
		Entity const wheel = scene.FindEntityByName(name);
		glm::mat4 const transform = local(wheel);
		glm::vec3 lowest(0.0f, std::numeric_limits<float>::max(), 0.0f);
		// Held while its vertices are read: a range-for keeps no temporary its range was reached through alive.
		Ref<MeshSource> const wheelMesh = AssetManager::GetAsset<MeshSource>(wheel.GetComponent<MeshComponent>().Mesh);
		REQUIRE(wheelMesh);
		for (Vertex const& vertex : wheelMesh->GetVertices())
		{
			glm::vec3 const point = glm::vec3(transform * glm::vec4(vertex.Position, 1.0f));
			lowest = point.y < lowest.y ? point : lowest;
		}
		return lowest;
	};
	glm::vec3 const front = contact("Front Wheel");
	glm::vec3 const rear = contact("Rear Wheel");
	CHECK(std::abs(front.y) <= Millimeter);
	CHECK(std::abs(rear.y) <= Millimeter);
	CHECK(std::abs((rear.z - front.z) - 1.485f) <= Millimeter);
	glm::vec3 const frontAxle = glm::vec3(local(scene.FindEntityByName("Front Wheel"))[3]);
	glm::vec3 const rearAxle = glm::vec3(local(scene.FindEntityByName("Rear Wheel"))[3]);
	CHECK(std::abs(frontAxle.z - front.z) <= Millimeter);
	CHECK(std::abs(rearAxle.z - rear.z) <= Millimeter);

	// The steering axis: the fork's Y, leaning back (towards +Z) by the rake, meeting the ground the trail ahead of the
	// front tyre's contact.
	glm::mat4 const fork = local(scene.FindEntityByName("Fork"));
	glm::vec3 const axis = glm::normalize(glm::vec3(fork[1]));
	glm::vec3 const foot = glm::vec3(fork[3]);
	CHECK(std::abs(glm::degrees(std::atan2(axis.z, axis.y)) - 24.0f) <= 0.05f);
	CHECK(std::abs(axis.x) <= 1e-5f);
	CHECK(std::abs(foot.y) <= Millimeter);
	CHECK(std::abs((front.z - foot.z) - 0.098f) <= Millimeter);
}

TEST_CASE("Project: Moto's identity names its game Strada Moto, whose data folder keeps that name when the project is renamed")
{
	// Projects/Moto/Tools/Author.py's identity part writes the identity into Moto.sproj: the product, and no company, so that
	// the exported game keeps its players' saves and settings in <per-user root>/Strada Moto whatever the project is called.
	// A company named after the engine would be kept apart from the engine's folder as "Strada_" (UserDataLocations).
	Result<std::string> text = FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_SAMPLE_PROJECTS_DIR) / "Moto" / "Moto.sproj");
	REQUIRE(text.IsOk());
	Result<Json> parsed = ParseJson(text.GetValue());
	REQUIRE(parsed.IsOk());
	Json document = parsed.TakeValue();
	CHECK(document["Project"]["Identity"]["Product"] == "Strada Moto");
	CHECK(document["Project"]["Identity"]["Company"] == "");
	document["Project"]["Name"] = "Renamed Moto";

	Testing::TemporaryDirectory directory;
	std::filesystem::path const file = directory.GetPath() / "Moto.sproj";
	REQUIRE(FileSystem::WriteTextFile(file, DumpJson(document)).IsOk());
	Result<GameIdentity> const identity = Project::ReadGameIdentity(file, ProjectFileKind::Project);
	REQUIRE_MESSAGE(identity.IsOk(), (identity ? std::string() : identity.GetError()));
	CHECK(identity.GetValue() == GameIdentity{"", "Strada Moto"});
	std::filesystem::path const root = directory.GetPath() / "Root";
	CHECK(UserDataLocations::MakeGameDirectory(root, identity.GetValue(), UserDataPlatform::Windows) == root / "Strada Moto");
	CHECK(UserDataLocations::MakeGameDirectory(root, identity.GetValue(), UserDataPlatform::MacOS) == root / "Strada Moto");
}

TEST_CASE("Project: a copy of Moto, everything but its tools, weighs under 20 MB")
{
	// What a copy holds by the project copy's rules (ProjectCopy: no Tools/, build output or caches), the files Moto plays
	// from, against 20 MB (20,000,000 bytes), the most a copy of it may weigh.
	constexpr uint64_t MaxBytes = 20'000'000;
	Testing::TemporaryDirectory directory;
	std::filesystem::path const copy = directory.GetPath() / "Moto";
	Result<ProjectCopyResult> const copied = ProjectCopy::Copy(FileSystem::PathFromUtf8(STRADA_SAMPLE_PROJECTS_DIR) / "Moto", copy);
	REQUIRE_MESSAGE(copied.IsOk(), (copied ? std::string() : copied.GetError()));
	CHECK(copied.GetValue().Warnings.empty());
	CHECK_FALSE(FileSystem::Exists(copy / "Tools"));
	CAPTURE(copied.GetValue().Bytes);
	CHECK(copied.GetValue().Bytes > 0);
	CHECK(copied.GetValue().Bytes < MaxBytes);
}
