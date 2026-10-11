#include "Strada/Scene/Entity.h"
#include "Strada/Scene/ParticleEmitter.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <limits>

using namespace Strada;

namespace
{
	ParticleSystemComponent StillSettings()
	{
		ParticleSystemComponent settings;
		settings.Emitting = false;
		settings.SpeedMin = 0.0f;
		settings.SpeedMax = 0.0f;
		settings.LifetimeMin = 10.0f;
		settings.LifetimeMax = 10.0f;
		return settings;
	}
}

TEST_CASE("ParticleEmitter: emitting fills up to MaxParticles from the shape placed by the transform")
{
	ParticleEmitter emitter(7);
	ParticleSystemComponent settings = StillSettings();
	settings.MaxParticles = 50;
	settings.Shape = ParticleEmitterShape::Sphere;
	settings.Radius = 2.0f;
	settings.SpeedMin = 1.0f;
	settings.SpeedMax = 1.0f;
	glm::mat4 const world = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f));
	emitter.Emit(settings, world, 30);
	CHECK(emitter.GetCount() == 30);
	emitter.Emit(settings, world, 30);
	CHECK(emitter.GetCount() == 50);
	for (Particle const& particle : emitter.GetParticles())
	{
		glm::vec3 const offset = particle.Position - glm::vec3(10.0f, 0.0f, 0.0f);
		CHECK(glm::length(offset) <= 2.0f + 1e-4f);
		// Sphere particles move outwards at their speed.
		CHECK(glm::length(particle.Velocity) == doctest::Approx(1.0f));
		CHECK(particle.Age == 0.0f);
		CHECK(particle.Lifetime == 10.0f);
	}

	// Cones start on a disc in the XY plane and go within the angle of -Z (here rotated to +Y).
	emitter.Clear();
	CHECK(emitter.GetCount() == 0);
	settings.Shape = ParticleEmitterShape::Cone;
	settings.Radius = 0.5f;
	settings.ConeAngle = 30.0f;
	glm::mat4 const up = glm::rotate(glm::mat4(1.0f), glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
	emitter.Emit(settings, up, 50);
	for (Particle const& particle : emitter.GetParticles())
	{
		CHECK(std::abs(particle.Position.y) < 1e-5f);
		CHECK(glm::length(particle.Position) <= 0.5f + 1e-4f);
		float const angle = glm::degrees(std::acos(glm::clamp(glm::normalize(particle.Velocity).y, -1.0f, 1.0f)));
		CHECK(angle <= 30.0f + 0.01f);
	}

	// Boxes fill their extents.
	emitter.Clear();
	settings.Shape = ParticleEmitterShape::Box;
	settings.BoxHalfExtents = {1.0f, 2.0f, 3.0f};
	emitter.Emit(settings, glm::mat4(1.0f), 50);
	for (Particle const& particle : emitter.GetParticles())
	{
		CHECK(glm::all(glm::lessThanEqual(glm::abs(particle.Position), glm::vec3(1.0f, 2.0f, 3.0f) + 1e-4f)));
	}

	// Local particles ignore the transform (the renderer places them).
	emitter.Clear();
	settings.WorldSpace = false;
	settings.Shape = ParticleEmitterShape::Point;
	emitter.Emit(settings, world, 5);
	for (Particle const& particle : emitter.GetParticles())
	{
		CHECK(particle.Position == glm::vec3(0.0f));
	}
}

