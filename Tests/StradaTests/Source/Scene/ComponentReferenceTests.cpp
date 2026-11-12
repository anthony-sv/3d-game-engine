#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Platform.h"
#include "Strada/Scene/ComponentRegistry.h"

#include <doctest/doctest.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

// Docs/Components.md is generated from the component registry, the same descriptions the inspector, scene files and the
// automation API's component.types use, so the reference cannot drift from the engine: this test writes the document
// and fails when the committed one differs. STRADA_UPDATE_DOCS=1 writes it instead.
namespace
{
	constexpr std::string_view TestName = "Components: Docs/Components.md is the registry's reference of every component and field";

	std::string Join(std::vector<std::string> const& parts, std::string_view separator)
	{
		std::string joined;
		for (size_t index = 0; index < parts.size(); index++)
		{
			if (index > 0)
			{
				joined += separator;
			}
			joined += parts[index];
		}
		return joined;
	}

	// What each category of the Add Component menu holds, for the reference's overview.
	std::string_view DescribeCategory(std::string_view category)
	{
		if (category == ComponentCategory::Core)
		{
			return "Every entity has these: its name and its place in the world.";
		}
		if (category == ComponentCategory::Rendering)
		{
			return "What the renderer draws: cameras, meshes, lights, the sky, text, sprites, particles and post-processing.";
		}
		if (category == ComponentCategory::Physics)
		{
			return "Jolt physics: bodies, their colliders, joints between bodies and character controllers.";
		}
		if (category == ComponentCategory::Audio)
		{
			return "Sound sources, positioned in 3D or not, and the listener that hears them.";
		}
		if (category == ComponentCategory::Scripting)
		{
			return "Behavior: C# scripts and visual scripts that run on the entity.";
		}
		return "Screen-space interfaces: canvases, rectangles laid out by anchors, images, text and buttons.";
	}

	// Floats are stored as float, so their shortest float spelling is the value the editor shows (0.05, not 0.0500000007).
	std::string FormatNumber(Json const& value)
	{
		if (value.is_number_integer())
		{
			return fmt::format("{}", value.get<int64_t>());
		}
		return fmt::format("{}", static_cast<float>(value.get<double>()));
	}

	std::string FormatValue(Json const& value)
	{
		if (value.is_boolean())
		{
			return value.get<bool>() ? "true" : "false";
		}
		if (value.is_number())
		{
			return FormatNumber(value);
		}
		if (value.is_string())
		{
			return value.get<std::string>();
		}
		if (value.is_array())
		{
			std::vector<std::string> parts;
			for (Json const& element : value)
			{
				parts.push_back(FormatValue(element));
			}
			return fmt::format("({})", Join(parts, ", "));
		}
		return value.dump();
	}

	bool IsEnum(Json const& field)
	{
		return field.contains("Values");
	}

	std::string FormatType(Json const& field)
	{
		std::string const type = field["Type"].get<std::string>();
		std::string const display = field.value("Display", "");
		if (IsEnum(field))
		{
			return "enum";
		}
		if (type == "asset")
		{
			return fmt::format("{} asset", field.value("AssetType", ""));
		}
		if (type == "asset[]")
		{
			return fmt::format("{} assets", field.value("AssetType", ""));
		}
		if (display == "Color")
		{
			return type == "vec4" ? "color (RGBA)" : "color (RGB)";
		}
		if (display == "Angle")
		{
			return fmt::format("{} (degrees)", type);
		}
		if (display == "MultilineText")
		{
			return "string (multiline)";
		}
		if (type == "quat")
		{
			return "rotation";
		}
		if (type == "bvec3")
		{
			return "bool per axis";
		}
		if (type == "uuid")
		{
			return "entity";
		}
		if (type == "scriptFields")
		{
			return "field values";
		}
		return type;
	}

	std::string FormatDefault(Json const& field)
	{
		std::string const type = field["Type"].get<std::string>();
		Json const& value = field["Default"];
		bool const reference = type == "asset" || type == "uuid";
		if ((reference && value == "0") || ((value.is_array() || value.is_object()) && value.empty()))
		{
			return "none";
		}
		if (value.is_string() && value.get<std::string>().empty())
		{
			return "empty";
		}
		if (value.is_string() && !IsEnum(field))
		{
			return fmt::format("`\"{}\"`", value.get<std::string>());
		}
		return fmt::format("`{}`", FormatValue(value));
	}

	std::string FormatRange(Json const& field)
	{
		bool const hasMin = field.contains("Min");
		bool const hasMax = field.contains("Max");
		if (hasMin && hasMax)
		{
			return fmt::format("{} to {}", FormatNumber(field["Min"]), FormatNumber(field["Max"]));
		}
		if (hasMin)
		{
			return fmt::format("at least {}", FormatNumber(field["Min"]));
		}
		if (hasMax)
		{
			return fmt::format("at most {}", FormatNumber(field["Max"]));
		}
		return "";
	}

	std::string Quote(std::string_view value)
	{
		return fmt::format("`{}`", value);
	}

