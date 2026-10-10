using System;
using System.Collections.Generic;
using System.Globalization;

namespace Strada.Tool;

/// <summary>A mistake on the command line: reported with the usage, exit code 2.</summary>
internal sealed class UsageException : Exception
{
	public UsageException(string message)
		: base(message)
	{
	}
}

/// <summary>strada's command line: the command, its operands and the options (<c>--name value</c>, <c>--name=value</c>;
/// everything after <c>--</c> is an operand).</summary>
internal sealed class ToolArguments
{
	private readonly HashSet<string> m_Options = new(StringComparer.Ordinal);

	public string? Command { get; private set; }
	public List<string> Operands { get; } = [];
	public string? Editor { get; private set; }
	public string? Project { get; private set; }
	public int? ProcessId { get; private set; }
	public bool Headless { get; private set; }
	public bool StartNew { get; private set; }
	public bool Json { get; private set; }
	public bool Help { get; private set; }
	public bool Version { get; private set; }

	/// <summary>Parses the arguments; throws <see cref="UsageException"/> for unknown options and missing or malformed
	/// values.</summary>
	public static ToolArguments Parse(IReadOnlyList<string> arguments)
	{
		ToolArguments parsed = new();
		for (int i = 0; i < arguments.Count; i++)
		{
			string argument = arguments[i];
			if (argument == "--")
			{
				for (i++; i < arguments.Count; i++)
				{
					parsed.AddOperand(arguments[i]);
				}
				break;
			}
			if (argument == "-h")
			{
				parsed.Help = true;
				continue;
			}
			if (!argument.StartsWith("--", StringComparison.Ordinal))
			{
				parsed.AddOperand(argument);
				continue;
			}

			int equals = argument.IndexOf('=');
			string name = equals < 0 ? argument : argument[..equals];
			string? inlineValue = equals < 0 ? null : argument[(equals + 1)..];
			if (!parsed.m_Options.Add(name))
			{
				throw new UsageException($"{name} is given twice");
			}
			switch (name)
			{
				case "--editor":
					parsed.Editor = TakeValue(arguments, ref i, name, inlineValue);
					break;
				case "--project":
					parsed.Project = TakeValue(arguments, ref i, name, inlineValue);
					break;
				case "--pid":
					parsed.ProcessId = ParseProcessId(TakeValue(arguments, ref i, name, inlineValue));
					break;
				case "--headless":
					parsed.Headless = TakeFlag(name, inlineValue);
					break;
				case "--new":
					parsed.StartNew = TakeFlag(name, inlineValue);
					break;
				case "--json":
					parsed.Json = TakeFlag(name, inlineValue);
					break;
				case "--help":
					parsed.Help = TakeFlag(name, inlineValue);
					break;
				case "--version":
					parsed.Version = TakeFlag(name, inlineValue);
					break;
				default:
					throw new UsageException($"unknown option {name}");
			}
		}
		return parsed;
	}

	/// <summary>Checks that the command got only the options it takes and a number of operands in the range.</summary>
	public void Require(IReadOnlySet<string> options, int minOperands, int maxOperands)
	{
		foreach (string option in m_Options)
		{
			if (!options.Contains(option))
			{
				throw new UsageException($"{Command} does not take {option}");
			}
		}
		if (Operands.Count < minOperands || Operands.Count > maxOperands)
		{
			throw new UsageException(maxOperands == 0 ? $"{Command} takes no operands" : $"{Command} takes {minOperands} to {maxOperands} operands");
		}
	}

	private void AddOperand(string operand)
	{
		if (Command is null)
		{
			Command = operand;
		}
		else
		{
			Operands.Add(operand);
		}
	}

	private static string TakeValue(IReadOnlyList<string> arguments, ref int index, string name, string? inlineValue)
	{
		if (inlineValue is not null)
		{
			return inlineValue.Length > 0 ? inlineValue : throw new UsageException($"{name} needs a value");
		}
		if (index + 1 >= arguments.Count)
		{
			throw new UsageException($"{name} needs a value");
		}
		index++;
		return arguments[index];
	}

	private static bool TakeFlag(string name, string? inlineValue) =>
		inlineValue is null ? true : throw new UsageException($"{name} takes no value");

	private static int ParseProcessId(string value) =>
		int.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out int processId) && processId > 0
			? processId
			: throw new UsageException($"--pid: '{value}' is not a process ID");
}