TEST_CASE("ParticleEmitter: particles age, fall, slow down and die; emission follows the rate")
{
	ParticleEmitter emitter(1);
	ParticleSystemComponent settings = StillSettings();
	settings.LifetimeMin = 1.0f;
	settings.LifetimeMax = 1.0f;
	settings.GravityFactor = 2.0f;
	settings.Shape = ParticleEmitterShape::Point;
	emitter.Emit(settings, glm::mat4(1.0f), 10);
	glm::vec3 const gravity(0.0f, -10.0f, 0.0f);
	emitter.Update(settings, glm::mat4(1.0f), gravity, 0.1f);
	REQUIRE(emitter.GetCount() == 10);
	for (Particle const& particle : emitter.GetParticles())
	{
		CHECK(particle.Age == doctest::Approx(0.1f));
		CHECK(particle.Velocity.y == doctest::Approx(-2.0f));
		CHECK(particle.Position.y == doctest::Approx(-0.2f));
	}
	// Drag takes the given fraction of the velocity per second.
	settings.GravityFactor = 0.0f;
	settings.Drag = 5.0f;
	emitter.Update(settings, glm::mat4(1.0f), gravity, 0.1f);
	CHECK(emitter.GetParticles()[0].Velocity.y == doctest::Approx(-1.0f));
	// Lifetimes end.
	for (int i = 0; i < 9; i++)
	{
		emitter.Update(settings, glm::mat4(1.0f), gravity, 0.1f);
	}
	CHECK(emitter.GetCount() == 0);

	// Rate: 10 per second, while emitting.
	settings.Emitting = true;
	settings.Rate = 10.0f;
	settings.LifetimeMin = 100.0f;
	settings.LifetimeMax = 100.0f;
	for (int i = 0; i < 20; i++)
	{
		emitter.Update(settings, glm::mat4(1.0f), gravity, 0.05f);
	}
	CHECK(emitter.GetCount() == 10);
	settings.Emitting = false;
	emitter.Update(settings, glm::mat4(1.0f), gravity, 1.0f);
	CHECK(emitter.GetCount() == 10);
	// A stall simulates at most MaxTimestep.
	settings.Emitting = true;
	emitter.Update(settings, glm::mat4(1.0f), gravity, 100.0f);
	CHECK(emitter.GetCount() == 10 + static_cast<uint32_t>(10.0f * ParticleEmitter::MaxTimestep));
	CHECK(emitter.GetParticles()[0].Age < 100.0f);
	// Steps that are not positive do nothing.
	uint32_t const before = emitter.GetCount();
	emitter.Update(settings, glm::mat4(1.0f), gravity, 0.0f);
	emitter.Update(settings, glm::mat4(1.0f), gravity, -1.0f);
	emitter.Update(settings, glm::mat4(1.0f), gravity, std::numeric_limits<float>::quiet_NaN());
	CHECK(emitter.GetCount() == before);
}

TEST_CASE("ParticleEmitter: a seed gives the same particles, and invalid settings are clamped")
{
	ParticleSystemComponent settings;
	settings.Shape = ParticleEmitterShape::Sphere;
	ParticleEmitter a(42);
	ParticleEmitter b(42);
	ParticleEmitter c(43);
	a.Emit(settings, glm::mat4(1.0f), 20);
	b.Emit(settings, glm::mat4(1.0f), 20);
	c.Emit(settings, glm::mat4(1.0f), 20);
	bool differs = false;
	for (size_t i = 0; i < 20; i++)
	{
		CHECK(a.GetParticles()[i].Position == b.GetParticles()[i].Position);
		CHECK(a.GetParticles()[i].Velocity == b.GetParticles()[i].Velocity);
		differs = differs || a.GetParticles()[i].Position != c.GetParticles()[i].Position;
	}
	CHECK(differs);

	// Scripts can set any value: nothing breaks, and everything stays finite.
	float const nan = std::numeric_limits<float>::quiet_NaN();
	float const infinity = std::numeric_limits<float>::infinity();
	ParticleSystemComponent broken;
	broken.Rate = infinity;
	broken.MaxParticles = 0;
	broken.LifetimeMin = 5.0f;
	broken.LifetimeMax = -1.0f;
	broken.SpeedMin = nan;
	broken.SpeedMax = -3.0f;
	broken.ConeAngle = 720.0f;
	broken.Radius = -1.0f;
	broken.BoxHalfExtents = {nan, -1.0f, infinity};
	broken.GravityFactor = nan;
	broken.Drag = infinity;
	broken.StartSize = nan;
	broken.Intensity = -infinity;
	ParticleEmitter emitter(3);
	for (ParticleEmitterShape const shape :
	     {ParticleEmitterShape::Point, ParticleEmitterShape::Sphere, ParticleEmitterShape::Cone, ParticleEmitterShape::Box})
	{
		broken.Shape = shape;
		emitter.Clear();
		emitter.Emit(broken, glm::mat4(0.0f), 5);
		emitter.Update(broken, glm::mat4(0.0f), glm::vec3(0.0f, -9.81f, 0.0f), 0.1f);
		// MaxParticles is at least 1.
		CHECK(emitter.GetCount() == 1);
		for (Particle const& particle : emitter.GetParticles())
		{
			CHECK(std::isfinite(particle.Position.x + particle.Position.y + particle.Position.z));
			CHECK(std::isfinite(particle.Velocity.x + particle.Velocity.y + particle.Velocity.z));
			CHECK(particle.Lifetime >= 0.01f);
			CHECK(ParticleEmitter::GetSize(broken, particle) == 0.0f);
			glm::vec4 const color = ParticleEmitter::GetColor(broken, particle);
			CHECK(std::isfinite(color.r + color.g + color.b + color.a));
		}
	}
}

