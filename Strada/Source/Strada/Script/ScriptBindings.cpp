#include "stpch.h"
#include "Strada/Script/ScriptBindings.h"

#include "Strada/Scene/ComponentRegistry.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptEngine.h"

#include <array>
#include <string>
#include <string_view>

namespace Strada
{
	namespace
	{
		// The layouts of Strada.Vector3 and Strada.Quaternion.
		struct ScriptVector3
		{
			float X;
			float Y;
			float Z;
		};

		struct ScriptQuaternion
		{
			float X;
			float Y;
			float Z;
			float W;
		};

		static_assert(sizeof(ScriptVector3) == 12 && sizeof(ScriptQuaternion) == 16);

		std::string_view ToStringView(char const* text, int32_t length)
		{
			return text != nullptr && length > 0 ? std::string_view(text, static_cast<size_t>(length)) : std::string_view();
		}

		// Scripts cannot crash the engine: calls on missing entities are logged as script errors and do nothing.
		Entity FindEntity(uint64_t id, char const* function)
		{
			Scene* const scene = ScriptEngine::GetSceneContext();
			if (scene == nullptr)
			{
				Log::GetScriptLogger().error("{}: no scene is running", function);
				return {};
			}
			Entity const entity = scene->GetEntityByUUID(UUID(id));
			if (!entity)
			{
				Log::GetScriptLogger().error("{}: entity {} does not exist", function, id);
			}
			return entity;
		}

		void Log_Write(int32_t level, char const* text, int32_t length)
		{
			spdlog::level::level_enum severity = spdlog::level::info;
			switch (static_cast<LogLevel>(level))
			{
				case LogLevel::Trace:
					severity = spdlog::level::trace;
					break;
				case LogLevel::Info:
					severity = spdlog::level::info;
					break;
				case LogLevel::Warn:
					severity = spdlog::level::warn;
					break;
				case LogLevel::Error:
					severity = spdlog::level::err;
					break;
				case LogLevel::Critical:
					severity = spdlog::level::critical;
					break;
			}
			Log::GetScriptLogger().log(severity, "{}", ToStringView(text, length));
		}

		uint8_t Entity_IsValid(uint64_t id)
		{
			Scene* const scene = ScriptEngine::GetSceneContext();
			return scene != nullptr && scene->HasEntity(UUID(id)) ? 1 : 0;
		}

		// The name stays valid until the entity is renamed or destroyed; the runtime copies it at once.
		char const* Entity_GetName(uint64_t id, int32_t* length)
		{
			*length = 0;
			Entity const entity = FindEntity(id, "Entity.Name");
			if (!entity)
			{
				return nullptr;
			}
			std::string const& name = entity.GetName();
			*length = static_cast<int32_t>(name.size());
			return name.data();
		}

		void Entity_SetName(uint64_t id, char const* name, int32_t length)
		{
			if (Entity entity = FindEntity(id, "Entity.Name"))
			{
				entity.GetComponent<TagComponent>().Tag = ToStringView(name, length);
			}
		}

		uint8_t Entity_HasComponent(uint64_t id, char const* name, int32_t length)
		{
			Entity const entity = FindEntity(id, "Entity.HasComponent");
			ComponentInfo const* const component = ComponentRegistry::Find(ToStringView(name, length));
			if (!entity || component == nullptr)
			{
				return 0;
			}
			return component->Has(entity.GetScene()->GetRegistry(), entity.GetHandle()) ? 1 : 0;
		}

		uint64_t Entity_Create(char const* name, int32_t length)
		{
			Scene* const scene = ScriptEngine::GetSceneContext();
			if (scene == nullptr)
			{
				Log::GetScriptLogger().error("Entity.Create: no scene is running");
				return 0;
			}
			return scene->CreateEntity(std::string(ToStringView(name, length))).GetUUID().GetValue();
		}

		void Entity_Destroy(uint64_t id)
		{
			if (Entity const entity = FindEntity(id, "Entity.Destroy"))
			{
				entity.GetScene()->DestroyEntity(entity);
			}
		}

