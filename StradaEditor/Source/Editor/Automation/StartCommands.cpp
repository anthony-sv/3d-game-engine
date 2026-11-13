#include "Editor/Automation/StartCommands.h"

#include "Editor/Automation/JsonSchema.h"
#include "Editor/Automation/ProjectContentCommands.h"

#include "Strada/Core/FileSystem.h"

#include <memory>
#include <string>
#include <utility>

namespace Strada
{
	namespace
	{
		// What the commands share, held as long as a command is registered.
		struct StartCommandState
		{
			StartScreenModel& Model;
			StartCommandsHost Host;
		};

		Json DescribeOptionalText(std::optional<std::string> const& text)
		{
			return text ? Json(*text) : Json();
		}

		Json DescribeRecentList(std::vector<RecentProjectInfo> const& projects)
		{
			Json described = Json::array();
			for (RecentProjectInfo const& project : projects)
			{
				described.push_back(DescribeRecentProjectInfo(project));
			}
			return described;
		}

		Json DescribeStartScreen(StartScreenModel const& model)
		{
			Json templates = Json::array();
			for (ProjectTemplate const& projectTemplate : model.GetTemplates().Templates)
			{
				templates.push_back(DescribeProjectTemplate(projectTemplate));
			}
			Json examples = Json::array();
			if (std::optional<ExampleCatalog> const& catalog = model.GetExamples())
			{
				for (ExampleEntry const& example : catalog->GetExamples())
				{
					examples.push_back(DescribeExample(*catalog, example));
				}
			}
			Json docs = Json::array();
			for (StartScreenGuide const& guide : model.GetGuides())
			{
				docs.push_back(Json::object({{"title", guide.Title}, {"path", FileSystem::PathToUtf8(guide.File)}}));
			}
			StartScreenEnvironment const& environment = model.GetEnvironment();
			std::filesystem::path const& defaultLocation = model.GetLocations().DefaultProjectLocation;
			return Json::object(
				{{"visible", model.IsVisible()},
			     {"defaultLocation", defaultLocation.empty() ? Json() : Json(FileSystem::PathToUtf8(defaultLocation))},
			     {"recent", DescribeRecentList(model.GetRecent())},
			     {"templates", std::move(templates)},
			     {"examples", std::move(examples)},
			     {"docs", std::move(docs)},
			     {"environment",
			      Json::object({{"dotnetRuntime", DescribeOptionalText(environment.DotNetRuntime)},
			                    {"dotnetSdk", environment.DotNetSdk ? Json(FileSystem::PathToUtf8(*environment.DotNetSdk)) : Json()},
			                    {"gpu", DescribeOptionalText(environment.Gpu)},
			                    {"vulkan", DescribeOptionalText(environment.Vulkan)}})}});
		}
	}

	std::string_view GetEditorDialogName(EditorDialog dialog)
	{
		switch (dialog)
		{
			case EditorDialog::NewProject:
				return "new-project";
			case EditorDialog::AgentSetup:
				return "agent-setup";
		}
		return "new-project";
	}

	std::optional<EditorDialog> ParseEditorDialog(std::string_view name)
	{
		for (EditorDialog const dialog : {EditorDialog::NewProject, EditorDialog::AgentSetup})
		{
			if (GetEditorDialogName(dialog) == name)
			{
				return dialog;
			}
		}
		return std::nullopt;
	}

	Json DescribeRecentProjectInfo(RecentProjectInfo const& project)
	{
		return Json::object({{"name", project.Name},
		                     {"path", FileSystem::PathToUtf8(project.File)},
		                     {"state", std::string(GetRecentProjectStateName(project.State))}});
	}

