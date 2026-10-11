#pragma once

#include "Strada/Scene/Components.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace Strada
{
	struct Particle
	{
		glm::vec3 Position = glm::vec3(0.0f);
		// Seconds since the particle was emitted.
		float Age = 0.0f;
		glm::vec3 Velocity = glm::vec3(0.0f);
		float Lifetime = 1.0f;
	};

	// The particles of one ParticleSystemComponent and their simulation, independent of the ECS. Positions are in world
	// space, or in the emitter's local space when the settings' WorldSpace is off (the renderer then places them with the
	// entity's transform). Random numbers come from a seeded generator of its own, so a seed gives the same particles on
	// every platform. Settings outside the component's field ranges (scripts can set any value) are clamped into them.
	class ParticleEmitter
	{
	public:
		// The longest step one update simulates: a stall (a breakpoint, a slow frame) does not throw particles far away.
		static constexpr float MaxTimestep = 0.25f;

		explicit ParticleEmitter(uint64_t seed = 0);

		// Emits up to count particles from the shape placed by the emitter's world transform (fewer when MaxParticles are
		// alive).
		void Emit(ParticleSystemComponent const& settings, glm::mat4 const& world, uint32_t count);
		// Ages and moves the particles (gravity in world space, times the settings' GravityFactor; drag), drops those
		// whose lifetime ended, then emits Rate particles per second while the settings say Emitting.
		void Update(ParticleSystemComponent const& settings, glm::mat4 const& world, glm::vec3 const& gravity, float deltaTime);
		void Clear();

		std::span<Particle const> GetParticles() const { return m_Particles; }
		uint32_t GetCount() const { return static_cast<uint32_t>(m_Particles.size()); }

		// A particle's size and color at its age (straight alpha, times the settings' intensity).
		static float GetSize(ParticleSystemComponent const& settings, Particle const& particle);
		static glm::vec4 GetColor(ParticleSystemComponent const& settings, Particle const& particle);

	private:
		// Uniform in [0, 1).
		float NextFloat();
		glm::vec3 RandomUnitVector();
		// Uniform within the cone around -Z.
		glm::vec3 RandomConeDirection(float coneAngleDegrees);

		std::vector<Particle> m_Particles;
		float m_EmitAccumulator = 0.0f;
		uint64_t m_RandomState = 0;
	};
}
