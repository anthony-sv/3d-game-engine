using System;
using System.Globalization;
using System.Text;

namespace Strada.ScriptCore.Tests;

// The grid graph of the performance guards (spec-slice4 section 9.0): the C# port of the engine tests' Testing::MakeGridGraph
// (Tests/StradaTests/Source/Script/VisualScriptGridGraph.h). Both are pinned to one document, Tests/Data/VisualScripting/
// Benchmarks/Grid-4x5.json (MakeGridGraph(4, 5, 2, 1)).
internal static class GridDocuments
{
	private readonly record struct Kind(string Type, string[] Inputs, string[] Outputs);

	// Five node kinds take turns by ID, with stored literals that make value chips.
	private static readonly Kind[] s_Kinds =
	[
		new("Flow.Branch", ["Execute", "Condition"], ["True", "False"]),
		new("Call:Strada.Log.Info(string)", ["Execute", "Message"], ["Then"]),
		new("Operator.Add", ["A", "B"], ["Result"]),
		new("Flow.Delay", ["Execute", "Duration"], ["Completed"]),
		new("Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,Strada.ForceMode)", ["Execute", "Target", "Force", "Mode"], ["Then"]),
	];

	// columns × rows nodes on a 320 × 180 grid, each with linksPerNode links to the nodes of the next column (the last column
	// links back to the one before), and comment boxes around blocks of 2 × 2 nodes. Node IDs start at 1, comment IDs follow
	// them. The content form, compact, as the engine writes it.
	internal static string Make(uint columns, uint rows, uint linksPerNode, uint comments)
	{
		uint Id(uint column, uint row) => 1 + (column * rows) + row;

		StringBuilder text = new("""{"Description":"","Variables":[],"Functions":[],"Graph":{"Nodes":[""");
		for (uint column = 0; column < columns; column++)
		{
			for (uint row = 0; row < rows; row++)
			{
				uint id = Id(column, row);
				Kind kind = s_Kinds[id % s_Kinds.Length];
				if (id > 1)
				{
					text.Append(',');
				}
				text.Append(Invariant($$"""{"ID":{{id}},"Type":"{{kind.Type}}","Position":[{{Number(column * 320.0)}},{{Number(row * 180.0)}}]"""));
				string? inputs = (id % s_Kinds.Length) switch
				{
					1 => """{"Message":"Hello"}""",
					2 => """{"B":1}""",
					3 => """{"Duration":0.5}""",
					4 => """{"Force":[1,0,0]}""",
					_ => null,
				};
				if (inputs != null)
				{
					text.Append(""","Inputs":""").Append(inputs);
				}
				text.Append('}');
			}
		}
		text.Append("""],"Links":[""");
		bool first = true;
		for (uint column = 0; column < columns && columns > 1; column++)
		{
			uint next = column + 1 < columns ? column + 1 : column - 1;
			for (uint row = 0; row < rows; row++)
			{
				uint id = Id(column, row);
				Kind kind = s_Kinds[id % s_Kinds.Length];
				for (uint link = 0; link < linksPerNode; link++)
				{
					uint target = Id(next, (row + link) % rows);
					Kind targetKind = s_Kinds[target % s_Kinds.Length];
					string from = kind.Outputs[link % kind.Outputs.Length];
					string to = targetKind.Inputs[(link + id) % targetKind.Inputs.Length];
					text.Append(first ? string.Empty : ",").Append(Invariant($$"""{"From":[{{id}},"{{from}}"],"To":[{{target}},"{{to}}"]}"""));
					first = false;
				}
			}
		}
		text.Append("""],"Comments":[""");
		uint blocksPerRow = Math.Max(columns / 2, 1u);
		for (uint index = 0; index < comments; index++)
		{
			uint blockColumn = index % blocksPerRow * 2;
			uint blockRow = index / blocksPerRow * 2;
			text.Append(index == 0 ? string.Empty : ",").Append(Invariant(
				$$"""{"ID":{{Id(columns, 0) + index}},"Text":"Block {{index}}","Position":[{{Number((blockColumn * 320.0) - 32.0)}},{{Number((blockRow * 180.0) - 72.0)}}],"Size":[584.0,400.0],"Color":[0.5,0.5,0.5,0.25]}"""));
		}
		return text.Append("]}}").ToString();
	}

	internal static byte[] MakeUtf8(uint columns, uint rows, uint linksPerNode, uint comments) =>
		Encoding.UTF8.GetBytes(Make(columns, rows, linksPerNode, comments));

	// A whole number as the engine writes a float that holds one ("320.0").
	private static string Number(double value) => value.ToString("0.0", CultureInfo.InvariantCulture);

	private static string Invariant(FormattableString text) => FormattableString.Invariant(text);
}
