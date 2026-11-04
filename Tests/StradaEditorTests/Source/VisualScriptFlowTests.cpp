#include "VisualScriptTestUtilities.h"

#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptFlow.h"
#include "Editor/VisualScripting/VisualScriptFlowTake.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace Strada;

namespace
{
	VisualScriptNode MakeNode(uint32_t id, std::string_view type, glm::vec2 position)
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::string(type);
		node.Position = position;
		return node;
	}

	VisualScriptLink MakeLink(uint32_t from, std::string fromPin, uint32_t to, std::string toPin)
	{
		return {{from, std::move(fromPin)}, {to, std::move(toPin)}};
	}

	// Create (1) -> Delay (2) -> Branch (3) -True-> Log (4), with a comment box around Create and Delay.
	VisualScriptDocument MakeFlowDocument()
	{
		VisualScriptDocument document;
		document.Graph.Nodes = {MakeNode(1, "Event.Create", glm::vec2(0.0f)), MakeNode(2, "Flow.Delay", glm::vec2(320.0f, 0.0f)),
		                        MakeNode(3, "Flow.Branch", glm::vec2(1200.0f, 0.0f)),
		                        MakeNode(4, "Call:Strada.Log.Info(string)", glm::vec2(1600.0f, 0.0f))};
		document.Graph.Links = {MakeLink(1, "Then", 2, "Execute"), MakeLink(2, "Completed", 3, "Execute"),
		                        MakeLink(3, "True", 4, "Execute")};
		VisualScriptComment comment;
		comment.ID = 20;
		comment.Text = "Start";
		comment.Position = glm::vec2(-100.0f);
		comment.Size = glm::vec2(900.0f, 500.0f);
		document.Graph.Comments = {comment};
		return document;
	}

	// The document's canvas with the fixture library; Revision counts its Syncs, as the editor model's canvas revision does.
	struct FlowFixture
	{
		Testing::FixtureVisualScriptSemantics Semantics;
		VisualScriptDocument Document = MakeFlowDocument();
		VisualScriptCanvasModel Canvas;
		VisualScriptTextMeasurer Measure = Testing::FixedWidthMeasurer(0.5f);
		VisualScriptThemeAnchors Theme;
		VisualScriptFlowModel Flow;
		VisualScriptFlowMarks Marks;
		uint64_t Revision = 0;

		FlowFixture() { Sync(); }

		void Sync()
		{
			VisualScriptCanvasSource source;
			source.Display = &Document;
			source.Library = &Semantics.GetFixtureLibrary();
			Canvas.Sync(source, Measure, Theme);
			Revision++;
		}

		void Apply(VisualScriptFlowTake const& take, double editorTime = 0.0) { Flow.Apply(take, Canvas, Revision, editorTime); }

		void Build(double flowTime, double editorTime = 0.0, bool reduceMotion = false)
		{
			Flow.Build(Canvas, Revision, flowTime, editorTime, reduceMotion, Marks);
		}

		uint32_t LinkOf(uint32_t node, std::string_view exit) const
		{
			std::optional<uint32_t> const index = Canvas.FindNode(node);
			REQUIRE(index.has_value());
			std::optional<uint32_t> const link = Canvas.FindExecLink(*index, exit);
			REQUIRE(link.has_value());
			return *link;
		}

		VisualScriptFlowLinkMark const* FindLink(uint32_t link) const
		{
			for (VisualScriptFlowLinkMark const& mark : Marks.Links)
			{
				if (mark.Link == link)
				{
					return &mark;
				}
			}
			return nullptr;
		}

		VisualScriptFlowNodeMark const* FindNode(uint32_t node) const
		{
			for (VisualScriptFlowNodeMark const& mark : Marks.Nodes)
			{
				if (mark.Node == node)
				{
					return &mark;
				}
			}
			return nullptr;
		}
	};

	VisualScriptFlowStep MakeStep(uint32_t node, std::string exit, uint32_t count, uint32_t order, double lastTime)
	{
		VisualScriptFlowStep step;
		step.Node = node;
		step.Exit = std::move(exit);
		step.Count = count;
		step.FirstOrder = order;
		step.LastTime = lastTime;
		return step;
	}

	VisualScriptFlowTake MakeTake(std::vector<VisualScriptFlowStep> steps, double time, uint32_t frames)
	{
		VisualScriptFlowTake take;
		take.Steps = std::move(steps);
		take.Time = time;
		take.Frames = frames;
		take.Held = frames == 0;
		return take;
	}

	// Create's and Branch's exits fired, Delay ran without one (it suspended), and an unknown node 99.
	VisualScriptFlowTake MakeFirstTake(double time)
	{
		return MakeTake({MakeStep(1, "Then", 1, 0, time), MakeStep(2, "", 1, 1, time), MakeStep(3, "True", 2, 2, time),
		                 MakeStep(99, "Then", 4, 3, time)},
		                time, 1);
	}
}

