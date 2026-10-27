#pragma once

#include "Editor/Commands/ComponentCommands.h"
#include "Editor/VisualScripting/VisualScriptCommit.h"
#include "Editor/VisualScripting/VisualScriptEditorModel.h"
#include "Editor/VisualScripting/VisualScriptEditorTypes.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"
#include "Strada/Script/ScriptField.h"
#include "Strada/Script/VisualScriptLibrary.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// The graph window's controller: what turns the canvas model's intents, the node menu's picks and the window's own menus and
// in-place fields into edits of the visual script. Each call computes at most one commit request (one undo step) from a copy
// of the window's display document with the document editing operations (VisualScriptEditing.h, the clipboard, Arrange and
// the quick fixes), prepared as every writer prepares documents (VisualScriptCommit.h), and says what the window shows or
// does besides (a clipboard text, toasts, a sub-graph, the palette, an in-place editor). No ImGui and no state: the panel
// executes the request with EditorOperations and keeps whatever changes from frame to frame. Main thread only (it reads the
// asset registry for dropped assets).
namespace Strada
{
	// Merge keys of the commits that merge into one undo step (the panel passes the key of the run in
	// VisualScriptEditContext::MergeKey): arrow-key nudges, "VSNG" plus the run's counter (a run lasts while presses come less
	// than VisualScriptNudgeRunSeconds apart); a creation and its first name (Add Comment Box and its title), "VSNM" plus a
	// counter of such runs.
	inline constexpr uint64_t VisualScriptNudgeMergeKey = 0x56534E4700000000ull;
	inline constexpr uint64_t VisualScriptNameMergeKey = 0x56534E4D00000000ull;
	inline constexpr double VisualScriptNudgeRunSeconds = 0.5;
	// How far the arrow keys move the selection, in canvas units: one grid step, and with Alt one unit.
	inline constexpr float VisualScriptNudgeStep = 16.0f;
	inline constexpr float VisualScriptFineNudgeStep = 1.0f;
	// How far apart the nodes of several dropped entities stand, one below the other.
	inline constexpr float VisualScriptDropSpacing = 64.0f;

	// The texts the controller refuses edits with, which the window's notices also show.
	namespace VisualScriptRefusals
	{
		// Without the scripting runtime (the degraded notice).
		inline constexpr std::string_view Degraded =
			"Node details need the scripting runtime, which is not running: you can arrange, comment, copy and delete nodes.";
		// Delete without the scripting runtime: only reroutes are bridged.
		inline constexpr std::string_view NotBridged = "Links are not bridged without the scripting runtime";
		// A memory asset.
		inline constexpr std::string_view ReadOnlyMemory = "Read only: this visual script has no file";
		// Entity drops while playing.
		inline constexpr std::string_view EntitiesWhilePlaying = "References to scene entities are added in edit mode";
		// Paste of a text that is not a visual script clipboard.
		inline constexpr std::string_view NoClipboard = "The clipboard holds no visual script nodes";
		// A My Script row dragged from another script's window.
		inline constexpr std::string_view OtherScriptRow = "Variables belong to their script: copy the nodes instead";
		// Promote to Variable or to Function Input on an array pin (v2 §3.2: arrays as variables come with slice 3).
		inline constexpr std::string_view PromoteArray = "Variables and function inputs cannot hold arrays";
	}

	// The note of a file without write permission, which the window's notice shows too: "<file name> is read-only on disk: edits
	// cannot be saved."
	[[nodiscard]] std::string MakeVisualScriptReadOnlyFileNote(std::string_view fileName);

	// What an entity drop needs of the edited scene (the running copy is never written: drops are refused while playing).
	struct VisualScriptSceneContext
	{
		// Playing or simulating: entity drops are refused.
		bool Playing = false;
		// The name of an entity of the edited scene; nothing for one that no longer exists.
		std::function<std::optional<std::string>(UUID entity)> EntityName;
		// The window's context entity, when its VisualScriptComponent runs this graph in the edited scene (invalid otherwise),
		// with its name and that component's Variables: an entity drop also sets the new variable there, writing the whole map.
		UUID ContextEntity = UUID::Invalid();
		std::string ContextEntityName;
		ScriptFieldMap ContextVariables;
	};

	// What the controller edits and with what. The model is the window's, synced and updated this frame: its display document,
	// analysis, canvas and shown sub-graph are what edits are computed from.
	struct VisualScriptEditContext
	{
		explicit VisualScriptEditContext(VisualScriptEditorModel const& model)
			: Model(model)
		{
		}

