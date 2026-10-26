#include "stpch.h"
#include "Strada/Asset/VisualScriptAsset.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <utility>

namespace Strada
{
	namespace
	{
		constexpr std::array<std::pair<VisualScriptNodeEnabled, std::string_view>, 3> s_EnabledNames = {{
			{VisualScriptNodeEnabled::Enabled, "Enabled"},
			{VisualScriptNodeEnabled::Disabled, "Disabled"},
			{VisualScriptNodeEnabled::DevelopmentOnly, "DevelopmentOnly"},
		}};

		// Whether two values (of variables, locals, settings and inputs) are alike to the scripting runtime, which reads them
		// from the document's text: whether they are written alike. Json's operator== compares numbers by value instead: it
		// takes the integer 1 and the float 1.0 as equal, though the runtime reads the literals of integer types and the IDs
		// in settings from integers only ("1.0" is none), and it takes an unsigned integer above the largest signed one as
		// equal to the negative number of the same bits.
		bool IsSameValue(Json const& a, Json const& b)
		{
			if (a.is_number_integer() && b.is_number_integer())
			{
				// Signed and unsigned integers of one value are written as the same digits.
				if (a.is_number_unsigned() && b.is_number_unsigned())
				{
					return a.get<uint64_t>() == b.get<uint64_t>();
				}
				if (!a.is_number_unsigned() && !b.is_number_unsigned())
				{
					return a.get<int64_t>() == b.get<int64_t>();
				}
				int64_t const signedValue = (a.is_number_unsigned() ? b : a).get<int64_t>();
				uint64_t const unsignedValue = (a.is_number_unsigned() ? a : b).get<uint64_t>();
				return signedValue >= 0 && static_cast<uint64_t>(signedValue) == unsignedValue;
			}
			if (a.is_number_float() && b.is_number_float())
			{
				// Bit for bit: -0.0 and 0.0 are written and read as different numbers.
				return std::bit_cast<uint64_t>(a.get<double>()) == std::bit_cast<uint64_t>(b.get<double>());
			}
			if (a.is_array() && b.is_array())
			{
				return std::equal(a.begin(), a.end(), b.begin(), b.end(), IsSameValue);
			}
			if (a.is_object() && b.is_object())
			{
				// Members in order, as the writer writes them.
				if (a.size() != b.size())
				{
					return false;
				}
				for (auto first = a.begin(), second = b.begin(); first != a.end(); ++first, ++second)
				{
					if (first.key() != second.key() || !IsSameValue(first.value(), second.value()))
					{
						return false;
					}
				}
				return true;
			}
			// Strings, booleans and null by value; values of different kinds, such as an integer and a float, are never alike.
			return !a.is_number() && !b.is_number() && a == b;
		}

		// The comparisons below bind every member of the structs they compare, so a member added later does not compile here
		// until it is compared or named as one the scripting runtime never reads.

		bool HaveSameProgram(VisualScriptVariable const& a, VisualScriptVariable const& b)
		{
			// The tooltip and the slider range are for the inspector.
			[[maybe_unused]] auto const& [id, name, type, value, isPublic, tooltip, range, formerNames] = a;
			return id == b.ID && name == b.Name && type == b.Type && IsSameValue(value, b.Value) && isPublic == b.Public &&
			       formerNames == b.FormerNames;
		}

		bool HaveSameProgram(VisualScriptLocal const& a, VisualScriptLocal const& b)
		{
			auto const& [id, name, type, value] = a;
			return id == b.ID && name == b.Name && type == b.Type && IsSameValue(value, b.Value);
		}

		bool HaveSameProgram(VisualScriptNode const& a, VisualScriptNode const& b)
		{
			// Where the node sits, its note and whether only its header shows are for the editor.
			[[maybe_unused]] auto const& [id, type, position, settings, inputs, split, comment, collapsed, enabled] = a;
			return id == b.ID && type == b.Type && IsSameValue(settings, b.Settings) && IsSameValue(inputs, b.Inputs) && split == b.Split &&
			       enabled == b.Enabled;
		}

