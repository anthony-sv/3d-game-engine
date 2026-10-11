#include "stpch.h"
#include "Strada/Scene/ParticleEmitter.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace Strada
{
	namespace
	{
		constexpr uint32_t MaxParticlesLimit = 100000;
		constexpr float MinLifetime = 0.01f;

		float Finite(float value, float fallback)
		{
			return std::isfinite(value) ? value : fallback;
		}

		// The settings within the ranges the component's field table declares.
		ParticleSystemComponent Sanitize(ParticleSystemComponent settings)
		{
			settings.Rate = std::clamp(Finite(settings.Rate, 0.0f), 0.0f, 10000.0f);
			settings.MaxParticles = std::clamp(settings.MaxParticles, 1u, MaxParticlesLimit);
			settings.LifetimeMin = std::clamp(Finite(settings.LifetimeMin, 1.0f), MinLifetime, 600.0f);
			settings.LifetimeMax = std::clamp(Finite(settings.LifetimeMax, 1.0f), settings.LifetimeMin, 600.0f);
			settings.SpeedMin = std::clamp(Finite(settings.SpeedMin, 0.0f), 0.0f, 1000.0f);
			settings.SpeedMax = std::clamp(Finite(settings.SpeedMax, 0.0f), settings.SpeedMin, 1000.0f);
			settings.ConeAngle = std::clamp(Finite(settings.ConeAngle, 0.0f), 0.0f, 180.0f);
			settings.Radius = std::clamp(Finite(settings.Radius, 0.0f), 0.0f, 1000.0f);
			for (int axis = 0; axis < 3; axis++)
			{
				settings.BoxHalfExtents[axis] = std::clamp(Finite(settings.BoxHalfExtents[axis], 0.0f), 0.0f, 1000.0f);
			}
			settings.GravityFactor = std::clamp(Finite(settings.GravityFactor, 0.0f), -100.0f, 100.0f);
			settings.Drag = std::clamp(Finite(settings.Drag, 0.0f), 0.0f, 100.0f);
			return settings;
		}
	}

	ParticleEmitter::ParticleEmitter(uint64_t seed)
		: m_RandomState(seed)
	{
	}

	float ParticleEmitter::NextFloat()
	{
		// SplitMix64: the same sequence on every platform (unlike the standard distributions).
		m_RandomState += 0x9E3779B97F4A7C15ull;
		uint64_t value = m_RandomState;
		value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
		value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
		value ^= value >> 31;
		// The top 24 bits give every float in [0, 1) with a step of 2^-24.
		return static_cast<float>(value >> 40) * (1.0f / 16777216.0f);
	}

	glm::vec3 ParticleEmitter::RandomUnitVector()
	{
		float const z = NextFloat() * 2.0f - 1.0f;
		float const angle = NextFloat() * glm::two_pi<float>();
		float const radius = std::sqrt(std::max(0.0f, 1.0f - z * z));
		return glm::vec3(radius * std::cos(angle), radius * std::sin(angle), z);
	}

	glm::vec3 ParticleEmitter::RandomConeDirection(float coneAngleDegrees)
	{
		// Uniform over the spherical cap: cos(theta) uniform between 1 and cos(angle).
		float const cosTheta = 1.0f - NextFloat() * (1.0f - std::cos(glm::radians(coneAngleDegrees)));
		float const sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
		float const angle = NextFloat() * glm::two_pi<float>();
		return glm::vec3(sinTheta * std::cos(angle), sinTheta * std::sin(angle), -cosTheta);
	}

	void ParticleEmitter::Emit(ParticleSystemComponent const& component, glm::mat4 const& world, uint32_t count)
	{
		ParticleSystemComponent const settings = Sanitize(component);
		uint32_t const room = settings.MaxParticles > GetCount() ? settings.MaxParticles - GetCount() : 0;
		count = std::min(count, room);
		// Local particles are placed by the entity when drawn.
		glm::mat4 const frame = settings.WorldSpace ? world : glm::mat4(1.0f);
		glm::mat3 const rotation(frame);
		m_Particles.reserve(m_Particles.size() + count);
		for (uint32_t i = 0; i < count; i++)
		{
			glm::vec3 position(0.0f);
			glm::vec3 direction(0.0f, 0.0f, -1.0f);
			switch (settings.Shape)
			{
				case ParticleEmitterShape::Point:
					direction = RandomConeDirection(settings.ConeAngle);
					break;
				case ParticleEmitterShape::Sphere:
					direction = RandomUnitVector();
					// Uniform in the volume.
					position = direction * settings.Radius * std::cbrt(NextFloat());
					break;
				case ParticleEmitterShape::Cone:
				{
					float const radius = settings.Radius * std::sqrt(NextFloat());
					float const angle = NextFloat() * glm::two_pi<float>();
					position = glm::vec3(radius * std::cos(angle), radius * std::sin(angle), 0.0f);
					direction = RandomConeDirection(settings.ConeAngle);
					break;
				}
				case ParticleEmitterShape::Box:
					position = (glm::vec3(NextFloat(), NextFloat(), NextFloat()) * 2.0f - 1.0f) * settings.BoxHalfExtents;
					direction = RandomConeDirection(settings.ConeAngle);
					break;
			}
			glm::vec3 worldDirection = rotation * direction;
			float const length = glm::length(worldDirection);
			worldDirection = length > 1e-6f ? worldDirection / length : glm::vec3(0.0f, 0.0f, -1.0f);

			Particle particle;
			particle.Position = glm::vec3(frame * glm::vec4(position, 1.0f));
			particle.Velocity = worldDirection * (settings.SpeedMin + (settings.SpeedMax - settings.SpeedMin) * NextFloat());
			particle.Lifetime = settings.LifetimeMin + (settings.LifetimeMax - settings.LifetimeMin) * NextFloat();
			m_Particles.push_back(particle);
		}
	}

	void ParticleEmitter::Update(ParticleSystemComponent const& component, glm::mat4 const& world, glm::vec3 const& gravity,
	                             float deltaTime)
	{
		if (!(deltaTime > 0.0f))
		{
			return;
		}
		deltaTime = std::min(deltaTime, MaxTimestep);
		ParticleSystemComponent const settings = Sanitize(component);
		glm::vec3 acceleration = gravity * settings.GravityFactor;
		if (!settings.WorldSpace)
		{
			// Gravity points down in the world, whichever way the emitter turns.
			glm::mat3 const basis(world);
			acceleration = std::abs(glm::determinant(basis)) > 1e-12f ? glm::inverse(basis) * acceleration : glm::vec3(0.0f);
		}
		float const damping = std::max(0.0f, 1.0f - settings.Drag * deltaTime);

		// Order does not matter (the renderer sorts), so dead particles are swapped out.
		for (size_t i = 0; i < m_Particles.size();)
		{
			Particle& particle = m_Particles[i];
			particle.Age += deltaTime;
			if (particle.Age >= particle.Lifetime)
			{
				particle = m_Particles.back();
				m_Particles.pop_back();
				continue;
			}
			particle.Velocity = (particle.Velocity + acceleration * deltaTime) * damping;
			particle.Position += particle.Velocity * deltaTime;
			i++;
		}

		if (!settings.Emitting)
		{
			m_EmitAccumulator = 0.0f;
			return;
		}
		m_EmitAccumulator += settings.Rate * deltaTime;
		uint32_t const count = static_cast<uint32_t>(m_EmitAccumulator);
		m_EmitAccumulator -= static_cast<float>(count);
		Emit(settings, world, count);
	}

	void ParticleEmitter::Clear()
	{
		m_Particles.clear();
		m_EmitAccumulator = 0.0f;
	}

	float ParticleEmitter::GetSize(ParticleSystemComponent const& settings, Particle const& particle)
	{
		float const t = std::clamp(particle.Age / std::max(particle.Lifetime, MinLifetime), 0.0f, 1.0f);
		return std::max(0.0f, Finite(settings.StartSize + (settings.EndSize - settings.StartSize) * t, 0.0f));
	}

	glm::vec4 ParticleEmitter::GetColor(ParticleSystemComponent const& settings, Particle const& particle)
	{
		float const t = std::clamp(particle.Age / std::max(particle.Lifetime, MinLifetime), 0.0f, 1.0f);
		glm::vec4 color = glm::mix(settings.StartColor, settings.EndColor, t);
		float const intensity = std::max(0.0f, Finite(settings.Intensity, 1.0f));
		color = glm::vec4(glm::vec3(color) * intensity, std::clamp(color.a, 0.0f, 1.0f));
		for (int channel = 0; channel < 4; channel++)
		{
			color[channel] = std::max(0.0f, Finite(color[channel], 0.0f));
		}
		return color;
	}
}
