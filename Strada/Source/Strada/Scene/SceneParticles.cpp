#include "stpch.h"
#include "Strada/Scene/Scene.h"

#include "Strada/Scene/Entity.h"

#include <vector>

namespace Strada
{
	Scene::ParticleSystemState& Scene::StartParticleSystem(entt::entity handle, ParticleSystemComponent const& component)
	{
		UUID const id = m_Registry.get<IDComponent>(handle).ID;
		auto found = m_ParticleSystems.find(handle);
		// A handle reused by another entity starts a new system.
		if (found != m_ParticleSystems.end() && found->second.Entity == id)
		{
			return found->second;
		}
		ParticleSystemState state;
		state.Entity = id;
		state.Emitter = ParticleEmitter(static_cast<uint64_t>(id) ^ (++m_ParticleSeed * 0x9E3779B97F4A7C15ull));
		state.Emitter.Emit(component, GetWorldTransform(Entity(handle, this)), component.Burst);
		ParticleSystemState& started = m_ParticleSystems[handle];
		started = std::move(state);
		return started;
	}

	void Scene::UpdateParticles(float deltaTime)
	{
		// Systems whose entity or component is gone.
		std::erase_if(m_ParticleSystems,
		              [this](auto const& entry)
		              {
						  auto const& [handle, state] = entry;
						  return !m_Registry.valid(handle) || !m_Registry.all_of<ParticleSystemComponent>(handle) ||
			                     m_Registry.get<IDComponent>(handle).ID != state.Entity;
					  });

		std::vector<entt::entity> finished;
		for (entt::entity const handle : m_Registry.view<ParticleSystemComponent>())
		{
			ParticleSystemComponent const& component = m_Registry.get<ParticleSystemComponent>(handle);
			ParticleSystemState& state = StartParticleSystem(handle, component);
			state.Emitter.Update(component, GetWorldTransform(Entity(handle, this)), m_Settings.Physics.Gravity, deltaTime);
			if (m_IsRunning && component.DestroyWhenFinished && !component.Emitting && state.Emitter.GetCount() == 0)
			{
				finished.push_back(handle);
			}
		}
		for (entt::entity const handle : finished)
		{
			DestroyEntity(Entity(handle, this));
		}
	}

	void Scene::EmitParticles(Entity entity, uint32_t count)
	{
		if (!entity || entity.GetScene() != this)
		{
			return;
		}
		if (ParticleSystemComponent const* component = entity.TryGetComponent<ParticleSystemComponent>())
		{
			StartParticleSystem(entity.GetHandle(), *component).Emitter.Emit(*component, GetWorldTransform(entity), count);
		}
	}

	void Scene::ClearParticles(Entity entity)
	{
		if (!entity || entity.GetScene() != this)
		{
			return;
		}
		if (auto const found = m_ParticleSystems.find(entity.GetHandle());
		    found != m_ParticleSystems.end() && found->second.Entity == entity.GetUUID())
		{
			found->second.Emitter.Clear();
		}
	}

	std::span<Particle const> Scene::GetParticles(Entity entity) const
	{
		if (!entity || entity.GetScene() != this)
		{
			return {};
		}
		auto const found = m_ParticleSystems.find(entity.GetHandle());
		if (found == m_ParticleSystems.end() || found->second.Entity != entity.GetUUID())
		{
			return {};
		}
		return found->second.Emitter.GetParticles();
	}

	void Scene::ResetParticles()
	{
		m_ParticleSystems.clear();
	}
}