		bool HaveSameProgram(VisualScriptGraph const& a, VisualScriptGraph const& b)
		{
			// Comment boxes are for the editor.
			[[maybe_unused]] auto const& [nodes, links, comments] = a;
			return std::ranges::equal(nodes, b.Nodes,
			                          [](VisualScriptNode const& first, VisualScriptNode const& second)
			                          {
										  return HaveSameProgram(first, second);
									  }) &&
			       links == b.Links;
		}

		bool HaveSameProgram(VisualScriptFunction const& a, VisualScriptFunction const& b)
		{
			auto const& [id, name, pure, inputs, outputs, locals, graph] = a;
			return id == b.ID && name == b.Name && pure == b.Pure && inputs == b.Inputs && outputs == b.Outputs &&
			       std::ranges::equal(locals, b.Locals,
			                          [](VisualScriptLocal const& first, VisualScriptLocal const& second)
			                          {
										  return HaveSameProgram(first, second);
									  }) &&
			       HaveSameProgram(graph, b.Graph);
		}
	}

	std::string_view VisualScriptNodeEnabledToString(VisualScriptNodeEnabled enabled)
	{
		for (auto const& [value, name] : s_EnabledNames)
		{
			if (value == enabled)
			{
				return name;
			}
		}
		return {};
	}

	std::optional<VisualScriptNodeEnabled> VisualScriptNodeEnabledFromString(std::string_view text)
	{
		for (auto const& [value, name] : s_EnabledNames)
		{
			if (text == name)
			{
				return value;
			}
		}
		return std::nullopt;
	}

	bool HaveSameVisualScriptProgram(VisualScriptDocument const& a, VisualScriptDocument const& b)
	{
		// The description is for the asset inspector.
		[[maybe_unused]] auto const& [description, variables, functions, graph] = a;
		auto const sameVariables = [](VisualScriptVariable const& first, VisualScriptVariable const& second)
		{
			return HaveSameProgram(first, second);
		};
		auto const sameFunctions = [](VisualScriptFunction const& first, VisualScriptFunction const& second)
		{
			return HaveSameProgram(first, second);
		};
		return std::ranges::equal(variables, b.Variables, sameVariables) && std::ranges::equal(functions, b.Functions, sameFunctions) &&
		       HaveSameProgram(graph, b.Graph);
	}

	bool HaveSameVisualScriptProgram(VisualScriptNode const& a, VisualScriptNode const& b)
	{
		return HaveSameProgram(a, b);
	}

	VisualScriptAsset::VisualScriptAsset(VisualScriptDocument document)
		: m_Document(CreateRef<VisualScriptDocument const>(std::move(document)))
	{
	}

	void VisualScriptAsset::SetDocument(VisualScriptDocument document)
	{
		SetDocument(CreateRef<VisualScriptDocument const>(std::move(document)));
	}

	void VisualScriptAsset::SetDocument(Ref<VisualScriptDocument const> document)
	{
		ST_CORE_ASSERT(document != nullptr, "A visual script's document is never null");
		if (document == nullptr)
		{
			return;
		}
		m_Document = std::move(document);
		m_Version++;
	}

	VisualScriptDocument MakeVisualScriptTemplate(VisualScriptTemplate kind)
	{
		VisualScriptDocument document;
		switch (kind)
		{
			case VisualScriptTemplate::Events:
			{
				VisualScriptNode create;
				create.ID = 1;
				create.Type = "Event.Create";
				create.Position = glm::vec2(0.0f, 0.0f);
				VisualScriptNode update;
				update.ID = 2;
				update.Type = "Event.Update";
				update.Position = glm::vec2(0.0f, 160.0f);
				document.Graph.Nodes.push_back(std::move(create));
				document.Graph.Nodes.push_back(std::move(update));
				break;
			}
			case VisualScriptTemplate::Empty:
				break;
		}
		return document;
	}
}
