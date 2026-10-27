#include "Editor/VisualScripting/VisualScriptDetails.h"

#include "Editor/UI/EditorUI.h"
#include "Editor/VisualScripting/VisualScriptCanvasMetrics.h"
#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptDrawer.h"
#include "Editor/VisualScripting/VisualScriptEditing.h"
#include "Editor/VisualScripting/VisualScriptTheme.h"

#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/Log.h"
#include "Strada/Math/Math.h"
#include "Strada/Script/VisualScriptLiterals.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <numbers>

namespace Strada
{
	namespace
	{
		// The loader's words for a name that is not one (VisualScriptSerializer::IsValidName).
		constexpr char const* NameRuleMessage =
			"expected a name of 1 to 64 bytes without control characters, '.' or a leading or trailing space";
		// The base names of new locals and parameters, made unique.
		constexpr char const* NewLocalName = "NewLocal";
		// How many samples an Easing plot draws.
		constexpr int EasingSamples = 32;

		std::string RemovesLinks(uint32_t removed)
		{
			if (removed == 0)
			{
				return {};
			}
			return fmt::format(" (removes {} {})", removed, removed == 1 ? "link" : "links");
		}

		std::string CountOf(size_t count, std::string_view singular, std::string_view plural)
		{
			return fmt::format("{} {}", count, count == 1 ? singular : plural);
		}

		std::string DescribeType(VisualScriptLibrary const* library, std::string_view type)
		{
			if (library != nullptr)
			{
				if (VisualScriptTypeInfo const* const info = library->FindType(type); info != nullptr && !info->Title.empty())
				{
					return info->Title;
				}
			}
			return std::string(type);
		}

		VisualScriptEnumLookup MakeEnumLookup(VisualScriptLibrary const* library)
		{
			if (library == nullptr)
			{
				return {};
			}
			return [library](std::string_view type)
			{
				return library->FindEnum(type);
			};
		}

		// Whether a variable of the type can be public: components can store it (an exposable type, or an enum).
		bool CanBePublic(std::string_view type, VisualScriptLibrary const* library)
		{
			if (FindVisualScriptExposableType(type))
			{
				return true;
			}
			return library != nullptr && library->FindEnum(type).has_value();
		}

		bool IsNumeric(std::string_view type, VisualScriptLibrary const* library)
		{
			if (library != nullptr)
			{
				if (VisualScriptTypeInfo const* const info = library->FindType(type))
				{
					return info->Kind == VisualScriptTypeKind::Integer || info->Kind == VisualScriptTypeKind::Float;
				}
			}
			return type == "int" || type == "float" || type == "double" || type == "long" || type == "uint" || type == "ulong";
		}

		glm::vec2 SnapToGrid(glm::vec2 position)
		{
			return glm::round(position / VisualScriptCanvasMetrics::GridSize) * VisualScriptCanvasMetrics::GridSize;
		}

		// A small square in a colour, on the text line.
		void ColorSquare(uint32_t color)
		{
			float const size = ImGui::GetFontSize() * 0.6f;
			ImVec2 const cursor = ImGui::GetCursorScreenPos();
			float const top = cursor.y + (ImGui::GetFrameHeight() - size) * 0.5f;
			ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(cursor.x, top), ImVec2(cursor.x + size, top + size), color, size * 0.2f);
			ImGui::Dummy(ImVec2(size, ImGui::GetFrameHeight()));
			ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
		}

		VisualScriptNodeExtents MakeExtents(VisualScriptEditorModel const& model)
		{
			return [&model](uint32_t node)
			{
				VisualScriptCanvasModel const& canvas = model.GetCanvas();
				std::optional<uint32_t> const index = canvas.FindNode(node);
				return index ? canvas.GetNodes()[*index].Size : glm::vec2(0.0f);
			};
		}

		// The enum lookup's enumerator names of a type, in declaration order.
		std::vector<std::string> GetEnumeratorNames(VisualScriptLibrary const* library, std::string_view type)
		{
			std::vector<std::string> names;
			if (library == nullptr)
			{
				return names;
			}
			if (VisualScriptTypeInfo const* const info = library->FindType(type))
			{
				for (ScriptEnumerator const& enumerator : info->Enumerators)
				{
					names.push_back(enumerator.Name);
				}
			}
			return names;
		}

		std::string GetString(Json const& settings, std::string_view name)
		{
			auto const found = settings.find(name);
			return found != settings.end() && found->is_string() ? found->get<std::string>() : std::string();
		}

		Json GetSettingValue(VisualScriptNode const& node, VisualScriptSetting const& setting)
		{
			if (node.Settings.is_object())
			{
				auto const found = node.Settings.find(setting.Name);
				if (found != node.Settings.end())
				{
					return *found;
				}
			}
			return setting.Default;
		}

