#include "Editor/VisualScripting/VisualScriptDebugValues.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <limits>

namespace Strada
{
	namespace
	{
		namespace Texts = VisualScriptDebugValueTexts;

		// Significant digits of floating-point values in cells and pills, and in tooltips.
		constexpr int CellPrecision = 3;
		constexpr int TooltipPrecision = 6;
		constexpr std::string_view ArraySuffix = "[]";

		// A value's members as the runtime encodes them (spec-slice4 §5.4): exactly one of Value, Entity and Text (Text with
		// Entity for components and script instances), with Length and Truncated for cut strings and arrays; an array nested
		// deeper than two levels has Length alone.
		struct Encoded
		{
			Json const* Value = nullptr;
			std::optional<UUID> Entity;
			bool EntityValid = true;
			std::string_view Text;
			std::optional<uint32_t> Length;
			bool Truncated = false;
		};

		Encoded FromValue(VisualScriptDebugValue const& value)
		{
			Encoded encoded;
			encoded.Entity = value.Entity;
			encoded.EntityValid = value.EntityValid;
			encoded.Text = value.Text;
			encoded.Length = value.Length;
			encoded.Truncated = value.Truncated;
			if (!value.Entity && value.Text.empty())
			{
				encoded.Value = &value.Value;
			}
			return encoded;
		}