	// The field's description, then what the inspector does with it: the enum's values, when the field shows, what
	// switches it on.
	std::string FormatDescription(Json const& field)
	{
		std::vector<std::string> sentences;
		if (field.contains("Description"))
		{
			sentences.push_back(field["Description"].get<std::string>());
		}
		if (IsEnum(field))
		{
			std::vector<std::string> values;
			for (Json const& value : field["Values"])
			{
				values.push_back(Quote(value.get<std::string>()));
			}
			sentences.push_back(fmt::format("One of {}.", Join(values, ", ")));
		}
		if (field.contains("VisibleWhen"))
		{
			Json const& condition = field["VisibleWhen"];
			std::string const switchName = condition["Field"].get<std::string>();
			std::vector<std::string> values;
			for (Json const& value : condition["Values"])
			{
				std::string const text = value.get<std::string>();
				values.push_back(text == "true" ? "on" : text == "false" ? "off" : Quote(text));
			}
			sentences.push_back(fmt::format("Shown when {} is {}.", Quote(switchName), Join(values, " or ")));
		}
		if (field.contains("EnabledBy"))
		{
			sentences.push_back(fmt::format("Applies while {} is on.", Quote(field["EnabledBy"].get<std::string>())));
		}
		if (field.value("HiddenInEditors", false))
		{
			sentences.push_back("The inspector edits it with controls of its own.");
		}
		std::string description = Join(sentences, " ");
		// A table cell ends at a bar.
		size_t position = 0;
		while ((position = description.find('|', position)) != std::string::npos)
		{
			description.replace(position, 1, "\\|");
			position += 2;
		}
		return description;
	}

	void WriteFields(std::string& document, Json const& fields, std::string const& prefix)
	{
		for (Json const& field : fields)
		{
			std::string const name = prefix + field["Name"].get<std::string>();
			document += fmt::format("| `{}` | {} | {} | {} | {} |\n", name, FormatType(field), FormatDefault(field), FormatRange(field),
			                        FormatDescription(field));
			if (field.contains("Fields"))
			{
				WriteFields(document, field["Fields"], name + ".");
			}
		}
	}

	size_t CountFields(Json const& fields)
	{
		size_t count = 0;
		for (Json const& field : fields)
		{
			count += 1 + (field.contains("Fields") ? CountFields(field["Fields"]) : 0);
		}
		return count;
	}

	std::string WriteComponentReference()
	{
		std::vector<ComponentInfo const*> components;
		size_t fieldCount = 0;
		for (ComponentInfo const& info : ComponentRegistry::GetComponents())
		{
			if (!info.IsInternal())
			{
				components.push_back(&info);
				fieldCount += CountFields(info.Describe()["Fields"]);
			}
		}

		std::string document;
		document += "# Component reference\n\n";
		document +=
			"<!-- Generated from the component registry by StradaTests (Tests/StradaTests/Source/Scene/ComponentReferenceTests.cpp):\n";
		document += "     edit the components' descriptions in the engine, then run StradaTests with STRADA_UPDATE_DOCS=1. -->\n\n";
		document += "Every component you can add to an entity, with every field: its type, its default, the values it accepts and what\n";
		document +=
			"it does. The inspector, scene files, the automation command `component.types` and the scripting API all read the same\n";
		document += "registry, so this page matches the engine: the engine's tests fail when it does not.\n\n";
		document += fmt::format("{} components in {} categories, with {} fields in all. Vectors are written `(x, y, z)`, colors are\n",
		                        components.size(), ComponentCategory::All.size(), fieldCount);
		document += "linear RGB or RGBA from 0 to 1, distances are in meters and angles in degrees. Fields that only matter for some\n";
		document += "settings say when the inspector shows them. Scripts reach the same fields through the component classes of\n";
		document += "[ScriptingAPI.md](ScriptingAPI.md), and agents through `component.get` and `component.set`\n";
		document += "([Automation.md](Automation.md)).\n\n";

		document += "| Category | Components |\n|---|---|\n";
		for (std::string_view const category : ComponentCategory::All)
		{
			std::vector<std::string> links;
			for (ComponentInfo const* info : components)
			{
				if (info->Category == category)
				{
					std::string anchor(info->Name);
					std::transform(anchor.begin(), anchor.end(), anchor.begin(),
					               [](char character)
					               {
									   return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
								   });
					links.push_back(fmt::format("[{}](#{})", info->Name, anchor));
				}
			}
			document += fmt::format("| {} | {} |\n", category, Join(links, ", "));
		}

		for (std::string_view const category : ComponentCategory::All)
		{
			document += fmt::format("\n## {}\n\n{}\n", category, DescribeCategory(category));
			for (ComponentInfo const* info : components)
			{
				if (info->Category != category)
				{
					continue;
				}
				Json const description = info->Describe();
				document += fmt::format("\n### {}\n\n", info->Name);
				if (description.contains("Description"))
				{
					document += description["Description"].get<std::string>() + "\n\n";
				}
				if (info->IsCore())
				{
					document += "Every entity has it; it cannot be removed.\n\n";
				}
				document += "| Field | Type | Default | Range | Description |\n|---|---|---|---|---|\n";
				WriteFields(document, description["Fields"], "");
			}
		}
		return document;
	}
}

TEST_CASE("Components: Docs/Components.md is the registry's reference of every component and field")
{
	std::string const expected = WriteComponentReference();
	std::filesystem::path const path = FileSystem::PathFromUtf8(STRADA_COMPONENT_REFERENCE_PATH);
	if (Platform::ReadEnvironmentVariable("STRADA_UPDATE_DOCS").value_or("") == "1")
	{
		REQUIRE(FileSystem::WriteTextFile(path, expected).IsOk());
		MESSAGE("Updated " << FileSystem::PathToUtf8(path));
		return;
	}
	Result<std::string> document = FileSystem::ReadTextFile(path);
	REQUIRE_MESSAGE(document.IsOk(), "Docs/Components.md is missing: run StradaTests -tc=\"", TestName, "\" with STRADA_UPDATE_DOCS=1");
	std::string text = document.TakeValue();
	std::erase(text, '\r');
	CHECK_MESSAGE(text == expected, "Docs/Components.md does not match the component registry: run StradaTests -tc=\"", TestName,
	              "\" with STRADA_UPDATE_DOCS=1 and commit the result");
}