TEST_CASE("ParticleEmitter: size and color blend from start to end over a particle's life")
{
	ParticleSystemComponent settings;
	settings.StartSize = 1.0f;
	settings.EndSize = 3.0f;
	settings.StartColor = {1.0f, 0.0f, 0.0f, 1.0f};
	settings.EndColor = {0.0f, 0.0f, 1.0f, 0.0f};
	settings.Intensity = 2.0f;
	Particle particle;
	particle.Lifetime = 2.0f;
	CHECK(ParticleEmitter::GetSize(settings, particle) == doctest::Approx(1.0f));
	CHECK(ParticleEmitter::GetColor(settings, particle) == glm::vec4(2.0f, 0.0f, 0.0f, 1.0f));
	particle.Age = 1.0f;
	CHECK(ParticleEmitter::GetSize(settings, particle) == doctest::Approx(2.0f));
	glm::vec4 const middle = ParticleEmitter::GetColor(settings, particle);
	CHECK(middle.r == doctest::Approx(1.0f));
	CHECK(middle.b == doctest::Approx(1.0f));
	CHECK(middle.a == doctest::Approx(0.5f));
	particle.Age = 5.0f;
	CHECK(ParticleEmitter::GetSize(settings, particle) == doctest::Approx(3.0f));
}

TEST_CASE("Scene: particle systems burst when they start, run with the scene and can finish their entity")
{
	Scene scene("Particles");
	Entity fire = scene.CreateEntity("Fire");
	ParticleSystemComponent& component = fire.AddComponent<ParticleSystemComponent>();
	component.Emitting = false;
	component.Burst = 8;
	component.LifetimeMin = 0.5f;
	component.LifetimeMax = 0.5f;
	CHECK(scene.GetParticles(fire).empty());

	// Editors preview: the system starts with its burst at the first update.
	scene.UpdateParticles(0.1f);
	CHECK(scene.GetParticles(fire).size() == 8);
	scene.UpdateParticles(0.1f);
	CHECK(scene.GetParticles(fire).size() == 8);
	scene.ClearParticles(fire);
	CHECK(scene.GetParticles(fire).empty());
	scene.EmitParticles(fire, 3);
	CHECK(scene.GetParticles(fire).size() == 3);

	// The runtime starts every system over: the burst comes again.
	scene.OnRuntimeStart();
	CHECK(scene.GetParticles(fire).empty());
	scene.OnUpdateRuntime(Timestep(0.1f));
	CHECK(scene.GetParticles(fire).size() == 8);
	// Paused scenes hold their particles.
	scene.SetPaused(true);
	scene.OnUpdateRuntime(Timestep(1.0f));
	CHECK(scene.GetParticles(fire).size() == 8);
	scene.SetPaused(false);

	// Systems that are done destroy their entity when asked to.
	scene.GetEntityByUUID(fire.GetUUID()).GetComponent<ParticleSystemComponent>().DestroyWhenFinished = true;
	UUID const id = fire.GetUUID();
	for (int i = 0; i < 4 && scene.HasEntity(id); i++)
	{
		scene.OnUpdateRuntime(Timestep(0.2f));
	}
	CHECK_FALSE(scene.HasEntity(id));

	// Emitting systems never finish; removing the component drops the particles.
	Entity smoke = scene.CreateEntity("Smoke");
	ParticleSystemComponent& smokeComponent = smoke.AddComponent<ParticleSystemComponent>();
	smokeComponent.DestroyWhenFinished = true;
	smokeComponent.Rate = 100.0f;
	for (int i = 0; i < 10; i++)
	{
		scene.OnUpdateRuntime(Timestep(0.1f));
	}
	CHECK(scene.HasEntity(smoke.GetUUID()));
	CHECK_FALSE(scene.GetParticles(smoke).empty());
	smoke.RemoveComponent<ParticleSystemComponent>();
	scene.OnUpdateRuntime(Timestep(0.1f));
	CHECK(scene.GetParticles(smoke).empty());
	scene.OnRuntimeStop();

	// Emitting on an entity without a system does nothing.
	scene.EmitParticles(smoke, 5);
	CHECK(scene.GetParticles(smoke).empty());
	scene.EmitParticles(Entity(), 5);
}