TEST_CASE("VisualScriptDebug: the flow model maps a take's steps onto the canvas's wires and nodes")
{
	FlowFixture fixture;
	fixture.Apply(MakeFirstTake(0.1));
	fixture.Build(0.1);
	REQUIRE(fixture.Marks.Links.size() == 2);
	VisualScriptFlowLinkMark const* create = fixture.FindLink(fixture.LinkOf(1, "Then"));
	VisualScriptFlowLinkMark const* branch = fixture.FindLink(fixture.LinkOf(3, "True"));
	REQUIRE(create != nullptr);
	REQUIRE(branch != nullptr);
	CHECK(create->Intensity == doctest::Approx(1.0f));
	CHECK(branch->Intensity == doctest::Approx(1.0f));
	// The Delay's wire did not fire: it ran without an exit.
	CHECK(fixture.FindLink(fixture.LinkOf(2, "Completed")) == nullptr);
	REQUIRE(fixture.Marks.Nodes.size() == 3);
	CHECK(fixture.FindNode(1)->Count == 1);
	CHECK(fixture.FindNode(2)->Count == 1);
	CHECK(fixture.FindNode(3)->Count == 2);
	CHECK(fixture.FindNode(4) == nullptr);
	// The unknown node is kept, but drawn nowhere.
	CHECK(fixture.FindNode(99) == nullptr);
	CHECK(fixture.Flow.GetCount(99) == 4);
	CHECK(fixture.Flow.GetCount(3) == 2);
	CHECK_FALSE(fixture.Marks.Held);
}

TEST_CASE("VisualScriptDebug: flow decays on the flow clock, halving every quarter second of game frames")
{
	FlowFixture fixture;
	fixture.Apply(MakeFirstTake(1.0));
	fixture.Build(1.25);
	CHECK(fixture.FindLink(fixture.LinkOf(1, "Then"))->Intensity == doctest::Approx(0.5f));
	CHECK(fixture.FindNode(2)->Intensity == doctest::Approx(0.5f));
	fixture.Build(1.5);
	CHECK(fixture.FindLink(fixture.LinkOf(1, "Then"))->Intensity == doctest::Approx(0.25f));
	// Drawn while at least 0.05: 0.25 × log2(20) seconds, about 1.08.
	double const visible = VisualScriptFlowModel::HalfLife * std::log2(20.0);
	fixture.Build(1.0 + visible - 0.01);
	CHECK(fixture.Marks.Links.size() == 2);
	fixture.Build(1.0 + visible + 0.01);
	CHECK(fixture.Marks.Links.empty());
	// Faded nodes keep their counts.
	REQUIRE(fixture.FindNode(3) != nullptr);
	CHECK(fixture.FindNode(3)->Intensity == 0.0f);
	CHECK(fixture.FindNode(3)->Count == 2);
	CHECK(fixture.Marks.Boxes.empty());
	CHECK(VisualScriptFlowModel::GetIntensity(1.0, 2.0) == doctest::Approx(1.0f));
}

TEST_CASE("VisualScriptDebug: flow decay holds while paused and resumes with frames")
{
	FlowFixture fixture;
	fixture.Apply(MakeFirstTake(1.0), 10.0);
	// Paused: every editor frame takes nothing, and the clock holds.
	for (int frame = 1; frame <= 60; frame++)
	{
		fixture.Apply(MakeTake({}, 1.0, 0), 10.0 + frame / 60.0);
		fixture.Build(1.0, 10.0 + frame / 60.0);
		CHECK(fixture.Marks.Held);
		CHECK(fixture.FindLink(fixture.LinkOf(1, "Then"))->Intensity == doctest::Approx(1.0f));
	}
	// Frames run again: a quarter second of them halves it.
	fixture.Apply(MakeTake({}, 1.25, 15), 12.0);
	fixture.Build(1.25, 12.0);
	CHECK_FALSE(fixture.Marks.Held);
	CHECK(fixture.FindLink(fixture.LinkOf(1, "Then"))->Intensity == doctest::Approx(0.5f));
}

