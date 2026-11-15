#include "Core/ProfilerTestUtilities.h"
#include "Fuzzing.h"
#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/PrefabAsset.h"
#include "Strada/Core/Platform.h"
#include "Strada/Core/Profiler.h"
#include "Strada/Math/Math.h"
#include "Strada/Physics/PhysicsScene.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"

#include <doctest/doctest.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
	template<typename T>
	bool HasSameBits(T const& a, T const& b)
	{
		return std::memcmp(&a, &b, sizeof(T)) == 0;
	}

	// [0, 1) with 24 bits, the same on every platform.
	float Unit(Testing::FuzzRandom& generator)
	{
		return static_cast<float>(generator.Next() >> 40) / 16777216.0f;
	}

	float Between(Testing::FuzzRandom& generator, float low, float high)
	{
		return low + (high - low) * Unit(generator);
	}

	glm::vec3 RandomVector(Testing::FuzzRandom& generator, float low, float high)
	{
		return {Between(generator, low, high), Between(generator, low, high), Between(generator, low, high)};
	}

	glm::quat RandomRotation(Testing::FuzzRandom& generator)
	{
		glm::quat const rotation(Between(generator, -1.0f, 1.0f), Between(generator, -1.0f, 1.0f), Between(generator, -1.0f, 1.0f),
		                         Between(generator, -1.0f, 1.0f));
		return glm::normalize(rotation);
	}

	// The cache's rule, walked without the cache: from the entity up through Parent UUIDs looked up in the scene (a parent
	// it does not hold ends the walk), every entity on a cycle of parents a root, then multiplied top down by the one
	// product every world matrix is made with.
	glm::mat4 ReferenceWorld(Scene& scene, entt::entity handle)
	{
		entt::registry& registry = scene.GetRegistry();
		std::vector<entt::entity> chain;
		size_t cycleStart = std::numeric_limits<size_t>::max();
		entt::entity current = handle;
		while (true)
		{
			chain.push_back(current);
			Entity const parent = scene.GetEntityByUUID(registry.get<RelationshipComponent>(current).Parent);
			if (!parent || !parent.HasComponent<TransformComponent>())
			{
				break;
			}
			auto const seen = std::find(chain.begin(), chain.end(), parent.GetHandle());
			if (seen != chain.end())
			{
				cycleStart = static_cast<size_t>(seen - chain.begin());
				break;
			}
			current = parent.GetHandle();
		}
		size_t const top = std::min(cycleStart, chain.size() - 1);
		glm::mat4 world = registry.get<TransformComponent>(chain[top]).GetTransform();
		for (size_t index = top; index-- > 0;)
		{
			world = Math::ComposeWorldTransform(world, registry.get<TransformComponent>(chain[index]).GetTransform());
		}
		return world;
	}

	// Slice 1's walk, before the cache: bottom up, L(root) * (... * L(entity)), bounded by the entity count.
	glm::mat4 BottomUpWorld(Scene& scene, entt::entity handle)
	{
		entt::registry& registry = scene.GetRegistry();
		glm::mat4 transform = registry.get<TransformComponent>(handle).GetTransform();
		UUID parent = registry.get<RelationshipComponent>(handle).Parent;
		for (size_t depth = 0; parent.IsValid() && depth <= scene.GetEntityCount(); depth++)
		{
			Entity const entity = scene.GetEntityByUUID(parent);
			if (!entity)
			{
				break;
			}
			transform = entity.GetComponent<TransformComponent>().GetTransform() * transform;
			parent = entity.GetComponent<RelationshipComponent>().Parent;
		}
		return transform;
	}

	// Whether the entity's chain of parents comes back to an entity it passed.
	bool ReachesCycle(Scene& scene, entt::entity handle)
	{
		std::vector<UUID> seen;
		Entity entity(handle, &scene);
		while (entity)
		{
			UUID const id = entity.GetUUID();
			if (std::find(seen.begin(), seen.end(), id) != seen.end())
			{
				return true;
			}
			seen.push_back(id);
			entity = scene.GetEntityByUUID(entity.GetComponent<RelationshipComponent>().Parent);
		}
		return false;
	}

	// Whether the children lists below the entity, followed down, reach each entity once. Direct writes of Parent leave
	// lists that disagree with it, and SetParent may then append an entity to a second list: destruction and duplication,
	// which follow the lists, need a tree.
	bool IsTreeBelow(Scene& scene, Entity entity)
	{
		std::vector<UUID> seen;
		std::vector<UUID> stack{entity.GetUUID()};
		while (!stack.empty())
		{
			UUID const id = stack.back();
			stack.pop_back();
			if (std::find(seen.begin(), seen.end(), id) != seen.end())
			{
				return false;
			}
			seen.push_back(id);
			if (Entity const below = scene.GetEntityByUUID(id))
			{
				std::vector<UUID> const& children = below.GetComponent<RelationshipComponent>().Children;
				stack.insert(stack.end(), children.begin(), children.end());
			}
		}
		return true;
	}

	bool IsFinite(glm::mat4 const& matrix)
	{
		for (glm::length_t column = 0; column < 4; ++column)
		{
			for (glm::length_t row = 0; row < 4; ++row)
			{
				if (!std::isfinite(matrix[column][row]))
				{
					return false;
				}
			}
		}
		return true;
	}

	// Equal within a relative tolerance of the matrix's largest element.
	bool IsClose(glm::mat4 const& a, glm::mat4 const& b, float tolerance)
	{
		float largest = 1.0f;
		for (glm::length_t column = 0; column < 4; ++column)
		{
			for (glm::length_t row = 0; row < 4; ++row)
			{
				largest = std::max({largest, std::abs(a[column][row]), std::abs(b[column][row])});
			}
		}
		for (glm::length_t column = 0; column < 4; ++column)
		{
			for (glm::length_t row = 0; row < 4; ++row)
			{
				if (std::abs(a[column][row] - b[column][row]) > tolerance * largest)
				{
					return false;
				}
			}
		}
		return true;
	}

	std::vector<entt::entity> GetHandles(Scene& scene)
	{
		std::vector<entt::entity> handles;
		for (entt::entity const handle : scene.GetAllEntitiesWith<IDComponent>())
		{
			handles.push_back(handle);
		}
		// The view's order follows the registry's history: sorted, a pick by index is the same on every platform.
		std::sort(handles.begin(), handles.end(),
		          [&scene](entt::entity a, entt::entity b)
		          {
					  return scene.GetRegistry().get<IDComponent>(a).ID.GetValue() < scene.GetRegistry().get<IDComponent>(b).ID.GetValue();
				  });
		return handles;
	}

	// The benchmark's scene: 100 roots of 119 children each, 12,000 entities.
	struct BenchmarkScene
	{
		static constexpr size_t Roots = 100;
		static constexpr size_t ChildrenPerRoot = 119;

		Scene World{"Benchmark"};
		std::vector<Entity> RootEntities;
		std::vector<Entity> Children;

		BenchmarkScene()
		{
			Testing::FuzzRandom generator(12000);
			for (size_t root = 0; root < Roots; ++root)
			{
				Entity group = World.CreateEntity("Group");
				group.GetComponent<TransformComponent>().Translation = {static_cast<float>(root % 10) * 40.0f, 0.0f,
				                                                        static_cast<float>(root / 10) * 40.0f};
				RootEntities.push_back(group);
				for (size_t child = 0; child < ChildrenPerRoot; ++child)
				{
					Entity prop = World.CreateEntity("Prop", group);
					TransformComponent& transform = prop.GetComponent<TransformComponent>();
					transform.Translation = RandomVector(generator, -20.0f, 20.0f);
					transform.Rotation = RandomRotation(generator);
					transform.Scale = RandomVector(generator, 0.5f, 2.0f);
					Children.push_back(prop);
				}
			}
		}
	};

	// Per frame: the frame's writes, then the scene's frame begun and its one refresh (the player's frame: its update,
	// then its view), measured by the refresh's profiler zone; the median per call.
	template<typename TWrite>
	double MeasureRefresh(BenchmarkScene& bench, int frames, TWrite const& write)
	{
		REQUIRE(Profiler::Start().IsOk());
		for (int frame = 0; frame < frames; ++frame)
		{
			Profiler::BeginFrame();
			bench.World.BeginTransformFrame();
			write(frame);
			bench.World.UpdateTransforms();
			Profiler::EndFrame();
		}
		Profiler::Stop();
		ProfilerMedians const medians = ComputeProfilerMedians(Profiler::GetRecording());
		for (ProfilerZoneMedian const& zone : medians.Zones)
		{
			if (zone.Name == "Scene.UpdateTransforms")
			{
				return zone.MillisecondsPerCall;
			}
		}
		FAIL("no Scene.UpdateTransforms zone was recorded");
		return 0.0;
	}
}