		VisualScriptEditorModel const& Model;
		// Null without the scripting runtime (degraded): the edits that need node details are refused.
		VisualScriptLibrary const* Library = nullptr;
		// The script's name, which ends every description ("Move 3 Nodes in Player").
		std::string_view ScriptName;
		AssetHandle Graph;
		// The key of the run the commit belongs to, used only by the commits that merge (VisualScriptNudgeMergeKey and
		// VisualScriptNameMergeKey runs, and widget values and node comments edited in place under the shared edit session's
		// key); every other commit is a step of its own (key 0).
		uint64_t MergeKey = 0;
		// Paste only: the clipboard's text, which the panel reads when pasting.
		std::string_view ClipboardText;
		// The document from before the undo step the commit belongs to, for a commit that merges into an earlier commit's step
		// (a creation's first name); null for the asset's document (the model's stored document).
		VisualScriptDocument const* Stored = nullptr;
		// The colour of new comment boxes (linear RGBA): the user's last comment colour.
		glm::vec4 CommentColor = VisualScriptComment().Color;
		// Entity drops only; null refuses them.
		VisualScriptSceneContext const* Scene = nullptr;
	};

	// An in-place text editor the window opens over the canvas.
	enum class VisualScriptTextEditKind : uint8_t
	{
		// A node's comment (its bubble): Ctrl+Enter or a click elsewhere commits it with a VisualScriptNodeCommentEdit.
		NodeComment = 0,
		// A comment box's title (the first line of its text): Enter commits it with a VisualScriptCommentEdit (Rename).
		CommentTitle
	};

	struct VisualScriptTextEditRequest
	{
		VisualScriptTextEditKind Kind = VisualScriptTextEditKind::NodeComment;
		// The node's or the comment's ID.
		uint32_t Element = 0;
		// What the editor starts with (selected when it is a new comment box's "Comment").
		std::string Text;
	};

	// What My Script or Details shows for the window.
	enum class VisualScriptRevealKind : uint8_t
	{
		// A node of the shown sub-graph, in Details.
		Node = 0,
		// My Script's rows.
		Variable,
		Local,
		Function,
		// A custom event: ID is its Event.Custom node.
		CustomEvent
	};

	struct VisualScriptReveal
	{
		VisualScriptRevealKind Kind = VisualScriptRevealKind::Node;
		uint32_t ID = 0;
		// Starts renaming it (F2, a promotion's new variable).
		bool Rename = false;
	};

	// A variable or local of My Script dropped on empty canvas without Ctrl or Alt: the window asks which node it makes, with
	// a menu "Get Speed / Set Speed" at the drop; the item picked resolves the drop again with Ctrl (Get) or Alt (Set).
	struct VisualScriptElementDropMenu
	{
		VisualScriptDropIntent Drop;
		// The variable's or local's name.
		std::string Name;
	};

	// What the window does with an intent or an edit.
	struct VisualScriptControllerResult
	{
		// The step to execute: with EditorOperations::EditVisualScript, or EditVisualScriptAndComponents when ComponentEdits is
		// not empty.
		std::optional<VisualScriptCommitRequest> Commit;
		// The components the same step changes (an entity drop that sets the context entity's variable).
		std::vector<ComponentEdit> ComponentEdits;
		// Copy and Cut: the text to put on the clipboard.
		std::optional<std::string> Clipboard;
		// Toasts: what was refused and why, what an edit left out.
		std::vector<std::string> Notes;
		// A sub-graph to show (0 for the event graph, else a function's ID).
		std::optional<uint32_t> ShowFunction;
		// The palette to open.
		std::optional<VisualScriptPaletteIntent> OpenPalette;
		// The menus to open: of an element, and of a collapsed anchor's pins.
		std::optional<VisualScriptContextMenuIntent> ContextMenu;
		std::optional<VisualScriptPinMenuIntent> PinMenu;
		// A jump within the script (the model applies it).
		std::optional<VisualScriptOpenRequest> Open;
		// What to find the references of.
		std::optional<VisualScriptReference> FindReferences;
		// An in-place editor to open.
		std::optional<VisualScriptTextEditRequest> EditText;
		// A row or node to show.
		std::optional<VisualScriptReveal> Reveal;
		// The Get or Set menu of a dropped variable.
		std::optional<VisualScriptElementDropMenu> DropMenu;
		// Arrange: where the arranged nodes stood before (node ID, position), for the renderer's glide.
		std::vector<std::pair<uint32_t, glm::vec2>> GlideFrom;
		// The edit needs the scripting runtime, which is not running: the degraded notice blinks.
		bool NeedsRuntime = false;
	};

	// Edits the window's own menus and fields make, beside the canvas model's intents. Pins are named as the canvas knows them
	// (as intents name pins); links as the document stores them.

	// A value typed into a value chip or its live widget; null removes the stored literal (Reset to Default).
	struct VisualScriptLiteralEdit
	{
		VisualScriptPinReference Pin;
		Json Literal = nullptr;
	};

	// An item of a pin's menu.
	enum class VisualScriptPinAction : uint8_t
	{
		// Breaks every link of the pin (a collapsed anchor: of every pin of its side).
		BreakAllLinks = 0,
		// Breaks one of its links (Link).
		BreakLink,
		ResetToDefault,
		Split,
		// Picked on a field pin of a split pin ("Force.X"): recombines the split pin it belongs to (the innermost one).
		Recombine,
		PromoteToVariable,
		PromoteToFunctionInput
	};

