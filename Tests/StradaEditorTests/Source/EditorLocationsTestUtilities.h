#pragma once

#include "Editor/EditorLocations.h"

#include "Strada/Core/EngineLayout.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Result.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

namespace Strada::Testing
{
	// The engine content of the editor's tests: Tests/Data/FrontDoor/Content, laid out as an engine that is not a build tree.
	inline std::filesystem::path GetTestContentDirectory()
	{
		return FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR "/FrontDoor/Content").lexically_normal();
	}

	// Locations for an editor under test: the test content as the engine content, and the user data and the default project
	// location inside temporaryRoot (a Testing::TemporaryDirectory's path), so that nothing a test does reaches the user's
	// folders. Creates nothing: "<temporaryRoot>/UserData/Editor" and "<temporaryRoot>/Projects" appear only when written.
	inline EditorLocations MakeEditorLocations(std::filesystem::path const& temporaryRoot)
	{
		// The layout of an editor in the content's folder (the executable need not exist).
		Result<EngineLayout> layout = EngineLayout::Resolve(GetTestContentDirectory() / "StradaEditor");
		REQUIRE_MESSAGE(layout.IsOk(), (layout ? std::string() : layout.GetError()));
		EditorLocations locations;
		locations.Layout = layout.TakeValue();
		locations.UserDataDirectory = temporaryRoot / "UserData" / "Editor";
		locations.DefaultProjectLocation = temporaryRoot / "Projects";
		return locations;
	}
}
