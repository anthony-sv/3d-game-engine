#pragma once

#include "HeadlessImGui.h"
#include "TestUtilities.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/Commands/CommandHistory.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/Panels/VisualScriptEditorPanel.h"
#include "Editor/UI/EditorUI.h"
#include "Editor/VisualScripting/VisualScriptBrushes.h"
#include "Editor/VisualScripting/VisualScriptCanvasRenderer.h"
#include "Editor/VisualScripting/VisualScriptEditorModel.h"
#include "Editor/VisualScripting/VisualScriptThemeStyle.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Helpers of the tests of the visual script editor's canvas UI: the canvas drawn headless, frame by frame, as its panel draws it.
namespace Strada::Testing
{
	// What the window manager does in every frame before any graph window draws (spec §4.4 rule 2), for tests that draw
	// graph windows without it: the edits of widgets that are no longer active are flushed (no item is active, an edit is
	// still pending), then the edit session all windows share ends its step when the active item is not the previous frame's.
	// previousActive keeps the active item from frame to frame (the window manager's m_LastActiveItem). Inside a frame.
	inline void EndInteractions(std::span<VisualScriptEditorPanel* const> panels, EditSession& session, CommandHistory& history,
	                            ImGuiID& previousActive)
	{
		ImGuiID const active = ImGui::GetActiveID();
		if (active == 0)
		{
			for (VisualScriptEditorPanel* panel : panels)
			{
				if (panel->HasPendingWidgetEdit())
				{
					panel->FlushWidgetEdit();
				}
			}
		}
		session.Update(active != 0 && active == previousActive, history);
		previousActive = active;
	}

	// A graph window's canvas drawn in a headless ImGui context: a project with one graph, its editor model synced and updated
	// with synthetic input, and the renderer drawing into a fixed window that covers the canvas (at the screen's origin,
	// without padding, so the window's clip rectangle is the canvas). Nodes are measured with the fixed-width measurer and
	// analyzed with the fixture library. Reduce Motion makes views move at once, so SetView applies in the next frame.
	class CanvasRenderFixture
	{
	public:
		explicit CanvasRenderFixture(VisualScriptDocument const& document, glm::vec2 canvasSize = glm::vec2(1600.0f, 900.0f),
		                             bool editorStyle = false)
			: Project(MakeGraphProject(Temporary, document)),
			  Settings(Temporary.GetPath() / "Views.json"),
			  Model(Project.Graph, Settings),
			  CanvasSize(canvasSize)
		{
			if (editorStyle)
			{
				Monospace = Gui.UseEditorStyle();
			}
			Settings.SetProject(Project.Project->GetFilePath());
			Settings.SetReduceMotion(true);
			Input.Hovered = true;
			Input.Focused = true;
			Input.Mouse = glm::vec2(-1000.0f);
			Frame();
		}