TEST_CASE("VisualScriptDebug: a 120-frame take followed by two seconds of paused editor frames keeps intensity 1")
{
	FlowFixture fixture;
	// play.advance 120 at 60 frames a second: what its last frame ran carries the last frame's time.
	fixture.Apply(MakeTake({MakeStep(1, "Then", 120, 0, 2.0), MakeStep(3, "True", 120, 1, 2.0)}, 2.0, 120), 5.0);
	for (int frame = 1; frame <= 120; frame++)
	{
		fixture.Apply(MakeTake({}, 2.0, 0), 5.0 + frame / 60.0);
	}
	fixture.Build(2.0, 7.0);
	REQUIRE(fixture.Marks.Links.size() == 2);
	for (VisualScriptFlowLinkMark const& mark : fixture.Marks.Links)
	{
		CHECK(mark.Intensity == doctest::Approx(1.0f));
		// Held: no dots.
		CHECK(mark.Phase < 0.0f);
	}
	CHECK(fixture.FindNode(1)->Count == 120);
}

TEST_CASE("VisualScriptDebug: one 120-frame take and 120 one-frame takes give the same counts and intensities")
{
	FlowFixture once;
	FlowFixture each;
	// Create fires in every frame, Branch in every other one, Delay only in the first.
	std::vector<VisualScriptFlowStep> merged = {MakeStep(1, "Then", 120, 0, 2.0), MakeStep(2, "", 1, 1, 1.0 / 60.0),
	                                            MakeStep(3, "True", 60, 2, 119.0 / 60.0)};
	once.Apply(MakeTake(merged, 2.0, 120));
	for (int frame = 1; frame <= 120; frame++)
	{
		double const time = frame / 60.0;
		std::vector<VisualScriptFlowStep> steps = {MakeStep(1, "Then", 1, 0, time)};
		if (frame == 1)
		{
			steps.push_back(MakeStep(2, "", 1, 1, time));
		}
		if (frame % 2 == 1)
		{
			steps.push_back(MakeStep(3, "True", 1, 2, time));
		}
		each.Apply(MakeTake(std::move(steps), time, 1));
	}
	for (double const now : {2.0, 2.2, 2.6})
	{
		CAPTURE(now);
		once.Build(now);
		each.Build(now);
		REQUIRE(once.Marks.Links.size() == each.Marks.Links.size());
		REQUIRE(once.Marks.Nodes.size() == each.Marks.Nodes.size());
		for (size_t index = 0; index < once.Marks.Links.size(); index++)
		{
			CHECK(once.Marks.Links[index].Link == each.Marks.Links[index].Link);
			CHECK(once.Marks.Links[index].Intensity == doctest::Approx(each.Marks.Links[index].Intensity));
		}
		for (size_t index = 0; index < once.Marks.Nodes.size(); index++)
		{
			CHECK(once.Marks.Nodes[index].Node == each.Marks.Nodes[index].Node);
			CHECK(once.Marks.Nodes[index].Count == each.Marks.Nodes[index].Count);
			CHECK(once.Marks.Nodes[index].Intensity == doctest::Approx(each.Marks.Nodes[index].Intensity));
		}
	}
}

TEST_CASE("VisualScriptDebug: Reset Run Counts starts the counts again and keeps what is lit")
{
	FlowFixture fixture;
	fixture.Apply(MakeFirstTake(0.5));
	fixture.Flow.ResetCounts();
	fixture.Build(0.5);
	REQUIRE(fixture.FindNode(3) != nullptr);
	CHECK(fixture.FindNode(3)->Count == 0);
	CHECK(fixture.FindNode(3)->Intensity == doctest::Approx(1.0f));
	CHECK(fixture.Marks.Links.size() == 2);
	// Counted from the reset on.
	fixture.Apply(MakeTake({MakeStep(3, "True", 3, 0, 0.6)}, 0.6, 6));
	fixture.Build(0.6);
	CHECK(fixture.FindNode(3)->Count == 3);
	// Once faded, a node counted since the reset only is listed; the others are gone.
	fixture.Build(5.0);
	REQUIRE(fixture.Marks.Nodes.size() == 1);
	CHECK(fixture.Marks.Nodes.front().Node == 3);
	fixture.Flow.Clear();
	fixture.Build(5.0);
	CHECK(fixture.Marks.Nodes.empty());
	CHECK(fixture.Flow.IsEmpty());
}

