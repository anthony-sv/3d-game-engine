#include "stpch.h"
#include "Strada/Script/ScriptEngine.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Script/DotNetHost.h"
#include "Strada/Script/ScriptBindings.h"
#include "Strada/Script/ScriptFieldSerialization.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace Strada
{
	namespace
	{
		constexpr std::string_view HostType = "Strada.Interop.Host, Strada.ScriptCore";
		constexpr char const* ScriptCoreFileName = "Strada.ScriptCore.dll";

		// The statuses of Strada.Interop.Host.
		enum class ScriptStatus : int32_t
		{
			Success = 0,
			Failure = 1,
			NotFound = 2,
			BindingMismatch = 3
		};

		// Strada.Interop.ScriptEvent.
		enum class ScriptEvent : int32_t
		{
			Create = 0,
			Update = 1,
			FixedUpdate = 2,
			Destroy = 3,
			CollisionEnter = 4,
			CollisionExit = 5,
			TriggerEnter = 6,
			TriggerExit = 7
		};

		// The [UnmanagedCallersOnly] entry points of Strada.Interop.Host.
		struct HostFunctions
		{
			int32_t (*Initialize)(ScriptBinding const* bindings, int32_t count) = nullptr;
			int32_t (*Shutdown)() = nullptr;
			int32_t (*LoadGameAssembly)(char const* path, int32_t pathLength) = nullptr;
			int32_t (*UnloadGameAssembly)() = nullptr;
			int32_t (*DescribeClasses)(void* context, void (*receive)(void* context, char const* text, int32_t length)) = nullptr;
			int32_t (*CreateInstance)(char const* className, int32_t classNameLength, uint64_t entity, char const* fields,
			                          int32_t fieldsLength) = nullptr;
			int32_t (*DestroyInstance)(uint64_t entity) = nullptr;
			int32_t (*DestroyAllInstances)() = nullptr;
			int32_t (*Invoke)(uint64_t entity, int32_t scriptEvent, float timeStep, uint64_t other) = nullptr;
		};

		struct ScriptEngineData
		{
			HostFunctions Host;
			std::filesystem::path GameAssemblyPath;
			std::vector<ScriptClassInfo> Classes;
			Scene* SceneContext = nullptr;
			std::unordered_set<UUID> Instances;
		};

		Scope<ScriptEngineData> s_Data;

		template<typename TFunction>
		Result<void> Resolve(TFunction& function, std::string_view name)
		{
			Result<void*> pointer = DotNet::GetFunction(HostType, name);
			if (!pointer)
			{
				return Error{pointer.GetError()};
			}
			function = reinterpret_cast<TFunction>(pointer.GetValue());
			return {};
		}

		Result<HostFunctions> ResolveHostFunctions()
		{
			HostFunctions functions;
			std::vector<Result<void>> const results = {
				Resolve(functions.Initialize, "Initialize"),
				Resolve(functions.Shutdown, "Shutdown"),
				Resolve(functions.LoadGameAssembly, "LoadGameAssembly"),
				Resolve(functions.UnloadGameAssembly, "UnloadGameAssembly"),
				Resolve(functions.DescribeClasses, "DescribeClasses"),
				Resolve(functions.CreateInstance, "CreateInstance"),
				Resolve(functions.DestroyInstance, "DestroyInstance"),
				Resolve(functions.DestroyAllInstances, "DestroyAllInstances"),
				Resolve(functions.Invoke, "Invoke"),
			};
			for (Result<void> const& result : results)
			{
				if (!result)
				{
					return Error{result.GetError()};
				}
			}
			return functions;
		}

		int32_t ToLength(size_t size)
		{
			return static_cast<int32_t>(std::min<size_t>(size, std::numeric_limits<int32_t>::max()));
		}

		void ReceiveText(void* context, char const* text, int32_t length)
		{
			static_cast<std::string*>(context)->assign(text, static_cast<size_t>(std::max(length, 0)));
		}

		Result<ScriptFieldInfo> ParseField(Json const& json)
		{
			auto const name = json.find("Name");
			auto const type = json.find("Type");
			auto const defaultValue = json.find("Default");
			if (name == json.end() || !name->is_string() || type == json.end() || !type->is_string() || defaultValue == json.end())
			{
				return Error{"expected a field { \"Name\", \"Type\", \"Default\" }"};
			}
			ScriptFieldInfo field;
			field.Name = name->get<std::string>();
			std::optional<ScriptFieldType> const fieldType = ScriptFieldTypeFromString(type->get_ref<std::string const&>());
			if (!fieldType || *fieldType == ScriptFieldType::None)
			{
				return MakeError("field {}: unknown type {}", field.Name, type->get_ref<std::string const&>());
			}
			field.Type = *fieldType;
			Result<ScriptFieldValue> value = ScriptFieldValueFromJson(field.Type, *defaultValue, DeserializationContext{});
			if (!value)
			{
				return MakeError("field {}: {}", field.Name, value.GetError());
			}
			field.DefaultValue = value.TakeValue();
			if (auto const hidden = json.find("Hidden"); hidden != json.end() && hidden->is_boolean())
			{
				field.Hidden = hidden->get<bool>();
			}
			if (auto const tooltip = json.find("Tooltip"); tooltip != json.end() && tooltip->is_string())
			{
				field.Tooltip = tooltip->get<std::string>();
			}
			auto const minimum = json.find("Min");
			auto const maximum = json.find("Max");
			if (minimum != json.end() && maximum != json.end() && minimum->is_number() && maximum->is_number())
			{
				field.Range = glm::vec2(minimum->get<float>(), maximum->get<float>());
			}
			return field;
		}

		Result<std::vector<ScriptClassInfo>> ParseClasses(std::string const& text)
		{
			Json const json = Json::parse(text, nullptr, false);
			if (json.is_discarded() || !json.is_array())
			{
				return Error{"the script classes cannot be read: expected a JSON array"};
			}
			std::vector<ScriptClassInfo> classes;
			for (Json const& classJson : json)
			{
				auto const name = classJson.find("Name");
				auto const fields = classJson.find("Fields");
				if (!classJson.is_object() || name == classJson.end() || !name->is_string() || fields == classJson.end() ||
				    !fields->is_array())
				{
					return Error{"the script classes cannot be read: expected classes { \"Name\", \"Fields\" }"};
				}
				ScriptClassInfo info;
				info.Name = name->get<std::string>();
				for (Json const& fieldJson : *fields)
				{
					if (!fieldJson.is_object())
					{
						return MakeError("script class {}: expected field objects", info.Name);
					}
					Result<ScriptFieldInfo> field = ParseField(fieldJson);
					if (!field)
					{
						return MakeError("script class {}: {}", info.Name, field.GetError());
					}
					info.Fields.push_back(field.TakeValue());
				}
				classes.push_back(std::move(info));
			}
			std::sort(classes.begin(), classes.end(),
			          [](ScriptClassInfo const& a, ScriptClassInfo const& b)
			          {
						  return a.Name < b.Name;
					  });
			return classes;
		}

		void Invoke(UUID entity, ScriptEvent scriptEvent, float timeStep, UUID other)
		{
			if (s_Data == nullptr || !s_Data->Instances.contains(entity))
			{
				return;
			}
			// Script exceptions were logged by the runtime; nothing else is left to do with them.
			int32_t const status = s_Data->Host.Invoke(entity.GetValue(), static_cast<int32_t>(scriptEvent), timeStep, other.GetValue());
			if (scriptEvent == ScriptEvent::Destroy || static_cast<ScriptStatus>(status) == ScriptStatus::NotFound)
			{
				s_Data->Instances.erase(entity);
			}
		}
	}

	ScriptFieldInfo const* ScriptClassInfo::FindField(std::string_view name) const
	{
		auto const field = std::find_if(Fields.begin(), Fields.end(),
		                                [name](ScriptFieldInfo const& candidate)
		                                {
											return candidate.Name == name;
										});
		return field != Fields.end() ? &*field : nullptr;
	}

	Result<void> ScriptEngine::Init(ScriptEngineSettings const& settings)
	{
		ST_CORE_ASSERT(!s_Data, "ScriptEngine is already initialized");
		std::filesystem::path const directory =
			settings.ScriptCoreDirectory.empty() ? FileSystem::GetExecutableDirectory() : settings.ScriptCoreDirectory;
		std::filesystem::path const scriptCore = directory / ScriptCoreFileName;
		std::error_code error;
		if (!std::filesystem::is_regular_file(scriptCore, error))
		{
			return MakeError("{} was not found", FileSystem::PathToUtf8(scriptCore));
		}
		if (Result<void> loaded = DotNet::LoadAssembly(scriptCore, FileSystem::GetExecutableDirectory()); !loaded)
		{
			return loaded;
		}

		Result<HostFunctions> functions = ResolveHostFunctions();
		if (!functions)
		{
			return Error{functions.GetError()};
		}
		std::span<ScriptBinding const> const bindings = GetScriptBindings();
		auto const status = static_cast<ScriptStatus>(functions.GetValue().Initialize(bindings.data(), ToLength(bindings.size())));
		if (status == ScriptStatus::BindingMismatch)
		{
			return MakeError("the engine and {} do not match (see the log); rebuild both", FileSystem::PathToUtf8(scriptCore));
		}
		if (status != ScriptStatus::Success)
		{
			return Error{"the scripting runtime cannot be initialized (see the log)"};
		}

		s_Data = CreateScope<ScriptEngineData>();
		s_Data->Host = functions.GetValue();
		ST_CORE_INFO("Scripting initialized ({}, {} bindings)", FileSystem::PathToUtf8(scriptCore), bindings.size());
		return {};
	}

	void ScriptEngine::Shutdown()
	{
		ST_CORE_ASSERT(s_Data, "ScriptEngine is not initialized");
		// The runtime keeps running: only the game's scripts and their instances go.
		s_Data->Host.Shutdown();
		s_Data.reset();
	}

	bool ScriptEngine::IsInitialized()
	{
		return s_Data != nullptr;
	}

	Result<void> ScriptEngine::LoadGameAssembly(std::filesystem::path const& path)
	{
		ST_CORE_ASSERT(s_Data, "ScriptEngine is not initialized");
		if (!s_Data->Instances.empty())
		{
			return Error{"the game's scripts cannot be replaced while a scene runs them"};
		}
		std::error_code error;
		std::filesystem::path const absolutePath = std::filesystem::absolute(path, error);
		if (error || !std::filesystem::is_regular_file(absolutePath, error))
		{
			return MakeError("{} was not found", FileSystem::PathToUtf8(path));
		}

		UnloadGameAssembly();
		std::string const utf8Path = FileSystem::PathToUtf8(absolutePath);
		if (static_cast<ScriptStatus>(s_Data->Host.LoadGameAssembly(utf8Path.data(), ToLength(utf8Path.size()))) != ScriptStatus::Success)
		{
			return MakeError("{} cannot be loaded (see the log)", utf8Path);
		}
		std::string description;
		if (static_cast<ScriptStatus>(s_Data->Host.DescribeClasses(&description, &ReceiveText)) != ScriptStatus::Success)
		{
			s_Data->Host.UnloadGameAssembly();
			return MakeError("the script classes of {} cannot be listed (see the log)", utf8Path);
		}
		Result<std::vector<ScriptClassInfo>> classes = ParseClasses(description);
		if (!classes)
		{
			s_Data->Host.UnloadGameAssembly();
			return Error{classes.GetError()};
		}
		s_Data->Classes = classes.TakeValue();
		s_Data->GameAssemblyPath = absolutePath;
		ST_CORE_INFO("Loaded {} ({} script classes)", utf8Path, s_Data->Classes.size());
		return {};
	}

	void ScriptEngine::UnloadGameAssembly()
	{
		ST_CORE_ASSERT(s_Data, "ScriptEngine is not initialized");
		if (s_Data->GameAssemblyPath.empty())
		{
			return;
		}
		s_Data->Instances.clear();
		s_Data->Classes.clear();
		s_Data->GameAssemblyPath.clear();
		s_Data->Host.UnloadGameAssembly();
	}

	bool ScriptEngine::HasGameAssembly()
	{
		return s_Data != nullptr && !s_Data->GameAssemblyPath.empty();
	}

	std::filesystem::path const& ScriptEngine::GetGameAssemblyPath()
	{
		ST_CORE_ASSERT(s_Data, "ScriptEngine is not initialized");
		return s_Data->GameAssemblyPath;
	}

	std::vector<ScriptClassInfo> const& ScriptEngine::GetClasses()
	{
		ST_CORE_ASSERT(s_Data, "ScriptEngine is not initialized");
		return s_Data->Classes;
	}

	ScriptClassInfo const* ScriptEngine::FindClass(std::string_view name)
	{
		if (s_Data == nullptr)
		{
			return nullptr;
		}
		auto const scriptClass = std::lower_bound(s_Data->Classes.begin(), s_Data->Classes.end(), name,
		                                          [](ScriptClassInfo const& candidate, std::string_view value)
		                                          {
													  return candidate.Name < value;
												  });
		return scriptClass != s_Data->Classes.end() && scriptClass->Name == name ? &*scriptClass : nullptr;
	}

	void ScriptEngine::SetSceneContext(Scene* scene)
	{
		// A scene that outlives the engine (shut down first) still clears its context when it stops.
		if (s_Data != nullptr)
		{
			s_Data->SceneContext = scene;
		}
	}

	Scene* ScriptEngine::GetSceneContext()
	{
		return s_Data != nullptr ? s_Data->SceneContext : nullptr;
	}

	Result<void> ScriptEngine::CreateInstance(UUID entity, std::string_view className, ScriptFieldMap const& fields)
	{
		ST_CORE_ASSERT(s_Data, "ScriptEngine is not initialized");
		std::string const fieldsJson = fields.empty() ? std::string() : JsonTraits<ScriptFieldMap>::ToJson(fields).dump();
		auto const status = static_cast<ScriptStatus>(s_Data->Host.CreateInstance(
			className.data(), ToLength(className.size()), entity.GetValue(), fieldsJson.data(), ToLength(fieldsJson.size())));
		switch (status)
		{
			case ScriptStatus::Success:
				s_Data->Instances.insert(entity);
				return {};
			case ScriptStatus::NotFound:
				return MakeError("there is no script class {}", className);
			case ScriptStatus::Failure:
			case ScriptStatus::BindingMismatch:
				break;
		}
		return MakeError("{} cannot be created (see the log)", className);
	}

	bool ScriptEngine::HasInstance(UUID entity)
	{
		return s_Data != nullptr && s_Data->Instances.contains(entity);
	}

	void ScriptEngine::DestroyInstance(UUID entity)
	{
		if (s_Data != nullptr && s_Data->Instances.erase(entity) > 0)
		{
			s_Data->Host.DestroyInstance(entity.GetValue());
		}
	}

	void ScriptEngine::DestroyAllInstances()
	{
		if (s_Data != nullptr)
		{
			s_Data->Instances.clear();
			s_Data->Host.DestroyAllInstances();
		}
	}

	size_t ScriptEngine::GetInstanceCount()
	{
		return s_Data != nullptr ? s_Data->Instances.size() : 0;
	}

	void ScriptEngine::InvokeOnCreate(UUID entity)
	{
		Invoke(entity, ScriptEvent::Create, 0.0f, UUID::Invalid());
	}

	void ScriptEngine::InvokeOnUpdate(UUID entity, float deltaTime)
	{
		Invoke(entity, ScriptEvent::Update, deltaTime, UUID::Invalid());
	}

	void ScriptEngine::InvokeOnFixedUpdate(UUID entity, float fixedDeltaTime)
	{
		Invoke(entity, ScriptEvent::FixedUpdate, fixedDeltaTime, UUID::Invalid());
	}

	void ScriptEngine::InvokeOnDestroy(UUID entity)
	{
		Invoke(entity, ScriptEvent::Destroy, 0.0f, UUID::Invalid());
	}

	void ScriptEngine::InvokeContactEvent(UUID entity, UUID other, ContactEventType type)
	{
		ScriptEvent scriptEvent = ScriptEvent::CollisionEnter;
		switch (type)
		{
			case ContactEventType::CollisionEnter:
				scriptEvent = ScriptEvent::CollisionEnter;
				break;
			case ContactEventType::CollisionExit:
				scriptEvent = ScriptEvent::CollisionExit;
				break;
			case ContactEventType::TriggerEnter:
				scriptEvent = ScriptEvent::TriggerEnter;
				break;
			case ContactEventType::TriggerExit:
				scriptEvent = ScriptEvent::TriggerExit;
				break;
		}
		Invoke(entity, scriptEvent, 0.0f, other);
	}
}
