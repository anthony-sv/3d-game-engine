using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Text;
using System.Xml;
using System.Xml.Linq;

namespace Strada.VisualScripting;

// The summaries of the XML documentation file of the scripting API (Strada.ScriptCore.xml next to the assembly), which
// become node descriptions (spec section 4.5): the summary text with <see cref> rendered as the member's name and
// <paramref>, <typeparamref>, <c> and <see langword> as their text. Without the file (exported games do not ship it) every
// description is empty.
internal sealed class Documentation
{
	private readonly Dictionary<string, string> m_Summaries;

	private Documentation(Dictionary<string, string> summaries)
	{
		m_Summaries = summaries;
	}

	internal static Documentation Empty { get; } = new(new Dictionary<string, string>(StringComparer.Ordinal));

	// The documentation in the file; empty when it is missing or cannot be read.
	internal static Documentation Load(string? path)
	{
		if (string.IsNullOrEmpty(path) || !File.Exists(path))
		{
			return Empty;
		}
		try
		{
			XmlReaderSettings settings = new() { DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null, IgnoreComments = true };
			using XmlReader reader = XmlReader.Create(path, settings);
			XDocument document = XDocument.Load(reader);
			Dictionary<string, string> summaries = new(StringComparer.Ordinal);
			XElement? members = document.Root?.Element("members");
			if (members == null)
			{
				return Empty;
			}
			foreach (XElement member in members.Elements("member"))
			{
				string? name = member.Attribute("name")?.Value;
				XElement? summary = member.Element("summary");
				if (name == null || summary == null)
				{
					continue;
				}
				string text = Render(summary);
				if (text.Length > 0)
				{
					summaries.TryAdd(name, text);
				}
			}
			return new Documentation(summaries);
		}
		catch (Exception exception) when (exception is XmlException or IOException or UnauthorizedAccessException)
		{
			return Empty;
		}
	}

	internal string Of(MemberInfo member) => m_Summaries.TryGetValue(Id(member), out string? summary) ? summary : string.Empty;

	// The documentation ID of a type, method, constructor, property or field (the "name" attributes of the file).
	internal static string Id(MemberInfo member) => member switch
	{
		Type type => "T:" + TypeName(type),
		ConstructorInfo constructor => "M:" + TypeName(constructor.DeclaringType!) + ".#ctor" + Parameters(constructor),
		MethodInfo method => "M:" + TypeName(method.DeclaringType!) + "." + method.Name +
			(method.IsGenericMethodDefinition ? "``" + method.GetGenericArguments().Length.ToString(System.Globalization.CultureInfo.InvariantCulture) : "") +
			Parameters(method),
		PropertyInfo property => "P:" + TypeName(property.DeclaringType!) + "." + property.Name,
		FieldInfo field => "F:" + TypeName(field.DeclaringType!) + "." + field.Name,
		_ => string.Empty,
	};

	private static string Parameters(MethodBase method)
	{
		ParameterInfo[] parameters = method.GetParameters();
		if (parameters.Length == 0)
		{
			return string.Empty;
		}
		StringBuilder text = new("(");
		for (int index = 0; index < parameters.Length; index++)
		{
			if (index > 0)
			{
				text.Append(',');
			}
			text.Append(ParameterTypeName(parameters[index].ParameterType));
		}
		return text.Append(')').ToString();
	}

	private static string ParameterTypeName(Type type)
	{
		if (type.IsByRef)
		{
			return ParameterTypeName(type.GetElementType()!) + "@";
		}
		if (type.IsSZArray)
		{
			return ParameterTypeName(type.GetElementType()!) + "[]";
		}
		if (type.IsGenericMethodParameter)
		{
			return "``" + type.GenericParameterPosition.ToString(System.Globalization.CultureInfo.InvariantCulture);
		}
		if (type.IsGenericTypeParameter)
		{
			return "`" + type.GenericParameterPosition.ToString(System.Globalization.CultureInfo.InvariantCulture);
		}
		return TypeName(type);
	}

	private static string TypeName(Type type) => (type.FullName ?? type.Name).Replace('+', '.');

	private static string Render(XElement summary)
	{
		StringBuilder text = new();
		Append(text, summary);
		return Collapse(text);
	}

	private static void Append(StringBuilder text, XElement element)
	{
		foreach (XNode node in element.Nodes())
		{
			switch (node)
			{
				case XText value:
					text.Append(value.Value);
					break;
				case XElement child when child.Name.LocalName is "see" or "seealso":
					text.Append(child.Attribute("cref") is { } cref ? CrefName(cref.Value)
						: child.Attribute("langword")?.Value ?? child.Value);
					break;
				case XElement child when child.Name.LocalName is "paramref" or "typeparamref":
					text.Append(child.Attribute("name")?.Value);
					break;
				case XElement child:
					text.Append(' ');
					Append(text, child);
					text.Append(' ');
					break;
			}
		}
	}

	// "M:Strada.Entity.GetComponent``1" → "GetComponent", "M:Strada.Vector3.#ctor(System.Single)" → "Vector3".
	private static string CrefName(string cref)
	{
		string name = cref.Length > 2 && cref[1] == ':' ? cref[2..] : cref;
		int parameters = name.IndexOf('(', StringComparison.Ordinal);
		if (parameters >= 0)
		{
			name = name[..parameters];
		}
		int arity = name.IndexOf('`', StringComparison.Ordinal);
		if (arity >= 0)
		{
			name = name[..arity];
		}
		string[] segments = name.Split('.');
		string last = segments[^1];
		return last == "#ctor" && segments.Length > 1 ? segments[^2] : last;
	}

	private static string Collapse(StringBuilder text)
	{
		StringBuilder collapsed = new(text.Length);
		bool space = false;
		for (int index = 0; index < text.Length; index++)
		{
			char character = text[index];
			if (char.IsWhiteSpace(character))
			{
				space = collapsed.Length > 0;
				continue;
			}
			if (space && !(character is '.' or ',' or ';' or ':' or ')'))
			{
				collapsed.Append(' ');
			}
			space = false;
			collapsed.Append(character);
		}
		return collapsed.ToString();
	}
}