		// Draws a frame step seconds after the last one: the model's Sync and Update with Input (whose one-frame members are
		// cleared afterwards), then the renderer, then Extra when set (inside the canvas's window).
		void Frame(double step = 1.0 / 60.0)
		{
			Time += step;
			Gui.Frame(
				[this]
				{
					ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
					ImGui::SetNextWindowSize(ImVec2(CanvasSize.x, CanvasSize.y));
					ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
					ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
					ImGui::Begin("Canvas", nullptr,
				                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
				                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
					ImGui::PopStyleVar(2);
					ImGuiStyle const& style = ImGui::GetStyle();
					Theme = MakeVisualScriptThemeAnchors(style);
					Input.CanvasMin = glm::vec2(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y);
					Input.CanvasSize = CanvasSize;
					Input.UIScale = style.FontScaleDpi;
					Input.FontSize = ImGui::GetFontSize();
					Input.CurveTessellationTolerance = style.CurveTessellationTol;
					Input.Time = Time;
					VisualScriptSemantics& semantics = SemanticsOverride != nullptr ? *SemanticsOverride : Semantics;
					Model.Sync(semantics, Measure, Theme, false, Time);
					Intents = Model.Update(Input);
					if (PendingCommit)
					{
						// As the panel commits: in the frame, after the model's Update, and synced again before drawing.
						Ref<VisualScriptDocument const> const committed = CreateRef<VisualScriptDocument const>(std::move(*PendingCommit));
						PendingCommit.reset();
						AssetManager::GetAsset<VisualScriptAsset>(Project.Graph)->SetDocument(committed);
						Model.OnCommitted(committed);
						Model.Sync(semantics, Measure, Theme, false, Time);
					}
					Input.Pressed = {};
					Input.Released = {};
					Input.DoubleClicked = {};
					Input.ExtraPressed = {};
					Input.Wheel = glm::vec2(0.0f);
					Input.SpacePressed = false;
					Input.SpaceReleased = false;
					Input.Characters.clear();
					Input.Actions.clear();
					Input.Drop.reset();

					Draw.DrawList = ImGui::GetWindowDrawList();
					Draw.Style = MakeVisualScriptCanvasStyle(style, ImGui::GetFontSize());
					Draw.Library = &Semantics.GetFixtureLibrary();
					Draw.Time = Time;
					Draw.Mouse = Input.Mouse;
					Draw.Hovered = Input.Hovered;
					Draw.ReduceMotion = Settings.IsReduceMotion();
					Draw.Brushes = UseBrushes ? &Brushes : nullptr;
					Draw.Record = &Record;
					Renderer.Draw(Model, Draw);
					if (Extra)
					{
						Extra();
					}
					ImGui::End();
				},
				static_cast<float>(step));
		}

		// Shows a view of the shown sub-graph (applied at the next frame's Update; Reduce Motion makes it instant).
		void SetView(glm::vec2 center, float zoom)
		{
			VisualScriptOpenRequest request;
			request.Graph = Project.Graph;
			request.Frame = VisualScriptFrame::Keep;
			request.Center = center;
			request.Zoom = zoom;
			Model.ApplyOpenRequest(request);
			Frame();
		}

		// The UI scale (ImGuiStyle::FontScaleDpi), as the editor sets it from the monitor.
		void SetUIScale(float scale)
		{
			ImGui::GetStyle().FontScaleDpi = scale;
			Frame();
		}

		// Moves the mouse over a screen point for a frame.
		void MoveTo(glm::vec2 point, double step = 1.0 / 60.0)
		{
			Input.Mouse = point;
			Frame(step);
		}

		// A commit as the panel makes it, drawn in a frame: the asset takes the object after the model's Update, the model is
		// told and syncs again, then the canvas is drawn.
		void Commit(VisualScriptDocument document, double step = 1.0 / 60.0)
		{
			PendingCommit = std::move(document);
			Frame(step);
		}

		// A change made outside the window between frames (an undo, automation): the asset takes a new document object.
		void ChangeOutside(VisualScriptDocument document)
		{
			AssetManager::GetAsset<VisualScriptAsset>(Project.Graph)
				->SetDocument(CreateRef<VisualScriptDocument const>(std::move(document)));
		}

		// The record's item of a node, a reroute or a comment; fails the test when it was not drawn.
		VisualScriptDrawRecordItem const& Item(VisualScriptDrawItemKind kind, uint32_t element) const
		{
			VisualScriptDrawRecordItem const* item = Record.Find(kind, element);
			REQUIRE_MESSAGE(item != nullptr, "element ", element, " was not drawn");
			return *item;
		}

		HeadlessImGui Gui;
		ImFont* Monospace = nullptr;
		AssetManagerScope Assets;
		TemporaryDirectory Temporary;
		GraphProject Project;
		VisualScriptViewSettings Settings;
		FixtureVisualScriptSemantics Semantics;
		// Analyzes instead of Semantics when set.
		VisualScriptSemantics* SemanticsOverride = nullptr;
		VisualScriptTextMeasurer Measure = FixedWidthMeasurer(0.5f);
		VisualScriptThemeAnchors Theme;
		VisualScriptEditorModel Model;
		VisualScriptCanvasRenderer Renderer;
		VisualScriptBrushes Brushes;
		bool UseBrushes = true;
		VisualScriptDrawRecord Record;
		VisualScriptCanvasInput Input;
		VisualScriptCanvasDrawInput Draw;
		std::vector<VisualScriptEditorIntent> Intents;
		std::function<void()> Extra;
		std::optional<VisualScriptDocument> PendingCommit;
		glm::vec2 CanvasSize;
		double Time = 0.0;
	};

	// The edit session prefix every graph window shares ("VSED").
	inline constexpr uint64_t SessionKey = 0x5653454400000000ull;

	inline ImVec2 ToImVec2(glm::vec2 point)
	{
		return ImVec2(point.x, point.y);
	}

