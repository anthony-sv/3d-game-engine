#pragma once

#include "Editor/Commands/ComponentCommands.h"
#include "Editor/Commands/EditorCommand.h"
#include "Editor/VisualScripting/VisualScriptCommit.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Strada
{
	// Applies a partial patch to the parameters of a material file (.smat) and writes the file. Undoable like scene edits,
	// but the scene does not count as modified. Every write is recorded (AssetManager::RecordFileWrite), so a refresh does
	// not reload what the step wrote. Commands with the same material and non-zero merge key merge into one step.
	class SetMaterialCommand final : public EditorCommand
	{
	public:
		SetMaterialCommand(AssetHandle material, Json patch, uint64_t mergeKey = 0);

		Result<void> Execute(EditorContext& context) override;
		Result<void> Undo(EditorContext& context) override;
		std::string GetDescription() const override { return "Edit Material"; }
		bool HasEffect() const override { return m_Before != m_After; }
		bool ModifiesScene() const override { return false; }
		bool CanMergeWith(EditorCommand const& next) const override;
		void MergeWith(EditorCommand& next) override;
		AssetHandle GetEditedAsset() const override { return m_Material; }

	private:
		AssetHandle m_Material;
		Json m_Patch;
		uint64_t m_MergeKey;
		// Every material field before and after, recorded by the first execution.
		Json m_Before;
		Json m_After;
	};

	// Replaces the document of a visual script file (.sgraph): the file is written first, then the loaded asset takes the
	// document object itself (running graphs take the edit at their next update). Undoable like scene edits, but the scene
	// does not count as modified. The asset is looked up by its handle each time, so moving the file keeps the step
	// working. A file that is read-only on disk is refused, by undo too (which then stays on the stack until the file is
	// writable again), on every platform. A file that does not load is replaced too.
	//
	// Undo gives the file back byte for byte: a version-1 file stays version 1, a file the loader repaired keeps what it
	// repairs, a byte order mark stays, an emptied file is empty again, and a file that did not load fails to load again.
	// Redo writes the canonical text of the document again. Every write is recorded (AssetManager::RecordFileWrite), so a
	// refresh does not reload what the step wrote. Commands of the same graph with the same non-zero merge key merge into one
	// step; consecutive steps share their documents (a step's document is the object the next one keeps as its document
	// before).
	class SetVisualScriptCommand final : public EditorCommand
	{
	public:
		// document: complete and prepared (PrepareVisualScriptCommit); description: the Edit menu's text. text: the canonical
		// text of document's file when the caller has it (VisualScriptCommit::Text, which only the pipeline makes), written by
		// the first execution as it is, unchecked; empty to have the document serialized. A redo serializes it again.
		SetVisualScriptCommand(AssetHandle graph, Ref<VisualScriptDocument const> document, std::string description, uint64_t mergeKey = 0,
		                       std::string text = {});

		Result<void> Execute(EditorContext& context) override;
		Result<void> Undo(EditorContext& context) override;
		std::string GetDescription() const override { return m_Description; }
		// The documents differ (IsSameVisualScriptDocument), or the step wrote over file bytes it keeps: undoing it gives
		// them back even when its document is the one the file had.
		bool HasEffect() const override;
		bool ModifiesScene() const override { return false; }
		// The same graph and the same non-zero merge key.
		bool CanMergeWith(EditorCommand const& next) const override;
		// Takes next's document only: the step keeps what the file had before its own first execution (next's document before
		// is this step's own last document, never the file's original bytes).
		void MergeWith(EditorCommand& next) override;
		AssetHandle GetEditedAsset() const override { return m_Graph; }
		// The canonical text written for the document, and the file's bytes kept to give back.
		size_t GetMemoryUse() const override { return m_AfterTextSize + m_BeforeBytes.size(); }
		// What the last execution spent: ReadBack, Compare, Write and SetDocument (the phases of a commit this step runs), and
		// Text when it serialized the document itself; the step a later one merged into takes the later one's.
		VisualScriptCommitTimings const& GetLastTimings() const { return m_LastTimings; }

	private:
		// Reads what the file holds before the first execution.
		Result<void> RecordBefore(EditorContext const& context, std::filesystem::path const& path, std::string const& name);

		AssetHandle m_Graph;
		Ref<VisualScriptDocument const> m_After;
		// The text the constructor was given, until the first execution takes it: the step never keeps it.
		std::string m_GivenText;
		// The asset's document before the first execution (the object itself); null when the file did not load.
		Ref<VisualScriptDocument const> m_Before;
		// The file's bytes before the first execution, when m_KeepsBytes.
		std::vector<uint8_t> m_BeforeBytes;
		std::string m_Description;
		uint64_t m_MergeKey = 0;
		// Bytes of the canonical text written for m_After.
		size_t m_AfterTextSize = 0;
		// The first execution read what the file held.
		bool m_Recorded = false;
		// The first execution kept the file's bytes, because the file did not load or they are not the canonical text of
		// m_Before (a version-1 file, a file the loader repaired, a byte order mark, an empty file): undo writes them back
		// instead of that text. Recorded apart from the bytes, which may be none at all.
		bool m_KeepsBytes = false;
		// An execution wrote the file.
		bool m_Wrote = false;
		// The kept bytes load as m_Before. They do not when the file changed outside the editor after the asset loaded and
		// no refresh has seen it yet: undo then writes them back without recording the write, so the next refresh loads them.
		bool m_BytesLoadAsBefore = true;
		VisualScriptCommitTimings m_LastTimings;
	};

	// One step that edits a visual script and the components of entities that run it (an entity dropped into a graph becomes
	// a variable, which the entity running the graph is given as its value), all or nothing: a SetVisualScriptCommand and a
	// SetComponentsCommand executed graph part first and undone component part first, the first part undone again when
	// the second one fails. It is a step of the scene (ModifiesScene() is true): it stays with the scene's history. Its
	// edited asset and memory use are its graph part's. When the graph is deleted, ForgetEditedAsset leaves a step that
	// undoes and redoes only the components.
	class SetVisualScriptAndComponentsCommand final : public EditorCommand
	{
	public:
		// text: as SetVisualScriptCommand takes it.
		SetVisualScriptAndComponentsCommand(AssetHandle graph, Ref<VisualScriptDocument const> document, std::string description,
		                                    std::vector<ComponentEdit> edits, std::string text = {});

		Result<void> Execute(EditorContext& context) override;
		Result<void> Undo(EditorContext& context) override;
		std::string GetDescription() const override { return m_Description; }
		bool HasEffect() const override { return ChangesGraph() || m_Components.HasEffect(); }
		AssetHandle GetEditedAsset() const override;
		size_t GetMemoryUse() const override;
		void ForgetEditedAsset() override;

	private:
		// The graph part wrote the graph (and has not been forgotten): undo and redo write it too.
		bool ChangesGraph() const { return m_Graph != nullptr && m_Graph->HasEffect(); }

		// Null once the graph was deleted.
		Scope<SetVisualScriptCommand> m_Graph;
		SetComponentsCommand m_Components;
		std::string m_Description;
		// The first execution succeeded.
		bool m_Recorded = false;
	};
}