	Result<void> RegisterStartCommands(CommandRegistry& registry, StartScreenModel& model, StartCommandsHost host)
	{
		auto const state = std::make_shared<StartCommandState>(StartCommandState{model, std::move(host)});

		CommandDefinition info;
		info.Name = "start.info";
		info.Description =
			"What the editor's start screen shows, read now: { visible (whether it shows; a headless editor never shows it), "
			"defaultLocation (the folder new projects go into by default, or null), recent: [{ name, path, state }] (the recent "
			"projects, most recent first; state Ok, Missing (the file is gone) or Unreadable (it exists and does not read as a "
			"project file)), templates and examples (as project.templates and project.examples list them), docs: [{ title, path "
			"}] (the guides it links to), environment: { dotnetRuntime (the .NET runtime scripting started with, or null when "
			"scripting is off), dotnetSdk (the .NET SDK's dotnet that builds C# scripts, or null), gpu, vulkan (the graphics "
			"adapter and its Vulkan version, or null) } }. Changes nothing.";
		info.ReadOnly = true;
		info.Handler = [state](Json const&) -> CommandResult
		{
			state->Model.Refresh();
			return DescribeStartScreen(state->Model);
		};
		Result<void> result = registry.Register(std::move(info));

		CommandDefinition show;
		show.Name = "start.show";
		show.Description =
			"Shows the start screen over the editor's window (visible true) or hides it (false), as File > Start Screen and "
			"Continue without a project do. While it shows, the scene behind it still loads and the commands still act on it. "
			"Opening or creating a project hides it. Returns { visible }. Not an undo step; Unavailable when the editor has no "
			"window (headless), which never shows it.";
		show.Parameters = SchemaBuilder::Object()
		                      .Property("visible", SchemaBuilder::Boolean("Show the start screen (true) or hide it (false)"), true)
		                      .Build();
		show.Handler = [state](Json const& params) -> CommandResult
		{
			if (!state->Host.ShowStartScreen)
			{
				return MakeCommandError(AutomationErrorCode::Unavailable, "the editor has no window (headless): it shows no start screen");
			}
			bool const visible = params["visible"].get<bool>();
			state->Host.ShowStartScreen(visible);
			return Json::object({{"visible", state->Model.IsVisible()}});
		};
		result = result ? registry.Register(std::move(show)) : result;

		CommandDefinition recent;
		recent.Name = "editor.recent";
		recent.Description =
			"The recent projects the start screen and File > Recent Projects list, most recent first, read now: { projects: [{ "
			"name, path, state (Ok, Missing: the file is gone, or Unreadable: it does not read as a project file) }] }. remove "
			"takes a project file off the list (any spelling of its path; one that is not listed changes nothing) and clear "
			"empties it; the list is saved in the editor's user data. Not an undo step. InvalidParams when both are given; "
			"FileError when the list cannot be saved.";
		recent.Parameters = SchemaBuilder::Object()
		                        .Property("remove", SchemaBuilder::String("A project file to take off the list").MinLength(1))
		                        .Property("clear", SchemaBuilder::Boolean("Empty the list").Default(false))
		                        .Build();
		recent.Handler = [state](Json const& params) -> CommandResult
		{
			bool const removes = params.contains("remove");
			if (removes && params.contains("clear"))
			{
				CommandError error = MakeCommandError(AutomationErrorCode::InvalidParams, "give either remove or clear, not both");
				error.Data = Json::object({{"path", "clear"}});
				return error;
			}
			StartScreenModel& startScreen = state->Model;
			Result<void> saved;
			if (removes)
			{
				saved = startScreen.RemoveRecentProject(FileSystem::PathFromUtf8(params["remove"].get<std::string>()));
			}
			else if (params.value("clear", false))
			{
				saved = startScreen.ClearRecentProjects();
			}
			if (!saved)
			{
				return MakeCommandError(AutomationErrorCode::FileError, "the recent projects cannot be saved: {}", saved.GetError());
			}
			return Json::object({{"projects", DescribeRecentList(startScreen.DescribeRecentProjects())}});
		};
		result = result ? registry.Register(std::move(recent)) : result;

		CommandDefinition dialog;
		dialog.Name = "editor.dialog";
		dialog.Description =
			"Opens a dialog of the File menu as if the user chose its item, so that it can be looked at (viewport.screenshot), or "
			"closes it as its Cancel does (open false): new-project (File > New Project..., the New Project form: templates, "
			"name, location, the setup for coding agents, the .gitignore) or agent-setup (File > Set Up for Coding Agents..., "
			"what setting the open project up for coding agents would write). It opens at the next frame and changes nothing "
			"else: project.create and project.setup-agents do the work. Returns { name, open }. Not an undo step. "
			"InvalidOperation for agent-setup without an open project and while another dialog is open; Unavailable when the "
			"editor has no window (headless).";
		dialog.Parameters =
			SchemaBuilder::Object()
				.Property("name", SchemaBuilder::String("The dialog: new-project or agent-setup").Enum({"new-project", "agent-setup"}),
		                  true)
				.Property("open", SchemaBuilder::Boolean("False closes the dialog").Default(true))
				.Build();
		dialog.Handler = [state](Json const& params) -> CommandResult
		{
			if (!state->Host.ShowDialog)
			{
				return MakeCommandError(AutomationErrorCode::Unavailable, "the editor has no window (headless): it shows no dialogs");
			}
			std::string const name = params["name"].get<std::string>();
			std::optional<EditorDialog> const which = ParseEditorDialog(name);
			if (!which)
			{
				CommandError error = MakeCommandError(AutomationErrorCode::InvalidParams, "there is no dialog '{}'", name);
				error.Data = Json::object({{"path", "name"}});
				return error;
			}
			bool const open = params.value("open", true);
			if (Result<void> shown = state->Host.ShowDialog(*which, open); !shown)
			{
				return MakeCommandError(AutomationErrorCode::InvalidOperation, "{}", shown.GetError());
			}
			return Json::object({{"name", name}, {"open", open}});
		};
		return result ? registry.Register(std::move(dialog)) : result;
	}
}