	// A graph window of the project's script "Player", drawn headless at a fixed place frame by frame as the editor draws it:
	// the window manager's end of widget interactions (spec §4.4 rule 2), then the window. The fixture library stands in for
	// the scripting runtime unless the test gives other semantics (the engine's, under ScriptRuntimeScope); Reduce Motion
	// makes views move at once. The first canvas's hint is taken as seen unless canvasHint (it covers the canvas's bottom).
	struct PanelFixture
	{
		explicit PanelFixture(VisualScriptDocument const& document, VisualScriptSemantics* semantics = nullptr, bool canvasHint = false)
			: UsedSemantics(semantics != nullptr ? semantics : &Semantics)
		{
			if (!canvasHint)
			{
				Settings.SetHintSeen(VisualScriptCanvasHint);
			}
			Services.MonospaceFont = Gui.UseEditorStyle();
			REQUIRE(Operations.CreateProject(Temporary.GetPath() / "Game", "Game", Scene("Main")).IsOk());
			File = AssetManager::GetAssetDirectory() / "Scripts" / "Player.sgraph";
			REQUIRE(FileSystem::CreateDirectories(File.parent_path()).IsOk());
			REQUIRE(VisualScriptSerializer::SaveToFile(document, File).IsOk());
			REQUIRE(Operations.RefreshAssets().IsOk());
			Graph = AssetManager::FindByPath("Scripts/Player.sgraph");
			REQUIRE(Graph.IsValid());
			Settings.SetProject(Context.GetProject()->GetFilePath());
			Settings.SetReduceMotion(true);
			Panel.emplace(Graph, Operations, *UsedSemantics, Settings, Session, Services);
			Frames(3);
		}

		void Draw()
		{
			VisualScriptEditorPanel* panels[] = {&*Panel};
			EndInteractions(panels, Session, Context.GetHistory(), PreviousActive);
			if (Probe)
			{
				Probe();
			}
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(WindowSize);
			Panel->OnImGuiRender(Open, std::nullopt);
		}

		std::function<void()> Drawer()
		{
			return [this]
			{
				Draw();
			};
		}

		void Frames(int count, float seconds = 1.0f / 60.0f)
		{
			for (int i = 0; i < count; i++)
			{
				Gui.Frame(Drawer(), seconds);
			}
		}

		// Presses and releases a key (with modifiers) in a frame that comes seconds after the previous one.
		void PressAfter(ImGuiKeyChord chord, float seconds)
		{
			ImGuiIO& io = ImGui::GetIO();
			ImGuiKey const key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
			if ((chord & ImGuiMod_Alt) != 0)
			{
				io.AddKeyEvent(ImGuiMod_Alt, true);
			}
			io.AddKeyEvent(key, true);
			Gui.Frame(Drawer(), seconds);
			io.AddKeyEvent(key, false);
			if ((chord & ImGuiMod_Alt) != 0)
			{
				io.AddKeyEvent(ImGuiMod_Alt, false);
			}
			Gui.Frame(Drawer(), 1.0f / 60.0f);
		}

		void Click(glm::vec2 point, ImGuiMouseButton button = ImGuiMouseButton_Left) { Gui.MouseClick(ToImVec2(point), button, Drawer()); }

		void MoveMouse(glm::vec2 point)
		{
			ImGui::GetIO().AddMousePosEvent(point.x, point.y);
			Gui.Frame(Drawer());
		}

		void SetButton(ImGuiMouseButton button, bool down)
		{
			ImGui::GetIO().AddMouseButtonEvent(button, down);
			Gui.Frame(Drawer());
		}

		VisualScriptEditorModel const& Model() const { return Panel->GetModel(); }

		std::string ReadFile() const
		{
			Result<std::string> text = FileSystem::ReadTextFile(File);
			REQUIRE(text.IsOk());
			return text.TakeValue();
		}

		uint64_t GetVersion() const { return AssetManager::GetAsset<VisualScriptAsset>(Graph)->GetVersion(); }
		VisualScriptDocument const& GetDocument() const { return AssetManager::GetAsset<VisualScriptAsset>(Graph)->GetDocument(); }
		size_t GetUndoCount() const { return Context.GetHistory().GetUndoCount(); }

		VisualScriptNode const* FindNode(uint32_t id) const
		{
			for (VisualScriptNode const& node : GetDocument().Graph.Nodes)
			{
				if (node.ID == id)
				{
					return &node;
				}
			}
			return nullptr;
		}

		bool HasLink(VisualScriptLink const& link) const
		{
			std::vector<VisualScriptLink> const& links = GetDocument().Graph.Links;
			return std::find(links.begin(), links.end(), link) != links.end();
		}

