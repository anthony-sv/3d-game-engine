#include "Editor/VisualScripting/VisualScriptFlow.h"

#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptGeometry.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <tuple>

namespace Strada
{
	namespace
	{
		bool StepLess(uint32_t node, std::string_view exit, uint32_t otherNode, std::string_view otherExit)
		{
			return std::tie(node, exit) < std::tie(otherNode, otherExit);
		}

		// Counts stop at the largest count rather than wrap.
		uint32_t AddCounts(uint32_t a, uint32_t b)
		{
			return b > std::numeric_limits<uint32_t>::max() - a ? std::numeric_limits<uint32_t>::max() : a + b;
		}

		// A node's runs from its steps' counts: the exits it fired (a sequence's outputs and a loop's iterations each count), or
		// the times it ended its chain without an exit when that is more. One run of a function call or a latent node is two
		// steps, the entry ending the chain (the call enters its function, the node suspends) and the exit when it returns or
		// completes, so the larger of the two counts each such run once, and a run still inside it too.
		uint32_t CountRuns(uint32_t withoutExit, uint32_t exits)
		{
			return std::max(withoutExit, exits);
		}

		bool Encloses(VisualScriptRect const& outer, VisualScriptRect const& inner)
		{
			return inner.Min.x >= outer.Min.x && inner.Min.y >= outer.Min.y && inner.Max.x <= outer.Max.x && inner.Max.y <= outer.Max.y;
		}
	}

