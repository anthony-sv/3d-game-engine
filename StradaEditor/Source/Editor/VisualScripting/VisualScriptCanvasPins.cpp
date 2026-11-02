#include "Editor/VisualScripting/VisualScriptCanvasPins.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		// The node of an ID in the shown sub-graph; null when it has none.
		VisualScriptNode const* FindShownNode(VisualScriptDocument const& document, uint32_t function, uint32_t id)
		{
			VisualScriptGraph const* graph = &document.Graph;
			if (function != 0)
			{
				auto const found = std::find_if(document.Functions.begin(), document.Functions.end(),
				                                [function](VisualScriptFunction const& candidate)
				                                {
													return candidate.ID == function;
												});
				if (found == document.Functions.end())
				{
					return nullptr;
				}
				graph = &found->Graph;
			}
			auto const node = std::find_if(graph->Nodes.begin(), graph->Nodes.end(),
			                               [id](VisualScriptNode const& candidate)
			                               {
											   return candidate.ID == id;
										   });
			return node != graph->Nodes.end() ? &*node : nullptr;
		}
	}

	VisualScriptPin const* FindVisualScriptCanvasLibraryPin(VisualScriptCanvasModel const& canvas, VisualScriptAnalysis const* analysis,
	                                                        VisualScriptDocument const* shownDocument, VisualScriptLibrary const* library,
	                                                        VisualScriptNodeVisual const& visual, std::string_view pin,
	                                                        VisualScriptPinDirection direction)
	{
		if (analysis != nullptr && visual.HasFlag(VisualScriptNodeFlags::Analyzed))
		{
			if (VisualScriptAnalyzedNode const* const analyzed = analysis->FindNode(visual.ID))
			{
				return analyzed->FindPin(pin, direction);
			}
		}
		if (library == nullptr || shownDocument == nullptr)
		{
			return nullptr;
		}
		VisualScriptNode const* const node = FindShownNode(*shownDocument, canvas.GetFunction(), visual.ID);
		VisualScriptNodeTemplate const* const kind = node != nullptr ? library->FindNode(node->Type) : nullptr;
		return kind != nullptr ? kind->FindPin(pin, direction) : nullptr;
	}

	VisualScriptNodePins MakeVisualScriptCanvasNodePins(VisualScriptCanvasModel const& canvas, VisualScriptAnalysis const* analysis,
	                                                    VisualScriptDocument const* shownDocument, VisualScriptLibrary const* library,
	                                                    std::optional<VisualScriptNewNodePins> newNode)
	{
		return [&canvas, analysis, shownDocument, library, newNode](uint32_t id)
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
			std::optional<uint32_t> const index = canvas.FindNode(id);
			if (!index)
			{
				return rows;
			}
			std::span<VisualScriptNodeVisual const> const nodes = canvas.GetNodes();
			VisualScriptNodeVisual const& visual = nodes[*index];
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
				if (VisualScriptPin const* const libraryPin =
				        FindVisualScriptCanvasLibraryPin(canvas, analysis, shownDocument, library, visual, name, pin.Direction))
				{
					row.Default = libraryPin->Default;
					row.DefaultsToSelf = libraryPin->DefaultsToSelf;
				}
				rows.push_back(std::move(row));
			}
			return rows;
		};
	}
}
