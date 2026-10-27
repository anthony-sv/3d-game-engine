#include "Editor/VisualScripting/VisualScriptPanes.h"

#include <algorithm>

namespace Strada
{
	namespace
	{
		// The shown sub-graph's node of an ID; null when it has none.
		VisualScriptNode const* FindShownNode(VisualScriptEditorModel const& model, uint32_t id)
		{
			VisualScriptDocument const& document = model.GetDisplayDocument();
			VisualScriptGraph const* graph = &document.Graph;
			if (model.GetFunction() != 0)
			{
				auto const function = std::find_if(document.Functions.begin(), document.Functions.end(),
				                                   [&model](VisualScriptFunction const& candidate)
				                                   {
													   return candidate.ID == model.GetFunction();
												   });
				if (function == document.Functions.end())
				{
					return nullptr;
				}
				graph = &function->Graph;
			}
			auto const node = std::find_if(graph->Nodes.begin(), graph->Nodes.end(),
			                               [id](VisualScriptNode const& candidate)
			                               {
											   return candidate.ID == id;
										   });
			return node != graph->Nodes.end() ? &*node : nullptr;
		}

		// The library pin behind a canvas pin: the analysis' while the canvas draws the node from it, else the template's.
		VisualScriptPin const* FindLibraryPin(VisualScriptEditorModel const& model, VisualScriptLibrary const* library,
		                                      VisualScriptNodeVisual const& visual, std::string_view pin,
		                                      VisualScriptPinDirection direction)
		{
			VisualScriptAnalysis const* const analysis = model.GetAnalysis();
			if (analysis != nullptr && visual.HasFlag(VisualScriptNodeFlags::Analyzed))
			{
				if (VisualScriptAnalyzedNode const* const analyzed = analysis->FindNode(visual.ID))
				{
					return analyzed->FindPin(pin, direction);
				}
			}
			if (library == nullptr)
			{
				return nullptr;
			}
			VisualScriptNode const* const node = FindShownNode(model, visual.ID);
			VisualScriptNodeTemplate const* const kind = node != nullptr ? library->FindNode(node->Type) : nullptr;
			return kind != nullptr ? kind->FindPin(pin, direction) : nullptr;
		}
	}

	std::optional<std::pair<VisualScriptElementKind, uint32_t>> UnpackVisualScriptElement(uint64_t packed)
	{
		auto const kind = static_cast<uint32_t>(packed >> 32);
		if (kind > static_cast<uint32_t>(VisualScriptElementKind::CustomEvent))
		{
			return std::nullopt;
		}
		return std::pair(static_cast<VisualScriptElementKind>(kind), static_cast<uint32_t>(packed & 0xFFFFFFFFu));
	}

	VisualScriptNodePins MakeVisualScriptNodePins(VisualScriptEditorModel const& model, VisualScriptLibrary const* library,
	                                              std::optional<VisualScriptNewNodePins> newNode)
	{
		return [&model, library, newNode](uint32_t id)
		{
			std::vector<VisualScriptNodePin> rows;
			if (newNode && newNode->Node == id)
			{
				rows.reserve(newNode->Pins.size());
				for (VisualScriptPin const& pin : newNode->Pins)
				{
					rows.push_back({pin.Name, pin.Direction, pin.Type, glm::vec2(0.0f), pin.Default, pin.DefaultsToSelf});
				}
				return rows;
			}
			VisualScriptCanvasModel const& canvas = model.GetCanvas();
			std::optional<uint32_t> const index = canvas.FindNode(id);
			if (!index)
			{
				return rows;
			}
			std::span<VisualScriptNodeVisual const> const nodes = canvas.GetNodes();
			VisualScriptNodeVisual const& visual = nodes[*index];
			VisualScriptAnalysis const* const analysis = model.GetAnalysis();
			VisualScriptAnalyzedNode const* const analyzed = analysis != nullptr ? analysis->FindNode(id) : nullptr;
			std::span<VisualScriptPinVisual const> const pins = canvas.GetPins(visual);
			rows.reserve(pins.size());
			for (VisualScriptPinVisual const& pin : pins)
			{
				if (pin.HasFlag(VisualScriptPinFlags::Orphan))
				{
					continue;
				}
				std::string_view const name = canvas.GetText(pin.Name);
				VisualScriptNodePin row;
				row.Name = VisualScriptEditing::ToStoredVisualScriptPin(analyzed, pin.Direction, name);
				row.Direction = pin.Direction;
				row.Type = pin.Type;
				row.Anchor = pin.Anchor;
				if (VisualScriptPin const* const libraryPin = FindLibraryPin(model, library, visual, name, pin.Direction))
				{
					row.Default = libraryPin->Default;
					row.DefaultsToSelf = libraryPin->DefaultsToSelf;
				}
				rows.push_back(std::move(row));
			}
			return rows;
		};
	}

	std::optional<VisualScriptPin> FindVisualScriptCanvasPin(VisualScriptEditorModel const& model, VisualScriptLibrary const* library,
	                                                         uint32_t node, std::string_view pin, VisualScriptPinDirection direction)
	{
		VisualScriptCanvasModel const& canvas = model.GetCanvas();
		std::optional<uint32_t> const index = canvas.FindNode(node);
		if (!index)
		{
			return std::nullopt;
		}
		std::optional<uint32_t> const pinIndex = canvas.FindPin(*index, pin, direction);
		if (!pinIndex)
		{
			return std::nullopt;
		}
		std::span<VisualScriptPinVisual const> const pins = canvas.GetPins();
		VisualScriptPinVisual const& visual = pins[*pinIndex];
		if (visual.HasFlag(VisualScriptPinFlags::Orphan))
		{
			return std::nullopt;
		}
		VisualScriptPin found;
		if (VisualScriptPin const* const libraryPin = FindLibraryPin(model, library, canvas.GetNodes()[*index], pin, direction))
		{
			found = *libraryPin;
		}
		found.Name = std::string(pin);
		found.Direction = direction;
		found.Type = visual.Type;
		return found;
	}
}