TEST_CASE("VisualScriptDebug: a take's dots start one after another by their first order, and keep flowing while the wire fires")
{
	FlowFixture fixture;
	fixture.Apply(MakeTake({MakeStep(1, "Then", 1, 0, 0.1), MakeStep(2, "Completed", 1, 1, 0.1), MakeStep(3, "True", 1, 2, 0.1)}, 0.1, 1),
	              10.0);
	fixture.Build(0.1, 10.0);
	CHECK(fixture.FindLink(fixture.LinkOf(1, "Then"))->Phase == doctest::Approx(0.0f));
	CHECK(fixture.FindLink(fixture.LinkOf(2, "Completed"))->Phase == doctest::Approx(-0.008f));
	CHECK(fixture.FindLink(fixture.LinkOf(3, "True"))->Phase == doctest::Approx(-0.016f));
	// A chain's dots all travel within 0.24 seconds: ranks past 30 start with the 30th.
	std::vector<VisualScriptFlowStep> chain;
	for (uint32_t rank = 0; rank < 40; rank++)
	{
		chain.push_back(MakeStep(1000 + rank, "Then", 1, rank, 0.2));
	}
	chain.push_back(MakeStep(3, "True", 1, 40, 0.2));
	fixture.Apply(MakeTake(std::move(chain), 0.2, 1), 11.0);
	fixture.Build(0.2, 11.0);
	// Branch's wire still moved its dots when it fired again: they go on from where they were.
	CHECK(fixture.FindLink(fixture.LinkOf(3, "True"))->Phase == doctest::Approx(11.0f - 10.016f));
	// Once it faded under the dots' intensity, firing again starts them anew, staggered.
	fixture.Apply(MakeTake({MakeStep(1, "Then", 1, 0, 1.0), MakeStep(3, "True", 1, 1, 1.0)}, 1.0, 48), 12.0);
	fixture.Build(1.0, 12.0);
	CHECK(fixture.FindLink(fixture.LinkOf(1, "Then"))->Phase == doctest::Approx(0.0f));
	CHECK(fixture.FindLink(fixture.LinkOf(3, "True"))->Phase == doctest::Approx(-0.008f));
	CHECK(VisualScriptFlowModel::MaxStaggerRank * VisualScriptFlowModel::StaggerSeconds == doctest::Approx(0.24));
}

TEST_CASE("VisualScriptDebug: flow draws no dots while the clock holds or with Reduce Motion, and the light fades in place")
{
	FlowFixture fixture;
	fixture.Apply(MakeFirstTake(0.1), 3.0);
	fixture.Build(0.1, 3.5);
	CHECK(fixture.FindLink(fixture.LinkOf(1, "Then"))->Phase == doctest::Approx(0.5f));
	fixture.Build(0.2, 3.5, true);
	VisualScriptFlowLinkMark const* reduced = fixture.FindLink(fixture.LinkOf(1, "Then"));
	REQUIRE(reduced != nullptr);
	CHECK(reduced->Phase < 0.0f);
	CHECK(reduced->Intensity == doctest::Approx(VisualScriptFlowModel::GetIntensity(0.2, 0.1)));
	fixture.Apply(MakeTake({}, 0.1, 0), 4.0);
	fixture.Build(0.1, 4.0);
	CHECK(fixture.Marks.Held);
	for (VisualScriptFlowLinkMark const& mark : fixture.Marks.Links)
	{
		CHECK(mark.Phase < 0.0f);
	}
	// Under half intensity the dots stop too.
	fixture.Apply(MakeTake({}, 0.5, 24), 5.0);
	fixture.Build(0.5, 5.0);
	REQUIRE(!fixture.Marks.Links.empty());
	for (VisualScriptFlowLinkMark const& mark : fixture.Marks.Links)
	{
		CHECK(mark.Intensity < VisualScriptFlowModel::DotIntensity);
		CHECK(mark.Phase < 0.0f);
	}
}

TEST_CASE("VisualScriptDebug: flow is found again by ID when the canvas is laid out anew")
{
	FlowFixture fixture;
	fixture.Apply(MakeFirstTake(0.1));
	fixture.Build(0.1);
	uint32_t const before = fixture.LinkOf(3, "True");
	REQUIRE(fixture.FindLink(before) != nullptr);
	// Links listed in another order and a node removed: indices change, IDs and pin names stay.
	fixture.Document.Graph.Links = {MakeLink(3, "True", 4, "Execute"), MakeLink(1, "Then", 2, "Execute")};
	fixture.Document.Graph.Nodes.erase(fixture.Document.Graph.Nodes.begin() + 1);
	fixture.Sync();
	uint32_t const after = fixture.LinkOf(3, "True");
	REQUIRE(after != before);
	fixture.Build(0.1);
	REQUIRE(fixture.Marks.Links.size() == 1);
	CHECK(fixture.Marks.Links.front().Link == after);
	// The removed node is drawn nowhere, but counted.
	CHECK(fixture.FindNode(2) == nullptr);
	CHECK(fixture.Flow.GetCount(2) == 1);
}

