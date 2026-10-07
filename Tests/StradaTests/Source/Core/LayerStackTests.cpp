#include "Strada/Core/LayerStack.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace Strada;

namespace
{
	class NamedLayer : public Layer
	{
	public:
		explicit NamedLayer(std::string name)
			: Layer(std::move(name))
		{
		}
	};

	std::vector<std::string> Names(LayerStack const& stack)
	{
		std::vector<std::string> names;
		for (Scope<Layer> const& layer : stack)
		{
			names.push_back(layer->GetName());
		}
		return names;
	}
}

TEST_CASE("LayerStack: overlays always come after layers")
{
	LayerStack stack;
	stack.PushLayer(CreateScope<NamedLayer>("Layer1"));
	stack.PushOverlay(CreateScope<NamedLayer>("Overlay1"));
	stack.PushLayer(CreateScope<NamedLayer>("Layer2"));
	stack.PushOverlay(CreateScope<NamedLayer>("Overlay2"));

	CHECK(Names(stack) == std::vector<std::string>{"Layer1", "Layer2", "Overlay1", "Overlay2"});
	CHECK(stack.GetSize() == 4);
}

TEST_CASE("LayerStack: removing layers keeps the insertion point consistent")
{
	LayerStack stack;
	Layer& first = stack.PushLayer(CreateScope<NamedLayer>("Layer1"));
	stack.PushLayer(CreateScope<NamedLayer>("Layer2"));
	Layer& overlay = stack.PushOverlay(CreateScope<NamedLayer>("Overlay"));

	Scope<Layer> removed = stack.Remove(&first);
	REQUIRE(removed != nullptr);
	CHECK(removed->GetName() == "Layer1");

	stack.PushLayer(CreateScope<NamedLayer>("Layer3"));
	CHECK(Names(stack) == std::vector<std::string>{"Layer2", "Layer3", "Overlay"});

	CHECK(stack.Remove(&overlay) != nullptr);
	CHECK(stack.Remove(&overlay) == nullptr);
	CHECK(Names(stack) == std::vector<std::string>{"Layer2", "Layer3"});
}

TEST_CASE("LayerStack: RemoveAll returns layers in teardown order")
{
	LayerStack stack;
	stack.PushLayer(CreateScope<NamedLayer>("Layer1"));
	stack.PushLayer(CreateScope<NamedLayer>("Layer2"));
	stack.PushOverlay(CreateScope<NamedLayer>("Overlay"));

	std::vector<Scope<Layer>> removed = stack.RemoveAll();
	REQUIRE(removed.size() == 3);
	CHECK(removed[0]->GetName() == "Overlay");
	CHECK(removed[1]->GetName() == "Layer2");
	CHECK(removed[2]->GetName() == "Layer1");
	CHECK(stack.IsEmpty());

	stack.PushLayer(CreateScope<NamedLayer>("Fresh"));
	CHECK(Names(stack) == std::vector<std::string>{"Fresh"});
}