		// An array element, Before or a summary's value: the encoding without Type, as a JSON object. Anything else is read as a
		// literal.
		Encoded FromBare(Json const& json)
		{
			Encoded encoded;
			if (!json.is_object())
			{
				encoded.Value = &json;
				return encoded;
			}
			if (auto const value = json.find("Value"); value != json.end())
			{
				encoded.Value = &*value;
			}
			if (auto const entity = json.find("Entity"); entity != json.end() && entity->is_string())
			{
				std::string const& text = entity->get_ref<std::string const&>();
				uint64_t id = 0;
				auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), id);
				encoded.Entity = error == std::errc() && end == text.data() + text.size() ? UUID(id) : UUID::Invalid();
			}
			if (auto const valid = json.find("Valid"); valid != json.end() && valid->is_boolean())
			{
				encoded.EntityValid = valid->get<bool>();
			}
			if (auto const text = json.find("Text"); text != json.end() && text->is_string())
			{
				encoded.Text = text->get_ref<std::string const&>();
			}
			if (auto const length = json.find("Length"); length != json.end() && length->is_number_integer() && length->get<int64_t>() >= 0)
			{
				encoded.Length = static_cast<uint32_t>(std::min<uint64_t>(length->get<uint64_t>(), std::numeric_limits<uint32_t>::max()));
			}
			if (auto const truncated = json.find("Truncated"); truncated != json.end() && truncated->is_boolean())
			{
				encoded.Truncated = truncated->get<bool>();
			}
			return encoded;
		}

		// One line of text, with where its newlines shown as \n start.
		struct Line
		{
			std::string Text;
			std::vector<uint32_t> Escapes;
		};

		std::string FormatNumber(double value, int precision)
		{
			return fmt::format("{:.{}g}", value == 0.0 ? 0.0 : value, precision);
		}

		class Formatter
		{
		public:
			Formatter(VisualScriptLibrary const* library, VisualScriptEnumLookup const& enums,
			          std::function<std::string(UUID)> const& describeEntity, VisualScriptAssetNameLookup const& assetName)
				: m_Library(library),
				  m_Enums(enums),
				  m_DescribeEntity(describeEntity),
				  m_AssetName(assetName)
			{
			}

			VisualScriptTypeInfo const* FindType(std::string_view type) const
			{
				return m_Library != nullptr && !type.empty() ? m_Library->FindType(type) : nullptr;
			}

			bool IsColor(std::string_view type) const
			{
				VisualScriptTypeInfo const* info = FindType(type);
				return info != nullptr && info->Kind == VisualScriptTypeKind::Color;
			}

			// Whether a literal is an array of the runtime's element encoding: of an array type, or (without the type) of objects.
			bool IsArray(std::string_view type, Json const& literal) const
			{
				if (!literal.is_array())
				{
					return false;
				}
				if (VisualScriptTypeInfo const* info = FindType(type))
				{
					return info->Kind == VisualScriptTypeKind::Array;
				}
				if (type.ends_with(ArraySuffix))
				{
					return true;
				}
				return !literal.empty() && std::all_of(literal.begin(), literal.end(),
				                                       [](Json const& element)
				                                       {
														   return element.is_object();
													   });
			}

			std::string GetElementType(std::string_view arrayType) const
			{
				if (VisualScriptTypeInfo const* info = FindType(arrayType); info != nullptr && !info->Element.empty())
				{
					return info->Element;
				}
				return arrayType.ends_with(ArraySuffix) ? std::string(arrayType.substr(0, arrayType.size() - ArraySuffix.size()))
				                                        : std::string();
			}

			std::string DescribeEntity(UUID entity) const
			{
				if (!entity.IsValid())
				{
					return std::string(Texts::None);
				}
				return m_DescribeEntity ? m_DescribeEntity(entity) : entity.ToString();
			}

			// A string on one line, quoted: a newline shows as the two characters \n, other control characters as spaces.
			static void AppendQuoted(Line& line, std::string_view text, bool truncated)
			{
				line.Text += '"';
				for (char const c : text)
				{
					if (c == '\n')
					{
						line.Escapes.push_back(static_cast<uint32_t>(line.Text.size()));
						line.Text.append(Texts::Newline);
					}
					else if (c == '\r')
					{
						continue;
					}
					else if (static_cast<unsigned char>(c) < 0x20u)
					{
						line.Text += ' ';
					}
					else
					{
						line.Text += c;
					}
				}
				if (truncated)
				{
					line.Text.append(Texts::Ellipsis);
				}
				line.Text += '"';
			}

			// A literal of a type, by the chips' formatter, on one line.
			std::string FormatLiteral(Json const& literal, std::string_view type, int precision) const
			{
				if (literal.is_null())
				{
					return std::string(Texts::None);
				}
				if (literal.is_number_float() && FindType(type) == nullptr)
				{
					// Without the type (no library), a number is still shown as numbers are.
					return FormatNumber(literal.get<double>(), precision);
				}
				VisualScriptChipValue const chip =
					FormatVisualScriptLiteral(literal, type, FindType(type), m_Enums, m_AssetName, precision);
				switch (chip.Kind)
				{
					case VisualScriptChipKind::Cells:
					{
						std::string text = "(";
						for (char const c : chip.Text)
						{
							text.append(c == '\n' ? std::string_view(", ") : std::string_view(&c, 1));
						}
						text += ')';
						return text;
					}
					case VisualScriptChipKind::Color:
						return FormatComponents(literal, precision);
					case VisualScriptChipKind::None:
						return chip.Text.empty() ? literal.dump(-1, ' ', false, Json::error_handler_t::replace) : chip.Text;
					case VisualScriptChipKind::Check:
					case VisualScriptChipKind::Number:
					case VisualScriptChipKind::Text:
					case VisualScriptChipKind::Choice:
					case VisualScriptChipKind::Asset:
					case VisualScriptChipKind::Matrix:
					case VisualScriptChipKind::Placeholder:
					case VisualScriptChipKind::Raw:
						break;
				}
				return chip.Text;
			}

			// A colour's or a vector's components, "(0.5, 0, 1, 1)".
			static std::string FormatComponents(Json const& literal, int precision)
			{
				if (!literal.is_array())
				{
					return literal.dump(-1, ' ', false, Json::error_handler_t::replace);
				}
				std::string text = "(";
				for (size_t index = 0; index < literal.size(); index++)
				{
					if (index > 0)
					{
						text.append(", ");
					}
					Json const& component = literal[index];
					text.append(component.is_number() ? FormatNumber(component.get<double>(), precision)
					                                  : component.dump(-1, ' ', false, Json::error_handler_t::replace));
				}
				text += ')';
				return text;
			}

			// A value on one line: "[3] 1, 2, 3", "\"Hi\"", an entity's name, the runtime's text, a literal's text.
			void AppendValue(Line& line, Encoded const& encoded, std::string_view type, int precision) const
			{
				if (encoded.Entity)
				{
					line.Text.append(encoded.Text.empty() ? DescribeEntity(*encoded.Entity) : std::string(encoded.Text));
					return;
				}
				if (encoded.Value == nullptr)
				{
					if (!encoded.Text.empty())
					{
						line.Text.append(encoded.Text);
					}
					else if (encoded.Length)
					{
						// An array nested deeper than the runtime sends.
						line.Text.append(fmt::format("[{}]", *encoded.Length));
					}
					else
					{
						line.Text.append(Texts::None);
					}
					return;
				}
				Json const& literal = *encoded.Value;
				if (IsArray(type, literal))
				{
					uint32_t const length = encoded.Length.value_or(static_cast<uint32_t>(literal.size()));
					line.Text.append(fmt::format("[{}]", length));
					std::string const element = GetElementType(type);
					for (size_t index = 0; index < literal.size(); index++)
					{
						line.Text.append(index == 0 ? " " : ", ");
						AppendValue(line, FromBare(literal[index]), element, precision);
					}
					if (encoded.Truncated || length > literal.size())
					{
						line.Text.append(literal.empty() ? " " : ", ");
						line.Text.append(Texts::Ellipsis);
					}
					return;
				}
				if (literal.is_string() &&
				    (type.empty() || FindType(type) == nullptr || FindType(type)->Kind == VisualScriptTypeKind::String))
				{
					AppendQuoted(line, literal.get_ref<std::string const&>(), encoded.Truncated);
					return;
				}
				line.Text.append(FormatLiteral(literal, type, precision));
			}

			// The full value for a tooltip: a string's own lines, an array's elements one per line, an entity's name and ID.
			std::string MakeTooltip(Encoded const& encoded, std::string_view type) const
			{
				if (encoded.Entity)
				{
					std::string const name = encoded.Text.empty() ? DescribeEntity(*encoded.Entity) : std::string(encoded.Text);
					return encoded.Entity->IsValid() && encoded.Text.empty() ? fmt::format("{} ({})", name, encoded.Entity->ToString())
					                                                         : name;
				}
				if (encoded.Value != nullptr && encoded.Value->is_string() && !IsArray(type, *encoded.Value))
				{
					std::string tooltip = encoded.Value->get<std::string>();
					if (encoded.Truncated)
					{
						tooltip.append(Texts::Ellipsis);
						if (encoded.Length)
						{
							tooltip.append(fmt::format(" ({}{})", *encoded.Length, Texts::Characters));
						}
					}
					return tooltip;
				}
				if (encoded.Value != nullptr && IsArray(type, *encoded.Value))
				{
					Json const& literal = *encoded.Value;
					uint32_t const length = encoded.Length.value_or(static_cast<uint32_t>(literal.size()));
					std::string tooltip = fmt::format("[{}]", length);
					std::string const element = GetElementType(type);
					for (size_t index = 0; index < literal.size(); index++)
					{
						Line line;
						AppendValue(line, FromBare(literal[index]), element, TooltipPrecision);
						tooltip.append(fmt::format("\n{}: {}", index, line.Text));
					}
					if (length > literal.size())
					{
						tooltip.append(fmt::format("\n{}{}{}", Texts::Ellipsis, length - literal.size(), Texts::More));
					}
					return tooltip;
				}
				Line line;
				AppendValue(line, encoded, type, TooltipPrecision);
				return line.Text;
			}

		private:
			VisualScriptLibrary const* m_Library = nullptr;
			VisualScriptEnumLookup const& m_Enums;
			std::function<std::string(UUID)> const& m_DescribeEntity;
			VisualScriptAssetNameLookup const& m_AssetName;
		};
	}

	VisualScriptValueCell FormatVisualScriptDebugValue(VisualScriptDebugValue const& value, VisualScriptLibrary const* library,
	                                                   VisualScriptEnumLookup const& enums,
	                                                   std::function<std::string(UUID)> const& describeEntity,
	                                                   VisualScriptAssetNameLookup const& assetName)
	{
		VisualScriptValueCell cell;
		if (value.Currency == VisualScriptValueCurrency::NotComputed)
		{
			cell.Kind = VisualScriptValueCellKind::None;
			cell.Text = std::string(Texts::NotComputed);
			cell.Tooltip = std::string(Texts::NotComputedTip);
			return cell;
		}
		Formatter const formatter(library, enums, describeEntity, assetName);
		Encoded const encoded = FromValue(value);
		if (encoded.Entity && encoded.Entity->IsValid())
		{
			cell.Kind = VisualScriptValueCellKind::Entity;
			cell.Entity = *encoded.Entity;
		}
		if (encoded.Value != nullptr && !encoded.Value->is_null() && formatter.IsColor(value.Type))
		{
			VisualScriptChipValue const chip =
				FormatVisualScriptLiteral(*encoded.Value, value.Type, formatter.FindType(value.Type), enums, assetName);
			if (chip.Kind == VisualScriptChipKind::Color && !chip.Invalid)
			{
				cell.Kind = VisualScriptValueCellKind::Swatch;
				cell.Color = chip.Color;
			}
		}
		if (cell.Kind != VisualScriptValueCellKind::Swatch)
		{
			Line line;
			formatter.AppendValue(line, encoded, value.Type, CellPrecision);
			cell.Text = std::move(line.Text);
			cell.Escapes = std::move(line.Escapes);
		}
		cell.Tooltip = formatter.MakeTooltip(encoded, value.Type);
		if (value.Currency == VisualScriptValueCurrency::Earlier || value.Currency == VisualScriptValueCurrency::Unknown)
		{
			cell.Dimmed = true;
			cell.Tooltip.append("\n");
			cell.Tooltip.append(value.Currency == VisualScriptValueCurrency::Earlier ? Texts::EarlierTip : Texts::UnknownTip);
		}
		if (value.Edited)
		{
			Line before;
			formatter.AppendValue(before, FromBare(value.Before), value.Type, CellPrecision);
			Line now;
			if (cell.Kind == VisualScriptValueCellKind::Swatch)
			{
				formatter.AppendValue(now, encoded, value.Type, CellPrecision);
			}
			cell.Note = fmt::format("{}{}{}{}", Texts::Edited, before.Text, Texts::Arrow,
			                        cell.Kind == VisualScriptValueCellKind::Swatch ? now.Text : cell.Text);
		}
		else if (value.Changed)
		{
			cell.Note = std::string(Texts::ChangedElsewhere);
		}
		return cell;
	}
}