TEST_CASE("VisualScriptDebug: a comment box glows with the highest intensity among the nodes inside it")
{
	FlowFixture fixture;
	// Create (inside) fired at 1.0, Delay (inside) at 1.25, Branch (outside) at 1.5.
	fixture.Apply(MakeTake({MakeStep(1, "Then", 1, 0, 1.0), MakeStep(2, "", 1, 1, 1.25), MakeStep(3, "True", 1, 2, 1.5)}, 1.5, 30));
	fixture.Build(1.5);
	REQUIRE(fixture.Marks.Boxes.size() == 1);
	CHECK(fixture.Marks.Boxes.front().Comment == 20);
	CHECK(fixture.Marks.Boxes.front().Intensity == doctest::Approx(0.5f));
	// Most intense first.
	REQUIRE(fixture.Marks.Nodes.size() == 3);
	CHECK(fixture.Marks.Nodes[0].Node == 3);
	CHECK(fixture.Marks.Nodes[1].Node == 2);
	CHECK(fixture.Marks.Nodes[2].Node == 1);
	// Only Branch, outside every box, lit: no box glows.
	fixture.Flow.Clear();
	fixture.Apply(MakeTake({MakeStep(3, "True", 1, 0, 2.0)}, 2.0, 1));
	fixture.Build(2.0);
	CHECK(fixture.Marks.Boxes.empty());
}

TEST_CASE("VisualScriptDebug: run counts are exact up to 9,999, then thousands and millions rounded down")
{
	CHECK(FormatVisualScriptRunCount(0) == "\xC3\x97"
	                                       "0");
	CHECK(FormatVisualScriptRunCount(37) == "\xC3\x97"
	                                        "37");
	CHECK(FormatVisualScriptRunCount(9'999) == "\xC3\x97"
	                                           "9999");
	CHECK(FormatVisualScriptRunCount(10'000) == "\xC3\x97"
	                                            "10k");
	CHECK(FormatVisualScriptRunCount(12'345) == "\xC3\x97"
	                                            "12k");
	CHECK(FormatVisualScriptRunCount(999'999) == "\xC3\x97"
	                                             "999k");
	CHECK(FormatVisualScriptRunCount(1'000'000) == "\xC3\x97"
	                                               "1.0M");
	CHECK(FormatVisualScriptRunCount(1'250'000) == "\xC3\x97"
	                                               "1.2M");
	CHECK(FormatVisualScriptRunCount(9'999'999) == "\xC3\x97"
	                                               "9.9M");
	CHECK(FormatVisualScriptRunCount(12'500'000) == "\xC3\x97"
	                                                "12M");
	CHECK(FormatVisualScriptRunCount(std::numeric_limits<uint32_t>::max()) == "\xC3\x97"
	                                                                          "4294M");
}

TEST_CASE("VisualScriptDebug: a take naming a step twice counts it as one step of both, and new steps join those seen before")
{
	FlowFixture fixture;
	fixture.Apply(MakeTake({MakeStep(3, "True", 1, 0, 0.5)}, 0.5, 1));
	fixture.Apply(MakeTake(
		{MakeStep(1, "Then", 2, 0, 1.0), MakeStep(3, "True", 4, 1, 1.0), MakeStep(1, "Then", 3, 2, 0.75), MakeStep(2, "", 1, 3, 1.0)}, 1.0,
		2));
	CHECK(fixture.Flow.GetCount(1) == 5);
	CHECK(fixture.Flow.GetCount(3) == 5);
	CHECK(fixture.Flow.GetCount(2) == 1);
	fixture.Build(1.0);
	REQUIRE(fixture.Marks.Links.size() == 2);
	CHECK(fixture.FindLink(fixture.LinkOf(1, "Then"))->Intensity == doctest::Approx(1.0f));
	REQUIRE(fixture.Marks.Nodes.size() == 3);
	CHECK(fixture.FindNode(1)->Count == 5);
}