		// The panel invariant (spec §15.4): three frames of drawing change no undo count, no file byte and no asset version.
		void CheckDrawingChangesNothing()
		{
			size_t const undo = GetUndoCount();
			std::string const bytes = ReadFile();
			uint64_t const version = GetVersion();
			Frames(3);
			CHECK(GetUndoCount() == undo);
			CHECK(ReadFile() == bytes);
			CHECK(GetVersion() == version);
			CHECK(Open);
		}

		ImGuiWindow* GetWindow() const { return ImGui::FindWindowByName(VisualScriptEditorPanel::GetWindowName(Graph, "Player").c_str()); }

		ImGuiWindow* GetChild(char const* name) const
		{
			ImGuiWindow const* parent = GetWindow();
			for (ImGuiWindow* window : GImGui->Windows)
			{
				if (window->ParentWindow == parent && std::strstr(window->Name, name) != nullptr)
				{
					return window;
				}
			}
			FAIL("the graph window has no child ", name);
			return nullptr;
		}

		ImGuiID GetSurface() const
		{
			ImGuiWindow const* canvas = GetChild("##canvas");
			return ImHashStr("##surface", 0, canvas->ID);
		}

		// A point of the canvas with nothing on it: near its bottom-left corner (the minimap is at the bottom-right).
		glm::vec2 EmptyPoint() const
		{
			ImGuiWindow const* canvas = GetChild("##canvas");
			return glm::vec2(canvas->Pos.x + 30.0f, canvas->Pos.y + canvas->Size.y - 30.0f);
		}

		// Moves the mouse down a popup's items a frame at a time until the item is hovered; the point found.
		std::optional<glm::vec2> FindPopupItem(char const* label)
		{
			if (GImGui->OpenPopupStack.Size == 0)
			{
				return std::nullopt;
			}
			ImGuiWindow const* popup = GImGui->OpenPopupStack.back().Window;
			REQUIRE(popup != nullptr);
			ImGuiID const item = ImHashStr(label, 0, popup->ID);
			for (float y = popup->Pos.y + 2.0f; y < popup->Pos.y + popup->Size.y; y += 3.0f)
			{
				glm::vec2 const point(popup->Pos.x + popup->Size.x * 0.5f, y);
				MoveMouse(point);
				if (GImGui->HoveredId == item)
				{
					return point;
				}
			}
			return std::nullopt;
		}

		// Draws a window of another graph in place of the fixture's.
		void Reopen(AssetHandle graph)
		{
			Panel.reset();
			Panel.emplace(graph, Operations, *UsedSemantics, Settings, Session, Services);
			Frames(3);
		}

		// The UI scale (ImGuiStyle::FontScaleDpi), with the style's sizes scaled as ImGuiLayer::OnAttach scales them for a
		// monitor's content scale. From scale 1 only.
		void SetUIScale(float scale)
		{
			ImGuiStyle& style = ImGui::GetStyle();
			REQUIRE(style.FontScaleDpi == 1.0f);
			style.ScaleAllSizes(scale);
			style.FontScaleDpi = scale;
			Frames(2);
		}

		// Where the last frame drew an item of the window; fails the test when it drew none.
		VisualScriptRect ItemRect(VisualScriptPanelItem item) const
		{
			std::optional<VisualScriptRect> const rect = Panel->GetItemRect(item);
			REQUIRE_MESSAGE(rect.has_value(), "item ", static_cast<int>(item), " was not drawn");
			return *rect;
		}

		// Clicks the middle of an item of the window.
		void ClickItem(VisualScriptPanelItem item)
		{
			VisualScriptRect const rect = ItemRect(item);
			Click(rect.GetCenter());
		}

		HeadlessImGui Gui;
		AssetManagerScope Assets;
		TemporaryDirectory Temporary;
		EditorContext Context;
		EditorOperations Operations{Context};
		AssetHandle Graph;
		std::filesystem::path File;
		VisualScriptViewSettings Settings{Temporary.GetPath() / "Views.json"};
		FixtureVisualScriptSemantics Semantics;
		// The semantics the window uses: Semantics, or the test's.
		VisualScriptSemantics* UsedSemantics = nullptr;
		VisualScriptEditorServices Services;
		EditSession Session{SessionKey};
		std::optional<VisualScriptEditorPanel> Panel;
		// Drawn before the window every frame (the editor's global shortcuts in the probe tests).
		std::function<void()> Probe;
		ImGuiID PreviousActive = 0;
		bool Open = true;
		ImVec2 WindowSize{1600.0f, 900.0f};
	};
}