	struct VisualScriptPinEdit
	{
		VisualScriptPinAction Action = VisualScriptPinAction::BreakAllLinks;
		VisualScriptPinReference Pin;
		VisualScriptPinDirection Direction = VisualScriptPinDirection::In;
		// A collapsed node's anchor standing for every pin of its side (VisualScriptContextMenuIntent::WholeSide).
		bool WholeSide = false;
		// BreakLink: the link.
		std::optional<VisualScriptLink> Link;
	};

	// The text of a node's comment, from its in-place editor: empty removes it.
	struct VisualScriptNodeCommentEdit
	{
		uint32_t Node = 0;
		std::string Text;
	};

	// An item of a comment box's menu, or its title field.
	enum class VisualScriptCommentAction : uint8_t
	{
		// Text becomes the title (the first line); the other lines stay.
		Rename = 0,
		// Color (linear RGBA).
		Recolour,
		// Frames the nodes and comment boxes entirely inside it, as a new comment box framing them would.
		FitToContents,
		// Removes the comment box; its contents stay.
		Delete,
		// Removes the comment box and its contents, bridging the flow as Delete does.
		DeleteWithContents
	};

	struct VisualScriptCommentEdit
	{
		VisualScriptCommentAction Action = VisualScriptCommentAction::Rename;
		uint32_t Comment = 0;
		std::string Text;
		glm::vec4 Color{0.0f};
	};

	// A node the window adds with a button (the empty event graph's Add Create Event): at Position (canvas units).
	struct VisualScriptAddNodeEdit
	{
		std::string Type;
		Json Settings = Json::object();
		glm::vec2 Position{0.0f};
	};

	using VisualScriptCanvasEdit = std::variant<VisualScriptLiteralEdit, VisualScriptPinEdit, VisualScriptNodeCommentEdit,
	                                            VisualScriptCommentEdit, VisualScriptAddNodeEdit>;

	// Resolves an intent of the window's model. Edits are refused (no commit, a note saying why) in read-only states and,
	// for those that need node details, without the scripting runtime; selection, view and menu intents pass through to the
	// result. A drop of one of the script's own My Script rows (VisualScriptDropKind::Element, PackVisualScriptElement; the
	// window refuses other scripts' rows at its drop target) makes its node (spec §9.4 rule 4): a variable or local dropped
	// on an input gets a Get linked into it, on a data output a Set fed from it (and linked after the output's node when that
	// node's exec output is free), on empty canvas a Get with Ctrl, a Set with Alt, else the DropMenu; a function makes a
	// Function.Call, a custom event an Event.Send of its name and parameters. Fails only when an edit the model asked for
	// cannot be made (an editor bug, such as a document the commit would not write).
	[[nodiscard]] Result<VisualScriptControllerResult> ResolveVisualScriptIntent(VisualScriptEditorIntent const& intent,
	                                                                             VisualScriptEditContext const& context);
	// Whether a drop can change the pin it is dropped on (spec §9.4 rule 5): an asset (payload: its handle) on an input whose
	// type takes assets of its type, entities on a Strada.Entity input, a My Script variable or local (payload: the packed row)
	// on a data pin; a function or a custom event is placed beside any pin. The canvas shows the not-allowed cursor while a
	// payload it would refuse hovers a pin, and the controller refuses it on delivery with a note, by this one rule.
	[[nodiscard]] bool CanDropOnVisualScriptPin(VisualScriptDropKind kind, uint64_t payload, VisualScriptPinVisual const& pin);
	// Resolves an edit of the window's menus and fields, as ResolveVisualScriptIntent does.
	[[nodiscard]] Result<VisualScriptControllerResult> ResolveVisualScriptEdit(VisualScriptCanvasEdit const& edit,
	                                                                           VisualScriptEditContext const& context);

	// An item of the node context menu.
	struct VisualScriptMenuItem
	{
		// Resolved as a VisualScriptActionIntent with the selection (the same path as the keys).
		VisualScriptEditorAction Action = VisualScriptEditorAction::Cut;
		std::string Label;
		// The submenu the item is listed in (consecutive items share it); empty at the top level.
		std::string_view Submenu;
		bool Enabled = true;
		// The tooltip of a disabled item: why it does not apply.
		std::string Reason;
		// A radio item that is chosen (Enabled State).
		bool Checked = false;
		// The first item of a group after another group: a separator goes before it.
		bool SeparatorBefore = false;
	};

	// The node context menu of the shown sub-graph's selection (the primary node first; the model selects an unselected node a
	// right click opens the menu on), in its order: Open, Edit, Links, Find, Node, Arrange, Fix. Items that do not apply to the
	// nodes' state are disabled with a reason; labels count the nodes they act on ("Collapse 3 Nodes"). Empty without a
	// selected node. The window adds each item's shortcut text (VisualScriptShortcuts.h).
	[[nodiscard]] std::vector<VisualScriptMenuItem> BuildVisualScriptNodeMenu(VisualScriptEditContext const& context);
}
