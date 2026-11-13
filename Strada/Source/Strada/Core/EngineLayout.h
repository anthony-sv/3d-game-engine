#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Strada
{
	// The kinds of files that come with the engine besides its executables.
	enum class EngineContent : uint8_t
	{
		// The project templates (Templates/<id>/template.json).
		Templates = 0,
		// The example projects and their catalog.
		Examples,
		// The guides and references.
		Docs,
		// What a project set up for coding agents is made from.
		AgentKit,
		// The game-making skill agents use (its folder, holding SKILL.md).
		MakeGameSkill
	};

	// Where the files that come with the engine lie, seen from one of its executables. A build tree holds the marker
	// engine.json beside its executables (CMake writes it), naming the source checkout the content is read from
	// ("a source tree"); without one, the content lies in the resources directory:
	//
	//   EngineContent   source tree (C = the checkout)          otherwise (R = the resources directory)
	//   Templates       C/Templates                             R/Templates
	//   Examples        C/Projects                              R/Examples
	//   Docs            C/Docs                                  R/Docs
	//   AgentKit        C/Agents                                R/Agents
	//   MakeGameSkill   C/.claude/skills/strada-make-game       R/Agents/skills/strada-make-game
	//
	// A default-constructed layout is empty: every directory and path it gives is empty (environments that name no
	// folder, such as tests' defaults). A layout is a value: copies are independent and every function is thread-safe.
	class EngineLayout
	{
	public:
		// engine.json's file type and the newest version this engine reads.
		static constexpr char const* ManifestFileName = "engine.json";
		static constexpr char const* ManifestType = "EngineManifest";
		static constexpr int ManifestVersion = 1;
		// A larger engine.json is damaged: the marker is a few lines.
		static constexpr uint64_t MaxManifestBytes = 64 * 1024;

		EngineLayout() = default;

		// Pure: the layout of an executable at this path, read from the files beside it (the executable itself need not
		// exist). Fails only for an empty path. A relative path gives relative directories (resolved by whoever uses them).
		// What is wrong with engine.json (unreadable, not JSON, another type or a newer version, a Content that is not an
		// absolute path to an existing directory) makes the layout not a source tree, with one warning.
		[[nodiscard]] static Result<EngineLayout> Resolve(std::filesystem::path const& executablePath);
		// This process's layout, resolved on first use from FileSystem::GetExecutablePath() and immutable afterwards: any
		// thread may read it. Empty, with a warning, when the executable's path is unknown.
		static EngineLayout const& Get();

		std::filesystem::path const& GetExecutableDirectory() const { return m_ExecutableDirectory; }
		// The executable's folder; Contents/Resources in a macOS bundle (an executable in Contents/MacOS beside which
		// Contents/Info.plist exists).
		std::filesystem::path const& GetResourcesDirectory() const { return m_ResourcesDirectory; }
		// True when engine.json names a source checkout (a build tree).
		bool IsSourceTree() const { return !m_SourceDirectory.empty(); }
		// The checkout engine.json names; empty when this is not a source tree.
		std::filesystem::path const& GetSourceDirectory() const { return m_SourceDirectory; }
		// Where this kind of content lies (see the table above); the path need not exist. Empty for an empty layout.
		std::filesystem::path GetContentPath(EngineContent content) const;
		// The catalog of the examples: Projects/catalog.json in a source tree, Examples/catalog.json otherwise. Empty for an
		// empty layout.
		std::filesystem::path GetExampleCatalogPath() const;
		// strada (strada.exe on Windows) beside the executable; the file need not exist. Empty for an empty layout.
		std::filesystem::path GetToolPath() const;
		// What resolving found wrong (a damaged engine.json); the application logs them after Log::Init.
		std::vector<std::string> const& GetWarnings() const { return m_Warnings; }

	private:
		std::filesystem::path m_ExecutableDirectory;
		std::filesystem::path m_ResourcesDirectory;
		std::filesystem::path m_SourceDirectory;
		std::vector<std::string> m_Warnings;
	};
}
