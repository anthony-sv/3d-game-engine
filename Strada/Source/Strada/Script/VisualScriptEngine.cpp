#include "stpch.h"
#include "Strada/Script/VisualScriptEngine.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Script/ScriptEngineInternal.h"
#include "Strada/Script/ScriptFieldSerialization.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		using ScriptEngineInternal::GetData;
		using ScriptEngineInternal::ReceiveText;
		using ScriptEngineInternal::ReportScriptException;
		using ScriptEngineInternal::ScriptEngineData;
		using ScriptEngineInternal::ScriptEvent;
		using ScriptEngineInternal::ScriptStatus;
		using ScriptEngineInternal::ToJsonText;
		using ScriptEngineInternal::ToLength;
		using ScriptEngineInternal::VisualScriptProgramRecord;

		constexpr char const* NotRunning = "the scripting runtime is not running";

		uint64_t NextGeneration(ScriptEngineData& data)
		{
			return ++data.LastProgramGeneration;
		}

		// The content form the runtime reads, compact. Text that is not UTF-8 is replaced rather than refused: the loader
		// never produces it, and the runtime reports what it then sees.
		std::string ToDocumentText(VisualScriptDocument const& document)
		{
			return VisualScriptSerializer::SerializeDocument(document).dump(-1, ' ', false, Json::error_handler_t::replace);
		}

		// The name messages give a graph: the path of a file asset, the name of a memory asset.
		std::string GetGraphName(AssetHandle graph)
		{
			std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(graph);
			if (!metadata)
			{
				return graph.ToString();
			}
			if (metadata->IsFileAsset())
			{
				return metadata->Path;
			}
			std::string name = metadata->GetDisplayName();
			return name.empty() ? graph.ToString() : name;
		}

		// The graph's asset, without loading an asset of another type; the error says why the graph is absent.
		Result<Ref<VisualScriptAsset>> FindGraphAsset(AssetHandle graph)
		{
			if (!graph.IsValid())
			{
				return Error{"no visual script is set"};
			}
			if (!AssetManager::IsInitialized())
			{
				return Error{"there are no assets"};
			}
			AssetType const type = AssetManager::GetAssetType(graph);
			if (type == AssetType::None)
			{
				return MakeError("asset {} is not registered", graph);
			}
			if (type != AssetType::VisualScript)
			{
				return MakeError("asset {} is a {}, not a VisualScript", graph, AssetTypeToString(type));
			}
			return AssetManager::TryGetAsset<VisualScriptAsset>(graph);
		}

		// Sends the asset's document to the runtime, which compiles a program of it. The record takes the document whatever
		// the outcome, so a document that does not compile is not sent again every frame.
		void SendDocument(ScriptEngineData& data, AssetHandle graph, VisualScriptProgramRecord& record, Ref<VisualScriptAsset> const& asset)
		{
			VisualScriptDocument const& document = asset->GetDocument();
			std::string const name = GetGraphName(graph);
			std::string const text = ToDocumentText(document);
			std::string answer;
			auto const status = static_cast<ScriptStatus>(data.Host.VisualScriptLoad(
				graph.GetUUID().GetValue(), name.data(), ToLength(name.size()), text.data(), ToLength(text.size()), &answer, &ReceiveText));
			record.Asset = asset;
			record.Version = asset->GetVersion();
			record.Document = document;
			record.Generation = NextGeneration(data);
			if (status != ScriptStatus::Success)
			{
				// The runtime logged why; the handle keeps the program it had.
				ST_CORE_ERROR("Visual script '{}' cannot be loaded (see the log)", name);
				return;
			}
			Result<std::vector<VisualScriptDiagnostic>> diagnostics = ParseVisualScriptDiagnostics(answer);
			if (!diagnostics)
			{
				ST_CORE_ERROR("The diagnostics of visual script '{}' cannot be read: {}", name, diagnostics.GetError());
				return;
			}
			std::string const errors = DescribeVisualScriptErrors(diagnostics.GetValue());
			if (errors.empty())
			{
				record.HasWorkingProgram = true;
				return;
			}
			ST_CORE_ERROR("Visual script '{}' has {}{}", name, errors,
			              record.HasWorkingProgram ? "; the previous version keeps running" : "");
		}

		// The runtime has no instance for the entity: the engine forgets it too.
		Error ForgetInstance(ScriptEngineData& data, UUID entity)
		{
			data.GraphInstances.erase(entity);
			return MakeError("entity {} runs no visual script", entity);
		}

		void Invoke(UUID entity, ScriptEvent scriptEvent, float timeStep, UUID other)
		{
			ScriptEngineData* const data = GetData();
			if (data == nullptr || !data->GraphInstances.contains(entity))
			{
				return;
			}
			auto const status = static_cast<ScriptStatus>(
				data->Host.VisualScriptInvoke(entity.GetValue(), static_cast<int32_t>(scriptEvent), timeStep, other.GetValue()));
			// The runtime logged the error; the application learns of it (test runs fail on it).
			if (status == ScriptStatus::Failure)
			{
				ReportScriptException();
			}
			if (scriptEvent == ScriptEvent::Destroy || status == ScriptStatus::NotFound)
			{
				data->GraphInstances.erase(entity);
			}
		}
	}

	namespace ScriptEngineInternal
	{
		void OnGameAssemblyChanged()
		{
			ScriptEngineData* const data = GetData();
			if (data == nullptr)
			{
				return;
			}
			data->GraphInstances.clear();
			data->Library.reset();
			data->IsLibraryUnavailable = false;
			uint64_t const generation = NextGeneration(*data);
			for (auto& entry : data->VisualScriptPrograms)
			{
				entry.second.Generation = generation;
				entry.second.HasWorkingProgram = false;
			}
		}
	}

	Ref<VisualScriptLibrary const> VisualScriptEngine::GetLibrary()
	{
		ScriptEngineData* const data = GetData();
		if (data == nullptr)
		{
			return nullptr;
		}
		if (data->Library != nullptr || data->IsLibraryUnavailable)
		{
			return data->Library;
		}
		std::string description;
		if (static_cast<ScriptStatus>(data->Host.VisualScriptDescribeLibrary(&description, &ReceiveText)) != ScriptStatus::Success)
		{
			ST_CORE_ERROR("The visual script node library cannot be described (see the log)");
			data->IsLibraryUnavailable = true;
			return nullptr;
		}
		Result<Scope<VisualScriptLibrary>> library = VisualScriptLibrary::Parse(description);
		if (!library)
		{
			ST_CORE_ERROR("The visual script node library cannot be read: {}", library.GetError());
			data->IsLibraryUnavailable = true;
			return nullptr;
		}
		data->Library = library.TakeValue();
		return data->Library;
	}

	VisualScriptEnumLookup VisualScriptEngine::MakeEnumLookup()
	{
		Ref<VisualScriptLibrary const> library = GetLibrary();
		if (library == nullptr)
		{
			return {};
		}
		return [library = std::move(library)](std::string_view typeId)
		{
			return library->FindEnum(typeId);
		};
	}

	Result<VisualScriptAnalysis> VisualScriptEngine::Analyze(VisualScriptDocument const& document)
	{
		ScriptEngineData* const data = GetData();
		if (data == nullptr)
		{
			return Error{NotRunning};
		}
		std::string const text = ToDocumentText(document);
		std::string answer;
		auto const status =
			static_cast<ScriptStatus>(data->Host.VisualScriptAnalyze(text.data(), ToLength(text.size()), &answer, &ReceiveText));
		if (status != ScriptStatus::Success)
		{
			return Error{"the visual script cannot be analyzed (see the log)"};
		}
		Result<VisualScriptAnalysis> analysis = VisualScriptAnalysis::Parse(answer);
		if (!analysis)
		{
			return MakeError("the analysis of the visual script cannot be read: {}", analysis.GetError());
		}
		return analysis;
	}

	Result<void> VisualScriptEngine::EnsureLoaded(AssetHandle graph)
	{
		ScriptEngineData* const data = GetData();
		if (data == nullptr)
		{
			return Error{NotRunning};
		}
		Result<Ref<VisualScriptAsset>> found = FindGraphAsset(graph);
		auto const record = data->VisualScriptPrograms.find(graph);
		if (!found)
		{
			if (record != data->VisualScriptPrograms.end())
			{
				record->second.IsAbsent = true;
			}
			return Error{found.GetError()};
		}
		Ref<VisualScriptAsset> const asset = found.TakeValue();
		if (record == data->VisualScriptPrograms.end())
		{
			SendDocument(*data, graph, data->VisualScriptPrograms[graph], asset);
			return {};
		}

		VisualScriptProgramRecord& program = record->second;
		bool const cameBack = std::exchange(program.IsAbsent, false);
		if (program.Asset != asset || program.Version != asset->GetVersion())
		{
			if (program.Document != asset->GetDocument())
			{
				SendDocument(*data, graph, program, asset);
				return {};
			}
			// Saved and loaded again, or set to what it was: the runtime has this document.
			program.Asset = asset;
			program.Version = asset->GetVersion();
		}
		if (cameBack)
		{
			program.Generation = NextGeneration(*data);
		}
		return {};
	}

	uint64_t VisualScriptEngine::GetProgramGeneration(AssetHandle graph)
	{
		ScriptEngineData const* const data = GetData();
		if (data == nullptr)
		{
			return 0;
		}
		auto const record = data->VisualScriptPrograms.find(graph);
		return record != data->VisualScriptPrograms.end() ? record->second.Generation : 0;
	}

	Result<void> VisualScriptEngine::CreateInstance(UUID entity, AssetHandle graph, ScriptFieldMap const& variables)
	{
		if (Result<void> loaded = EnsureLoaded(graph); !loaded)
		{
			return loaded;
		}
		ScriptEngineData& data = *GetData();
		std::string const variablesText = ToJsonText(variables);
		auto const status = static_cast<ScriptStatus>(data.Host.VisualScriptCreateInstance(
			entity.GetValue(), graph.GetUUID().GetValue(), variablesText.data(), ToLength(variablesText.size())));
		VisualScriptProgramRecord& record = data.VisualScriptPrograms[graph];
		switch (status)
		{
			case ScriptStatus::Success:
				data.GraphInstances.insert(entity);
				record.HasWorkingProgram = true;
				return {};
			case ScriptStatus::NotFound:
				// The runtime failed to load the document (logged when it was sent).
				return MakeError("the visual script '{}' is not loaded (see the log)", GetGraphName(graph));
			case ScriptStatus::Failure:
				// It has no working program: the runtime logged its errors.
				record.HasWorkingProgram = false;
				ReportScriptException();
				return MakeError("the visual script '{}' cannot run (see the log)", GetGraphName(graph));
			case ScriptStatus::BindingMismatch:
				break;
		}
		return MakeError("the visual script '{}' cannot be created (see the log)", GetGraphName(graph));
	}

	bool VisualScriptEngine::HasInstance(UUID entity)
	{
		ScriptEngineData const* const data = GetData();
		return data != nullptr && data->GraphInstances.contains(entity);
	}

	void VisualScriptEngine::DestroyInstance(UUID entity)
	{
		ScriptEngineData* const data = GetData();
		if (data != nullptr && data->GraphInstances.erase(entity) > 0)
		{
			data->Host.VisualScriptDestroyInstance(entity.GetValue());
		}
	}

	void VisualScriptEngine::DestroyAllInstances()
	{
		ScriptEngineData* const data = GetData();
		if (data != nullptr)
		{
			data->GraphInstances.clear();
			data->Host.VisualScriptDestroyAllInstances();
		}
	}

	size_t VisualScriptEngine::GetInstanceCount()
	{
		ScriptEngineData const* const data = GetData();
		return data != nullptr ? data->GraphInstances.size() : 0;
	}

	Result<ScriptFieldMap> VisualScriptEngine::GetInstanceVariables(UUID entity)
	{
		ScriptEngineData* const data = GetData();
		if (data == nullptr || !data->GraphInstances.contains(entity))
		{
			return MakeError("entity {} runs no visual script", entity);
		}
		std::string description;
		auto const status =
			static_cast<ScriptStatus>(data->Host.VisualScriptDescribeInstanceVariables(entity.GetValue(), &description, &ReceiveText));
		if (status == ScriptStatus::NotFound)
		{
			return ForgetInstance(*data, entity);
		}
		if (status != ScriptStatus::Success)
		{
			return MakeError("the variables of entity {}'s visual script cannot be read (see the log)", entity);
		}
		Result<Json> const json = ParseJson(description);
		ScriptFieldMap variables;
		Result<void> const read = json ? JsonTraits<ScriptFieldMap>::FromJson(json.GetValue(), variables, DeserializationContext{})
		                               : Result<void>(Error{json.GetError()});
		if (!read)
		{
			return MakeError("the variables of entity {}'s visual script cannot be read: {}", entity, read.GetError());
		}
		return variables;
	}

	Result<void> VisualScriptEngine::SetInstanceVariables(UUID entity, ScriptFieldMap const& variables)
	{
		ScriptEngineData* const data = GetData();
		if (data == nullptr || !data->GraphInstances.contains(entity))
		{
			return MakeError("entity {} runs no visual script", entity);
		}
		std::string const variablesText = ToJsonText(variables);
		auto const status = static_cast<ScriptStatus>(
			data->Host.VisualScriptSetInstanceVariables(entity.GetValue(), variablesText.data(), ToLength(variablesText.size())));
		if (status == ScriptStatus::NotFound)
		{
			return ForgetInstance(*data, entity);
		}
		if (status != ScriptStatus::Success)
		{
			return MakeError("the variables of entity {}'s visual script cannot be set (see the log)", entity);
		}
		return {};
	}

	void VisualScriptEngine::InvokeOnCreate(UUID entity)
	{
		Invoke(entity, ScriptEvent::Create, 0.0f, UUID::Invalid());
	}

	void VisualScriptEngine::InvokeOnUpdate(UUID entity, float deltaTime)
	{
		Invoke(entity, ScriptEvent::Update, deltaTime, UUID::Invalid());
	}

	void VisualScriptEngine::InvokeOnFixedUpdate(UUID entity, float fixedDeltaTime)
	{
		Invoke(entity, ScriptEvent::FixedUpdate, fixedDeltaTime, UUID::Invalid());
	}

	void VisualScriptEngine::InvokeOnDestroy(UUID entity)
	{
		Invoke(entity, ScriptEvent::Destroy, 0.0f, UUID::Invalid());
	}

	void VisualScriptEngine::InvokeContactEvent(UUID entity, UUID other, ContactEventType type)
	{
		Invoke(entity, ScriptEngineInternal::ToScriptEvent(type), 0.0f, other);
	}

	void VisualScriptEngine::InvokeOnClick(UUID entity, UUID button)
	{
		Invoke(entity, ScriptEvent::Click, 0.0f, button);
	}
}