		uint64_t Entity_FindByName(char const* name, int32_t length)
		{
			Scene* const scene = ScriptEngine::GetSceneContext();
			if (scene == nullptr)
			{
				return 0;
			}
			Entity const entity = scene->FindEntityByName(ToStringView(name, length));
			return entity ? entity.GetUUID().GetValue() : 0;
		}

		void TransformComponent_GetTranslation(uint64_t id, ScriptVector3* value)
		{
			glm::vec3 translation(0.0f);
			if (Entity const entity = FindEntity(id, "TransformComponent.Translation"))
			{
				translation = entity.GetComponent<TransformComponent>().Translation;
			}
			*value = {translation.x, translation.y, translation.z};
		}

		void TransformComponent_SetTranslation(uint64_t id, ScriptVector3 const* value)
		{
			if (Entity entity = FindEntity(id, "TransformComponent.Translation"))
			{
				entity.GetComponent<TransformComponent>().Translation = {value->X, value->Y, value->Z};
			}
		}

		void TransformComponent_GetRotation(uint64_t id, ScriptQuaternion* value)
		{
			glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
			if (Entity const entity = FindEntity(id, "TransformComponent.Rotation"))
			{
				rotation = entity.GetComponent<TransformComponent>().Rotation;
			}
			*value = {rotation.x, rotation.y, rotation.z, rotation.w};
		}

		void TransformComponent_SetRotation(uint64_t id, ScriptQuaternion const* value)
		{
			if (Entity entity = FindEntity(id, "TransformComponent.Rotation"))
			{
				entity.GetComponent<TransformComponent>().Rotation = glm::quat::wxyz(value->W, value->X, value->Y, value->Z);
			}
		}

		void TransformComponent_GetScale(uint64_t id, ScriptVector3* value)
		{
			glm::vec3 scale(1.0f);
			if (Entity const entity = FindEntity(id, "TransformComponent.Scale"))
			{
				scale = entity.GetComponent<TransformComponent>().Scale;
			}
			*value = {scale.x, scale.y, scale.z};
		}

		void TransformComponent_SetScale(uint64_t id, ScriptVector3 const* value)
		{
			if (Entity entity = FindEntity(id, "TransformComponent.Scale"))
			{
				entity.GetComponent<TransformComponent>().Scale = {value->X, value->Y, value->Z};
			}
		}

		template<typename TFunction, size_t NameSize>
		ScriptBinding Bind(char const (&name)[NameSize], TFunction* function)
		{
			// Function pointers convert to one another; the runtime casts it back to the field's signature.
			return ScriptBinding{name, static_cast<int32_t>(NameSize - 1), reinterpret_cast<void (*)()>(function)};
		}
	}

	std::span<ScriptBinding const> GetScriptBindings()
	{
		// The name of each binding is that of its Strada.Interop.InternalCalls field.
		static std::array const s_Bindings = {
			Bind("Log_Write", &Log_Write),
			Bind("Entity_IsValid", &Entity_IsValid),
			Bind("Entity_GetName", &Entity_GetName),
			Bind("Entity_SetName", &Entity_SetName),
			Bind("Entity_HasComponent", &Entity_HasComponent),
			Bind("Entity_Create", &Entity_Create),
			Bind("Entity_Destroy", &Entity_Destroy),
			Bind("Entity_FindByName", &Entity_FindByName),
			Bind("TransformComponent_GetTranslation", &TransformComponent_GetTranslation),
			Bind("TransformComponent_SetTranslation", &TransformComponent_SetTranslation),
			Bind("TransformComponent_GetRotation", &TransformComponent_GetRotation),
			Bind("TransformComponent_SetRotation", &TransformComponent_SetRotation),
			Bind("TransformComponent_GetScale", &TransformComponent_GetScale),
			Bind("TransformComponent_SetScale", &TransformComponent_SetScale),
		};
		return s_Bindings;
	}
}