	std::string FormatVisualScriptRunCount(uint32_t count)
	{
		if (count < 10'000u)
		{
			return fmt::format("{}{}", VisualScriptRunCountPrefix, count);
		}
		if (count < 1'000'000u)
		{
			return fmt::format("{}{}k", VisualScriptRunCountPrefix, count / 1'000u);
		}
		if (count < 10'000'000u)
		{
			uint32_t const tenths = count / 100'000u;
			return fmt::format("{}{}.{}M", VisualScriptRunCountPrefix, tenths / 10u, tenths % 10u);
		}
		return fmt::format("{}{}M", VisualScriptRunCountPrefix, count / 1'000'000u);
	}

	void VisualScriptFlowMarks::Clear()
	{
		Links.clear();
		Nodes.clear();
		Boxes.clear();
		Held = false;
	}

	float VisualScriptFlowModel::GetIntensity(double flowTime, double lastFired)
	{
		double const age = std::max(flowTime - lastFired, 0.0);
		return static_cast<float>(std::exp2(-age / HalfLife));
	}

	VisualScriptFlowModel::Step* VisualScriptFlowModel::Find(uint32_t node, std::string_view exit, size_t count)
	{
		auto const end = m_Steps.begin() + static_cast<std::ptrdiff_t>(std::min(count, m_Steps.size()));
		auto const found = std::lower_bound(m_Steps.begin(), end, std::pair<uint32_t, std::string_view>(node, exit),
		                                    [](Step const& step, std::pair<uint32_t, std::string_view> const& key)
		                                    {
												return StepLess(step.Node, step.Exit, key.first, key.second);
											});
		return found != end && found->Node == node && found->Exit == exit ? &*found : nullptr;
	}

	void VisualScriptFlowModel::Map(Step& step, VisualScriptCanvasModel const& canvas) const
	{
		step.NodeIndex = canvas.FindNode(step.Node);
		step.Link.reset();
		if (step.NodeIndex && !step.Exit.empty())
		{
			step.Link = canvas.FindExecLink(*step.NodeIndex, step.Exit);
		}
	}

	void VisualScriptFlowModel::Remap(VisualScriptCanvasModel const& canvas, uint64_t canvasRevision)
	{
		if (m_Mapped && m_MappedRevision == canvasRevision)
		{
			return;
		}
		for (Step& step : m_Steps)
		{
			Map(step, canvas);
		}
		m_Mapped = true;
		m_MappedRevision = canvasRevision;
	}

	void VisualScriptFlowModel::Apply(VisualScriptFlowTake const& take, VisualScriptCanvasModel const& canvas, uint64_t canvasRevision,
	                                  double editorTime)
	{
		Remap(canvas, canvasRevision);
		m_Held = take.Held;
		// Steps seen before are found in the sorted list; new ones are appended and merged in once, so a first take of thousands
		// of steps costs a sort, not an insertion each.
		size_t const known = m_Steps.size();
		// The take lists its steps in FirstOrder order: a step's rank is its place among them.
		uint32_t rank = 0;
		for (VisualScriptFlowStep const& taken : take.Steps)
		{
			uint32_t const stagger = std::min(rank, MaxStaggerRank);
			rank++;
			if (taken.Count == 0)
			{
				continue;
			}
			Step* step = Find(taken.Node, taken.Exit, known);
			if (step == nullptr)
			{
				step = &m_Steps.emplace_back();
				step->Node = taken.Node;
				step->Exit = taken.Exit;
				step->LastFired = taken.LastTime;
				Map(*step, canvas);
			}
			// A wire still moving its dots goes on from where they are, so one that fires every frame flows steadily.
			bool const moving = step->DotStart && GetIntensity(taken.LastTime, step->LastFired) >= DotIntensity;
			step->Count = AddCounts(step->Count, taken.Count);
			step->LastFired = std::max(step->LastFired, taken.LastTime);
			if (!moving)
			{
				step->DotStart = editorTime + static_cast<double>(stagger) * StaggerSeconds;
			}
		}
		if (m_Steps.size() == known)
		{
			return;
		}
		auto const less = [](Step const& a, Step const& b)
		{
			return StepLess(a.Node, a.Exit, b.Node, b.Exit);
		};
		auto const middle = m_Steps.begin() + static_cast<std::ptrdiff_t>(known);
		std::stable_sort(middle, m_Steps.end(), less);
		std::inplace_merge(m_Steps.begin(), middle, m_Steps.end(), less);
		// A take that named a step twice: one step of both, as one take of it would have been.
		size_t head = 0;
		for (size_t index = 1; index < m_Steps.size(); index++)
		{
			Step& first = m_Steps[head];
			Step& other = m_Steps[index];
			if (first.Node == other.Node && first.Exit == other.Exit)
			{
				first.Count = AddCounts(first.Count, other.Count);
				first.LastFired = std::max(first.LastFired, other.LastFired);
				other.Folded = true;
			}
			else
			{
				head = index;
			}
		}
		std::erase_if(m_Steps,
		              [](Step const& folded)
		              {
						  return folded.Folded;
					  });
	}

	void VisualScriptFlowModel::Build(VisualScriptCanvasModel const& canvas, uint64_t canvasRevision, double flowTime, double editorTime,
	                                  bool reduceMotion, VisualScriptFlowMarks& marks)
	{
		marks.Clear();
		marks.Held = m_Held;
		m_LitNodes.clear();
		Remap(canvas, canvasRevision);
		bool const dots = !m_Held && !reduceMotion;
		// Steps are sorted by node: each node's steps are one run of them.
		for (size_t first = 0; first < m_Steps.size();)
		{
			size_t last = first;
			float nodeIntensity = 0.0f;
			uint32_t withoutExit = 0;
			uint32_t exits = 0;
			for (; last < m_Steps.size() && m_Steps[last].Node == m_Steps[first].Node; last++)
			{
				Step const& step = m_Steps[last];
				float const intensity = GetIntensity(flowTime, step.LastFired);
				nodeIntensity = std::max(nodeIntensity, intensity);
				if (step.Exit.empty())
				{
					withoutExit = AddCounts(withoutExit, step.Count);
				}
				else
				{
					exits = AddCounts(exits, step.Count);
				}
				if (step.Link && intensity >= MinIntensity)
				{
					float phase = -1.0f;
					if (dots && intensity >= DotIntensity && step.DotStart)
					{
						phase = static_cast<float>(editorTime - *step.DotStart);
					}
					marks.Links.push_back({*step.Link, intensity, phase});
				}
			}
			Step const& head = m_Steps[first];
			uint32_t const count = CountRuns(withoutExit, exits);
			if (head.NodeIndex)
			{
				bool const lit = nodeIntensity >= MinIntensity;
				if (lit || count > 0)
				{
					marks.Nodes.push_back({head.Node, lit ? nodeIntensity : 0.0f, count});
				}
				if (lit)
				{
					m_LitNodes.emplace_back(*head.NodeIndex, nodeIntensity);
				}
			}
			first = last;
		}

		// A comment box glows with its brightest node inside it.
		std::span<VisualScriptNodeVisual const> const nodes = canvas.GetNodes();
		for (VisualScriptCommentVisual const& comment : canvas.GetComments())
		{
			float intensity = 0.0f;
			for (auto const& [litIndex, litIntensity] : m_LitNodes)
			{
				if (litIndex < nodes.size() && litIntensity > intensity &&
				    Encloses(comment.Rect, VisualScriptGeometry::GetNodeRect(nodes[litIndex])))
				{
					intensity = litIntensity;
				}
			}
			if (intensity >= MinIntensity)
			{
				marks.Boxes.push_back({comment.ID, intensity});
			}
		}

		std::stable_sort(marks.Links.begin(), marks.Links.end(),
		                 [](VisualScriptFlowLinkMark const& a, VisualScriptFlowLinkMark const& b)
		                 {
							 return a.Intensity > b.Intensity;
						 });
		std::stable_sort(marks.Nodes.begin(), marks.Nodes.end(),
		                 [](VisualScriptFlowNodeMark const& a, VisualScriptFlowNodeMark const& b)
		                 {
							 return a.Intensity > b.Intensity;
						 });
		std::stable_sort(marks.Boxes.begin(), marks.Boxes.end(),
		                 [](VisualScriptFlowBoxMark const& a, VisualScriptFlowBoxMark const& b)
		                 {
							 return a.Intensity > b.Intensity;
						 });
	}

	void VisualScriptFlowModel::ResetCounts()
	{
		for (Step& step : m_Steps)
		{
			step.Count = 0;
		}
	}

	void VisualScriptFlowModel::Clear()
	{
		m_Steps.clear();
		m_LitNodes.clear();
		m_Mapped = false;
		m_Held = false;
	}

	uint32_t VisualScriptFlowModel::GetCount(uint32_t node) const
	{
		uint32_t withoutExit = 0;
		uint32_t exits = 0;
		for (Step const& step : m_Steps)
		{
			if (step.Node == node)
			{
				uint32_t& counted = step.Exit.empty() ? withoutExit : exits;
				counted = AddCounts(counted, step.Count);
			}
		}
		return CountRuns(withoutExit, exits);
	}
}
