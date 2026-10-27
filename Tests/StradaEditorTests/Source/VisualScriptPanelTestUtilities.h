#pragma once

#include "HeadlessImGui.h"
#include "TestUtilities.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/VisualScripting/VisualScriptBrushes.h"
#include "Editor/VisualScripting/VisualScriptCanvasRenderer.h"
#include "Editor/VisualScripting/VisualScriptEditorModel.h"
#include "Editor/VisualScripting/VisualScriptThemeStyle.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <imgui.h>

#include <functional>
#include <optional>
#include <vector>

// Helpers of the tests of the visual script editor's canvas UI: the canvas drawn headless, frame by frame, as its panel draws it.
namespace Strada::Testing
{
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
}
