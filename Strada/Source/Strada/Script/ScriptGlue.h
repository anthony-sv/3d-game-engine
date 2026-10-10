#pragma once

// Internal to the Script module: what the native functions scripts call (ScriptBindings*.cpp) have in common.

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/UUID.h"
#include "Strada/Scene/ComponentTraits.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptBindings.h"
#include "Strada/Script/ScriptEngine.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <deque>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Strada::ScriptGlue
{
	// The layouts of Strada.Vector2, Vector3, Vector4 (and Color) and Quaternion.
	struct Vector2
	{
		float X;
		float Y;
	};

	struct Vector3
	{
		float X;
		float Y;
		float Z;
	};

	struct Vector4
	{
		float X;
		float Y;
		float Z;
		float W;
	};

	struct Quaternion
	{
		float X;
		float Y;
		float Z;
		float W;
	};

	// Strada.Matrix4: column-major, like glm.
	struct Matrix4
	{
		float Values[16];
	};

	static_assert(sizeof(Vector2) == 8 && sizeof(Vector3) == 12 && sizeof(Vector4) == 16 && sizeof(Quaternion) == 16 &&
	              sizeof(Matrix4) == 64);

	inline Vector2 ToScript(glm::vec2 const& value)
	{
		return {value.x, value.y};
	}

	inline Vector3 ToScript(glm::vec3 const& value)
	{
		return {value.x, value.y, value.z};
	}

	inline Vector4 ToScript(glm::vec4 const& value)
	{
		return {value.x, value.y, value.z, value.w};
	}

	inline Quaternion ToScript(glm::quat const& value)
	{
		return {value.x, value.y, value.z, value.w};
	}

	inline glm::vec2 FromScript(Vector2 const& value)
	{
		return {value.X, value.Y};
	}

	inline glm::vec3 FromScript(Vector3 const& value)
	{
		return {value.X, value.Y, value.Z};
	}

	inline glm::vec4 FromScript(Vector4 const& value)
	{
		return {value.X, value.Y, value.Z, value.W};
	}

	inline glm::quat FromScript(Quaternion const& value)
	{
		return glm::quat::wxyz(value.W, value.X, value.Y, value.Z);
	}

	// How a component field crosses the boundary: Type is the C# side's representation.
	template<typename T>
	struct FieldValue;

	template<>
	struct FieldValue<float>
	{
		using Type = float;
		static Type ToScript(float value) { return value; }
		static float FromScript(Type value) { return value; }
	};

	template<>
	struct FieldValue<uint32_t>
	{
		using Type = uint32_t;
		static Type ToScript(uint32_t value) { return value; }
		static uint32_t FromScript(Type value) { return value; }
	};

	template<>
	struct FieldValue<bool>
	{
		using Type = uint8_t;
		static Type ToScript(bool value) { return value ? 1 : 0; }
		static bool FromScript(Type value) { return value != 0; }
	};

	template<>
	struct FieldValue<glm::vec2>
	{
		using Type = Vector2;
		static Type ToScript(glm::vec2 const& value) { return ScriptGlue::ToScript(value); }
		static glm::vec2 FromScript(Type const& value) { return ScriptGlue::FromScript(value); }
	};

	template<>
	struct FieldValue<glm::vec3>
	{
		using Type = Vector3;
		static Type ToScript(glm::vec3 const& value) { return ScriptGlue::ToScript(value); }
		static glm::vec3 FromScript(Type const& value) { return ScriptGlue::FromScript(value); }
	};

	template<>
	struct FieldValue<glm::vec4>
	{
		using Type = Vector4;
		static Type ToScript(glm::vec4 const& value) { return ScriptGlue::ToScript(value); }
		static glm::vec4 FromScript(Type const& value) { return ScriptGlue::FromScript(value); }
	};

	template<>
	struct FieldValue<glm::quat>
	{
		using Type = Quaternion;
		static Type ToScript(glm::quat const& value) { return ScriptGlue::ToScript(value); }
		static glm::quat FromScript(Type const& value) { return ScriptGlue::FromScript(value); }
	};

	template<>
	struct FieldValue<AssetHandle>
	{
		using Type = uint64_t;
		static Type ToScript(AssetHandle value) { return value.GetUUID().GetValue(); }
		static AssetHandle FromScript(Type value) { return AssetHandle(UUID(value)); }
	};

	// Enums cross as their underlying values widened to int.
	template<typename T>
		requires std::is_enum_v<T>
	struct FieldValue<T>
	{
		using Type = int32_t;
		static Type ToScript(T value) { return static_cast<Type>(value); }
		static T FromScript(Type value) { return static_cast<T>(value); }
	};

	template<typename T>
	struct MemberPointer;

	template<typename TComponent, typename TField>
	struct MemberPointer<TField TComponent::*>
	{
		using Component = TComponent;
		using Field = TField;
	};

	// The bindings and their names (which it owns).
	class BindingTable
	{
	public:
		BindingTable();

		BindingTable(BindingTable const&) = delete;
		BindingTable& operator=(BindingTable const&) = delete;

		template<typename TFunction>
		void Add(std::string name, TFunction* function)
		{
			// Element addresses of a deque stay valid as it grows.
			std::string const& stored = m_Names.emplace_back(std::move(name));
			// Function pointers convert to one another; the runtime casts it back to the field's signature.
			m_Bindings.push_back({stored.c_str(), static_cast<int32_t>(stored.size()), reinterpret_cast<void (*)()>(function)});
		}

		std::vector<ScriptBinding> const& GetBindings() const { return m_Bindings; }

	private:
		std::deque<std::string> m_Names;
		std::vector<ScriptBinding> m_Bindings;
	};

	void RegisterEntityBindings(BindingTable& table);
	void RegisterComponentBindings(BindingTable& table);
	void RegisterRuntimeBindings(BindingTable& table);

	inline std::string_view ToStringView(char const* text, int32_t length)
	{
		return text != nullptr && length > 0 ? std::string_view(text, static_cast<size_t>(length)) : std::string_view();
	}

	// Scripts cannot crash the engine: calls without a running scene, or on missing entities and components, are logged
	// as script errors and do nothing (reads return defaults).
	inline Scene* GetScene(std::string_view function)
	{
		Scene* const scene = ScriptEngine::GetSceneContext();
		if (scene == nullptr)
		{
			Log::GetScriptLogger().error("{}: no scene is running", function);
		}
		return scene;
	}

	inline Entity FindEntity(uint64_t id, std::string_view function)
	{
		Scene* const scene = GetScene(function);
		if (scene == nullptr)
		{
			return {};
		}
		Entity const entity = scene->GetEntityByUUID(UUID(id));
		if (!entity)
		{
			Log::GetScriptLogger().error("{}: entity {} does not exist", function, id);
		}
		return entity;
	}

	template<typename TComponent>
	TComponent* FindComponent(uint64_t id)
	{
		Entity entity = FindEntity(id, ComponentTraits<TComponent>::Name);
		if (!entity)
		{
			return nullptr;
		}
		TComponent* const component = entity.TryGetComponent<TComponent>();
		if (component == nullptr)
		{
			Log::GetScriptLogger().error("Entity {} has no {} component", id, ComponentTraits<TComponent>::Name);
		}
		return component;
	}

	// Writes a field through the registry, so systems watching the component (physics) see the change.
	template<typename TComponent, typename TFunction>
	void PatchComponent(uint64_t id, TFunction&& change)
	{
		Entity entity = FindEntity(id, ComponentTraits<TComponent>::Name);
		if (!entity)
		{
			return;
		}
		if (!entity.HasComponent<TComponent>())
		{
			Log::GetScriptLogger().error("Entity {} has no {} component", id, ComponentTraits<TComponent>::Name);
			return;
		}
		entity.GetScene()->GetRegistry().patch<TComponent>(entity.GetHandle(), std::forward<TFunction>(change));
	}

	template<auto Member>
	void GetField(uint64_t id, typename FieldValue<typename MemberPointer<decltype(Member)>::Field>::Type* value)
	{
		using Traits = MemberPointer<decltype(Member)>;
		using Value = FieldValue<typename Traits::Field>;
		if (typename Traits::Component const* component = FindComponent<typename Traits::Component>(id))
		{
			*value = Value::ToScript(component->*Member);
		}
		else
		{
			*value = Value::ToScript(typename Traits::Field{});
		}
	}

	template<auto Member>
	void SetField(uint64_t id, typename FieldValue<typename MemberPointer<decltype(Member)>::Field>::Type const* value)
	{
		using Traits = MemberPointer<decltype(Member)>;
		auto const fieldValue = FieldValue<typename Traits::Field>::FromScript(*value);
		PatchComponent<typename Traits::Component>(id,
		                                           [&fieldValue](typename Traits::Component& component)
		                                           {
													   component.*Member = fieldValue;
												   });
	}

	// <ComponentClass>_Get<Field> and <ComponentClass>_Set<Field>.
	template<auto Member>
	void AddFieldBindings(BindingTable& table, std::string_view componentClass, std::string_view field)
	{
		table.Add(std::string(componentClass) + "_Get" + std::string(field), &GetField<Member>);
		table.Add(std::string(componentClass) + "_Set" + std::string(field), &SetField<Member>);
	}
}
