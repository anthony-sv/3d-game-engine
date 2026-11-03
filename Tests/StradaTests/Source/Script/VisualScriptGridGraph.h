#pragma once

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// The grid graph of the performance guards, shared by the engine's and the editor's tests. Tests/Data/VisualScripting/Benchmarks/
// Grid-4x5.json pins its form (MakeGridGraph(4, 5, 2, 1)), and the scripting runtime's tests port it to C# against that file.
namespace Strada::Testing
{
	// A large graph for the performance guards: columns × rows nodes of the editor's fixture library (all of them nodes of the
	// scripting runtime's library too) on a 320 × 180 grid, each with linksPerNode links to the nodes of the next column (the
	// last column links back to the one before), and comment boxes around blocks of 2 × 2 nodes. Five node kinds take turns, with stored
	// literals that make value chips: Branch, Log Info (a string), Add (a number, so it keeps its full shape), Delay (a float) and Add
	// Force (a vector and an enum). Every link names pins of the library's templates. Node IDs start at 1, comment IDs follow them.
	inline VisualScriptDocument MakeGridGraph(uint32_t columns, uint32_t rows, uint32_t linksPerNode, uint32_t comments)
	{
		struct Kind
		{
			std::string_view Type;
			std::array<std::string_view, 4> Inputs;
			size_t InputCount;
			std::array<std::string_view, 2> Outputs;
			size_t OutputCount;
		};
		static constexpr std::array<Kind, 5> Kinds = {{
			{"Flow.Branch", {"Execute", "Condition"}, 2, {"True", "False"}, 2},
			{"Call:Strada.Log.Info(string)", {"Execute", "Message"}, 2, {"Then"}, 1},
			{"Operator.Add", {"A", "B"}, 2, {"Result"}, 1},
			{"Flow.Delay", {"Execute", "Duration"}, 2, {"Completed"}, 1},
			{"Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,Strada.ForceMode)",
		     {"Execute", "Target", "Force", "Mode"},
		     4,
		     {"Then"},
		     1},
		}};
		auto const getID = [rows](uint32_t column, uint32_t row)
		{
			return 1 + column * rows + row;
		};
		VisualScriptDocument document;
		document.Graph.Nodes.reserve(static_cast<size_t>(columns) * rows);
		for (uint32_t column = 0; column < columns; column++)
		{
			for (uint32_t row = 0; row < rows; row++)
			{
				uint32_t const id = getID(column, row);
				Kind const& kind = Kinds[id % Kinds.size()];
				VisualScriptNode& node = document.Graph.Nodes.emplace_back();
				node.ID = id;
				node.Type = std::string(kind.Type);
				node.Position = glm::vec2(static_cast<float>(column) * 320.0f, static_cast<float>(row) * 180.0f);
				switch (id % Kinds.size())
				{
					case 1:
						node.Inputs["Message"] = "Hello";
						break;
					case 2:
						node.Inputs["B"] = 1;
						break;
					case 3:
						node.Inputs["Duration"] = 0.5;
						break;
					case 4:
						node.Inputs["Force"] = Json::array({1, 0, 0});
						break;
					default:
						break;
				}
			}
		}
		document.Graph.Links.reserve(static_cast<size_t>(columns) * rows * linksPerNode);
		for (uint32_t column = 0; column < columns && columns > 1; column++)
		{
			uint32_t const next = column + 1 < columns ? column + 1 : column - 1;
			for (uint32_t row = 0; row < rows; row++)
			{
				uint32_t const id = getID(column, row);
				Kind const& kind = Kinds[id % Kinds.size()];
				for (uint32_t link = 0; link < linksPerNode; link++)
				{
					uint32_t const target = getID(next, (row + link) % rows);
					Kind const& targetKind = Kinds[target % Kinds.size()];
					std::string_view const from = kind.Outputs[link % kind.OutputCount];
					std::string_view const to = targetKind.Inputs[(link + id) % targetKind.InputCount];
					document.Graph.Links.push_back({{id, std::string(from)}, {target, std::string(to)}});
				}
			}
		}
		uint32_t const blocksPerRow = std::max(columns / 2, 1u);
		for (uint32_t index = 0; index < comments; index++)
		{
			VisualScriptComment& comment = document.Graph.Comments.emplace_back();
			comment.ID = getID(columns, 0) + index;
			comment.Text = "Block " + std::to_string(index);
			uint32_t const blockColumn = (index % blocksPerRow) * 2;
			uint32_t const blockRow = (index / blocksPerRow) * 2;
			comment.Position = glm::vec2(static_cast<float>(blockColumn) * 320.0f - 32.0f, static_cast<float>(blockRow) * 180.0f - 72.0f);
			comment.Size = glm::vec2(584.0f, 400.0f);
		}
		return document;
	}
}