		// The Easing plot: the curve over [0, 1], scaled to the rectangle with room for the overshoot of Back and Elastic.
		void DrawEasingPlot(std::string_view easing, ImVec2 min, ImVec2 max, uint32_t color)
		{
			std::array<double, EasingSamples + 1> samples{};
			double low = 0.0;
			double high = 1.0;
			for (int i = 0; i <= EasingSamples; i++)
			{
				samples[static_cast<size_t>(i)] =
					EvaluateVisualScriptEasing(easing, static_cast<double>(i) / static_cast<double>(EasingSamples)).value_or(0.0);
				low = std::min(low, samples[static_cast<size_t>(i)]);
				high = std::max(high, samples[static_cast<size_t>(i)]);
			}
			std::array<ImVec2, EasingSamples + 1> points{};
			for (int i = 0; i <= EasingSamples; i++)
			{
				float const x = static_cast<float>(i) / static_cast<float>(EasingSamples);
				auto const y = static_cast<float>((samples[static_cast<size_t>(i)] - low) / (high - low));
				points[static_cast<size_t>(i)] = ImVec2(min.x + (max.x - min.x) * x, max.y - (max.y - min.y) * y);
			}
			ImDrawList* const drawList = ImGui::GetWindowDrawList();
			drawList->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border));
			drawList->AddPolyline(points.data(), static_cast<int>(points.size()), color, 1.0f, ImDrawFlags_None);
		}

		// Moves Up, Down and Enter of a search field into the int the user data points to (-1, +1).
		int OnPickerKey(ImGuiInputTextCallbackData* data)
		{
			int* const move = static_cast<int*>(data->UserData);
			if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory)
			{
				*move += data->EventKey == ImGuiKey_UpArrow ? -1 : 1;
			}
			return 0;
		}

		bool ContainsIgnoringCase(std::string_view text, std::string_view part)
		{
			if (part.empty())
			{
				return true;
			}
			auto const lower = [](char c)
			{
				return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
			};
			auto const found = std::search(text.begin(), text.end(), part.begin(), part.end(),
			                               [&lower](char a, char b)
			                               {
											   return lower(a) == lower(b);
										   });
			return found != text.end();
		}

		std::string_view GetKindGroupName(VisualScriptTypeKind kind)
		{
			switch (kind)
			{
				case VisualScriptTypeKind::Exec:
					return "Flow";
				case VisualScriptTypeKind::Bool:
					return "Booleans";
				case VisualScriptTypeKind::Integer:
					return "Integers";
				case VisualScriptTypeKind::Float:
					return "Floats";
				case VisualScriptTypeKind::String:
					return "Strings";
				case VisualScriptTypeKind::Vector:
					return "Vectors";
				case VisualScriptTypeKind::Quaternion:
					return "Quaternions";
				case VisualScriptTypeKind::Color:
					return "Colours";
				case VisualScriptTypeKind::Matrix:
					return "Matrices";
				case VisualScriptTypeKind::Entity:
					return "Entities";
				case VisualScriptTypeKind::Component:
					return "Components";
				case VisualScriptTypeKind::Asset:
					return "Assets";
				case VisualScriptTypeKind::Enum:
					return "Enums";
				case VisualScriptTypeKind::Struct:
					return "Structs";
				case VisualScriptTypeKind::Class:
					return "Classes";
				case VisualScriptTypeKind::Array:
					return "Arrays";
				case VisualScriptTypeKind::Object:
					return "Objects";
			}
			return "Types";
		}

		// How many nodes of the document's graphs name a variable or local (Variable.Get and Variable.Set) or call a function.
		uint32_t CountUses(VisualScriptDocument const& document, std::string_view type, std::string_view setting, uint32_t id)
		{
			uint32_t count = 0;
			auto const countIn = [&](VisualScriptGraph const& graph)
			{
				for (VisualScriptNode const& node : graph.Nodes)
				{
					if (node.Type != type && !(type == "Variable.Get" && node.Type == "Variable.Set"))
					{
						continue;
					}
					auto const found = node.Settings.find(setting);
					if (found != node.Settings.end() && found->is_number_unsigned() && found->get<uint64_t>() == id)
					{
						count++;
					}
					else if (found != node.Settings.end() && found->is_number_integer() &&
					         found->get<int64_t>() == static_cast<int64_t>(id))
					{
						count++;
					}
				}
			};
			countIn(document.Graph);
			for (VisualScriptFunction const& function : document.Functions)
			{
				countIn(function.Graph);
			}
			return count;
		}

		VisualScriptFunction const* FindFunction(VisualScriptDocument const& document, uint32_t id)
		{
			auto const found = std::find_if(document.Functions.begin(), document.Functions.end(),
			                                [id](VisualScriptFunction const& function)
			                                {
												return function.ID == id;
											});
			return found != document.Functions.end() ? &*found : nullptr;
		}

		// The nodes and comments of a selection, and the words for them ("4 Nodes", "Comment", "3 Elements").
		std::string DescribeElements(VisualScriptDocument const& document, uint32_t function, std::span<uint32_t const> elements)
		{
			VisualScriptGraph const* const graph = FindVisualScriptGraph(document, function);
			size_t comments = 0;
			if (graph != nullptr)
			{
				for (uint32_t const element : elements)
				{
					comments += static_cast<size_t>(std::any_of(graph->Comments.begin(), graph->Comments.end(),
					                                            [element](VisualScriptComment const& comment)
					                                            {
																	return comment.ID == element;
																}));
				}
			}
			size_t const nodes = elements.size() - comments;
			if (comments == 0)
			{
				return nodes == 1 ? "Node" : fmt::format("{} Nodes", nodes);
			}
			if (nodes == 0)
			{
				return comments == 1 ? "Comment" : fmt::format("{} Comments", comments);
			}
			return fmt::format("{} Elements", elements.size());
		}
	}

	std::optional<double> EvaluateVisualScriptEasing(std::string_view easing, double t)
	{
		// The constants and forms of the scripting runtime's Easings (LatentNodes.cs), which spec v2 §4.2 defines.
		constexpr double BounceScale = 7.5625;
		constexpr double BounceSpan = 2.75;
		constexpr double BackSquare = 1.70158;
		constexpr double BackCube = 2.70158;
		t = std::clamp(t, 0.0, 1.0);
		auto const square = [](double value)
		{
			return value * value;
		};
		auto const cube = [](double value)
		{
			return value * value * value;
		};
		if (easing == "Linear")
		{
			return t;
		}
		if (easing == "EaseIn")
		{
			return t * t;
		}
		if (easing == "EaseOut")
		{
			return 1.0 - square(1.0 - t);
		}
		if (easing == "EaseInOut")
		{
			return t < 0.5 ? 2.0 * t * t : 1.0 - square(-2.0 * t + 2.0) / 2.0;
		}
		if (easing == "EaseInCubic")
		{
			return t * t * t;
		}
		if (easing == "EaseOutCubic")
		{
			return 1.0 - cube(1.0 - t);
		}
		if (easing == "EaseInOutCubic")
		{
			return t < 0.5 ? 4.0 * t * t * t : 1.0 - cube(-2.0 * t + 2.0) / 2.0;
		}
		if (easing == "EaseOutBack")
		{
			return 1.0 + BackCube * cube(t - 1.0) + BackSquare * square(t - 1.0);
		}
		if (easing == "EaseOutBounce")
		{
			if (t < 1.0 / BounceSpan)
			{
				return BounceScale * t * t;
			}
			if (t < 2.0 / BounceSpan)
			{
				return BounceScale * square(t - 1.5 / BounceSpan) + 0.75;
			}
			if (t < 2.5 / BounceSpan)
			{
				return BounceScale * square(t - 2.25 / BounceSpan) + 0.9375;
			}
			return BounceScale * square(t - 2.625 / BounceSpan) + 0.984375;
		}
		if (easing == "EaseOutElastic")
		{
			if (t == 0.0 || t == 1.0)
			{
				return t;
			}
			return std::pow(2.0, -10.0 * t) * std::sin((10.0 * t - 0.75) * (2.0 * std::numbers::pi / 3.0)) + 1.0;
		}
		return std::nullopt;
	}

	bool IsVisualScriptTypeOffered(VisualScriptTypeInfo const& type, VisualScriptTypeUse use, std::string_view constraint)
	{
		if (type.Kind == VisualScriptTypeKind::Exec || type.ID == VisualScriptExecType || type.ID == VisualScriptWildcardType)
		{
			return false;
		}
		switch (use)
		{
			case VisualScriptTypeUse::Value:
				return type.Kind != VisualScriptTypeKind::Array;
			case VisualScriptTypeUse::PublicVariable:
				return type.FieldType != ScriptFieldType::None || type.Kind == VisualScriptTypeKind::Enum;
			case VisualScriptTypeUse::Setting:
				break;
		}
		if (constraint == "Literal")
		{
			return type.HasLiteral;
		}
		if (constraint == "Exposable")
		{
			return type.FieldType != ScriptFieldType::None || type.Kind == VisualScriptTypeKind::Enum;
		}
		if (constraint == "Struct")
		{
			return type.Kind == VisualScriptTypeKind::Struct;
		}
		if (constraint == "Makeable")
		{
			return type.Makeable;
		}
		if (constraint == "Breakable")
		{
			return type.Breakable;
		}
		if (constraint == "Enum")
		{
			return type.Kind == VisualScriptTypeKind::Enum;
		}
		if (constraint == "Switch")
		{
			return type.ID == "int" || type.ID == "string" || type.Kind == VisualScriptTypeKind::Enum;
		}
		if (constraint == "Component")
		{
			return type.Kind == VisualScriptTypeKind::Component;
		}
		if (constraint == "Asset")
		{
			return type.Kind == VisualScriptTypeKind::Asset && type.AcceptedAssetType != AssetType::None;
		}
		if (constraint == "Script")
		{
			return type.Kind == VisualScriptTypeKind::Entity && type.ID != "Strada.Entity";
		}
		// None, Any and constraints this editor does not know: any type with values.
		return true;
	}

	// What a Draw works with.
	struct VisualScriptDetails::Frame
	{
		VisualScriptPaneContext& Context;
		VisualScriptPaneResult& Result;
		// The pending widget value shown, kept alive while a widget replaces it.
		Ref<VisualScriptDocument const> Hold;
		VisualScriptDocument const& Document;
		Ref<VisualScriptLibrary const> LibraryHold;
		VisualScriptLibrary const* Library = nullptr;
		VisualScriptAnalysis const* Analysis = nullptr;
		// The model edits nothing: edit widgets are disabled, what only shows or goes somewhere is not (ViewScope).
		bool ReadOnly = false;
	};

	namespace
	{
		// Enables what only shows or navigates (copying a type, finding references, going to a node) inside Details' disabled
		// edit widgets of a read-only document.
		class ViewScope
		{
		public:
			explicit ViewScope(bool readOnly)
				: m_ReadOnly(readOnly)
			{
				if (m_ReadOnly)
				{
					ImGui::EndDisabled();
				}
			}
			~ViewScope()
			{
				if (m_ReadOnly)
				{
					ImGui::BeginDisabled();
				}
			}
			ViewScope(ViewScope const&) = delete;
			ViewScope& operator=(ViewScope const&) = delete;

		private:
			bool m_ReadOnly;
		};
	}

	std::optional<std::pair<glm::vec2, glm::vec2>> VisualScriptDetails::FindRow(std::string_view label) const
	{
		auto const found = std::find_if(m_Rows.begin(), m_Rows.end(),
		                                [label](auto const& row)
		                                {
											return row.first == label;
										});
		return found != m_Rows.end() ? std::optional(found->second) : std::nullopt;
	}

	void VisualScriptDetails::BeginRow(std::string_view label)
	{
		ImVec2 const start = ImGui::GetCursorScreenPos();
		float const labelWidth = ImGui::GetContentRegionAvail().x * LabelShare;
		ImGui::AlignTextToFramePadding();
		// Labels longer than their column are clipped to it.
		ImGui::PushClipRect(start, ImVec2(start.x + labelWidth - ImGui::GetStyle().ItemSpacing.x, start.y + ImGui::GetFrameHeight()), true);
		ImGui::TextUnformatted(label.data(), label.data() + label.size());
		ImGui::PopClipRect();
		ImGui::SameLine(0.0f, 0.0f);
		ImGui::SetCursorScreenPos(ImVec2(start.x + labelWidth, start.y));
	}

	void VisualScriptDetails::EndRow(std::string_view label)
	{
		ImVec2 const min = ImGui::GetItemRectMin();
		ImVec2 const max = ImGui::GetItemRectMax();
		m_Rows.emplace_back(std::string(label), std::pair(glm::vec2(min.x, min.y), glm::vec2(max.x, max.y)));
	}

	std::optional<std::string> VisualScriptDetails::GetRowText(std::string_view label) const
	{
		auto const found = std::find_if(m_RowTexts.begin(), m_RowTexts.end(),
		                                [label](auto const& row)
		                                {
											return row.first == label;
										});
		return found != m_RowTexts.end() ? std::optional(found->second) : std::nullopt;
	}

	void VisualScriptDetails::TextRow(std::string_view label, std::string_view text)
	{
		BeginRow(label);
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextUnformatted(text.data(), text.data() + text.size());
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
		EndRow(label);
		m_RowTexts.emplace_back(std::string(label), std::string(text));
	}

	template<typename TDraw>
	void VisualScriptDetails::Track(Frame& frame, std::string_view label, TDraw&& draw)
	{
		BeginRow(label);
		ImGui::BeginGroup();
		ImGui::PushItemWidth(-FLT_MIN);
		std::optional<Edit> edit = draw();
		ImGui::PopItemWidth();
		ImGui::EndGroup();
		bool const active = ImGui::IsItemActive();
		bool const deactivated = ImGui::IsItemDeactivated();
		EndRow(label);
		if (edit)
		{
			m_Commits.Change(frame.Context, std::move(edit->Document), std::move(edit->Action), active, frame.Result);
		}
		else if (active)
		{
			frame.Result.WidgetActive = true;
		}
		if (deactivated)
		{
			m_Commits.End(frame.Context, frame.Result);
		}
	}

	VisualScriptPaneResult VisualScriptDetails::Draw(VisualScriptPaneContext& context)
	{
		VisualScriptPaneResult result;
		m_Rows.clear();
		m_RowTexts.clear();
		m_PickerTypes.clear();
		m_Commits.BeginFrame(context, result);
		VisualScriptEditorModel& model = context.Model;
		if (model.GetStoredDocument() == nullptr)
		{
			ImGui::TextDisabled("No script is shown.");
			return result;
		}
		Ref<VisualScriptDocument const> const hold = m_Commits.GetPendingDocument(model);
		Ref<VisualScriptLibrary const> const library = context.Semantics.GetLibrary();
		Frame frame{context,
		            result,
		            hold,
		            hold != nullptr ? *hold : model.GetDisplayDocument(),
		            library,
		            library.get(),
		            model.GetAnalysis(),
		            model.IsReadOnly()};

		std::span<uint32_t const> const selection = model.GetSelection();
		if (!selection.empty())
		{
			context.Shared.Element.reset();
		}
		ImGui::BeginDisabled(frame.ReadOnly);
		if (selection.size() > 1)
		{
			m_Subject = VisualScriptDetailsSubject::Several;
			DrawSeveral(frame, selection);
		}
		else if (selection.size() == 1)
		{
			uint32_t const element = selection.front();
			VisualScriptComment const* comment = nullptr;
			if (VisualScriptGraph const* const graph = FindVisualScriptGraph(frame.Document, model.GetFunction()))
			{
				auto const found = std::find_if(graph->Comments.begin(), graph->Comments.end(),
				                                [element](VisualScriptComment const& candidate)
				                                {
													return candidate.ID == element;
												});
				comment = found != graph->Comments.end() ? &*found : nullptr;
			}
			if (comment != nullptr)
			{
				m_Subject = VisualScriptDetailsSubject::Comment;
				DrawComment(frame, *comment);
			}
			else if (VisualScriptNode const* const node = FindVisualScriptNode(frame.Document, element))
			{
				m_Subject = VisualScriptDetailsSubject::Node;
				DrawNode(frame, *node);
			}
		}
		else if (std::optional<VisualScriptElement> const element = context.Shared.Element)
		{
			bool shown = false;
			switch (element->Kind)
			{
				case VisualScriptElementKind::Variable:
				{
					auto const variable = std::find_if(frame.Document.Variables.begin(), frame.Document.Variables.end(),
					                                   [&element](VisualScriptVariable const& candidate)
					                                   {
														   return candidate.ID == element->ID;
													   });
					if (variable != frame.Document.Variables.end())
					{
						m_Subject = VisualScriptDetailsSubject::Variable;
						DrawVariable(frame, *variable);
						shown = true;
					}
					break;
				}
				case VisualScriptElementKind::Local:
					for (VisualScriptFunction const& function : frame.Document.Functions)
					{
						auto const local = std::find_if(function.Locals.begin(), function.Locals.end(),
						                                [&element](VisualScriptLocal const& candidate)
						                                {
															return candidate.ID == element->ID;
														});
						if (local != function.Locals.end())
						{
							m_Subject = VisualScriptDetailsSubject::Local;
							DrawLocal(frame, function, *local);
							shown = true;
							break;
						}
					}
					break;
				case VisualScriptElementKind::Function:
					if (element->ID == 0)
					{
						// The event graph's row: the script.
						m_Subject = VisualScriptDetailsSubject::Script;
						DrawScript(frame);
						shown = true;
					}
					else if (VisualScriptFunction const* const function = FindFunction(frame.Document, element->ID))
					{
						m_Subject = VisualScriptDetailsSubject::Function;
						DrawFunction(frame, *function);
						shown = true;
					}
					break;
				case VisualScriptElementKind::CustomEvent:
					if (VisualScriptNode const* const node = FindVisualScriptNode(frame.Document, element->ID))
					{
						m_Subject = VisualScriptDetailsSubject::CustomEvent;
						DrawNode(frame, *node);
						shown = true;
					}
					break;
			}
			if (!shown)
			{
				// The row's element is gone (an undo, an outside change): Details shows the script.
				context.Shared.Element.reset();
				m_Subject = VisualScriptDetailsSubject::Script;
				DrawScript(frame);
			}
		}
		else
		{
			m_Subject = VisualScriptDetailsSubject::Script;
			DrawScript(frame);
		}
		ImGui::EndDisabled();
		return result;
	}

	void VisualScriptDetails::DrawScript(Frame& frame)
	{
		VisualScriptDocument const& document = frame.Document;
		VisualScriptEditorModel const& model = frame.Context.Model;
		ImGui::TextUnformatted(frame.Context.ScriptName.data(), frame.Context.ScriptName.data() + frame.Context.ScriptName.size());
		Track(frame, "Description",
		      [&]() -> std::optional<Edit>
		      {
				  std::string text = document.Description;
				  float const height = ImGui::GetTextLineHeight() * 4.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
				  if (!ImGui::InputTextMultiline("##description", &text, ImVec2(-FLT_MIN, height)))
				  {
					  return std::nullopt;
				  }
				  Edit edit{document, "Set Description"};
				  if (!VisualScriptEditing::SetDescription(edit.Document, std::move(text)))
				  {
					  return std::nullopt;
				  }
				  return edit;
			  });
		uint32_t const function = model.GetFunction();
		if (VisualScriptGraph const* const graph = FindVisualScriptGraph(document, function))
		{
			ImGui::TextDisabled("%s",
			                    fmt::format("{}: {} \xC2\xB7 {} \xC2\xB7 {}", GetVisualScriptGraphName(document, function),
			                                CountOf(graph->Nodes.size(), "node", "nodes"), CountOf(graph->Links.size(), "link", "links"),
			                                CountOf(graph->Comments.size(), "comment", "comments"))
			                        .c_str());
		}
		if (function != 0)
		{
			if (VisualScriptFunction const* const shown = FindFunction(document, function))
			{
				ImGui::Spacing();
				DrawFunction(frame, *shown);
			}
		}
	}

	void VisualScriptDetails::DrawNode(Frame& frame, VisualScriptNode const& node)
	{
		std::string const title = GetVisualScriptNodeTitle(frame.Document, frame.Analysis, node);
		ImGui::PushID(static_cast<int>(node.ID));
		DrawNodeHeader(frame, node, title);
		DrawGeneral(frame, node, title);
		DrawSettings(frame, node, title);
		DrawInputs(frame, node, title);
		DrawNodeProblems(frame, node);
		DrawAbout(frame, node);
		ImGui::PopID();
		if (!frame.Context.Shared.FocusSetting.empty())
		{
			// Given to its row above when the node has it; either way it is done with.
			frame.Context.Shared.FocusSetting.clear();
		}
	}

	void VisualScriptDetails::DrawNodeHeader(Frame& frame, VisualScriptNode const& node, std::string const& title)
	{
		VisualScriptCanvasModel const& canvas = frame.Context.Model.GetCanvas();
		uint32_t color = VisualScriptHoverOutlineColor;
		std::string subtitle;
		if (std::optional<uint32_t> const index = canvas.FindNode(node.ID))
		{
			std::span<VisualScriptNodeVisual const> const nodes = canvas.GetNodes();
			VisualScriptNodeVisual const& visual = nodes[*index];
			color = visual.HeaderColor;
			subtitle = std::string(canvas.GetText(visual.Subtitle));
		}
		else if (frame.Library != nullptr)
		{
			if (VisualScriptNodeTemplate const* const kind = frame.Library->FindNode(node.Type))
			{
				color = GetVisualScriptHeaderColor(kind->Kind, kind->Category.substr(0, kind->Category.find('/')));
				subtitle = kind->Subtitle;
			}
		}
		ColorSquare(color);
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(title.c_str());
		if (!subtitle.empty())
		{
			ImGui::SameLine();
			ImGui::TextDisabled("%s", subtitle.c_str());
		}
		ImGui::TextDisabled("%s", fmt::format("Node {} \xC2\xB7 {}", node.ID, node.Type).c_str());
		ImGui::SameLine();
		ViewScope const view(frame.ReadOnly);
		if (ImGui::SmallButton("Copy"))
		{
			ImGui::SetClipboardText(node.Type.c_str());
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
		{
			ImGui::SetTooltip("Copy the node's type");
		}
	}

	void VisualScriptDetails::DrawGeneral(Frame& frame, VisualScriptNode const& node, std::string const& title)
	{
		VisualScriptDocument const& document = frame.Document;
		ImGui::SeparatorText("General");
		Track(frame, "Comment",
		      [&]() -> std::optional<Edit>
		      {
				  std::string text = node.Comment;
				  float const height = ImGui::GetTextLineHeight() * 3.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
				  if (!ImGui::InputTextMultiline("##comment", &text, ImVec2(-FLT_MIN, height)))
				  {
					  return std::nullopt;
				  }
				  std::string action = node.Comment.empty() ? "Comment " + title
			                           : text.empty()       ? "Remove Comment of " + title
			                                                : "Edit Comment of " + title;
				  Edit edit{document, std::move(action)};
				  uint32_t const id = node.ID;
				  if (!VisualScriptEditing::SetNodeComment(edit.Document, std::span(&id, 1), text))
				  {
					  return std::nullopt;
				  }
				  return edit;
			  });
		Track(frame, "Enabled",
		      [&]() -> std::optional<Edit>
		      {
				  constexpr std::array<char const*, 3> Labels = {"Enabled", "Disabled", "Development Only"};
				  int current = static_cast<int>(node.Enabled);
				  if (!ImGui::Combo("##enabled", &current, Labels.data(), static_cast<int>(Labels.size())))
				  {
					  return std::nullopt;
				  }
				  auto const enabled = static_cast<VisualScriptNodeEnabled>(current);
				  std::string action = enabled == VisualScriptNodeEnabled::Enabled    ? "Enable " + title
			                           : enabled == VisualScriptNodeEnabled::Disabled ? "Disable " + title
			                                                                          : "Set " + title + " to Development Only";
				  Edit edit{document, std::move(action)};
				  uint32_t const id = node.ID;
				  if (!VisualScriptEditing::SetNodeEnabled(edit.Document, std::span(&id, 1), enabled))
				  {
					  return std::nullopt;
				  }
				  return edit;
			  });
		Track(frame, "Collapsed",
		      [&]() -> std::optional<Edit>
		      {
				  bool collapsed = node.Collapsed;
				  if (!ImGui::Checkbox("##collapsed", &collapsed))
				  {
					  return std::nullopt;
				  }
				  Edit edit{document, (collapsed ? "Collapse " : "Expand ") + title};
				  uint32_t const id = node.ID;
				  if (!VisualScriptEditing::SetNodeCollapsed(edit.Document, std::span(&id, 1), collapsed))
				  {
					  return std::nullopt;
				  }
				  return edit;
			  });
	}

	void VisualScriptDetails::DrawSettings(Frame& frame, VisualScriptNode const& node, std::string const& title)
	{
		if (frame.Library == nullptr)
		{
			if (node.Settings.is_object() && !node.Settings.empty())
			{
				ImGui::SeparatorText("Settings");
				ImGui::TextDisabled("Settings need the scripting runtime, which is not running.");
			}
			return;
		}
		VisualScriptNodeTemplate const* kind = frame.Library->FindNode(node.Type);
		if (kind == nullptr && frame.Analysis != nullptr)
		{
			if (VisualScriptAnalyzedNode const* const analyzed = frame.Analysis->FindNode(node.ID); analyzed != nullptr)
			{
				kind = frame.Library->FindNode(analyzed->CanonicalType);
			}
		}
		if (kind == nullptr || kind->Settings.empty())
		{
			return;
		}
		ImGui::SeparatorText("Settings");
		for (VisualScriptSetting const& setting : kind->Settings)
		{
			ImGui::PushID(setting.Name.c_str());
			DrawSetting(frame, node, title, setting);
			ImGui::PopID();
		}
	}

	void VisualScriptDetails::DrawSetting(Frame& frame, VisualScriptNode const& node, std::string const& title,
	                                      VisualScriptSetting const& setting)
	{
		VisualScriptDocument const& document = frame.Document;
		VisualScriptPaneContext& context = frame.Context;
		std::string const label = UI::FormatDisplayName(setting.Name);
		Json const value = GetSettingValue(node, setting);
		bool const focus = context.Shared.FocusSetting == setting.Name;
		std::string const action = fmt::format("Set {} on {}", label, title);
		auto const setSetting = [&](Json newValue) -> std::optional<Edit>
		{
			Edit edit{document, action};
			if (!VisualScriptEditing::SetSetting(edit.Document, node.ID, setting.Name, std::move(newValue)))
			{
				return std::nullopt;
			}
			return edit;
		};
		if (focus && setting.Kind != "String" && setting.Kind != "Type")
		{
			ImGui::SetKeyboardFocusHere(0);
		}

		if (setting.Kind == "Bool")
		{
			Track(frame, label,
			      [&]() -> std::optional<Edit>
			      {
					  bool checked = value.is_boolean() && value.get<bool>();
					  return ImGui::Checkbox("##value", &checked) ? setSetting(checked) : std::nullopt;
				  });
		}
		else if (setting.Kind == "Int" && node.Type == "Flow.Sequence" && setting.Name == "Count")
		{
			DrawSequenceOutputs(frame, node, title, setting);
		}
		else if (setting.Kind == "Int")
		{
			Track(frame, label,
			      [&]() -> std::optional<Edit>
			      {
					  auto const min = static_cast<int>(setting.Min.value_or(std::numeric_limits<int>::min()));
					  auto const max = static_cast<int>(setting.Max.value_or(std::numeric_limits<int>::max()));
					  int number = value.is_number_integer() ? static_cast<int>(std::clamp<int64_t>(value.get<int64_t>(), min, max)) : 0;
					  if (!ImGui::DragInt("##value", &number, 0.1f, min, max, "%d", ImGuiSliderFlags_AlwaysClamp))
					  {
						  return std::nullopt;
					  }
					  return setSetting(number);
				  });
		}
		else if (setting.Kind == "String" && node.Type == "Event.Custom" && setting.Name == "Name")
		{
			BeginRow(label);
			ImGui::SetNextItemWidth(-FLT_MIN);
			std::string const current = value.is_string() ? value.get<std::string>() : std::string();
			VisualScriptNameField::Outcome const outcome = m_Names.Draw(
				"##name", current,
				[&](std::string_view name) -> std::optional<std::string>
				{
					if (!VisualScriptSerializer::IsValidName(name))
					{
						return std::string(NameRuleMessage);
					}
					if (VisualScriptEditing::IsCustomEventNameTaken(document, name))
					{
						return fmt::format("a custom event is named {} already", name);
					}
					VisualScriptDocument copy = document;
					Result<uint32_t> const renamed = VisualScriptEditing::RenameCustomEvent(copy, node.ID, std::string(name));
					return renamed ? std::nullopt : std::optional<std::string>(renamed.GetError());
				},
				focus);
			EndRow(label);
			if (outcome.Committed)
			{
				VisualScriptDocument edited = document;
				Result<uint32_t> const senders = VisualScriptEditing::RenameCustomEvent(edited, node.ID, *outcome.Committed);
				if (senders)
				{
					std::string description = fmt::format("Rename Event {} to {}", current, *outcome.Committed);
					if (senders.GetValue() > 0)
					{
						description += fmt::format(" (and {} {})", senders.GetValue(), senders.GetValue() == 1 ? "sender" : "senders");
					}
					m_Commits.Commit(context, std::move(edited), std::move(description), 0, frame.Result);
				}
			}
			else if (outcome.Refused)
			{
				frame.Result.Notes.push_back(fmt::format("{} keeps its name: {}", current, *outcome.Refused));
			}
		}
		else if (setting.Kind == "String")
		{
			bool const format = node.Type == "FormatString" && setting.Name == "Format";
			if (focus)
			{
				ImGui::SetKeyboardFocusHere(0);
			}
			Track(frame, label,
			      [&]() -> std::optional<Edit>
			      {
					  std::string text = value.is_string() ? value.get<std::string>() : std::string();
					  bool const changed = format ? ImGui::InputTextMultiline("##value", &text,
				                                                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3.0f +
				                                                                                   ImGui::GetStyle().FramePadding.y * 2.0f))
				                                  : ImGui::InputText("##value", &text);
					  return changed ? setSetting(std::move(text)) : std::nullopt;
				  });
			if (format && frame.Analysis != nullptr)
			{
				// The inputs the runtime made of its placeholders.
				std::vector<std::string> placeholders;
				if (VisualScriptAnalyzedNode const* const analyzed = frame.Analysis->FindNode(node.ID))
				{
					for (VisualScriptPin const& pin : analyzed->Pins)
					{
						if (pin.Direction == VisualScriptPinDirection::In && pin.Type != VisualScriptExecType)
						{
							placeholders.push_back(pin.Name);
						}
					}
				}
				std::string text;
				for (size_t i = 0; i < placeholders.size(); i++)
				{
					text += (i > 0 ? ", {" : "{") + placeholders[i] + "}";
				}
				TextRow("Placeholders", placeholders.empty() ? std::string("None: write {Name} to add an input") : text);
			}
		}
		else if (setting.Kind == "Type")
		{
			BeginRow(label);
			ImGui::BeginGroup();
			std::optional<std::string> const chosen =
				DrawTypePicker(frame, label, value.is_string() ? value.get<std::string>() : std::string(), VisualScriptTypeUse::Setting,
			                   setting.Constraint, 0.0f, focus);
			ImGui::EndGroup();
			EndRow(label);
			if (chosen)
			{
				if (std::optional<Edit> edit = setSetting(*chosen))
				{
					m_Commits.Commit(context, std::move(edit->Document), std::move(edit->Action), 0, frame.Result);
				}
			}
		}
		else if (setting.Kind == "TypeList")
		{
			std::vector<std::string> types;
			if (value.is_array())
			{
				for (Json const& entry : value)
				{
					types.push_back(entry.is_string() ? entry.get<std::string>() : std::string());
				}
			}
			if (types.empty())
			{
				types.emplace_back();
			}
			for (size_t i = 0; i < types.size(); i++)
			{
				std::string const entryLabel = types.size() == 1 ? label : fmt::format("{} {}", label, i + 1);
				ImGui::PushID(static_cast<int>(i));
				BeginRow(entryLabel);
				ImGui::BeginGroup();
				std::optional<std::string> const chosen =
					DrawTypePicker(frame, entryLabel, types[i], VisualScriptTypeUse::Setting, setting.Constraint);
				ImGui::EndGroup();
				EndRow(entryLabel);
				ImGui::PopID();
				if (chosen)
				{
					std::vector<std::string> changed = types;
					changed[i] = *chosen;
					if (std::optional<Edit> edit = setSetting(Json(changed)))
					{
						m_Commits.Commit(context, std::move(edit->Document), std::move(edit->Action), 0, frame.Result);
					}
				}
			}
		}
		else if (setting.Kind == "Variable")
		{
			// The variables, and the locals of the function whose graph holds the node.
			std::vector<std::pair<uint32_t, std::string>> items;
			for (VisualScriptVariable const& variable : document.Variables)
			{
				items.emplace_back(variable.ID, fmt::format("{} \xC2\xB7 {}", variable.Name, DescribeType(frame.Library, variable.Type)));
			}
			if (std::optional<uint32_t> const owner = FindVisualScriptElementGraph(document, node.ID); owner && *owner != 0)
			{
				if (VisualScriptFunction const* const function = FindFunction(document, *owner))
				{
					for (VisualScriptLocal const& local : function->Locals)
					{
						items.emplace_back(local.ID,
						                   fmt::format("{} \xC2\xB7 {} (local)", local.Name, DescribeType(frame.Library, local.Type)));
					}
				}
			}
			Track(frame, label,
			      [&]() -> std::optional<Edit>
			      {
					  uint64_t const current = value.is_number_unsigned() || value.is_number_integer() ? value.get<uint64_t>() : 0;
					  auto const selected = std::find_if(items.begin(), items.end(),
				                                         [current](auto const& item)
				                                         {
															 return item.first == current;
														 });
					  std::string const preview = selected != items.end() ? selected->second : std::string("None");
					  std::optional<Edit> edit;
					  if (ImGui::BeginCombo("##value", preview.c_str()))
					  {
						  for (auto const& [id, text] : items)
						  {
							  ImGui::PushID(static_cast<int>(id));
							  if (ImGui::Selectable(text.c_str(), id == current))
							  {
								  edit = setSetting(id);
							  }
							  ImGui::PopID();
						  }
						  ImGui::EndCombo();
					  }
					  return edit;
				  });
		}
		else if (setting.Kind == "Function")
		{
			Track(frame, label,
			      [&]() -> std::optional<Edit>
			      {
					  uint64_t const current = value.is_number_unsigned() || value.is_number_integer() ? value.get<uint64_t>() : 0;
					  auto const describe = [&frame](VisualScriptFunction const& function)
					  {
						  std::string text = function.Name + " (";
						  for (size_t i = 0; i < function.Inputs.size(); i++)
						  {
							  text += (i > 0 ? ", " : "") + DescribeType(frame.Library, function.Inputs[i].Type);
						  }
						  text += ")";
						  if (!function.Outputs.empty())
						  {
							  text += " : ";
							  for (size_t i = 0; i < function.Outputs.size(); i++)
							  {
								  text += (i > 0 ? ", " : "") + DescribeType(frame.Library, function.Outputs[i].Type);
							  }
						  }
						  return text;
					  };
					  VisualScriptFunction const* const selected = FindFunction(document, static_cast<uint32_t>(current));
					  std::string const preview = selected != nullptr ? describe(*selected) : std::string("None");
					  std::optional<Edit> edit;
					  if (ImGui::BeginCombo("##value", preview.c_str()))
					  {
						  for (VisualScriptFunction const& function : document.Functions)
						  {
							  ImGui::PushID(static_cast<int>(function.ID));
							  if (ImGui::Selectable(describe(function).c_str(), function.ID == current))
							  {
								  edit = setSetting(function.ID);
							  }
							  ImGui::PopID();
						  }
						  ImGui::EndCombo();
					  }
					  return edit;
				  });
		}
		else if (setting.Kind == "Parameters")
		{
			std::vector<VisualScriptParameter> parameters;
			if (value.is_array())
			{
				for (Json const& entry : value)
				{
					if (entry.is_object())
					{
						parameters.push_back({GetString(entry, "Name"), GetString(entry, "Type")});
					}
				}
			}
			if (auto changed = DrawParameters(frame, setting.Name, parameters, "Parameter"))
			{
				VisualScriptDocument edited = document;
				Result<uint32_t> const removed = VisualScriptEditing::SetEventParameters(edited, node.ID, std::move(changed->first));
				if (!removed)
				{
					frame.Result.Notes.push_back(fmt::format("{} failed: {}", changed->second, removed.GetError()));
				}
				else
				{
					m_Commits.Commit(context, std::move(edited),
					                 fmt::format("{} of {}{}", changed->second, title, RemovesLinks(removed.GetValue())), 0, frame.Result);
				}
			}
		}
		else if (setting.Kind == "Cases")
		{
			DrawCases(frame, node, title);
		}
		else if (setting.Kind == "Value")
		{
			std::string const type = GetString(node.Settings, "Type");
			if (type.empty())
			{
				TextRow(label, "Choose a Type first");
			}
			else
			{
				Track(frame, label,
				      [&]() -> std::optional<Edit>
				      {
						  std::optional<Json> literal = DrawLiteral(frame, type, value, Json());
						  return literal ? setSetting(std::move(*literal)) : std::nullopt;
					  });
			}
		}
		else if (setting.Kind == "Enum")
		{
			Track(frame, label,
			      [&]() -> std::optional<Edit>
			      {
					  std::optional<Json> literal = DrawLiteral(frame, setting.Type, value, setting.Default);
					  return literal ? setSetting(std::move(*literal)) : std::nullopt;
				  });
		}
		else if (setting.Kind == "Choice")
		{
			DrawChoice(frame, node, title, setting);
		}
		else
		{
			TextRow(label, value.dump());
		}
	}

	void VisualScriptDetails::DrawSequenceOutputs(Frame& frame, VisualScriptNode const& node, std::string const& title,
	                                              VisualScriptSetting const& setting)
	{
		VisualScriptDocument const& document = frame.Document;
		Json const value = GetSettingValue(node, setting);
		auto const min = static_cast<int>(setting.Min.value_or(2));
		auto const max = static_cast<int>(setting.Max.value_or(64));
		int const count = value.is_number_integer() ? static_cast<int>(std::clamp<int64_t>(value.get<int64_t>(), min, max)) : min;
		Track(frame, "Count",
		      [&]() -> std::optional<Edit>
		      {
				  int number = count;
				  if (!ImGui::DragInt("##count", &number, 0.1f, min, max, "%d", ImGuiSliderFlags_AlwaysClamp) || number == count)
				  {
					  return std::nullopt;
				  }
				  Edit edit{document, {}};
				  Result<uint32_t> const removed =
					  VisualScriptEditing::SetSequenceOutputs(edit.Document, node.ID, number, std::nullopt, std::nullopt);
				  if (!removed)
				  {
					  return std::nullopt;
				  }
				  edit.Action = fmt::format("Set Count on {}{}", title, RemovesLinks(removed.GetValue()));
				  return edit;
			  });
		std::optional<int> remove;
		for (int output = 0; output < count; output++)
		{
			std::string const name = fmt::format("Then{}", output);
			ImGui::PushID(output);
			BeginRow(name);
			ImGui::BeginDisabled(count <= min);
			if (ImGui::SmallButton("Remove Output"))
			{
				remove = output;
			}
			ImGui::EndDisabled();
			EndRow(name);
			ImGui::PopID();
		}
		BeginRow("");
		ImGui::BeginDisabled(count >= max);
		bool const add = ImGui::SmallButton("+ Add Output");
		ImGui::EndDisabled();
		EndRow("Add Output");
		if (remove || add)
		{
			VisualScriptDocument edited = document;
			Result<uint32_t> const removed =
				remove ? VisualScriptEditing::SetSequenceOutputs(edited, node.ID, count - 1, std::nullopt, *remove)
					   : VisualScriptEditing::SetSequenceOutputs(edited, node.ID, count + 1, count, std::nullopt);
			if (!removed)
			{
				frame.Result.Notes.push_back(fmt::format("Could not change the outputs of {}: {}", title, removed.GetError()));
				return;
			}
			std::string action = remove ? fmt::format("Remove Output Then{} of {}{}", *remove, title, RemovesLinks(removed.GetValue()))
			                            : fmt::format("Add Output to {}", title);
			m_Commits.Commit(frame.Context, std::move(edited), std::move(action), 0, frame.Result);
		}
	}

	void VisualScriptDetails::DrawCases(Frame& frame, VisualScriptNode const& node, std::string const& title)
	{
		VisualScriptDocument const& document = frame.Document;
		std::string const type = GetString(node.Settings, "Type");
		auto const found = node.Settings.find("Cases");
		Json const cases = found != node.Settings.end() ? *found : Json();
		std::string const action = fmt::format("Set Cases on {}", title);
		auto const commit = [&](Json changed)
		{
			VisualScriptDocument edited = document;
			Result<uint32_t> const removed = VisualScriptEditing::SetSwitchCases(edited, node.ID, std::move(changed));
			if (!removed)
			{
				frame.Result.Notes.push_back(fmt::format("{} failed: {}", action, removed.GetError()));
				return;
			}
			m_Commits.Commit(frame.Context, std::move(edited), action + RemovesLinks(removed.GetValue()), 0, frame.Result);
		};
		if (type.empty())
		{
			TextRow("Cases", "Choose a Type first");
			return;
		}
		std::vector<std::string> const enumerators = GetEnumeratorNames(frame.Library, type);
		if (!enumerators.empty())
		{
			// An enum: every enumerator (the setting omitted), or the ones checked.
			bool every = !cases.is_array();
			BeginRow("Cases");
			if (ImGui::Checkbox("Every enumerator", &every))
			{
				commit(every ? Json() : Json(enumerators));
			}
			EndRow("Cases");
			if (every)
			{
				return;
			}
			for (std::string const& enumerator : enumerators)
			{
				bool checked = std::find(cases.begin(), cases.end(), Json(enumerator)) != cases.end();
				ImGui::PushID(enumerator.c_str());
				BeginRow(enumerator);
				if (ImGui::Checkbox("##case", &checked))
				{
					// In declaration order.
					Json changed = Json::array();
					for (std::string const& candidate : enumerators)
					{
						bool const keep =
							candidate == enumerator ? checked : std::find(cases.begin(), cases.end(), Json(candidate)) != cases.end();
						if (keep)
						{
							changed.push_back(candidate);
						}
					}
					commit(std::move(changed));
				}
				EndRow(enumerator);
				ImGui::PopID();
			}
			return;
		}
		bool const integers = type == "int";
		Json const list = cases.is_array() ? cases : Json::array();
		std::optional<size_t> remove;
		for (size_t i = 0; i < list.size(); i++)
		{
			std::string const caseLabel = fmt::format("Case {}", i + 1);
			ImGui::PushID(static_cast<int>(i));
			Track(frame, caseLabel,
			      [&]() -> std::optional<Edit>
			      {
					  float const button =
						  ImGui::CalcTextSize("Remove").x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
					  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - button);
					  Json changed = list;
					  bool edited = false;
					  if (integers)
					  {
						  int number = list[i].is_number_integer() ? static_cast<int>(list[i].get<int64_t>()) : 0;
						  edited = ImGui::InputInt("##case", &number, 0, 0);
						  changed[i] = number;
					  }
					  else
					  {
						  std::string text = list[i].is_string() ? list[i].get<std::string>() : std::string();
						  edited = ImGui::InputText("##case", &text);
						  changed[i] = text;
					  }
					  ImGui::SameLine();
					  if (ImGui::SmallButton("Remove"))
					  {
						  remove = i;
					  }
					  if (!edited)
					  {
						  return std::nullopt;
					  }
					  Edit edit{document, {}};
					  Result<uint32_t> const removed = VisualScriptEditing::SetSwitchCases(edit.Document, node.ID, std::move(changed));
					  if (!removed)
					  {
						  return std::nullopt;
					  }
					  edit.Action = action + RemovesLinks(removed.GetValue());
					  return edit;
				  });
			ImGui::PopID();
		}
		BeginRow("");
		bool const add = ImGui::SmallButton("+ Add Case");
		EndRow("Add Case");
		if (remove)
		{
			Json changed = list;
			changed.erase(changed.begin() + static_cast<std::ptrdiff_t>(*remove));
			commit(std::move(changed));
		}
		else if (add)
		{
			Json changed = list;
			if (integers)
			{
				int64_t next = 0;
				for (Json const& entry : list)
				{
					if (entry.is_number_integer())
					{
						next = std::max(next, entry.get<int64_t>() + 1);
					}
				}
				changed.push_back(next);
			}
			else
			{
				std::string const name = VisualScriptEditing::MakeUniqueVisualScriptName(
					"Case",
					[&list](std::string_view candidate)
					{
						return std::find(list.begin(), list.end(), Json(candidate)) != list.end();
					});
				changed.push_back(name);
			}
			commit(std::move(changed));
		}
	}

	void VisualScriptDetails::DrawChoice(Frame& frame, VisualScriptNode const& node, std::string const& title,
	                                     VisualScriptSetting const& setting)
	{
		std::string const label = UI::FormatDisplayName(setting.Name);
		Json const value = GetSettingValue(node, setting);
		std::string const current = value.is_string() ? value.get<std::string>() : std::string();
		bool const easing = node.Type == "Flow.Tween" && setting.Name == "Easing";
		Track(frame, label,
		      [&]() -> std::optional<Edit>
		      {
				  std::optional<Edit> edit;
				  if (!ImGui::BeginCombo("##value", current.c_str()))
				  {
					  return edit;
				  }
				  float const scale = ImGui::GetStyle().FontScaleDpi;
				  ImVec2 const plot(EasingPlotWidth * scale, EasingPlotHeight * scale);
				  for (std::string const& choice : setting.Choices)
				  {
					  ImGui::PushID(choice.c_str());
					  float const height = easing ? std::max(plot.y, ImGui::GetTextLineHeight()) : 0.0f;
					  if (ImGui::Selectable(choice.c_str(), choice == current, ImGuiSelectableFlags_None, ImVec2(0.0f, height)))
					  {
						  Edit changed{frame.Document, fmt::format("Set {} on {}", label, title)};
						  if (VisualScriptEditing::SetSetting(changed.Document, node.ID, setting.Name, choice))
						  {
							  edit = std::move(changed);
						  }
					  }
					  if (easing)
					  {
						  ImVec2 const itemMin = ImGui::GetItemRectMin();
						  ImVec2 const itemMax = ImGui::GetItemRectMax();
						  ImVec2 const min(itemMax.x - plot.x - ImGui::GetStyle().FramePadding.x,
					                       itemMin.y + (itemMax.y - itemMin.y - plot.y) * 0.5f);
						  DrawEasingPlot(choice, min, ImVec2(min.x + plot.x, min.y + plot.y), ImGui::GetColorU32(ImGuiCol_Text));
					  }
					  ImGui::PopID();
				  }
				  ImGui::EndCombo();
				  return edit;
			  });
	}

	void VisualScriptDetails::DrawInputs(Frame& frame, VisualScriptNode const& node, std::string const& title)
	{
		VisualScriptEditorModel const& model = frame.Context.Model;
		VisualScriptCanvasModel const& canvas = model.GetCanvas();
		std::optional<uint32_t> const index = canvas.FindNode(node.ID);
		if (!index || FindVisualScriptElementGraph(frame.Document, node.ID) != model.GetFunction())
		{
			return;
		}
		std::span<VisualScriptNodeVisual const> const nodes = canvas.GetNodes();
		VisualScriptNodeVisual const& visual = nodes[*index];
		std::span<VisualScriptPinVisual const> const pins = canvas.GetPins(visual);
		bool const any = std::any_of(pins.begin(), pins.end(),
		                             [](VisualScriptPinVisual const& pin)
		                             {
										 return pin.Direction == VisualScriptPinDirection::In && pin.Type != VisualScriptExecType &&
			                                    !pin.HasFlag(VisualScriptPinFlags::Orphan);
									 });
		if (!any)
		{
			return;
		}
		ImGui::SeparatorText("Inputs");
		VisualScriptAnalyzedNode const* const analyzed = frame.Analysis != nullptr ? frame.Analysis->FindNode(node.ID) : nullptr;
		VisualScriptGraph const* const graph = FindVisualScriptGraph(frame.Document, model.GetFunction());
		for (VisualScriptPinVisual const& pin : pins)
		{
			if (pin.Direction != VisualScriptPinDirection::In || pin.Type == VisualScriptExecType ||
			    pin.HasFlag(VisualScriptPinFlags::Orphan))
			{
				continue;
			}
			std::string const name(canvas.GetText(pin.Name));
			std::string const shown(canvas.GetText(pin.Label));
			std::string const label = shown.empty() ? name : shown;
			std::string const stored = VisualScriptEditing::ToStoredVisualScriptPin(analyzed, VisualScriptPinDirection::In, name);
			auto const literalEntry = node.Inputs.is_object() ? node.Inputs.find(stored) : node.Inputs.end();
			Json const literal = node.Inputs.is_object() && literalEntry != node.Inputs.end() ? *literalEntry : Json();
			ImGui::PushID(stored.c_str());
			if (pin.HasFlag(VisualScriptPinFlags::Linked))
			{
				std::optional<VisualScriptLink> link;
				if (graph != nullptr)
				{
					auto const found = std::find_if(graph->Links.begin(), graph->Links.end(),
					                                [&node, &stored](VisualScriptLink const& candidate)
					                                {
														return candidate.To.Node == node.ID && candidate.To.Pin == stored;
													});
					if (found != graph->Links.end())
					{
						link = *found;
					}
				}
				VisualScriptNode const* const source = link ? FindVisualScriptNode(frame.Document, link->From.Node) : nullptr;
				std::string const text =
					fmt::format("{} \xE2\x86\x90 {}", label,
				                source != nullptr ? GetVisualScriptNodeTitle(frame.Document, frame.Analysis, *source) : "?");
				ImVec2 const size = UI::CalcArrowTextSize(text);
				ImVec2 const position = ImGui::GetCursorScreenPos();
				ViewScope const view(frame.ReadOnly);
				if (ImGui::InvisibleButton("##source", ImVec2(std::max(size.x, 1.0f), ImGui::GetFrameHeight())) && source != nullptr)
				{
					frame.Result.SelectAndFrame = {source->ID};
				}
				uint32_t const color = ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_Text : ImGuiCol_TextDisabled);
				UI::DrawArrowText(ImGui::GetWindowDrawList(), ImVec2(position.x, position.y + ImGui::GetStyle().FramePadding.y), color,
				                  text);
				m_Rows.emplace_back(label, std::pair(glm::vec2(position.x, position.y),
				                                     glm::vec2(position.x + size.x, position.y + ImGui::GetFrameHeight())));
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
				{
					ImGui::SetTooltip("Select the node it comes from");
				}
				if (!literal.is_null())
				{
					ImGui::SameLine();
					ImGui::TextDisabled("(ignored while linked)");
				}
			}
			else if (pin.HasFlag(VisualScriptPinFlags::DefaultsToSelf) && literal.is_null())
			{
				TextRow(label, "Self: the entity running the graph");
			}
			else if (VisualScriptLiteralEditor::CanEdit(pin.Type, frame.Library))
			{
				std::optional<VisualScriptPin> const libraryPin =
					FindVisualScriptCanvasPin(model, frame.Library, node.ID, name, VisualScriptPinDirection::In);
				Json const fallback = libraryPin ? libraryPin->Default : Json();
				std::string const action = fmt::format("Set {} on {}", label, title);
				bool const resettable = !literal.is_null();
				float const reset =
					resettable ? ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x
							   : 0.0f;
				Track(frame, label,
				      [&]() -> std::optional<Edit>
				      {
						  if (reset > 0.0f)
						  {
							  ImGui::PushItemWidth(-reset);
						  }
						  std::optional<Json> changed = DrawLiteral(frame, pin.Type, literal, fallback);
						  if (reset > 0.0f)
						  {
							  ImGui::PopItemWidth();
						  }
						  if (!changed)
						  {
							  return std::nullopt;
						  }
						  Edit edit{frame.Document, action};
						  if (!VisualScriptEditing::SetInput(edit.Document, node.ID, stored, std::move(*changed)))
						  {
							  return std::nullopt;
						  }
						  return edit;
					  });
				if (resettable)
				{
					ImGui::SameLine();
					if (ImGui::SmallButton("Reset"))
					{
						VisualScriptDocument edited = frame.Document;
						if (VisualScriptEditing::SetInput(edited, node.ID, stored, Json()))
						{
							m_Commits.Commit(frame.Context, std::move(edited), fmt::format("Reset {} on {}", label, title), 0,
							                 frame.Result);
						}
					}
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
					{
						ImGui::SetTooltip("Remove the value: the input takes its default");
					}
				}
			}
			else
			{
				TextRow(label, "Link a value");
			}
			ImGui::PopID();
		}
	}

	void VisualScriptDetails::DrawNodeProblems(Frame& frame, VisualScriptNode const& node)
	{
		std::vector<VisualScriptDiagnostic> const problems = GetVisualScriptNodeProblems(frame.Analysis, node.ID);
		if (problems.empty())
		{
			return;
		}
		ImGui::SeparatorText("Problems");
		ImGui::PushID("problems");
		// The fixes that edit disable themselves in a read-only document; those that navigate stay.
		ViewScope const view(frame.ReadOnly);
		DrawVisualScriptProblems(frame.Context, problems, m_Commits, frame.Result);
		ImGui::PopID();
	}

	void VisualScriptDetails::DrawAbout(Frame& frame, VisualScriptNode const& node)
	{
		VisualScriptNodeTemplate const* const kind = frame.Library != nullptr ? frame.Library->FindNode(node.Type) : nullptr;
		size_t const colon = node.Type.find(':');
		bool const reflected = colon != std::string::npos;
		if ((kind == nullptr || kind->Description.empty()) && !reflected)
		{
			return;
		}
		ImGui::SeparatorText("About");
		if (kind != nullptr && !kind->Description.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextWrapped("%s", kind->Description.c_str());
			ImGui::PopStyleColor();
		}
		if (reflected)
		{
			ImGui::TextDisabled("C# member:");
			ImFont* const monospace = frame.Context.Services.MonospaceFont;
			if (monospace != nullptr)
			{
				ImGui::PushFont(monospace, 0.0f);
			}
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextWrapped("%s", node.Type.substr(colon + 1).c_str());
			ImGui::PopStyleColor();
			if (monospace != nullptr)
			{
				ImGui::PopFont();
			}
		}
	}

	void VisualScriptDetails::DrawComment(Frame& frame, VisualScriptComment const& comment)
	{
		VisualScriptDocument const& document = frame.Document;
		ImGui::PushID(static_cast<int>(comment.ID));
		ImGui::TextUnformatted("Comment Box");
		Track(frame, "Text",
		      [&]() -> std::optional<Edit>
		      {
				  std::string text = comment.Text;
				  float const height = ImGui::GetTextLineHeight() * 3.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
				  if (!ImGui::InputTextMultiline("##text", &text, ImVec2(-FLT_MIN, height)))
				  {
					  return std::nullopt;
				  }
				  Edit edit{document, "Rename Comment"};
				  if (!VisualScriptEditing::SetCommentText(edit.Document, comment.ID, std::move(text)))
				  {
					  return std::nullopt;
				  }
				  return edit;
			  });
		BeginRow("Colour");
		ImGui::BeginGroup();
		std::optional<glm::vec4> swatch;
		for (size_t i = 0; i < VisualScriptCommentSwatches.size(); i++)
		{
			VisualScriptCommentSwatch const& candidate = VisualScriptCommentSwatches[i];
			ImGui::PushID(static_cast<int>(i));
			if (i > 0)
			{
				ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
			}
			glm::vec4 const color = candidate.Color;
			ImVec4 const shown(Math::LinearToSrgb(color.r), Math::LinearToSrgb(color.g), Math::LinearToSrgb(color.b), 1.0f);
			std::string const name(candidate.Name);
			if (ImGui::ColorButton(name.c_str(), shown, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha,
			                       ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight())))
			{
				swatch = color;
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
			{
				ImGui::SetTooltip("%s", name.c_str());
			}
			ImGui::PopID();
		}
		ImGui::EndGroup();
		EndRow("Colour");
		if (swatch)
		{
			VisualScriptDocument edited = document;
			if (VisualScriptEditing::SetCommentColor(edited, comment.ID, *swatch))
			{
				frame.Context.Settings.SetCommentColor(*swatch);
				m_Commits.Commit(frame.Context, std::move(edited), "Recolour Comment", 0, frame.Result);
			}
		}
		Track(frame, "Custom Colour",
		      [&]() -> std::optional<Edit>
		      {
				  // Shown in sRGB, stored linear.
				  std::array<float, 4> shown = {Math::LinearToSrgb(comment.Color.r), Math::LinearToSrgb(comment.Color.g),
			                                    Math::LinearToSrgb(comment.Color.b), comment.Color.a};
				  if (!ImGui::ColorEdit4("##colour", shown.data(), ImGuiColorEditFlags_AlphaBar))
				  {
					  return std::nullopt;
				  }
				  glm::vec4 const linear(Math::SrgbToLinear(shown[0]), Math::SrgbToLinear(shown[1]), Math::SrgbToLinear(shown[2]),
			                             shown[3]);
				  Edit edit{document, "Recolour Comment"};
				  if (!VisualScriptEditing::SetCommentColor(edit.Document, comment.ID, linear))
				  {
					  return std::nullopt;
				  }
				  frame.Context.Settings.SetCommentColor(glm::clamp(linear, glm::vec4(0.0f), glm::vec4(1.0f)));
				  return edit;
			  });
		Track(frame, "Position",
		      [&]() -> std::optional<Edit>
		      {
				  glm::vec2 position = comment.Position;
				  if (!ImGui::DragFloat2("##position", &position.x, 1.0f, 0.0f, 0.0f, "%.0f"))
				  {
					  return std::nullopt;
				  }
				  Edit edit{document, "Move Comment"};
				  if (!VisualScriptEditing::SetCommentGeometry(edit.Document, comment.ID, SnapToGrid(position), comment.Size))
				  {
					  return std::nullopt;
				  }
				  return edit;
			  });
		Track(frame, "Size",
		      [&]() -> std::optional<Edit>
		      {
				  glm::vec2 size = comment.Size;
				  if (!ImGui::DragFloat2("##size", &size.x, 1.0f, VisualScriptSerializer::MinCommentSize,
			                             VisualScriptSerializer::MaxCommentSize, "%.0f", ImGuiSliderFlags_AlwaysClamp))
				  {
					  return std::nullopt;
				  }
				  Edit edit{document, "Resize Comment"};
				  glm::vec2 const snapped = glm::max(SnapToGrid(size), glm::vec2(VisualScriptSerializer::MinCommentSize));
				  if (!VisualScriptEditing::SetCommentGeometry(edit.Document, comment.ID, comment.Position, snapped))
				  {
					  return std::nullopt;
				  }
				  return edit;
			  });
		ImGui::PopID();
	}

	void VisualScriptDetails::DrawSeveral(Frame& frame, std::span<uint32_t const> selection)
	{
		VisualScriptDocument const& document = frame.Document;
		VisualScriptEditorModel const& model = frame.Context.Model;
		uint32_t const function = model.GetFunction();
		std::vector<uint32_t> const elements(selection.begin(), selection.end());
		std::vector<uint32_t> nodes;
		VisualScriptGraph const* const graph = FindVisualScriptGraph(document, function);
		std::vector<VisualScriptNode const*> selectedNodes;
		if (graph != nullptr)
		{
			for (VisualScriptNode const& node : graph->Nodes)
			{
				if (std::find(elements.begin(), elements.end(), node.ID) != elements.end())
				{
					nodes.push_back(node.ID);
					selectedNodes.push_back(&node);
				}
			}
		}
		std::string const what = DescribeElements(document, function, elements);
		ImGui::TextUnformatted(fmt::format("{} selected", what).c_str());
		if (!nodes.empty())
		{
			std::string const nodeWords = nodes.size() == 1 ? GetVisualScriptNodeTitle(document, frame.Analysis, *selectedNodes.front())
			                                                : fmt::format("{} Nodes", nodes.size());
			bool const sameEnabled = std::all_of(selectedNodes.begin(), selectedNodes.end(),
			                                     [&selectedNodes](VisualScriptNode const* node)
			                                     {
													 return node->Enabled == selectedNodes.front()->Enabled;
												 });
			Track(frame, "Enabled",
			      [&]() -> std::optional<Edit>
			      {
					  constexpr std::array<char const*, 3> Labels = {"Enabled", "Disabled", "Development Only"};
					  char const* const preview =
						  sameEnabled ? Labels[static_cast<size_t>(selectedNodes.front()->Enabled)] : "\xE2\x80\x93";
					  std::optional<Edit> edit;
					  if (ImGui::BeginCombo("##enabled", preview))
					  {
						  for (size_t i = 0; i < Labels.size(); i++)
						  {
							  if (ImGui::Selectable(Labels[i], sameEnabled && static_cast<size_t>(selectedNodes.front()->Enabled) == i))
							  {
								  auto const enabled = static_cast<VisualScriptNodeEnabled>(i);
								  std::string action = enabled == VisualScriptNodeEnabled::Enabled ? "Enable " + nodeWords
							                           : enabled == VisualScriptNodeEnabled::Disabled
							                               ? "Disable " + nodeWords
							                               : "Set " + nodeWords + " to Development Only";
								  Edit changed{document, std::move(action)};
								  if (VisualScriptEditing::SetNodeEnabled(changed.Document, nodes, enabled))
								  {
									  edit = std::move(changed);
								  }
							  }
						  }
						  ImGui::EndCombo();
					  }
					  return edit;
				  });
			bool const allCollapsed = std::all_of(selectedNodes.begin(), selectedNodes.end(),
			                                      [](VisualScriptNode const* node)
			                                      {
													  return node->Collapsed;
												  });
			bool const noneCollapsed = std::none_of(selectedNodes.begin(), selectedNodes.end(),
			                                        [](VisualScriptNode const* node)
			                                        {
														return node->Collapsed;
													});
			Track(frame, "Collapsed",
			      [&]() -> std::optional<Edit>
			      {
					  bool collapsed = allCollapsed;
					  ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, !allCollapsed && !noneCollapsed);
					  bool const clicked = ImGui::Checkbox("##collapsed", &collapsed);
					  ImGui::PopItemFlag();
					  if (!clicked)
					  {
						  return std::nullopt;
					  }
					  Edit edit{document, (collapsed ? "Collapse " : "Expand ") + nodeWords};
					  if (!VisualScriptEditing::SetNodeCollapsed(edit.Document, nodes, collapsed))
					  {
						  return std::nullopt;
					  }
					  return edit;
				  });
		}

		ImGui::SeparatorText("Arrange");
		VisualScriptNodeExtents const extents = MakeExtents(model);
		struct AlignButton
		{
			char const* Label;
			VisualScriptAlignment Alignment;
			char const* Words;
		};
		constexpr std::array<AlignButton, 6> Aligns = {{{"Left", VisualScriptAlignment::Left, "Left"},
		                                                {"Right", VisualScriptAlignment::Right, "Right"},
		                                                {"Top", VisualScriptAlignment::Top, "Top"},
		                                                {"Bottom", VisualScriptAlignment::Bottom, "Bottom"},
		                                                {"Centres", VisualScriptAlignment::Centers, "Centres"},
		                                                {"Middles", VisualScriptAlignment::Middles, "Middles"}}};
		BeginRow("Align");
		ImGui::BeginGroup();
		for (size_t i = 0; i < Aligns.size(); i++)
		{
			if (i > 0 && i % 3 != 0)
			{
				ImGui::SameLine();
			}
			if (ImGui::SmallButton(Aligns[i].Label))
			{
				VisualScriptDocument edited = document;
				Result<void> const aligned = VisualScriptEditing::AlignElements(edited, function, elements, Aligns[i].Alignment, extents);
				if (aligned)
				{
					m_Commits.Commit(frame.Context, std::move(edited), fmt::format("Align {} {}", what, Aligns[i].Words), 0, frame.Result);
				}
				else
				{
					frame.Result.Notes.push_back(fmt::format("Could not align: {}", aligned.GetError()));
				}
			}
		}
		ImGui::EndGroup();
		EndRow("Align");
		BeginRow("Distribute");
		ImGui::BeginGroup();
		ImGui::BeginDisabled(elements.size() < 3);
		std::optional<VisualScriptDistribution> distribution;
		if (ImGui::SmallButton("Horizontally"))
		{
			distribution = VisualScriptDistribution::Horizontal;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Vertically"))
		{
			distribution = VisualScriptDistribution::Vertical;
		}
		ImGui::EndDisabled();
		ImGui::EndGroup();
		EndRow("Distribute");
		if (distribution)
		{
			VisualScriptDocument edited = document;
			Result<void> const distributed = VisualScriptEditing::DistributeElements(edited, function, elements, *distribution, extents);
			if (distributed)
			{
				m_Commits.Commit(frame.Context, std::move(edited), fmt::format("Distribute {}", what), 0, frame.Result);
			}
			else
			{
				frame.Result.Notes.push_back(fmt::format("Could not distribute: {}", distributed.GetError()));
			}
		}
		BeginRow("");
		if (ImGui::SmallButton("Comment Selection"))
		{
			VisualScriptDocument edited = document;
			Result<uint32_t> const added = VisualScriptEditing::AddComment(edited, function, "Comment", elements, extents, glm::vec2(0.0f),
			                                                               frame.Context.Settings.GetCommentColor());
			if (added)
			{
				m_Commits.Commit(frame.Context, std::move(edited), "Add Comment Box", 0, frame.Result, {added.GetValue()});
			}
			else
			{
				frame.Result.Notes.push_back(fmt::format("Could not add a comment box: {}", added.GetError()));
			}
		}
		EndRow("Comment Selection");
	}

	void VisualScriptDetails::DrawVariable(Frame& frame, VisualScriptVariable const& variable)
	{
		VisualScriptDocument const& document = frame.Document;
		VisualScriptPaneContext& context = frame.Context;
		VisualScriptLibrary const* const library = frame.Library;
		VisualScriptEnumLookup const enums = MakeEnumLookup(library);
		ImGui::PushID(static_cast<int>(variable.ID));
		ImGui::TextUnformatted("Variable");
		// A copy of the variable with a change, as SetVariable writes it; nothing when it refuses it.
		auto const setVariable = [&](VisualScriptVariable const& changed, std::string action) -> std::optional<Edit>
		{
			Edit edit{document, std::move(action)};
			Result<bool> const set = VisualScriptEditing::SetVariable(edit.Document, variable.ID, changed, enums);
			if (!set)
			{
				return std::nullopt;
			}
			if (set.GetValue())
			{
				edit.Action += " (clears its value)";
			}
			return edit;
		};

		BeginRow("Name");
		ImGui::SetNextItemWidth(-FLT_MIN);
		VisualScriptNameField::Outcome const name =
			m_Names.Draw("##name", variable.Name,
		                 [&](std::string_view candidate) -> std::optional<std::string>
		                 {
							 if (!VisualScriptSerializer::IsValidName(candidate))
							 {
								 return std::string(NameRuleMessage);
							 }
							 VisualScriptDocument copy = document;
							 VisualScriptVariable renamed = variable;
							 renamed.Name = std::string(candidate);
							 Result<bool> const set = VisualScriptEditing::SetVariable(copy, variable.ID, renamed, enums);
							 return set ? std::nullopt : std::optional<std::string>(set.GetError());
						 });
		EndRow("Name");
		if (name.Committed)
		{
			VisualScriptVariable renamed = variable;
			renamed.Name = *name.Committed;
			if (std::optional<Edit> edit = setVariable(renamed, fmt::format("Rename {} to {}", variable.Name, *name.Committed)))
			{
				m_Commits.Commit(context, std::move(edit->Document), std::move(edit->Action), 0, frame.Result);
			}
		}
		else if (name.Refused)
		{
			frame.Result.Notes.push_back(fmt::format("{} keeps its name: {}", variable.Name, *name.Refused));
		}

		BeginRow("Type");
		ImGui::BeginGroup();
		std::optional<std::string> const type = DrawTypePicker(
			frame, "Type", variable.Type, variable.Public ? VisualScriptTypeUse::PublicVariable : VisualScriptTypeUse::Value);
		ImGui::EndGroup();
		EndRow("Type");
		if (type && *type != variable.Type)
		{
			VisualScriptVariable changed = variable;
			changed.Type = *type;
			if (std::optional<Edit> edit =
			        setVariable(changed, fmt::format("Change {} to {}", variable.Name, DescribeType(library, *type))))
			{
				context.Settings.SetLastVariableType(*type);
				m_Commits.Commit(context, std::move(edit->Document), std::move(edit->Action), 0, frame.Result);
			}
		}

		bool const exposable = CanBePublic(variable.Type, library);
		Track(frame, "Public",
		      [&]() -> std::optional<Edit>
		      {
				  bool isPublic = variable.Public;
				  ImGui::BeginDisabled(!exposable && !variable.Public);
				  bool const clicked = ImGui::Checkbox("##public", &isPublic);
				  ImGui::EndDisabled();
				  if (!exposable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				  {
					  ImGui::SetTooltip("%s cannot be public: components cannot store it", variable.Type.c_str());
				  }
				  if (!clicked)
				  {
					  return std::nullopt;
				  }
				  VisualScriptVariable changed = variable;
				  changed.Public = isPublic;
				  return setVariable(changed, fmt::format("Make {} {}", variable.Name, isPublic ? "Public" : "Private"));
			  });
		if (variable.Public)
		{
			Track(frame, "Tooltip",
			      [&]() -> std::optional<Edit>
			      {
					  std::string text = variable.Tooltip;
					  if (!ImGui::InputText("##tooltip", &text))
					  {
						  return std::nullopt;
					  }
					  VisualScriptVariable changed = variable;
					  changed.Tooltip = std::move(text);
					  return setVariable(changed, fmt::format("Set Tooltip of {}", variable.Name));
				  });
			if (IsNumeric(variable.Type, library))
			{
				Track(frame, "Range",
				      [&]() -> std::optional<Edit>
				      {
						  bool ranged = variable.Range.has_value();
						  glm::vec2 range = variable.Range.value_or(glm::vec2(0.0f, 1.0f));
						  bool changed = ImGui::Checkbox("##ranged", &ranged);
						  if (ranged)
						  {
							  ImGui::SameLine();
							  ImGui::SetNextItemWidth(-FLT_MIN);
							  constexpr auto Limit = static_cast<float>(VisualScriptSerializer::MaxFloatMagnitude);
							  changed = ImGui::DragFloat2("##range", &range.x, 0.1f, -Limit, Limit, "%g", ImGuiSliderFlags_AlwaysClamp) ||
						                changed;
						  }
						  if (!changed)
						  {
							  return std::nullopt;
						  }
						  VisualScriptVariable edited = variable;
						  edited.Range = ranged ? std::optional<glm::vec2>(range) : std::nullopt;
						  return setVariable(edited, fmt::format("Set Range of {}", variable.Name));
					  });
			}
		}
		if (VisualScriptLiteralEditor::CanEdit(variable.Type, library))
		{
			Track(frame, "Default Value",
			      [&]() -> std::optional<Edit>
			      {
					  std::optional<Json> literal = DrawLiteral(frame, variable.Type, variable.Value, Json());
					  if (!literal)
					  {
						  return std::nullopt;
					  }
					  VisualScriptVariable changed = variable;
					  changed.Value = std::move(*literal);
					  return setVariable(changed, fmt::format("Set Default Value of {}", variable.Name));
				  });
		}
		else
		{
			TextRow("Default Value", "None");
		}
		if (!variable.FormerNames.empty())
		{
			ImGui::SeparatorText("Former Names");
			for (std::string const& former : variable.FormerNames)
			{
				ImGui::PushID(former.c_str());
				BeginRow(former);
				if (ImGui::SmallButton("Forget"))
				{
					VisualScriptVariable changed = variable;
					std::erase(changed.FormerNames, former);
					if (std::optional<Edit> edit = setVariable(changed, fmt::format("Forget Former Name {} of {}", former, variable.Name)))
					{
						m_Commits.Commit(context, std::move(edit->Document), std::move(edit->Action), 0, frame.Result);
					}
				}
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
				{
					ImGui::SetTooltip("Components that still use this name lose their value");
				}
				EndRow(former);
				ImGui::PopID();
			}
		}
		uint32_t const uses = CountUses(document, "Variable.Get", "Variable", variable.ID);
		ImGui::Spacing();
		ViewScope const view(frame.ReadOnly);
		if (ImGui::SmallButton(fmt::format("Used by {}", CountOf(uses, "node", "nodes")).c_str()))
		{
			frame.Result.FindReferences = VisualScriptReference{VisualScriptReference::Kind::Variable, variable.ID, {}};
		}
		DrawElementProblems(frame, variable.ID, std::nullopt);
		ImGui::PopID();
	}

	void VisualScriptDetails::DrawLocal(Frame& frame, VisualScriptFunction const& function, VisualScriptLocal const& local)
	{
		VisualScriptDocument const& document = frame.Document;
		VisualScriptPaneContext& context = frame.Context;
		VisualScriptEnumLookup const enums = MakeEnumLookup(frame.Library);
		ImGui::PushID(static_cast<int>(local.ID));
		ImGui::TextUnformatted(fmt::format("Local of {}", function.Name).c_str());
		auto const setLocal = [&](VisualScriptLocal const& changed, std::string action) -> std::optional<Edit>
		{
			Edit edit{document, std::move(action)};
			Result<bool> const set = VisualScriptEditing::SetLocal(edit.Document, function.ID, local.ID, changed, enums);
			if (!set)
			{
				return std::nullopt;
			}
			if (set.GetValue())
			{
				edit.Action += " (clears its value)";
			}
			return edit;
		};
		BeginRow("Name");
		ImGui::SetNextItemWidth(-FLT_MIN);
		VisualScriptNameField::Outcome const name =
			m_Names.Draw("##name", local.Name,
		                 [&](std::string_view candidate) -> std::optional<std::string>
		                 {
							 if (!VisualScriptSerializer::IsValidName(candidate))
							 {
								 return std::string(NameRuleMessage);
							 }
							 VisualScriptDocument copy = document;
							 VisualScriptLocal renamed = local;
							 renamed.Name = std::string(candidate);
							 Result<bool> const set = VisualScriptEditing::SetLocal(copy, function.ID, local.ID, renamed, enums);
							 return set ? std::nullopt : std::optional<std::string>(set.GetError());
						 });
		EndRow("Name");
		if (name.Committed)
		{
			VisualScriptLocal renamed = local;
			renamed.Name = *name.Committed;
			if (std::optional<Edit> edit = setLocal(renamed, fmt::format("Rename {} to {}", local.Name, *name.Committed)))
			{
				m_Commits.Commit(context, std::move(edit->Document), std::move(edit->Action), 0, frame.Result);
			}
		}
		else if (name.Refused)
		{
			frame.Result.Notes.push_back(fmt::format("{} keeps its name: {}", local.Name, *name.Refused));
		}
		BeginRow("Type");
		ImGui::BeginGroup();
		std::optional<std::string> const type = DrawTypePicker(frame, "Type", local.Type, VisualScriptTypeUse::Value);
		ImGui::EndGroup();
		EndRow("Type");
		if (type && *type != local.Type)
		{
			VisualScriptLocal changed = local;
			changed.Type = *type;
			if (std::optional<Edit> edit =
			        setLocal(changed, fmt::format("Change {} to {}", local.Name, DescribeType(frame.Library, *type))))
			{
				context.Settings.SetLastVariableType(*type);
				m_Commits.Commit(context, std::move(edit->Document), std::move(edit->Action), 0, frame.Result);
			}
		}
		if (VisualScriptLiteralEditor::CanEdit(local.Type, frame.Library))
		{
			Track(frame, "Default Value",
			      [&]() -> std::optional<Edit>
			      {
					  std::optional<Json> literal = DrawLiteral(frame, local.Type, local.Value, Json());
					  if (!literal)
					  {
						  return std::nullopt;
					  }
					  VisualScriptLocal changed = local;
					  changed.Value = std::move(*literal);
					  return setLocal(changed, fmt::format("Set Default Value of {}", local.Name));
				  });
		}
		uint32_t const uses = CountUses(document, "Variable.Get", "Variable", local.ID);
		ImGui::Spacing();
		ViewScope const view(frame.ReadOnly);
		if (ImGui::SmallButton(fmt::format("Used by {}", CountOf(uses, "node", "nodes")).c_str()))
		{
			frame.Result.FindReferences = VisualScriptReference{VisualScriptReference::Kind::Variable, local.ID, {}};
		}
		DrawElementProblems(frame, local.ID, std::nullopt);
		ImGui::PopID();
	}

	void VisualScriptDetails::DrawFunction(Frame& frame, VisualScriptFunction const& function)
	{
		VisualScriptDocument const& document = frame.Document;
		VisualScriptPaneContext& context = frame.Context;
		ImGui::PushID(static_cast<int>(function.ID));
		ImGui::TextUnformatted("Function");
		BeginRow("Name");
		ImGui::SetNextItemWidth(-FLT_MIN);
		VisualScriptNameField::Outcome const name =
			m_Names.Draw("##name", function.Name,
		                 [&](std::string_view candidate) -> std::optional<std::string>
		                 {
							 if (!VisualScriptSerializer::IsValidName(candidate))
							 {
								 return std::string(NameRuleMessage);
							 }
							 VisualScriptDocument copy = document;
							 Result<void> const renamed = VisualScriptEditing::RenameFunction(copy, function.ID, std::string(candidate));
							 return renamed ? std::nullopt : std::optional<std::string>(renamed.GetError());
						 });
		EndRow("Name");
		if (name.Committed)
		{
			VisualScriptDocument edited = document;
			if (VisualScriptEditing::RenameFunction(edited, function.ID, *name.Committed))
			{
				m_Commits.Commit(context, std::move(edited), fmt::format("Rename {} to {}", function.Name, *name.Committed), 0,
				                 frame.Result);
			}
		}
		else if (name.Refused)
		{
			frame.Result.Notes.push_back(fmt::format("{} keeps its name: {}", function.Name, *name.Refused));
		}
		Track(frame, "Pure",
		      [&]() -> std::optional<Edit>
		      {
				  bool pure = function.Pure;
				  if (!ImGui::Checkbox("##pure", &pure))
				  {
					  return std::nullopt;
				  }
				  Edit edit{document, {}};
				  Result<uint32_t> const removed = VisualScriptEditing::SetFunctionPure(edit.Document, function.ID, pure);
				  if (!removed)
				  {
					  return std::nullopt;
				  }
				  edit.Action = fmt::format("Make {} {}{}", function.Name, pure ? "Pure" : "Impure", RemovesLinks(removed.GetValue()));
				  return edit;
			  });
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
		{
			ImGui::SetTooltip("Pure functions have no flow pins and run whenever an output of their call is read");
		}
		auto const commitSignature =
			[&](std::vector<VisualScriptParameter> inputs, std::vector<VisualScriptParameter> outputs, std::string const& action)
		{
			VisualScriptDocument edited = document;
			Result<uint32_t> const removed =
				VisualScriptEditing::SetFunctionSignature(edited, function.ID, std::move(inputs), std::move(outputs));
			if (!removed)
			{
				frame.Result.Notes.push_back(fmt::format("{} failed: {}", action, removed.GetError()));
				return;
			}
			m_Commits.Commit(context, std::move(edited), fmt::format("{} of {}{}", action, function.Name, RemovesLinks(removed.GetValue())),
			                 0, frame.Result);
		};
		ImGui::SeparatorText("Inputs");
		if (auto changed = DrawParameters(frame, "inputs", function.Inputs, "Input"))
		{
			commitSignature(std::move(changed->first), function.Outputs, changed->second);
		}
		ImGui::SeparatorText("Outputs");
		if (auto changed = DrawParameters(frame, "outputs", function.Outputs, "Output"))
		{
			commitSignature(function.Inputs, std::move(changed->first), changed->second);
		}
		ImGui::SeparatorText("Locals");
		VisualScriptEnumLookup const enums = MakeEnumLookup(frame.Library);
		for (VisualScriptLocal const& local : function.Locals)
		{
			ImGui::PushID(static_cast<int>(local.ID));
			BeginRow(local.Name);
			ImGui::BeginGroup();
			std::optional<std::string> const type = DrawTypePicker(frame, local.Name, local.Type, VisualScriptTypeUse::Value);
			ImGui::EndGroup();
			EndRow(local.Name);
			if (type && *type != local.Type)
			{
				VisualScriptDocument edited = document;
				VisualScriptLocal changed = local;
				changed.Type = *type;
				Result<bool> const set = VisualScriptEditing::SetLocal(edited, function.ID, local.ID, changed, enums);
				if (set)
				{
					std::string action = fmt::format("Change {} to {}", local.Name, DescribeType(frame.Library, *type));
					if (set.GetValue())
					{
						action += " (clears its value)";
					}
					m_Commits.Commit(context, std::move(edited), std::move(action), 0, frame.Result);
				}
			}
			ImGui::PopID();
		}
		BeginRow("");
		if (ImGui::SmallButton("+ Add Local"))
		{
			VisualScriptDocument edited = document;
			std::string const localName = VisualScriptEditing::MakeUniqueVisualScriptName(NewLocalName,
			                                                                              [&edited, &function](std::string_view candidate)
			                                                                              {
																							  return VisualScriptEditing::IsLocalNameTaken(
																								  edited, function.ID, candidate);
																						  });
			Result<uint32_t> const added =
				VisualScriptEditing::AddLocal(edited, function.ID, localName, context.Settings.GetLastVariableType());
			if (added)
			{
				m_Commits.Commit(context, std::move(edited), fmt::format("Add Local {} to {}", localName, function.Name), 0, frame.Result);
			}
			else
			{
				frame.Result.Notes.push_back(fmt::format("Could not add a local: {}", added.GetError()));
			}
		}
		EndRow("Add Local");
		uint32_t const calls = CountUses(document, "Function.Call", "Function", function.ID);
		ImGui::Spacing();
		ViewScope const view(frame.ReadOnly);
		if (ImGui::SmallButton(fmt::format("Called by {}", CountOf(calls, "node", "nodes")).c_str()))
		{
			frame.Result.FindReferences = VisualScriptReference{VisualScriptReference::Kind::Function, function.ID, {}};
		}
		DrawElementProblems(frame, std::nullopt, function.ID);
		ImGui::PopID();
	}

	std::optional<std::pair<std::vector<VisualScriptParameter>, std::string>>
	VisualScriptDetails::DrawParameters(Frame& frame, std::string_view id, std::vector<VisualScriptParameter> const& parameters,
	                                    std::string_view noun)
	{
		std::optional<std::pair<std::vector<VisualScriptParameter>, std::string>> changed;
		ImGui::PushID(id.data(), id.data() + id.size());
		ImGuiStyle const& style = ImGui::GetStyle();
		for (size_t i = 0; i < parameters.size(); i++)
		{
			VisualScriptParameter const& parameter = parameters[i];
			std::string const label = fmt::format("{} {}", noun, i + 1);
			ImGui::PushID(static_cast<int>(i));
			BeginRow(label);
			ImGui::BeginGroup();
			float const buttons = ImGui::CalcTextSize("^v x").x + style.FramePadding.x * 6.0f + style.ItemSpacing.x * 3.0f;
			float const available = std::max(ImGui::GetContentRegionAvail().x - buttons, ImGui::GetFontSize() * 4.0f);
			ImGui::SetNextItemWidth(available * 0.5f);
			VisualScriptNameField::Outcome const name =
				m_Names.Draw("##name", parameter.Name,
			                 [&parameters, i](std::string_view candidate) -> std::optional<std::string>
			                 {
								 if (!VisualScriptSerializer::IsValidName(candidate))
								 {
									 return std::string(NameRuleMessage);
								 }
								 if (VisualScriptEditing::IsReservedPinName(candidate))
								 {
									 return fmt::format("{} is the name of a pin every node has", candidate);
								 }
								 std::vector<VisualScriptParameter> others = parameters;
								 others.erase(others.begin() + static_cast<std::ptrdiff_t>(i));
								 if (VisualScriptEditing::IsParameterNameTaken(others, candidate))
								 {
									 return fmt::format("a parameter is named {} already", candidate);
								 }
								 return std::nullopt;
							 });
			ImGui::SameLine();
			ImGui::BeginGroup();
			std::optional<std::string> const type =
				DrawTypePicker(frame, label, parameter.Type, VisualScriptTypeUse::Value, {}, available * 0.5f - style.ItemSpacing.x);
			ImGui::EndGroup();
			ImGui::SameLine();
			ImGui::BeginDisabled(i == 0);
			bool const up = ImGui::SmallButton("^");
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(i + 1 == parameters.size());
			bool const down = ImGui::SmallButton("v");
			ImGui::EndDisabled();
			ImGui::SameLine();
			bool const remove = ImGui::SmallButton("x");
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
			{
				ImGui::SetTooltip("Remove %s", parameter.Name.c_str());
			}
			ImGui::EndGroup();
			EndRow(label);
			ImGui::PopID();
			if (changed)
			{
				continue;
			}
			std::vector<VisualScriptParameter> list = parameters;
			if (name.Committed)
			{
				list[i].Name = *name.Committed;
				changed.emplace(std::move(list), fmt::format("Rename {} {} to {}", noun, parameter.Name, *name.Committed));
			}
			else if (name.Refused)
			{
				frame.Result.Notes.push_back(fmt::format("{} keeps its name: {}", parameter.Name, *name.Refused));
			}
			else if (type && *type != parameter.Type)
			{
				list[i].Type = *type;
				changed.emplace(std::move(list),
				                fmt::format("Change {} {} to {}", noun, parameter.Name, DescribeType(frame.Library, *type)));
			}
			else if (up || down)
			{
				size_t const other = up ? i - 1 : i + 1;
				std::swap(list[i], list[other]);
				changed.emplace(std::move(list), fmt::format("Move {} {} {}", noun, parameter.Name, up ? "Up" : "Down"));
			}
			else if (remove)
			{
				list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
				changed.emplace(std::move(list), fmt::format("Remove {} {}", noun, parameter.Name));
			}
		}
		BeginRow("");
		bool const add = ImGui::SmallButton(fmt::format("+ Add {}", noun).c_str());
		EndRow(fmt::format("Add {}", noun));
		if (add && !changed)
		{
			std::vector<VisualScriptParameter> list = parameters;
			std::string const name =
				VisualScriptEditing::MakeUniqueVisualScriptName(noun,
			                                                    [&list](std::string_view candidate)
			                                                    {
																	return VisualScriptEditing::IsParameterNameTaken(list, candidate);
																});
			list.push_back({name, frame.Context.Settings.GetLastVariableType()});
			changed.emplace(std::move(list), fmt::format("Add {} {}", noun, name));
		}
		ImGui::PopID();
		return changed;
	}

	void VisualScriptDetails::DrawElementProblems(Frame& frame, std::optional<uint32_t> variable, std::optional<uint32_t> function)
	{
		std::vector<VisualScriptDiagnostic> const problems = variable
		                                                         ? GetVisualScriptVariableProblems(frame.Analysis, *variable)
		                                                         : GetVisualScriptFunctionProblems(frame.Analysis, function.value_or(0));
		if (problems.empty())
		{
			return;
		}
		ImGui::SeparatorText("Problems");
		ImGui::PushID("problems");
		// The fixes that edit disable themselves in a read-only document; those that navigate stay.
		ViewScope const view(frame.ReadOnly);
		DrawVisualScriptProblems(frame.Context, problems, m_Commits, frame.Result);
		ImGui::PopID();
	}

	std::optional<Json> VisualScriptDetails::DrawLiteral(Frame& frame, std::string_view type, Json const& literal, Json const& fallback)
	{
		return m_Literals.Draw(type, literal, fallback, frame.Library);
	}

	std::optional<std::string> VisualScriptDetails::DrawTypePicker(Frame& frame, std::string_view label, std::string const& current,
	                                                               VisualScriptTypeUse use, std::string_view constraint, float width,
	                                                               bool focus)
	{
		VisualScriptLibrary const* const library = frame.Library;
		std::string const text = current.empty() ? std::string("Choose a type") : DescribeType(library, current);
		ColorSquare(current.empty() ? VisualScriptWildcardColor : GetVisualScriptTypeColor(current, library));
		ImGui::BeginDisabled(library == nullptr);
		float const buttonWidth =
			width > 0.0f ? width - ImGui::GetFontSize() * 0.6f - ImGui::GetStyle().ItemInnerSpacing.x : ImGui::GetContentRegionAvail().x;
		bool const clicked =
			ImGui::Button(fmt::format("{}##type", text).c_str(), ImVec2(std::max(buttonWidth, ImGui::GetFontSize()), 0.0f));
		bool const open = clicked || (focus && library != nullptr);
		ImGui::EndDisabled();
		if (library == nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
		{
			ImGui::SetTooltip("Types need the scripting runtime, which is not running");
		}
		if (open)
		{
			ImGui::OpenPopup("##types");
			m_PickerSearch.clear();
			m_PickerHighlight = 0;
			m_PickerFocus = true;
		}
		std::optional<std::string> chosen;
		ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 22.0f, 0.0f));
		if (library == nullptr || !ImGui::BeginPopup("##types", ImGuiWindowFlags_NoSavedSettings))
		{
			return chosen;
		}
		m_PickerLabel = std::string(label);
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
		{
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return chosen;
		}
		if (m_PickerFocus)
		{
			ImGui::SetKeyboardFocusHere();
			m_PickerFocus = false;
		}
		int move = 0;
		ImGui::SetNextItemWidth(-FLT_MIN);
		bool const entered =
			ImGui::InputTextWithHint("##search", "Search types", &m_PickerSearch,
		                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory, OnPickerKey, &move);
		if (ImGui::IsItemEdited())
		{
			m_PickerHighlight = 0;
		}

		// The rows: the recent types first, then every type by kind; a header row has no type.
		struct Row
		{
			VisualScriptTypeInfo const* Type = nullptr;
			std::string_view Header;
			VisualScriptTypeKind Kind = VisualScriptTypeKind::Object;
		};
		auto const matches = [&](VisualScriptTypeInfo const& type)
		{
			return IsVisualScriptTypeOffered(type, use, constraint) &&
			       (ContainsIgnoringCase(type.Title, m_PickerSearch) || ContainsIgnoringCase(type.ID, m_PickerSearch));
		};
		std::vector<Row> rows;
		bool recentHeader = false;
		for (std::string const& recent : frame.Context.Settings.GetRecentTypes())
		{
			VisualScriptTypeInfo const* const type = library->FindType(recent);
			if (type == nullptr || !matches(*type))
			{
				continue;
			}
			if (!recentHeader)
			{
				rows.push_back({nullptr, "Recent", VisualScriptTypeKind::Object});
				recentHeader = true;
			}
			rows.push_back({type, {}, type->Kind});
		}
		for (auto kind = static_cast<uint8_t>(VisualScriptTypeKind::Bool); kind <= static_cast<uint8_t>(VisualScriptTypeKind::Object);
		     kind++)
		{
			bool header = false;
			for (VisualScriptTypeInfo const& type : library->GetTypes())
			{
				if (static_cast<uint8_t>(type.Kind) != kind || !matches(type))
				{
					continue;
				}
				if (!header)
				{
					rows.push_back({nullptr, GetKindGroupName(type.Kind), type.Kind});
					header = true;
				}
				rows.push_back({&type, {}, type.Kind});
			}
		}
		std::vector<size_t> typeRows;
		for (size_t i = 0; i < rows.size(); i++)
		{
			if (rows[i].Type != nullptr)
			{
				typeRows.push_back(i);
				m_PickerTypes.push_back(rows[i].Type->ID);
			}
		}
		m_PickerHighlight = std::clamp(m_PickerHighlight + move, 0, std::max(static_cast<int>(typeRows.size()) - 1, 0));
		size_t const highlighted = typeRows.empty() ? rows.size() : typeRows[static_cast<size_t>(m_PickerHighlight)];
		if (entered && !typeRows.empty())
		{
			chosen = rows[highlighted].Type->ID;
		}

		float const rowHeight = ImGui::GetTextLineHeightWithSpacing();
		float const height =
			rowHeight * static_cast<float>(std::min(static_cast<int>(rows.size()), PickerRows)) + ImGui::GetStyle().WindowPadding.y;
		ImGui::BeginChild("##list", ImVec2(0.0f, std::max(height, rowHeight)), ImGuiChildFlags_None, ImGuiWindowFlags_NoNavInputs);
		if (rows.empty())
		{
			ImGui::TextDisabled("No types match '%s'", m_PickerSearch.c_str());
		}
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(rows.size()), rowHeight);
		if (move != 0 && highlighted < rows.size())
		{
			clipper.IncludeItemByIndex(static_cast<int>(highlighted));
		}
		while (clipper.Step())
		{
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
			{
				Row const& row = rows[static_cast<size_t>(i)];
				if (row.Type == nullptr)
				{
					ImGui::TextDisabled("%.*s", static_cast<int>(row.Header.size()), row.Header.data());
					continue;
				}
				ImGui::PushID(i);
				float const square = ImGui::GetFontSize() * 0.6f;
				ImVec2 const position = ImGui::GetCursorScreenPos();
				bool const selected = static_cast<size_t>(i) == highlighted;
				if (ImGui::Selectable("##type", selected))
				{
					chosen = row.Type->ID;
				}
				if (selected && move != 0)
				{
					ImGui::SetScrollHereY();
				}
				ImDrawList* const drawList = ImGui::GetWindowDrawList();
				float const top = position.y + (ImGui::GetTextLineHeight() - square) * 0.5f;
				drawList->AddRectFilled(ImVec2(position.x, top), ImVec2(position.x + square, top + square),
				                        GetVisualScriptTypeColor(row.Type->ID, library), square * 0.2f);
				float x = position.x + square + ImGui::GetStyle().ItemInnerSpacing.x;
				drawList->AddText(ImVec2(x, position.y), ImGui::GetColorU32(ImGuiCol_Text), row.Type->Title.c_str());
				if (row.Type->Title != row.Type->ID)
				{
					x += ImGui::CalcTextSize(row.Type->Title.c_str()).x;
					std::string const id = " \xC2\xB7 " + row.Type->ID;
					drawList->AddText(ImVec2(x, position.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), id.c_str());
				}
				ImGui::PopID();
			}
		}
		ImGui::EndChild();
		if (chosen)
		{
			frame.Context.Settings.AddRecentType(*chosen);
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
		return chosen;
	}
}
