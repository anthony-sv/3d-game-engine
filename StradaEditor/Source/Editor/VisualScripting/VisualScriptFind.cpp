#include "Editor/VisualScripting/VisualScriptFind.h"

#include "Editor/VisualScripting/VisualScriptNodeTitles.h"

#include "Strada/Serialization/JsonSerialization.h"

#include <algorithm>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace Strada
{
	namespace
	{
		constexpr std::string_view GetVariableType = "Variable.Get";
		constexpr std::string_view SetVariableType = "Variable.Set";
		constexpr std::string_view CallType = "Function.Call";
		constexpr std::string_view CustomEventType = "Event.Custom";
		constexpr std::string_view SendEventType = "Event.Send";
		constexpr std::string_view NameSetting = "Name";

		char ToLowerAscii(char character)
		{
			return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
		}

		// Whether the text holds the lower-case needle, ASCII letters compared in any case.
		bool Contains(std::string_view text, std::string_view needle)
		{
			if (needle.size() > text.size())
			{
				return false;
			}
			auto const found = std::search(text.begin(), text.end(), needle.begin(), needle.end(),
			                               [](char a, char b)
			                               {
											   return ToLowerAscii(a) == b;
										   });
			return found != text.end();
		}

		std::string CompactJson(Json const& json)
		{
			return json.dump(-1, ' ', false, Json::error_handler_t::replace);
		}

		bool IsCustomEvent(VisualScriptNode const& node)
		{
			return node.Type == CustomEventType || node.Type == SendEventType;
		}

		// The ID a setting of the node stores; nothing when it stores no unsigned integer there.
		std::optional<uint64_t> ReadID(VisualScriptNode const& node, char const* setting)
		{
			if (!node.Settings.is_object())
			{
				return std::nullopt;
			}
			auto const value = node.Settings.find(setting);
			if (value == node.Settings.end() || !value->is_number_integer())
			{
				return std::nullopt;
			}
			if (value->is_number_unsigned())
			{
				return value->get<uint64_t>();
			}
			int64_t const signedValue = value->get<int64_t>();
			return signedValue >= 0 ? std::optional<uint64_t>(static_cast<uint64_t>(signedValue)) : std::nullopt;
		}

		std::optional<std::string> ReadString(VisualScriptNode const& node, std::string_view setting)
		{
			if (!node.Settings.is_object())
			{
				return std::nullopt;
			}
			auto const value = node.Settings.find(std::string(setting));
			if (value == node.Settings.end() || !value->is_string())
			{
				return std::nullopt;
			}
			return value->get<std::string>();
		}

		class Finder
		{
		public:
			Finder(VisualScriptDocument const& document, VisualScriptAnalysis const* analysis, VisualScriptFindQuery const& query)
				: m_Document(document),
				  m_Query(query)
			{
				for (char const character : query.Text)
				{
					m_Needle += ToLowerAscii(character);
				}
				// The analysis' nodes by ID, the first of each as VisualScriptAnalysis::FindNode finds it.
				if (analysis != nullptr)
				{
					m_AnalyzedNodes.reserve(analysis->Nodes.size());
					for (VisualScriptAnalyzedNode const& analyzed : analysis->Nodes)
					{
						m_AnalyzedNodes.emplace(analyzed.ID, &analyzed);
					}
				}
			}

			std::vector<VisualScriptFindResult> Run()
			{
				bool const byText = !m_Query.Text.empty();
				if (byText == m_Query.References.has_value())
				{
					return {};
				}
				if (byText)
				{
					for (VisualScriptVariable const& variable : m_Document.Variables)
					{
						FindName(0, variable.ID, VisualScriptFindField::VariableName, variable.Name);
					}
				}
				FindInGraph(0, m_Document.Graph);
				for (VisualScriptFunction const& function : m_Document.Functions)
				{
					if (byText)
					{
						FindName(function.ID, 0, VisualScriptFindField::FunctionName, function.Name);
						for (VisualScriptLocal const& local : function.Locals)
						{
							FindName(function.ID, local.ID, VisualScriptFindField::VariableName, local.Name);
						}
					}
					FindInGraph(function.ID, function.Graph);
				}
				return std::move(m_Results);
			}

		private:
			void FindName(uint32_t function, uint32_t variable, VisualScriptFindField field, std::string const& name)
			{
				if (Contains(name, m_Needle))
				{
					m_Results.push_back({function, 0, 0, variable, field, name});
				}
			}

			VisualScriptAnalyzedNode const* FindAnalyzedNode(uint32_t id) const
			{
				auto const found = m_AnalyzedNodes.find(id);
				return found != m_AnalyzedNodes.end() ? found->second : nullptr;
			}

			std::string GetTitle(VisualScriptNode const& node, VisualScriptAnalyzedNode const* analyzed) const
			{
				return analyzed != nullptr && !analyzed->Title.empty() ? analyzed->Title : DeriveVisualScriptNodeTitle(node, m_Document);
			}

			void FindInGraph(uint32_t function, VisualScriptGraph const& graph)
			{
				for (VisualScriptNode const& node : graph.Nodes)
				{
					if (m_Query.References.has_value())
					{
						if (IsReference(node))
						{
							m_Results.push_back(
								{function, node.ID, 0, 0, VisualScriptFindField::Title, GetTitle(node, FindAnalyzedNode(node.ID))});
						}
						continue;
					}
					FindInNode(function, node);
				}
				if (m_Query.References.has_value())
				{
					return;
				}
				for (VisualScriptComment const& comment : graph.Comments)
				{
					if (Contains(comment.Text, m_Needle))
					{
						m_Results.push_back({function, 0, comment.ID, 0, VisualScriptFindField::CommentText, comment.Text});
					}
				}
			}

			bool IsReference(VisualScriptNode const& node) const
			{
				VisualScriptReference const& reference = *m_Query.References;
				switch (reference.ReferenceKind)
				{
					case VisualScriptReference::Kind::Variable:
						return (node.Type == GetVariableType || node.Type == SetVariableType) && ReadID(node, "Variable") == reference.ID;
					case VisualScriptReference::Kind::Function:
						return node.Type == CallType && ReadID(node, "Function") == reference.ID;
					case VisualScriptReference::Kind::CustomEvent:
						return IsCustomEvent(node) && ReadString(node, NameSetting) == reference.Name;
				}
				return false;
			}

			void FindInNode(uint32_t function, VisualScriptNode const& node)
			{
				auto const add = [&](VisualScriptFindField field, std::string text)
				{
					m_Results.push_back({function, node.ID, 0, 0, field, std::move(text)});
				};
				VisualScriptAnalyzedNode const* const analyzed = FindAnalyzedNode(node.ID);
				if (std::string title = GetTitle(node, analyzed); Contains(title, m_Needle))
				{
					add(VisualScriptFindField::Title, std::move(title));
				}
				if (analyzed != nullptr && Contains(analyzed->Subtitle, m_Needle))
				{
					add(VisualScriptFindField::Subtitle, analyzed->Subtitle);
				}
				if (Contains(node.Type, m_Needle))
				{
					add(VisualScriptFindField::Type, node.Type);
				}
				if (Contains(node.Comment, m_Needle))
				{
					add(VisualScriptFindField::NodeComment, node.Comment);
				}
				if (analyzed != nullptr)
				{
					FindPinLabel(*analyzed, add);
				}
				FindValue(node, add);
			}

			template<typename TAdd>
			void FindPinLabel(VisualScriptAnalyzedNode const& analyzed, TAdd const& add) const
			{
				// A node's only exec input Execute and only exec output Then are drawn without labels.
				auto const countExec = [&analyzed](VisualScriptPinDirection direction)
				{
					return std::count_if(analyzed.Pins.begin(), analyzed.Pins.end(),
					                     [direction](VisualScriptPin const& pin)
					                     {
											 return pin.Direction == direction && pin.Type == VisualScriptExecType;
										 });
				};
				bool const singleInput = countExec(VisualScriptPinDirection::In) == 1;
				bool const singleOutput = countExec(VisualScriptPinDirection::Out) == 1;
				for (VisualScriptPin const& pin : analyzed.Pins)
				{
					bool const unlabelled = pin.Type == VisualScriptExecType &&
					                        ((pin.Direction == VisualScriptPinDirection::In && singleInput && pin.Name == "Execute") ||
					                         (pin.Direction == VisualScriptPinDirection::Out && singleOutput && pin.Name == "Then"));
					std::string const& label = pin.Label.empty() ? pin.Name : pin.Label;
					if (!unlabelled && Contains(label, m_Needle))
					{
						add(VisualScriptFindField::PinLabel, label);
						return;
					}
				}
			}

			template<typename TAdd>
			void FindValue(VisualScriptNode const& node, TAdd const& add) const
			{
				if (IsCustomEvent(node))
				{
					if (std::optional<std::string> name = ReadString(node, NameSetting); name.has_value() && Contains(*name, m_Needle))
					{
						add(VisualScriptFindField::EventName, std::move(*name));
					}
				}
				if (node.Inputs.is_object())
				{
					for (auto const& input : node.Inputs.items())
					{
						std::string text = CompactJson(input.value());
						if (Contains(text, m_Needle))
						{
							add(VisualScriptFindField::Value, std::move(text));
							return;
						}
					}
				}
				if (node.Settings.is_object())
				{
					for (auto const& setting : node.Settings.items())
					{
						if (!setting.value().is_string() || (IsCustomEvent(node) && setting.key() == NameSetting))
						{
							continue;
						}
						std::string const& text = setting.value().get_ref<std::string const&>();
						if (Contains(text, m_Needle))
						{
							add(VisualScriptFindField::Value, text);
							return;
						}
					}
				}
			}

			VisualScriptDocument const& m_Document;
			std::unordered_map<uint32_t, VisualScriptAnalyzedNode const*> m_AnalyzedNodes;
			VisualScriptFindQuery const& m_Query;
			std::string m_Needle;
			std::vector<VisualScriptFindResult> m_Results;
		};
	}

	std::vector<VisualScriptFindResult> FindInVisualScript(VisualScriptDocument const& document, VisualScriptAnalysis const* analysis,
	                                                       VisualScriptFindQuery const& query)
	{
		return Finder(document, analysis, query).Run();
	}
}
