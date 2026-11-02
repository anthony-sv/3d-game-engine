#include "stpch.h"
#include "Strada/Script/VisualScriptEngine.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptReferences.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Script/ScriptEngineInternal.h"
#include "Strada/Script/ScriptFieldSerialization.h"
#include "Strada/Serialization/JsonTextWriter.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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
		using ScriptEngineInternal::SentReference;
		using ScriptEngineInternal::ToJsonText;
		using ScriptEngineInternal::ToLength;
		using ScriptEngineInternal::VisualScriptProgramRecord;
		using ScriptEngineInternal::VisualScriptReferenceText;

		constexpr char const* NotRunning = "the scripting runtime is not running";

		uint64_t NextGeneration(ScriptEngineData& data)
		{
			return ++data.LastProgramGeneration;
		}

		// The content form the runtime reads, compact. Text that is not UTF-8 is replaced rather than refused: the loader
		// never produces it, and the runtime reports what it then sees.
		std::string ToDocumentText(VisualScriptDocument const& document)
		{
			return VisualScriptSerializer::WriteContentText(document);
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

		// --- References: the other graphs a document's nodes refer to, as the runtime receives them ---

		// The Error of the graphs beyond VisualScriptEngine::MaxReferencedGraphs.
		std::string const& GetTooManyReferencesError()
		{
			static std::string const s_Error =
				fmt::format("more than {} visual scripts are referenced", VisualScriptEngine::MaxReferencedGraphs);
			return s_Error;
		}

		// A reference beyond VisualScriptEngine::MaxReferencedGraphs, sent as absent whatever its graph is.
		bool IsBeyondLimit(SentReference const& reference)
		{
			return reference.Document == nullptr && reference.Error == GetTooManyReferencesError();
		}

		// The name references give a graph: its asset path (the name of a memory asset); empty when it is not registered or
		// there are no assets (the runtime then names it by its handle).
		std::string GetReferenceName(AssetHandle graph)
		{
			if (!AssetManager::IsInitialized())
			{
				return {};
			}
			std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(graph);
			if (!metadata)
			{
				return {};
			}
			return metadata->IsFileAsset() ? metadata->Path : metadata->GetDisplayName();
		}

		// The graph a reference names, or why it is absent.
		Result<Ref<VisualScriptAsset>> FindReferencedGraph(AssetHandle graph)
		{
			if (AssetManager::IsInitialized() && AssetManager::GetAssetType(graph) == AssetType::None)
			{
				return MakeError("no visual script {} in the project (deleted, or not a visual script)", graph);
			}
			return FindGraphAsset(graph);
		}

		// Every graph the document refers to and, through the libraries among them, theirs, each once, ascending by handle,
		// with the strongest use any of them makes of it, as the assets are now. self is the document's own graph: a node
		// naming it refers to the document itself, so it is never an entry. The first MaxReferencedGraphs graphs found
		// (breadth first: the document's own references by handle, then each library's) are looked up; those beyond are
		// absent.
		std::vector<SentReference> CollectReferencedGraphs(VisualScriptDocument const& document, AssetHandle self)
		{
			std::vector<SentReference> references;
			std::unordered_map<AssetHandle, size_t> indices;
			// The entries whose graphs' references are collected next: the libraries, in the order they were found.
			std::vector<size_t> libraries;
			size_t lookedUp = 0;
			auto const add = [&](VisualScriptGraphReference const& reference)
			{
				if (reference.Graph == self)
				{
					return;
				}
				if (auto const known = indices.find(reference.Graph); known != indices.end())
				{
					SentReference& entry = references[known->second];
					if (reference.Use > entry.Use)
					{
						entry.Use = reference.Use;
						if (entry.Use == VisualScriptReferenceUse::Library && entry.Document != nullptr)
						{
							libraries.push_back(known->second);
						}
					}
					return;
				}
				SentReference entry;
				entry.Graph = reference.Graph;
				entry.Use = reference.Use;
				entry.Name = GetReferenceName(reference.Graph);
				if (lookedUp == VisualScriptEngine::MaxReferencedGraphs)
				{
					entry.Error = GetTooManyReferencesError();
				}
				else
				{
					lookedUp++;
					if (Result<Ref<VisualScriptAsset>> found = FindReferencedGraph(reference.Graph))
					{
						entry.Document = found.GetValue()->GetSharedDocument();
					}
					else
					{
						entry.Error = found.GetError();
					}
				}
				indices.emplace(entry.Graph, references.size());
				if (entry.Use == VisualScriptReferenceUse::Library && entry.Document != nullptr)
				{
					libraries.push_back(references.size());
				}
				references.push_back(std::move(entry));
			};
			for (VisualScriptGraphReference const& reference : CollectVisualScriptReferences(document))
			{
				add(reference);
			}
			for (size_t next = 0; next < libraries.size(); next++)
			{
				// Held apart from the entries, which grow while its references are added.
				Ref<VisualScriptDocument const> const library = references[libraries[next]].Document;
				for (VisualScriptGraphReference const& reference : CollectVisualScriptReferences(*library))
				{
					add(reference);
				}
			}
			std::sort(references.begin(), references.end(),
			          [](SentReference const& a, SentReference const& b)
			          {
						  return a.Graph.GetUUID().GetValue() < b.Graph.GetUUID().GetValue();
					  });
			return references;
		}

		// Whether the runtime receives references with the document: when one of its nodes names a graph, itself included
		// (the references say which graph the document is).
		bool SendsReferences(VisualScriptDocument const& document, std::vector<SentReference> const& references)
		{
			return !references.empty() || !CollectVisualScriptReferences(document).empty();
		}

		// Whether two references give the same program to one that compiles with them: the same graph, use, name and absence,
		// and the same program (a library) or link interface (another script) in the documents sent. The runtime reads
		// nothing of a graph it only verifies.
		bool HaveSameReference(SentReference const& a, SentReference const& b)
		{
			if (a.Graph != b.Graph || a.Use != b.Use || a.Name != b.Name || (a.Document == nullptr) != (b.Document == nullptr))
			{
				return false;
			}
			if (a.Document == b.Document)
			{
				return true;
			}
			switch (a.Use)
			{
				case VisualScriptReferenceUse::Library:
					return HaveSameVisualScriptProgram(*a.Document, *b.Document);
				case VisualScriptReferenceUse::Interface:
					return HaveSameVisualScriptLinkInterface(*a.Document, *b.Document);
				case VisualScriptReferenceUse::Verification:
					break;
			}
			return true;
		}

		// Whether two sends give the same program: their documents do, and their references match entry by entry.
		bool HaveSameProgram(Ref<VisualScriptDocument const> const& document, std::vector<SentReference> const& references,
		                     Ref<VisualScriptDocument const> const& otherDocument, std::vector<SentReference> const& otherReferences)
		{
			if (document != otherDocument && !HaveSameVisualScriptProgram(*document, *otherDocument))
			{
				return false;
			}
			return std::ranges::equal(references, otherReferences, &HaveSameReference);
		}

		// A graph's document as references send it (its full form for a library, its interface otherwise), compact, from the
		// texts kept per handle: written only when the graph's shared document is another object than the one they were
		// written of.
		std::string const& GetReferenceText(ScriptEngineData& data, SentReference const& reference, bool disableDevelopmentNodes)
		{
			VisualScriptReferenceText& texts = data.VisualScriptReferenceTexts[reference.Graph];
			if (texts.Document != reference.Document || texts.DevelopmentNodesDisabled != disableDevelopmentNodes)
			{
				texts = VisualScriptReferenceText{reference.Document, disableDevelopmentNodes, {}, {}};
			}
			bool const full = reference.Use == VisualScriptReferenceUse::Library;
			std::string& text = full ? texts.Full : texts.Interface;
			// No content form is empty, so an empty text is one not written yet.
			if (text.empty())
			{
				VisualScriptDocument sent = full ? *reference.Document : MakeVisualScriptInterface(*reference.Document);
				if (disableDevelopmentNodes)
				{
					sent = DisableDevelopmentOnlyNodes(std::move(sent));
				}
				text = VisualScriptSerializer::WriteContentText(sent);
			}
			return text;
		}

		// A string as the compact JSON text writes it, quotes included.
		std::string ToJsonString(std::string_view text)
		{
			JsonTextWriter::Style style;
			style.Pretty = false;
			JsonTextWriter writer(style, text.size() + 2);
			writer.String(text);
			return writer.TakeText();
		}

		// The references the runtime receives beside a document (spec-slice3 section 2.4), compact:
		// {"Self":"<handle>","Graphs":[{"Graph":"<handle>","Name":"<path>","Form":"Full"|"Interface","Document":{...}}, or
		// {"Graph":"<handle>","Name":"<path>","Error":"<why it is absent>"}, ...]}. The documents are the content form that
		// VisualScriptSerializer::WriteContentText writes, kept per handle; the rest is written around them.
		std::string WriteReferences(ScriptEngineData& data, std::vector<SentReference> const& references, AssetHandle self,
		                            bool disableDevelopmentNodes)
		{
			std::string text = R"({"Self":")" + self.ToString() + R"(","Graphs":[)";
			for (size_t index = 0; index < references.size(); index++)
			{
				SentReference const& reference = references[index];
				text += index == 0 ? R"({"Graph":")" : R"(,{"Graph":")";
				text += reference.Graph.ToString();
				text += R"(","Name":)";
				text += ToJsonString(reference.Name);
				if (reference.Document != nullptr)
				{
					text += reference.Use == VisualScriptReferenceUse::Library ? R"(,"Form":"Full","Document":)"
					                                                           : R"(,"Form":"Interface","Document":)";
					text += GetReferenceText(data, reference, disableDevelopmentNodes);
				}
				else
				{
					text += R"(,"Error":)";
					text += ToJsonString(reference.Error);
				}
				text += '}';
			}
			text += "]}";
			return text;
		}

		// Whether a graph a program was compiled with changed since: it appeared, disappeared or was renamed (a rename is told
		// by a change of the registry), or has another program (a library) or link interface (another script). A reference
		// whose graph holds the same program or interface in a new document takes it, in the working references too, so an
		// unchanged graph costs a lookup and a pointer comparison. Graphs beyond MaxReferencedGraphs are not looked up.
		bool HaveReferencesChanged(VisualScriptProgramRecord& record, bool registryChanged)
		{
			for (SentReference& reference : record.References)
			{
				if (IsBeyondLimit(reference))
				{
					continue;
				}
				Result<Ref<VisualScriptAsset>> const found = FindReferencedGraph(reference.Graph);
				if (found.IsOk() != (reference.Document != nullptr))
				{
					return true;
				}
				if (registryChanged && GetReferenceName(reference.Graph) != reference.Name)
				{
					return true;
				}
				if (!found || reference.Use == VisualScriptReferenceUse::Verification)
				{
					continue;
				}
				Ref<VisualScriptDocument const> const& document = found.GetValue()->GetSharedDocument();
				if (document == reference.Document)
				{
					continue;
				}
				bool const same = reference.Use == VisualScriptReferenceUse::Library
				                      ? HaveSameVisualScriptProgram(*reference.Document, *document)
				                      : HaveSameVisualScriptLinkInterface(*reference.Document, *document);
				if (!same)
				{
					return true;
				}
				for (SentReference& working : record.WorkingReferences)
				{
					if (working.Graph == reference.Graph && working.Use == reference.Use && working.Document == reference.Document)
					{
						working.Document = document;
					}
				}
				reference.Document = document;
			}
			return false;
		}

		// Sends a document of the graph to the runtime with the references it is compiled with, and the runtime compiles a
		// program of them for the handle: a working one replaces the program of the handle's instances, a broken one leaves it
		// running. Every document sent goes through here, so while development nodes do not run, the runtime receives each,
		// and each document of its references, with its DevelopmentOnly nodes Disabled. Answers the errors of its Error
		// diagnostics (empty when it compiles), or nothing when the runtime failed or its answer cannot be read (logged).
		std::optional<std::string> LoadDocument(ScriptEngineData& data, AssetHandle graph, std::string const& name,
		                                        VisualScriptDocument const& document, std::vector<SentReference> const& references)
		{
			bool const disableDevelopmentNodes = !data.DevelopmentNodesEnabled;
			std::string const text =
				disableDevelopmentNodes ? ToDocumentText(DisableDevelopmentOnlyNodes(document)) : ToDocumentText(document);
			std::string const referencesText =
				SendsReferences(document, references) ? WriteReferences(data, references, graph, disableDevelopmentNodes) : std::string();
			std::string answer;
			auto const status = static_cast<ScriptStatus>(data.Host.VisualScriptLoad(
				graph.GetUUID().GetValue(), name.data(), ToLength(name.size()), text.data(), ToLength(text.size()), referencesText.data(),
				ToLength(referencesText.size()), &answer, &ReceiveText));
			if (status != ScriptStatus::Success)
			{
				// The runtime logged why; the handle keeps the programs it had.
				ST_CORE_ERROR("Visual script '{}' cannot be loaded (see the log)", name);
				return std::nullopt;
			}
			Result<std::vector<VisualScriptDiagnostic>> diagnostics = ParseVisualScriptDiagnostics(answer);
			if (!diagnostics)
			{
				ST_CORE_ERROR("The diagnostics of visual script '{}' cannot be read: {}", name, diagnostics.GetError());
				return std::nullopt;
			}
			return DescribeVisualScriptErrors(diagnostics.GetValue());
		}

		// Gives the runtime a document of the graph under the graph's name, with the other graphs it refers to as they are now,
		// and records both whatever the outcome, so that a document that does not compile is not sent again every frame; its
		// errors are logged once. The working program (its document with the references it was compiled with) goes first
		// when the runtime must hear of it again: under a new name, which the runtime's messages give, and after a
		// game-assembly change, when only compiling it again tells whether it still runs (and so whether a broken document
		// leaves it running). A send of the same program tells that itself, so no program is compiled twice.
		void SendDocument(ScriptEngineData& data, AssetHandle graph, VisualScriptProgramRecord& record,
		                  Ref<VisualScriptDocument const> document, std::string name)
		{
			bool const renamed = name != record.Name;
			record.Name = std::move(name);
			record.RegistryVersion = AssetManager::GetVersion();
			record.IsSendPending = false;
			std::vector<SentReference> references = CollectReferencedGraphs(*document, graph);
			if (record.WorkingDocument != nullptr && (renamed || record.IsStale) &&
			    !HaveSameProgram(record.WorkingDocument, record.WorkingReferences, document, references))
			{
				if (std::optional<std::string> const working =
				        LoadDocument(data, graph, record.Name, *record.WorkingDocument, record.WorkingReferences))
				{
					record.IsStale = false;
					if (!working->empty())
					{
						// It no longer compiles for the changed game assembly (the runtime logged that it was compiled again), so
						// nothing runs.
						record.WorkingDocument = nullptr;
						record.WorkingReferences.clear();
					}
				}
			}
			record.Document = std::move(document);
			record.References = std::move(references);
			std::optional<std::string> const errors = LoadDocument(data, graph, record.Name, *record.Document, record.References);
			record.DocumentCompiles = errors.has_value() && errors->empty();
			if (record.DocumentCompiles)
			{
				record.WorkingDocument = record.Document;
				record.WorkingReferences = record.References;
				record.IsStale = false;
				return;
			}
			if (errors)
			{
				// After a game-assembly change, a document of the working program that does not compile tells that the working
				// program does not either (the runtime compiled it again and dropped it), as sending it first would have.
				if (record.IsStale && record.WorkingDocument != nullptr &&
				    HaveSameProgram(record.WorkingDocument, record.WorkingReferences, record.Document, record.References))
				{
					record.WorkingDocument = nullptr;
					record.WorkingReferences.clear();
					record.IsStale = false;
				}
				ST_CORE_ERROR("Visual script '{}' has {}{}", record.Name, *errors,
				              record.HasWorkingProgram() ? "; the previous version keeps running" : "");
			}
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
				VisualScriptProgramRecord& record = entry.second;
				record.Generation = generation;
				record.IsStale = true;
				record.IsSendPending = !record.DocumentCompiles;
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

	VisualScriptLibraryState VisualScriptEngine::GetLibraryState()
	{
		ScriptEngineData const* const data = GetData();
		if (data == nullptr)
		{
			return VisualScriptLibraryState::NoRuntime;
		}
		if (data->Library != nullptr)
		{
			return VisualScriptLibraryState::Built;
		}
		return data->IsLibraryUnavailable ? VisualScriptLibraryState::Failed : VisualScriptLibraryState::NotBuilt;
	}

	std::optional<VisualScriptEnumInfo> VisualScriptEngine::FindEnum(std::string_view typeId)
	{
		Ref<VisualScriptLibrary const> const library = GetLibrary();
		return library != nullptr ? library->FindEnum(typeId) : std::nullopt;
	}

	Result<VisualScriptAnalysis> VisualScriptEngine::Analyze(VisualScriptDocument const& document, AssetHandle self)
	{
		ScriptEngineData* const data = GetData();
		if (data == nullptr)
		{
			return Error{NotRunning};
		}
		std::string const text = ToDocumentText(document);
		// The editor is a development host: analyses read every document as it is.
		std::vector<SentReference> const references = CollectReferencedGraphs(document, self);
		std::string const referencesText =
			SendsReferences(document, references) ? WriteReferences(*data, references, self, false) : std::string();
		std::string answer;
		auto const status = static_cast<ScriptStatus>(data->Host.VisualScriptAnalyze(
			text.data(), ToLength(text.size()), referencesText.data(), ToLength(referencesText.size()), &answer, &ReceiveText));
		if (status != ScriptStatus::Success)
		{
			return Error{"the visual script cannot be analyzed (see the log)"};
		}
		Result<VisualScriptAnalysis> analysis = VisualScriptAnalysis::Parse(answer);
		if (!analysis)
		{
			return MakeError("the analysis of the visual script cannot be read: {}", analysis.GetError());
		}
		std::vector<std::pair<AssetHandle, Ref<VisualScriptDocument const>>>& graphs = analysis.GetValue().Graphs;
		graphs.reserve(references.size());
		for (SentReference const& reference : references)
		{
			graphs.emplace_back(reference.Graph, reference.Document);
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
		if (asset->GetDocument().Kind == VisualScriptDocumentKind::Library)
		{
			// Libraries travel in the references of the graphs that call them; one runs on no entity, as an absent graph.
			if (record != data->VisualScriptPrograms.end())
			{
				record->second.IsAbsent = true;
			}
			return MakeError("{} is a function library: graphs call its functions; it runs on no entity", GetGraphName(graph));
		}
		if (record == data->VisualScriptPrograms.end())
		{
			VisualScriptProgramRecord& program = data->VisualScriptPrograms[graph];
			program.Asset = asset;
			program.Version = asset->GetVersion();
			program.Generation = NextGeneration(*data);
			SendDocument(*data, graph, program, asset->GetSharedDocument(), GetGraphName(graph));
			return {};
		}

		VisualScriptProgramRecord& program = record->second;
		bool const cameBack = std::exchange(program.IsAbsent, false);
		if (program.Asset != asset || program.Version != asset->GetVersion())
		{
			program.Asset = asset;
			program.Version = asset->GetVersion();
			Ref<VisualScriptDocument const> document = asset->GetSharedDocument();
			if (document != program.Document && !HaveSameVisualScriptProgram(*program.Document, *document))
			{
				program.Generation = NextGeneration(*data);
				SendDocument(*data, graph, program, std::move(document), GetGraphName(graph));
				return {};
			}
			// The runtime has the program: saved and loaded again, set to what it was, or changed only where the runtime never
			// reads (a node moved, a comment edited). The record keeps the newest document, for the running program too when
			// the program last sent is the one that runs.
			if (program.WorkingDocument == program.Document)
			{
				program.WorkingDocument = document;
			}
			program.Document = std::move(document);
		}
		if (program.IsSendPending)
		{
			program.Generation = NextGeneration(*data);
			SendDocument(*data, graph, program, program.Document, GetGraphName(graph));
			return {};
		}
		// Only a change of the registry can give the graph, or a graph it refers to, another name.
		uint64_t const registryVersion = AssetManager::GetVersion();
		bool const registryChanged = registryVersion != program.RegistryVersion;
		if (HaveReferencesChanged(program, registryChanged))
		{
			// The program compiled with them is another one now.
			program.Generation = NextGeneration(*data);
			SendDocument(*data, graph, program, program.Document, GetGraphName(graph));
			return {};
		}
		if (registryChanged)
		{
			program.RegistryVersion = registryVersion;
			if (std::string name = GetGraphName(graph); name != program.Name)
			{
				// Moved or renamed: the same programs under the new name, so no entity can start now that could not before.
				SendDocument(*data, graph, program, program.Document, std::move(name));
			}
		}
		if (cameBack)
		{
			program.Generation = NextGeneration(*data);
		}
		return {};
	}

	Ref<VisualScriptDocument const> VisualScriptEngine::GetWorkingDocument(AssetHandle graph)
	{
		ScriptEngineData const* const data = GetData();
		if (data == nullptr)
		{
			return nullptr;
		}
		auto const record = data->VisualScriptPrograms.find(graph);
		return record != data->VisualScriptPrograms.end() ? record->second.WorkingDocument : nullptr;
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

	bool VisualScriptEngine::HasWorkingProgram(AssetHandle graph)
	{
		ScriptEngineData const* const data = GetData();
		if (data == nullptr)
		{
			return false;
		}
		Result<Ref<VisualScriptAsset>> const found = FindGraphAsset(graph);
		if (found && found.GetValue()->GetDocument().Kind == VisualScriptDocumentKind::Library)
		{
			// A library has no program of its own: the graphs whose working programs were compiled with it run its last
			// version that worked.
			auto const compiledWith = [graph](SentReference const& reference)
			{
				return reference.Graph == graph && reference.Use == VisualScriptReferenceUse::Library && reference.Document != nullptr;
			};
			return std::ranges::any_of(data->VisualScriptPrograms,
			                           [&compiledWith](auto const& entry)
			                           {
										   return entry.second.HasWorkingProgram() &&
				                                  std::ranges::any_of(entry.second.WorkingReferences, compiledWith);
									   });
		}
		auto const record = data->VisualScriptPrograms.find(graph);
		return record != data->VisualScriptPrograms.end() && record->second.HasWorkingProgram();
	}

	void VisualScriptEngine::SetDevelopmentNodesEnabled(bool enabled)
	{
		ScriptEngineData* const data = GetData();
		ST_CORE_ASSERT(data != nullptr, "ScriptEngine is not initialized");
		// The programs sent so far would keep the other setting.
		ST_CORE_ASSERT(data == nullptr || data->VisualScriptPrograms.empty(),
		               "Whether development nodes run is set before any visual script is loaded");
		if (data == nullptr || !data->VisualScriptPrograms.empty())
		{
			return;
		}
		data->DevelopmentNodesEnabled = enabled;
	}

	bool VisualScriptEngine::AreDevelopmentNodesEnabled()
	{
		ScriptEngineData const* const data = GetData();
		return data == nullptr || data->DevelopmentNodesEnabled;
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
				// After a game-assembly change the runtime compiled the working program again: it still runs.
				record.IsStale = false;
				return {};
			case ScriptStatus::NotFound:
				// The runtime failed to load the document (logged when it was sent).
				return MakeError("the visual script '{}' is not loaded (see the log)", GetGraphName(graph));
			case ScriptStatus::Failure:
				// It has no working program, and the document last sent does not compile either: the runtime logged the errors.
				record.WorkingDocument = nullptr;
				record.WorkingReferences.clear();
				record.DocumentCompiles = false;
				record.IsStale = false;
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

	VisualScriptDocument DisableDevelopmentOnlyNodes(VisualScriptDocument document)
	{
		auto const disable = [](VisualScriptGraph& graph)
		{
			for (VisualScriptNode& node : graph.Nodes)
			{
				if (node.Enabled == VisualScriptNodeEnabled::DevelopmentOnly)
				{
					node.Enabled = VisualScriptNodeEnabled::Disabled;
				}
			}
		};
		disable(document.Graph);
		for (VisualScriptFunction& function : document.Functions)
		{
			disable(function.Graph);
		}
		return document;
	}
}