TEST_CASE("Scene: world transforms equal an uncached walk after every edit of fuzzed hierarchies")
{
	int const rounds = Testing::GetFuzzRounds(2000);
	Testing::FuzzRandom generator(0x5EED'7124'0001ull);
	Scene scene("Fuzzed");
	std::vector<UUID> destroyed;
	for (int index = 0; index < 200; ++index)
	{
		Entity entity = scene.CreateEntity("Entity");
		entity.GetComponent<TransformComponent>().Translation = RandomVector(generator, -10.0f, 10.0f);
	}

	auto const checkReads = [&scene, &generator](int checkedRound, size_t reads)
	{
		std::vector<entt::entity> const readable = GetHandles(scene);
		for (size_t read = 0; read < reads && !readable.empty(); ++read)
		{
			entt::entity const picked = readable[generator.Pick(readable.size())];
			glm::mat4 const world = scene.GetWorldTransform(Entity(picked, &scene));
			CAPTURE(checkedRound);
			REQUIRE(HasSameBits(world, ReferenceWorld(scene, picked)));
			if (IsFinite(world) && !ReachesCycle(scene, picked))
			{
				CHECK(IsClose(world, BottomUpWorld(scene, picked), 1e-5f));
			}
		}
	};

	for (int round = 0; round < rounds; ++round)
	{
		std::vector<entt::entity> const handles = GetHandles(scene);
		Entity entity = handles.empty() ? Entity() : Entity(handles[generator.Pick(handles.size())], &scene);
		Entity other = handles.empty() ? Entity() : Entity(handles[generator.Pick(handles.size())], &scene);
		switch (generator.Pick(14))
		{
			case 0:
			case 1:
			case 2:
				if (entity)
				{
					// Writes of the public fields, among them values that only bits tell apart.
					TransformComponent& transform = entity.GetComponent<TransformComponent>();
					switch (generator.Pick(5))
					{
						case 0:
							transform.Translation = RandomVector(generator, -10.0f, 10.0f);
							break;
						case 1:
							transform.Rotation = RandomRotation(generator);
							break;
						case 2:
							transform.Scale = RandomVector(generator, 0.8f, 1.25f);
							break;
						case 3:
							transform.Translation.x = std::signbit(transform.Translation.x) ? 0.0f : -0.0f;
							break;
						default:
							transform.Translation.y = generator.Pick(8) == 0 ? std::numeric_limits<float>::quiet_NaN() : 1.0f;
							break;
					}
				}
				break;
			case 3:
				if (entity)
				{
					scene.SetWorldTransform(
						entity, Math::ComposeTransform(RandomVector(generator, -10.0f, 10.0f), RandomRotation(generator), glm::vec3(1.0f)));
				}
				break;
			case 4:
				if (entity)
				{
					static_cast<void>(scene.SetParent(entity, generator.Pick(4) == 0 ? Entity() : other, generator.Pick(2) == 0));
				}
				break;
			case 5:
				if (entity && other)
				{
					// What a serializer or an edit of the component may leave: a parent its children list does not name,
					// a cycle, the entity as its own parent.
					entity.GetComponent<RelationshipComponent>().Parent = other.GetUUID();
				}
				break;
			case 6:
				if (entity)
				{
					// A parent the scene does not hold, perhaps the UUID of an entity made again later.
					entity.GetComponent<RelationshipComponent>().Parent = !destroyed.empty() && generator.Pick(2) == 0
					                                                          ? destroyed[generator.Pick(destroyed.size())]
					                                                          : UUID(generator.Next() | 1u);
				}
				break;
			case 7:
				if (entity && handles.size() > 20 && IsTreeBelow(scene, entity))
				{
					destroyed.push_back(entity.GetUUID());
					scene.DestroyEntity(entity);
				}
				break;
			case 8:
				if (!destroyed.empty() && generator.Pick(2) == 0)
				{
					// Made again with its old UUID, as undo does: a new handle under that UUID.
					size_t const pick = generator.Pick(destroyed.size());
					UUID const id = destroyed[pick];
					destroyed.erase(destroyed.begin() + static_cast<std::ptrdiff_t>(pick));
					if (!scene.HasEntity(id))
					{
						scene.CreateEntityWithUUID(id, "Again").GetComponent<TransformComponent>().Translation =
							RandomVector(generator, -10.0f, 10.0f);
					}
				}
				else if (handles.size() < 300)
				{
					Entity created = scene.CreateEntity("Created", generator.Pick(2) == 0 ? other : Entity());
					created.GetComponent<TransformComponent>().Rotation = RandomRotation(generator);
				}
				break;
			case 9:
				if (entity && handles.size() < 300 && IsTreeBelow(scene, entity))
				{
					static_cast<void>(scene.DuplicateEntity(entity));
				}
				break;
			case 10:
			case 11:
			{
				if (generator.Pick(2) == 0)
				{
					scene.BeginTransformFrame();
				}
				scene.UpdateTransforms();
				// After a refresh every cached matrix is exact.
				for (entt::entity const handle : GetHandles(scene))
				{
					CAPTURE(round);
					CachedWorldTransform const cached = scene.GetCachedWorldTransform(handle);
					REQUIRE(HasSameBits(cached.World, ReferenceWorld(scene, handle)));
				}
				break;
			}
			default:
				if (entity)
				{
					entity.GetComponent<TransformComponent>().Translation += glm::vec3(0.5f, 0.0f, 0.0f);
				}
				break;
		}
		checkReads(round, 4);
	}
}

TEST_CASE("Scene: a child read in the frame its parent moved sees the move, however the parent was written")
{
	Scene scene("Moved");
	Entity parent = scene.CreateEntity("Parent");
	TransformComponent& parentTransform = parent.GetComponent<TransformComponent>();
	parentTransform.Translation = {2.0f, 0.0f, 0.0f};
	parentTransform.Rotation = glm::angleAxis(0.7f, glm::normalize(glm::vec3(1.0f, 2.0f, 0.5f)));
	parentTransform.Scale = {1.5f, 0.5f, 2.0f};
	Entity child = scene.CreateEntity("Child", parent);
	child.GetComponent<TransformComponent>().Translation = {0.0f, 1.0f, 3.0f};
	Entity grandchild = scene.CreateEntity("Grandchild", child);
	grandchild.GetComponent<TransformComponent>().Translation = {1.0f, 0.0f, 0.0f};
	scene.BeginTransformFrame();
	scene.UpdateTransforms();
	glm::mat4 const before = scene.GetCachedWorldTransform(grandchild.GetHandle()).World;
	REQUIRE(HasSameBits(before, ReferenceWorld(scene, grandchild.GetHandle())));

	// A direct write of the public field, as scripts, the inspector and physics make: nothing tells the cache.
	parentTransform.Translation.x += 1.0f;
	glm::mat4 const moved = scene.GetWorldTransform(grandchild);
	CHECK(HasSameBits(moved, ReferenceWorld(scene, grandchild.GetHandle())));
	CHECK_FALSE(HasSameBits(moved, before));
	// The read changed no cached matrix: the cache still holds what the refresh computed.
	CHECK(HasSameBits(scene.GetCachedWorldTransform(grandchild.GetHandle()).World, before));

	// Through Entity::GetComponent, on the child in the middle.
	child.GetComponent<TransformComponent>().Rotation = glm::angleAxis(-0.4f, glm::vec3(0.0f, 1.0f, 0.0f));
	CHECK(HasSameBits(scene.GetWorldTransform(grandchild), ReferenceWorld(scene, grandchild.GetHandle())));

	// Through SetWorldTransform, which itself reads the parent's world matrix.
	scene.SetWorldTransform(parent, Math::ComposeTransform({-3.0f, 4.0f, 1.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(2.0f)));
	CHECK(HasSameBits(scene.GetWorldTransform(child), ReferenceWorld(scene, child.GetHandle())));
	CHECK(HasSameBits(scene.GetWorldTransform(grandchild), ReferenceWorld(scene, grandchild.GetHandle())));
	CHECK(glm::vec3(scene.GetWorldTransform(parent)[3]).x == doctest::Approx(-3.0f));

	// The next refresh computes the same bits the reads did.
	glm::mat4 const read = scene.GetWorldTransform(grandchild);
	scene.UpdateTransforms();
	CHECK(HasSameBits(scene.GetCachedWorldTransform(grandchild.GetHandle()).World, read));
}

TEST_CASE("Scene: scripts read their children where a parent moved in the same frame left them")
{
	Testing::ScriptEngineScope scripting;
	Scene scene("Scripted");
	Entity mover = scene.CreateEntity("Mover");
	mover.GetComponent<TransformComponent>().Scale = {2.0f, 2.0f, 2.0f};
	mover.GetComponent<TransformComponent>().Rotation = glm::angleAxis(0.3f, glm::vec3(0.0f, 0.0f, 1.0f));
	Entity child = scene.CreateEntity("Child", mover);
	child.GetComponent<TransformComponent>().Translation = {0.25f, 1.0f, 0.0f};
	ScriptComponent& script = mover.AddComponent<ScriptComponent>();
	script.ClassName = "Strada.Tests.ParentMover";
	script.Fields = {{"Child", ScriptFieldValue::FromEntity(child.GetUUID())}};

	scene.OnRuntimeStart();
	for (int frame = 1; frame <= 5; ++frame)
	{
		CAPTURE(frame);
		// The view of the frame before filled the cache: the script's read of the child must not take it.
		scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
		std::string const name = mover.GetName();
		char const* text = name.c_str();
		glm::vec3 seen(0.0f);
		for (glm::length_t axis = 0; axis < 3; ++axis)
		{
			char* end = nullptr;
			seen[axis] = std::strtof(text, &end);
			REQUIRE(end != text);
			text = *end == ';' ? end + 1 : end;
		}
		glm::mat4 const world = ReferenceWorld(scene, child.GetHandle());
		CHECK(HasSameBits(seen, glm::vec3(world[3])));
		CHECK(mover.GetComponent<TransformComponent>().Translation.x == doctest::Approx(static_cast<float>(frame)));
		scene.UpdateTransforms();
	}
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: the cache follows reparenting, parent links the children lists do not name, cycles and a parent made later")
{
	Scene scene("Links");
	Entity first = scene.CreateEntity("First");
	first.GetComponent<TransformComponent>().Translation = {10.0f, 0.0f, 0.0f};
	first.GetComponent<TransformComponent>().Rotation = glm::angleAxis(1.1f, glm::vec3(0.0f, 1.0f, 0.0f));
	Entity second = scene.CreateEntity("Second");
	second.GetComponent<TransformComponent>().Translation = {0.0f, -5.0f, 0.0f};
	second.GetComponent<TransformComponent>().Scale = {3.0f, 3.0f, 3.0f};
	Entity child = scene.CreateEntity("Child", first);
	child.GetComponent<TransformComponent>().Translation = {1.0f, 2.0f, 3.0f};
	scene.BeginTransformFrame();
	scene.UpdateTransforms();
	auto const checkAll = [&scene]()
	{
		for (entt::entity const handle : GetHandles(scene))
		{
			CHECK(HasSameBits(scene.GetWorldTransform(Entity(handle, &scene)), ReferenceWorld(scene, handle)));
		}
		scene.UpdateTransforms();
		for (entt::entity const handle : GetHandles(scene))
		{
			CHECK(HasSameBits(scene.GetCachedWorldTransform(handle).World, ReferenceWorld(scene, handle)));
		}
	};

	// Keeping the world transform: the child stays where it was.
	glm::mat4 const before = scene.GetWorldTransform(child);
	REQUIRE(scene.SetParent(child, second, true).IsOk());
	CHECK(IsClose(scene.GetWorldTransform(child), before, 1e-5f));
	checkAll();
	// Without: the local transform now counts from the new parent.
	REQUIRE(scene.SetParent(child, first, false).IsOk());
	checkAll();

	// The Parent field written directly, as a serializer writes it: the field decides, whatever the lists say.
	child.GetComponent<RelationshipComponent>().Parent = second.GetUUID();
	checkAll();

	// A parent the scene does not hold makes the entity a root, until an entity of that UUID is made.
	UUID const missing(0x0123'4567'89AB'CDEFull);
	child.GetComponent<RelationshipComponent>().Parent = missing;
	CHECK(HasSameBits(scene.GetWorldTransform(child), child.GetComponent<TransformComponent>().GetTransform()));
	checkAll();
	Entity late = scene.CreateEntityWithUUID(missing, "Late");
	late.GetComponent<TransformComponent>().Translation = {0.0f, 0.0f, 7.0f};
	CHECK(
		HasSameBits(scene.GetWorldTransform(child), Math::ComposeWorldTransform(late.GetComponent<TransformComponent>().GetTransform(),
	                                                                            child.GetComponent<TransformComponent>().GetTransform())));
	checkAll();

	// A cycle written directly: each entity on it is a root, and what hangs below it is computed under it.
	first.GetComponent<RelationshipComponent>().Parent = second.GetUUID();
	second.GetComponent<RelationshipComponent>().Parent = first.GetUUID();
	child.GetComponent<RelationshipComponent>().Parent = first.GetUUID();
	CHECK(HasSameBits(scene.GetWorldTransform(first), first.GetComponent<TransformComponent>().GetTransform()));
	CHECK(HasSameBits(scene.GetWorldTransform(second), second.GetComponent<TransformComponent>().GetTransform()));
	checkAll();
	// An entity its own parent.
	late.GetComponent<RelationshipComponent>().Parent = late.GetUUID();
	CHECK(HasSameBits(scene.GetWorldTransform(late), late.GetComponent<TransformComponent>().GetTransform()));
	checkAll();
	// Broken again: the former members of the cycle are children once more.
	second.GetComponent<RelationshipComponent>().Parent = UUID::Invalid();
	CHECK(
		HasSameBits(scene.GetWorldTransform(first), Math::ComposeWorldTransform(second.GetComponent<TransformComponent>().GetTransform(),
	                                                                            first.GetComponent<TransformComponent>().GetTransform())));
	checkAll();
}

TEST_CASE("Scene: an entity destroyed and made again under its UUID is computed afresh, under its parent's new handle")
{
	Scene scene("Again");
	Entity parent = scene.CreateEntity("Parent");
	parent.GetComponent<TransformComponent>().Translation = {4.0f, 0.0f, 0.0f};
	UUID const parentId = parent.GetUUID();
	Entity child = scene.CreateEntity("Child", parent);
	child.GetComponent<TransformComponent>().Translation = {0.0f, 1.0f, 0.0f};
	UUID const childId = child.GetUUID();
	scene.BeginTransformFrame();
	scene.UpdateTransforms();
	scene.BeginTransformFrame();

	// Undo's way: the parent and its child go, then come back under their UUIDs with new handles and other values.
	scene.DestroyEntity(parent);
	CHECK(scene.GetEntityCount() == 0);
	Entity again = scene.CreateEntityWithUUID(parentId, "Parent");
	again.GetComponent<TransformComponent>().Translation = {-4.0f, 0.0f, 0.0f};
	Entity againChild = scene.CreateEntityWithUUID(childId, "Child");
	REQUIRE(scene.SetParent(againChild, again, false).IsOk());
	againChild.GetComponent<TransformComponent>().Translation = {0.0f, 1.0f, 0.0f};

	// Before any refresh: computed as a read would, with no previous matrix of its own.
	CachedWorldTransform const unrefreshed = scene.GetCachedWorldTransform(againChild.GetHandle());
	CHECK(HasSameBits(unrefreshed.World, ReferenceWorld(scene, againChild.GetHandle())));
	CHECK(HasSameBits(unrefreshed.Previous, unrefreshed.World));
	CHECK(glm::vec3(unrefreshed.World[3]).x == doctest::Approx(-4.0f));
	scene.UpdateTransforms();
	CachedWorldTransform const refreshed = scene.GetCachedWorldTransform(againChild.GetHandle());
	CHECK(HasSameBits(refreshed.World, unrefreshed.World));
	CHECK(HasSameBits(refreshed.Previous, refreshed.World));
}

TEST_CASE("Scene: copies, duplicates and prefab instances start without entries and without motion, and read right")
{
	Testing::AssetManagerScope assets;
	Scene scene("Original");
	Entity crate = scene.CreateEntity("Crate");
	crate.GetComponent<TransformComponent>().Translation = {1.0f, 0.0f, 0.0f};
	crate.GetComponent<TransformComponent>().Scale = {1.0f, 2.0f, 1.0f};
	Entity lid = scene.CreateEntity("Lid", crate);
	lid.GetComponent<TransformComponent>().Translation = {0.0f, 0.5f, 0.0f};
	scene.BeginTransformFrame();
	scene.UpdateTransforms();
	scene.BeginTransformFrame();
	crate.GetComponent<TransformComponent>().Translation.x += 1.0f;
	scene.UpdateTransforms();
	CachedWorldTransform const moved = scene.GetCachedWorldTransform(lid.GetHandle());
	REQUIRE_FALSE(HasSameBits(moved.Previous, moved.World));

	auto const checkStill = [](Scene& target, Entity entity)
	{
		CachedWorldTransform const cached = target.GetCachedWorldTransform(entity.GetHandle());
		CHECK(HasSameBits(cached.World, ReferenceWorld(target, entity.GetHandle())));
		CHECK(HasSameBits(cached.Previous, cached.World));
	};

	// A copy (play mode's) is another scene: its own serial, no frame begun, no entries.
	Ref<Scene> const copy = Scene::Copy(scene);
	CHECK(copy->GetSerial() != scene.GetSerial());
	CHECK(copy->GetSerial() != 0);
	CHECK(copy->GetTransformFrame() == 0);
	Entity const copiedLid = copy->GetEntityByUUID(lid.GetUUID());
	REQUIRE(copiedLid);
	checkStill(*copy, copiedLid);
	copy->BeginTransformFrame();
	copy->UpdateTransforms();
	checkStill(*copy, copiedLid);

	// A duplicate made in the frame the original moved does not move in it.
	Entity const duplicate = scene.DuplicateEntity(crate);
	REQUIRE(duplicate);
	Entity const duplicateLid = scene.GetChildren(duplicate).front();
	checkStill(scene, duplicateLid);
	scene.UpdateTransforms();
	checkStill(scene, duplicateLid);
	CachedWorldTransform const original = scene.GetCachedWorldTransform(lid.GetHandle());
	CHECK_FALSE(HasSameBits(original.Previous, original.World));

	// A prefab instance under the moved crate.
	Result<Ref<PrefabAsset>> prefabAsset = PrefabAsset::Create(PrefabSerializer::Serialize(scene, crate));
	REQUIRE(prefabAsset.IsOk());
	AssetHandle const prefab = AssetManager::AddMemoryAsset(prefabAsset.GetValue(), "Crate");
	Result<Entity> const instance = scene.InstantiatePrefab(prefab, {0.0f, 3.0f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), crate);
	REQUIRE(instance.IsOk());
	Entity const instanceLid = scene.GetChildren(instance.GetValue()).front();
	checkStill(scene, instance.GetValue());
	checkStill(scene, instanceLid);
	scene.UpdateTransforms();
	checkStill(scene, instanceLid);
}

TEST_CASE("Scene: the previous matrix is the one each frame began with, for every view of the frame")
{
	Scene scene("Frames");
	Entity entity = scene.CreateEntity("Entity");
	Entity child = scene.CreateEntity("Child", entity);
	child.GetComponent<TransformComponent>().Translation = {0.0f, 1.0f, 0.0f};
	TransformComponent& transform = entity.GetComponent<TransformComponent>();
	auto const cached = [&scene, &child]()
	{
		return scene.GetCachedWorldTransform(child.GetHandle());
	};

	// A scene that never began a frame (a thumbnail's, a test's) has no motion.
	CHECK(scene.GetTransformFrame() == 0);
	scene.UpdateTransforms();
	transform.Translation.x = 1.0f;
	scene.UpdateTransforms();
	CHECK(HasSameBits(cached().Previous, cached().World));
	glm::mat4 const unbegun = cached().World;

	// The first frame begun: what the cache held before it is its previous matrix.
	scene.BeginTransformFrame();
	CHECK(scene.GetTransformFrame() == 1);
	transform.Translation.x = 2.0f;
	scene.UpdateTransforms();
	glm::mat4 const first = cached().World;
	CHECK(HasSameBits(cached().Previous, unbegun));

	// Moved: the previous matrix is the frame before's, in every view of the frame, also after a second move.
	scene.BeginTransformFrame();
	transform.Translation.x = 3.0f;
	scene.UpdateTransforms();
	glm::mat4 const second = cached().World;
	CHECK(HasSameBits(cached().Previous, first));
	CHECK_FALSE(HasSameBits(second, first));
	scene.UpdateTransforms();
	CHECK(HasSameBits(cached().Previous, first));
	CHECK(HasSameBits(cached().World, second));
	transform.Translation.x = 4.0f;
	scene.UpdateTransforms();
	glm::mat4 const third = cached().World;
	CHECK(HasSameBits(cached().Previous, first));

	// Still: no motion.
	scene.BeginTransformFrame();
	scene.UpdateTransforms();
	CHECK(HasSameBits(cached().Previous, third));
	CHECK(HasSameBits(cached().World, third));

	// Frames that no view drew refresh nothing: the previous matrix is the last refresh's, and the frame count tells a
	// renderer that frames passed.
	uint64_t const drawn = scene.GetTransformFrame();
	for (int frame = 0; frame < 3; ++frame)
	{
		scene.BeginTransformFrame();
		transform.Translation.x += 1.0f;
	}
	scene.UpdateTransforms();
	CHECK(scene.GetTransformFrame() == drawn + 3);
	CHECK(HasSameBits(cached().Previous, third));
	CHECK(HasSameBits(cached().World, ReferenceWorld(scene, child.GetHandle())));

	// An entity made since the refresh is computed, without motion.
	Entity const made = scene.CreateEntity("Made", entity);
	CachedWorldTransform const fresh = scene.GetCachedWorldTransform(made.GetHandle());
	CHECK(HasSameBits(fresh.World, ReferenceWorld(scene, made.GetHandle())));
	CHECK(HasSameBits(fresh.Previous, fresh.World));

	// Serials tell scenes apart.
	Scene other("Other");
	CHECK(other.GetSerial() != scene.GetSerial());
}

TEST_CASE("Scene: a running scene begins a frame in every update, paused or not, and a stopped one in none")
{
	Scene scene("Running");
	Entity entity = scene.CreateEntity("Entity");
	SceneRuntimeSettings settings;
	settings.RunScripts = false;
	settings.PlayAudio = false;
	scene.OnRuntimeStart(settings);
	scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
	CHECK(scene.GetTransformFrame() == 1);
	CHECK(scene.GetRuntimeFrame() == 1);

	// Paused, the views still draw frames: each begins one, and no runtime frame runs.
	scene.SetPaused(true);
	scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
	scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
	CHECK(scene.GetTransformFrame() == 3);
	CHECK(scene.GetRuntimeFrame() == 1);
	// An edit made while paused shows as motion in the frame that draws it.
	scene.UpdateTransforms();
	scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
	entity.GetComponent<TransformComponent>().Translation.y = 2.0f;
	scene.UpdateTransforms();
	CachedWorldTransform const edited = scene.GetCachedWorldTransform(entity.GetHandle());
	CHECK(edited.Previous[3].y == 0.0f);
	CHECK(edited.World[3].y == 2.0f);

	// A step runs a frame, which begins one.
	scene.Step(1);
	scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
	CHECK(scene.GetTransformFrame() == 5);
	CHECK(scene.GetRuntimeFrame() == 2);
	scene.OnRuntimeStop();
	scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
	CHECK(scene.GetTransformFrame() == 5);
}

TEST_CASE("ScenePhysics: bodies drawn between steps are cached where they are drawn, and steps read the parents moved before them")
{
	Testing::PhysicsSystemScope physics;
	Scene scene("Drawn");
	SceneRuntimeSettings settings;
	settings.RunScripts = false;
	settings.PlayAudio = false;

	// A body that keeps its velocity, drawn between steps at 144 frames per second, and an entity riding on it.
	Entity body = scene.CreateEntity("Body");
	body.GetComponent<TransformComponent>().Translation = {0.0f, 2.0f, 0.0f};
	RigidBodyComponent& rigidBody = body.AddComponent<RigidBodyComponent>();
	rigidBody.Type = RigidBodyType::Dynamic;
	rigidBody.GravityFactor = 0.0f;
	rigidBody.LinearDamping = 0.0f;
	rigidBody.AllowSleep = false;
	rigidBody.InitialLinearVelocity = {3.0f, 0.0f, 0.0f};
	rigidBody.Interpolation = RigidBodyInterpolation::Interpolate;
	body.AddComponent<BoxColliderComponent>().HalfExtents = {0.5f, 0.5f, 0.5f};
	Entity rider = scene.CreateEntity("Rider", body);
	rider.GetComponent<TransformComponent>().Translation = {0.0f, 1.0f, 0.0f};

	// A kinematic body under a carrier that each frame moves before its steps, with no refresh between: the steps read the
	// carrier's move through the body's chain.
	Entity carrier = scene.CreateEntity("Carrier");
	carrier.GetComponent<TransformComponent>().Rotation = glm::angleAxis(0.5f, glm::vec3(0.0f, 1.0f, 0.0f));
	Entity platform = scene.CreateEntity("Platform", carrier);
	platform.GetComponent<TransformComponent>().Translation = {2.0f, 0.0f, 0.0f};
	RigidBodyComponent& kinematic = platform.AddComponent<RigidBodyComponent>();
	kinematic.Type = RigidBodyType::Kinematic;
	kinematic.Interpolation = RigidBodyInterpolation::None;
	platform.AddComponent<BoxColliderComponent>().HalfExtents = {0.5f, 0.1f, 0.5f};

	scene.OnRuntimeStart(settings);
	REQUIRE(scene.GetPhysicsScene() != nullptr);
	glm::mat4 lastRider = scene.GetWorldTransform(rider);
	for (int frame = 0; frame < 72; ++frame)
	{
		CAPTURE(frame);
		carrier.GetComponent<TransformComponent>().Translation.z = 0.05f * static_cast<float>(frame);
		uint64_t const steps = scene.GetFixedStepCount();
		scene.OnUpdateRuntime(Timestep(1.0f / 144.0f));
		if (scene.GetFixedStepCount() != steps)
		{
			std::optional<BodyTransform> const simulated = scene.GetPhysicsScene()->GetBodyTransform(platform.GetUUID());
			REQUIRE(simulated.has_value());
			CHECK(glm::length(simulated->Position - glm::vec3(ReferenceWorld(scene, platform.GetHandle())[3])) < 1e-4f);
		}
		// The view's refresh: what the frame drew, and the frame before's matrix.
		scene.UpdateTransforms();
		for (Entity const& entity : {body, rider, carrier, platform})
		{
			CHECK(HasSameBits(scene.GetCachedWorldTransform(entity.GetHandle()).World, ReferenceWorld(scene, entity.GetHandle())));
		}
		CachedWorldTransform const riding = scene.GetCachedWorldTransform(rider.GetHandle());
		CHECK(HasSameBits(riding.Previous, lastRider));
		lastRider = riding.World;
	}
	CHECK(scene.GetFixedStepCount() >= 29);
	CHECK(lastRider[3].x > 1.0f);
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: the world-transform cache's refresh of 12,000 entities, benchmark" *
          doctest::skip(!Platform::ReadEnvironmentVariable("STRADA_TRANSFORM_BENCHMARK")))
{
	// A measurement, never asserted on timings: run with STRADA_TRANSFORM_BENCHMARK=1 in a Release build.
	using Clock = std::chrono::steady_clock;
	constexpr int Frames = 300;
	Testing::ProfilerScope const profiler;
	BenchmarkScene bench;
	REQUIRE(bench.World.GetEntityCount() == 12000);
	// The cache starts full, as after a scene's first frame.
	bench.World.BeginTransformFrame();
	bench.World.UpdateTransforms();

	double const still = MeasureRefresh(bench, Frames, [](int) {});
	double const some = MeasureRefresh(bench, Frames,
	                                   [&bench](int frame)
	                                   {
										   // 1,000 children, another thousand each frame.
										   for (size_t index = 0; index < 1000; ++index)
										   {
											   size_t const child = (static_cast<size_t>(frame) * 1000 + index) % bench.Children.size();
											   bench.Children[child].GetComponent<TransformComponent>().Translation.y += 0.01f;
										   }
									   });
	double const all = MeasureRefresh(bench, Frames,
	                                  [&bench](int)
	                                  {
										  // Every root, so every matrix changes.
										  for (Entity& root : bench.RootEntities)
										  {
											  root.GetComponent<TransformComponent>().Translation.y += 0.01f;
										  }
									  });

	// Reads through checked chains of a current cache, against slice 1's walk.
	std::vector<double> cachedReads;
	std::vector<double> walkedReads;
	glm::vec4 sink(0.0f);
	for (int repeat = 0; repeat < 21; ++repeat)
	{
		auto const cachedStart = Clock::now();
		for (Entity const& child : bench.Children)
		{
			sink += bench.World.GetWorldTransform(child)[3];
		}
		auto const walkedStart = Clock::now();
		for (Entity const& child : bench.Children)
		{
			sink += BottomUpWorld(bench.World, child.GetHandle())[3];
		}
		auto const walkedEnd = Clock::now();
		cachedReads.push_back(std::chrono::duration<double, std::milli>(walkedStart - cachedStart).count());
		walkedReads.push_back(std::chrono::duration<double, std::milli>(walkedEnd - walkedStart).count());
	}
	std::sort(cachedReads.begin(), cachedReads.end());
	std::sort(walkedReads.begin(), walkedReads.end());

	// The product alone, the affine one every world matrix is made with against glm's full one, over 12,000 pairs.
	std::vector<glm::mat4> locals;
	for (Entity const& child : bench.Children)
	{
		locals.push_back(child.GetComponent<TransformComponent>().GetTransform());
	}
	glm::mat4 const parent = bench.RootEntities.front().GetComponent<TransformComponent>().GetTransform();
	std::vector<double> affine;
	std::vector<double> full;
	for (int repeat = 0; repeat < 21; ++repeat)
	{
		auto const affineStart = Clock::now();
		for (glm::mat4 const& local : locals)
		{
			sink += Math::ComposeWorldTransform(parent, local)[3];
		}
		auto const fullStart = Clock::now();
		for (glm::mat4 const& local : locals)
		{
			sink += (parent * local)[3];
		}
		auto const fullEnd = Clock::now();
		affine.push_back(std::chrono::duration<double, std::milli>(fullStart - affineStart).count());
		full.push_back(std::chrono::duration<double, std::milli>(fullEnd - fullStart).count());
	}
	std::sort(affine.begin(), affine.end());
	std::sort(full.begin(), full.end());

	MESSAGE("Refresh of 12,000 entities (target 0.5 ms): nothing moved " << still << " ms, 1,000 children moved " << some
	                                                                     << " ms, every root moved " << all << " ms");
	MESSAGE("12,000 reads of a current cache: " << cachedReads[cachedReads.size() / 2]
	                                            << " ms; slice 1's walk: " << walkedReads[walkedReads.size() / 2] << " ms");
	MESSAGE("12,000 products: affine " << affine[affine.size() / 2] << " ms, glm's full product " << full[full.size() / 2] << " ms"
	                                   << " (checksum " << sink.x << ", which keeps the loops from being optimized away)");
}
