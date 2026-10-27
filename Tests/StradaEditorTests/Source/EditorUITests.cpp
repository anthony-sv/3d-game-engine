#include "HeadlessImGui.h"
#include "TestUtilities.h"

#include "Editor/Commands/AssetCommands.h"
#include "Editor/ComponentInspection.h"
#include "Editor/EditorOperations.h"
#include "Editor/EntityPresets.h"
#include "Editor/HierarchyEditing.h"
#include "Editor/UI/EditorUI.h"
#include "Editor/UI/FieldEditor.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/Buffer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Math/Math.h"
#include "Strada/Scene/Entity.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	UUID Create(EditorOperations& operations, std::string name, UUID parent = UUID::Invalid(), Json components = Json::object())
	{
		EntityCreateInfo info;
		info.Name = std::move(name);
		info.Parent = parent;
		info.Components = std::move(components);
		Result<UUID> entity = operations.CreateEntity(info);
		REQUIRE(entity.IsOk());
		return entity.GetValue();
	}

	std::vector<std::string_view> Names(std::vector<ComponentInfo const*> const& components)
	{
		std::vector<std::string_view> names;
		for (ComponentInfo const* component : components)
		{
			names.push_back(component->Name);
		}
		return names;
	}

	// A project open in an editor model, whose asset files the steps under test edit.
	struct AssetStepFixture
	{
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};

		AssetStepFixture() { REQUIRE(Operations.CreateProject(Directory.GetPath() / "Game", "Game", Scene("Main")).IsOk()); }

		std::filesystem::path GetFile(std::string_view path) const
		{
			return AssetManager::GetAssetDirectory() / FileSystem::PathFromUtf8(path);
		}

		std::string ReadFile(std::string_view path) const
		{
			Result<Buffer> const bytes = FileSystem::ReadBinaryFile(GetFile(path));
			REQUIRE(bytes.IsOk());
			return std::string(bytes.GetValue().As<char>(), static_cast<size_t>(bytes.GetValue().GetSize()));
		}

		// Writes a file into the asset directory and registers it: the asset loads it at its first use.
		AssetHandle AddFile(std::string_view path, std::string_view bytes)
		{
			REQUIRE(FileSystem::WriteTextFile(GetFile(path), bytes).IsOk());
			REQUIRE(Operations.RefreshAssets().IsOk());
			AssetHandle const handle = AssetManager::FindByPath(path);
			REQUIRE(handle.IsValid());
			return handle;
		}

		AssetHandle CreateGraph(std::string_view path)
		{
			Result<AssetHandle> const created = Operations.CreateVisualScript(path);
			REQUIRE(created.IsOk());
			return created.GetValue();
		}

		Ref<VisualScriptAsset> GetGraph(AssetHandle graph) const
		{
			Ref<VisualScriptAsset> asset = AssetManager::GetAsset<VisualScriptAsset>(graph);
			REQUIRE(asset != nullptr);
			return asset;
		}

		// Commits a document as the graph windows do; returns the object the asset takes.
		Ref<VisualScriptDocument const> Edit(AssetHandle graph, VisualScriptDocument document, std::string description = "Edit Graph",
		                                     uint64_t mergeKey = 0)
		{
			Ref<VisualScriptDocument const> shared = CreateRef<VisualScriptDocument const>(std::move(document));
			Result<void> const edited = Operations.EditVisualScript(graph, shared, std::move(description), mergeKey);
			REQUIRE_MESSAGE(edited.IsOk(), (edited ? std::string() : edited.GetError()));
			return shared;
		}

		// Rescans the asset directory: the loaded assets whose files changed since they loaded or the editor wrote them.
		std::vector<AssetHandle> Refresh()
		{
			Result<AssetRefreshResult> refreshed = Operations.RefreshAssets();
			REQUIRE(refreshed.IsOk());
			return refreshed.GetValue().Modified;
		}
	};

	// An update event that runs a sequence of three outputs.
	VisualScriptDocument MakeGraphDocument()
	{
		VisualScriptDocument document;
		VisualScriptVariable speed;
		speed.ID = 1;
		speed.Name = "Speed";
		speed.Type = "int";
		speed.Value = 1;
		speed.Public = true;
		document.Variables.push_back(speed);
		VisualScriptNode update;
		update.ID = 10;
		update.Type = "Event.Update";
		update.Position = glm::vec2(0.0f, 0.0f);
		document.Graph.Nodes.push_back(update);
		VisualScriptNode sequence;
		sequence.ID = 11;
		sequence.Type = "Flow.Sequence";
		sequence.Position = glm::vec2(320.0f, 0.0f);
		sequence.Settings = Json::object({{"Count", 3}});
		document.Graph.Nodes.push_back(sequence);
		document.Graph.Links.push_back({{10, "Then"}, {11, "Execute"}});
		return document;
	}

	// The document with its first node moved to x.
	VisualScriptDocument Moved(VisualScriptDocument document, float x)
	{
		document.Graph.Nodes.front().Position = glm::vec2(x, 0.0f);
		return document;
	}

	VisualScriptDocument Described(VisualScriptDocument document, std::string description)
	{
		document.Description = std::move(description);
		return document;
	}

	std::string Canonical(VisualScriptDocument const& document)
	{
		return DumpJson(VisualScriptSerializer::Serialize(document));
	}

	std::string ReadVersion1Example()
	{
		Result<Buffer> const bytes =
			FileSystem::ReadBinaryFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Version1" / "Example.sgraph");
		REQUIRE(bytes.IsOk());
		return std::string(bytes.GetValue().As<char>(), static_cast<size_t>(bytes.GetValue().GetSize()));
	}

	// A step for the history's own tests: whether it changes the scene, the asset whose file it edits, the memory it keeps
	// and its merge key.
	struct FakeStepInfo
	{
		std::string Name;
		bool ModifiesScene = true;
		AssetHandle Asset = AssetHandle();
		size_t Memory = 0;
		uint64_t MergeKey = 0;
	};

	// Logs what the history does with it ("do", "undo", "forget" and its name). A merge takes the next step's memory.
	class FakeStep final : public EditorCommand
	{
	public:
		FakeStep(FakeStepInfo info, std::vector<std::string>& log)
			: m_Info(std::move(info)),
			  m_Log(log)
		{
		}

		Result<void> Execute(EditorContext&) override
		{
			m_Log.push_back("do " + m_Info.Name);
			return {};
		}

		Result<void> Undo(EditorContext&) override
		{
			m_Log.push_back("undo " + m_Info.Name);
			return {};
		}

		std::string GetDescription() const override { return m_Info.Name; }
		bool ModifiesScene() const override { return m_Info.ModifiesScene; }

		bool CanMergeWith(EditorCommand const& next) const override
		{
			auto const* other = dynamic_cast<FakeStep const*>(&next);
			return m_Info.MergeKey != 0 && other != nullptr && other->m_Info.MergeKey == m_Info.MergeKey;
		}

		void MergeWith(EditorCommand& next) override { m_Info.Memory = static_cast<FakeStep&>(next).m_Info.Memory; }
		size_t GetMemoryUse() const override { return m_Info.Memory; }
		AssetHandle GetEditedAsset() const override { return m_Info.Asset; }

		void ForgetEditedAsset() override
		{
			m_Info.Asset = AssetHandle();
			m_Log.push_back("forget " + m_Info.Name);
		}

	private:
		FakeStepInfo m_Info;
		std::vector<std::string>& m_Log;
	};

	void Record(CommandHistory& history, EditorContext& context, std::vector<std::string>& log, FakeStepInfo info)
	{
		REQUIRE(history.Execute(CreateScope<FakeStep>(std::move(info), log), context).IsOk());
	}

	void UndoAll(CommandHistory& history, EditorContext& context)
	{
		while (history.CanUndo())
		{
			REQUIRE(history.Undo(context).IsOk());
		}
	}
}

TEST_CASE("UI: display names split PascalCase words and keep acronyms together")
{
	CHECK(UI::FormatDisplayName("PerspectiveFOV") == "Perspective FOV");
	CHECK(UI::FormatDisplayName("UVTiling") == "UV Tiling");
	CHECK(UI::FormatDisplayName("EV100") == "EV100");
	CHECK(UI::FormatDisplayName("FXAA") == "FXAA");
	CHECK(UI::FormatDisplayName("AmbientOcclusionRadius") == "Ambient Occlusion Radius");
	CHECK(UI::FormatDisplayName("PBRNeutral") == "PBR Neutral");
	CHECK(UI::FormatDisplayName("VSync") == "VSync");
	CHECK(UI::FormatDisplayName("FixedVSyncRate") == "Fixed VSync Rate");
	CHECK(UI::FormatDisplayName("Layer2Mask") == "Layer2 Mask");
	CHECK(UI::FormatDisplayName("Tag") == "Tag");
	// Script fields: private members drop their prefix.
	CHECK(UI::FormatDisplayName("m_JumpHeight") == "Jump Height");
	CHECK(UI::FormatDisplayName("s_Count") == "Count");
	CHECK(UI::FormatDisplayName("m_") == "m_");
	CHECK(UI::FormatDisplayName("m_lower") == "m_lower");
	CHECK(UI::FormatDisplayName("").empty());
}

TEST_CASE("UI: arrows in text are drawn as shapes, which the editor's fonts have no glyphs for, and measured with the text")
{
	Testing::HeadlessImGui imgui;
	ImFont* const monospace = imgui.UseEditorStyle();
	REQUIRE(monospace != nullptr);
	imgui.Frame(
		[&]
		{
			ImFont* const font = ImGui::GetFont();
			float const size = ImGui::GetFontSize();
			// Neither editor font has the two arrows: ImGui would draw its fallback character for them.
			for (ImFont* const editorFont : {font, monospace})
			{
				CHECK(!editorFont->IsGlyphInFont(0x2192));
				CHECK(!editorFont->IsGlyphInFont(0x2190));
			}
			auto const width = [font](float pixels, char const* text)
			{
				return font->CalcTextSizeA(pixels, FLT_MAX, 0.0f, text).x;
			};

			// Text without them is measured as ImGui measures it, the other arrows of their block and a cut sequence included.
			CHECK(UI::CalcArrowTextSize("Count (int)") == ImGui::CalcTextSize("Count (int)"));
			CHECK(UI::CalcArrowTextSize("Two\nlines\n") == ImGui::CalcTextSize("Two\nlines\n"));
			CHECK(UI::CalcArrowTextSize(font, 31.0f, "Count\nAlpha") == font->CalcTextSizeA(31.0f, FLT_MAX, 0.0f, "Count\nAlpha"));
			CHECK(UI::CalcArrowTextSize("") == ImGui::CalcTextSize(""));
			CHECK(UI::CalcArrowTextSize(std::string_view()) == ImVec2(0.0f, size));
			CHECK(UI::CalcArrowTextSize(font, 20.0f, "↑") == font->CalcTextSizeA(20.0f, FLT_MAX, 0.0f, "↑"));
			CHECK(UI::CalcArrowTextSize(font, 20.0f, "a\xE2\x86") == font->CalcTextSizeA(20.0f, FLT_MAX, 0.0f, "a\xE2\x86"));

			// An arrow is about an em wide, either way, and adds to the text around it.
			float const arrow = UI::CalcArrowTextSize(font, 20.0f, "→").x;
			CHECK(arrow > 10.0f);
			CHECK(arrow < 24.0f);
			CHECK(UI::CalcArrowTextSize(font, 20.0f, "←").x == arrow);
			ImVec2 const pill = UI::CalcArrowTextSize(font, 20.0f, "Count → Alpha");
			CHECK(pill.x == doctest::Approx(width(20.0f, "Count ") + arrow + width(20.0f, " Alpha")));
			CHECK(pill.y == 20.0f);
			ImVec2 const lines = UI::CalcArrowTextSize(font, 20.0f, "a → b\nCondition ← Is Key Down");
			CHECK(lines.x == doctest::Approx(width(20.0f, "Condition ") + arrow + width(20.0f, " Is Key Down")));
			CHECK(lines.y == 40.0f);
			CHECK(UI::CalcArrowTextSize(font, 20.0f, "a →\n").y == 20.0f);
			// At the current font and size the width is rounded up, as ImGui::CalcTextSize rounds it.
			ImVec2 const current = UI::CalcArrowTextSize("x → y");
			CHECK(current.x == std::ceil(UI::CalcArrowTextSize(font, size, "x → y").x));
			CHECK(current.y == size);
		});

	imgui.Frame(
		[&]
		{
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(ImVec2(800.0f, 600.0f));
			ImGui::Begin("Arrows");
			ImDrawList* const drawList = ImGui::GetWindowDrawList();
			ImFont* const font = ImGui::GetFont();
			float const size = ImGui::GetFontSize();
			ImVec2 const white = drawList->_Data->TexUvWhitePixel;
			auto const drawn = [drawList](auto const& draw)
			{
				int const first = drawList->VtxBuffer.Size;
				draw();
				return std::vector<ImDrawVert>(drawList->VtxBuffer.begin() + first, drawList->VtxBuffer.end());
			};
			auto const same = [](std::vector<ImDrawVert> const& a, std::vector<ImDrawVert> const& b)
			{
				return std::equal(a.begin(), a.end(), b.begin(), b.end(),
			                      [](ImDrawVert const& x, ImDrawVert const& y)
			                      {
									  return x.pos == y.pos && x.uv == y.uv && x.col == y.col;
								  });
			};

			// Text without arrows is drawn exactly as ImGui draws it; a null font and a size of 0 are the current ones.
			ImVec2 const at(10.5f, 20.25f);
			std::vector<ImDrawVert> const hello = drawn(
				[&]
				{
					UI::DrawArrowText(drawList, at, IM_COL32_WHITE, "Hello\nWorld");
				});
			CHECK(same(hello, drawn(
								  [&]
								  {
									  drawList->AddText(at, IM_COL32_WHITE, "Hello\nWorld");
								  })));
			std::vector<ImDrawVert> const resolved = drawn(
				[&]
				{
					UI::DrawArrowText(drawList, nullptr, 0.0f, at, IM_COL32_WHITE, "x → y");
				});
			CHECK(same(resolved, drawn(
									 [&]
									 {
										 UI::DrawArrowText(drawList, font, size, at, IM_COL32_WHITE, "x → y");
									 })));

			// An arrow is shapes in the text's colour, no glyph (not the fallback either), within its advance and its line,
		    // with its head on the side it points to.
			ImU32 const orange = IM_COL32(255, 128, 0, 255);
			float const advance = UI::CalcArrowTextSize(font, size, "→").x;
			std::vector<float> widths;
			for (bool const pointsRight : {true, false})
			{
				CAPTURE(pointsRight);
				std::vector<ImDrawVert> const shapes = drawn(
					[&]
					{
						UI::DrawArrowText(drawList, ImVec2(100.0f, 100.0f), orange, pointsRight ? "→" : "←");
					});
				REQUIRE(!shapes.empty());
				ImVec2 minimum(FLT_MAX, FLT_MAX);
				ImVec2 maximum(-FLT_MAX, -FLT_MAX);
				for (ImDrawVert const& vertex : shapes)
				{
					CHECK(vertex.uv == white);
					CHECK((vertex.col & ~IM_COL32_A_MASK) == (orange & ~IM_COL32_A_MASK));
					CHECK(((vertex.col & IM_COL32_A_MASK) == IM_COL32_A_MASK || (vertex.col & IM_COL32_A_MASK) == 0));
					minimum = ImMin(minimum, vertex.pos);
					maximum = ImMax(maximum, vertex.pos);
				}
				CHECK(minimum.x >= 99.0f);
				CHECK(maximum.x <= 100.0f + advance + 1.0f);
				CHECK(minimum.y >= 99.0f);
				CHECK(maximum.y <= 100.0f + size + 1.0f);
				widths.push_back(maximum.x - minimum.x);
				// The tip is the one opaque point on the arrow's middle row: the stem's corners lie on whole rows of pixels
			    // above and below it, and the anti-aliased fringe is transparent.
				float const middle = (minimum.y + maximum.y) * 0.5f;
				std::vector<float> tips;
				for (ImDrawVert const& vertex : shapes)
				{
					if ((vertex.col & IM_COL32_A_MASK) == IM_COL32_A_MASK && std::abs(vertex.pos.y - middle) < 0.25f)
					{
						tips.push_back(vertex.pos.x);
					}
				}
				REQUIRE(tips.size() == 1);
				CHECK((tips[0] > (minimum.x + maximum.x) * 0.5f) == pointsRight);
			}
			CHECK(widths[0] == doctest::Approx(widths[1]).epsilon(0.01));

			// The text around an arrow is drawn where it is measured.
			std::vector<ImDrawVert> const pill = drawn(
				[&]
				{
					UI::DrawArrowText(drawList, ImVec2(100.0f, 200.0f), IM_COL32_WHITE, "A → B");
				});
			float const arrowStart = 100.0f + font->CalcTextSizeA(size, FLT_MAX, 0.0f, "A ").x;
			std::vector<ImDrawVert> glyphs;
			for (ImDrawVert const& vertex : pill)
			{
				if (vertex.uv == white)
				{
					CHECK(vertex.pos.x >= arrowStart - 1.0f);
					CHECK(vertex.pos.x <= arrowStart + advance + 1.0f);
				}
				else
				{
					glyphs.push_back(vertex);
				}
			}
			// Two glyphs of four vertices each: "A" before the arrow, "B" after it.
			REQUIRE(glyphs.size() == 8);
			for (size_t index = 0; index < 4; index++)
			{
				CHECK(glyphs[index].pos.x < arrowStart);
				CHECK(glyphs[index + 4].pos.x > arrowStart + advance);
			}

			// Lines break as ImGui's do: the arrow of the second line is drawn on it.
			std::vector<ImDrawVert> const twoLines = drawn(
				[&]
				{
					UI::DrawArrowText(drawList, ImVec2(100.0f, 300.0f), IM_COL32_WHITE, "a\n→");
				});
			for (ImDrawVert const& vertex : twoLines)
			{
				if (vertex.uv == white)
				{
					CHECK(vertex.pos.y >= 300.0f + size - 1.0f);
					CHECK(vertex.pos.y <= 300.0f + 2.0f * size + 1.0f);
				}
			}

			// An arrow grows with the size it is drawn at; a transparent colour draws nothing.
			auto const extent = [&](float pixels)
			{
				std::vector<ImDrawVert> const shapes = drawn(
					[&]
					{
						UI::DrawArrowText(drawList, font, pixels, ImVec2(100.0f, 400.0f), IM_COL32_WHITE, "→");
					});
				float left = FLT_MAX;
				float right = -FLT_MAX;
				for (ImDrawVert const& vertex : shapes)
				{
					left = std::min(left, vertex.pos.x);
					right = std::max(right, vertex.pos.x);
				}
				return right - left;
			};
			CHECK(extent(2.0f * size) == doctest::Approx(2.0f * extent(size)).epsilon(0.1));
			std::vector<ImDrawVert> const transparent = drawn(
				[&]
				{
					UI::DrawArrowText(drawList, ImVec2(100.0f, 500.0f), IM_COL32(255, 255, 255, 0), "A → B");
				});
			CHECK(transparent.empty());
			ImGui::End();
		});
}

TEST_CASE("EditSession: one interaction is one undo step")
{
	EditorContext context;
	EditorOperations operations(context);
	UUID const entity = Create(operations, "Entity");
	size_t const steps = context.GetHistory().GetUndoCount();
	EditSession session(0x5445535400000000ull);

	auto const setX = [&](float x)
	{
		Json const patch = Json::object({{"Translation", {x, 0, 0}}});
		REQUIRE(operations.SetComponentFields(entity, "Transform", patch, session.GetMergeKey()).IsOk());
	};

	// A drag: several changes while an item stays active merge.
	session.Update(true, context.GetHistory());
	setX(1.0f);
	session.Update(true, context.GetHistory());
	setX(2.0f);
	CHECK(context.GetHistory().GetUndoCount() == steps + 1);

	// Releasing ends the interaction: the next change is a new step even though it reuses no state.
	session.Update(false, context.GetHistory());
	session.Update(true, context.GetHistory());
	setX(3.0f);
	CHECK(context.GetHistory().GetUndoCount() == steps + 2);

	uint64_t const key = session.GetMergeKey();
	CHECK(session.GetMergeKey() == key);
	session.Update(false, context.GetHistory());
	CHECK(session.GetMergeKey() != key);
}

TEST_CASE("FieldChange: patches apply the changed component to each object's own value")
{
	FieldChange change;
	change.Path = {"Translation"};
	change.Value = Json::array({5.0, 6.0, 7.0});
	change.Component = 1;
	Json const values = Json::object({{"Translation", {1.0, 2.0, 3.0}}, {"Scale", {1, 1, 1}}});
	CHECK(change.MakePatch(values) == Json::object({{"Translation", {1.0, 6.0, 3.0}}}));

	// Whole-value changes replace the value.
	change.Component.reset();
	CHECK(change.MakePatch(values) == Json::object({{"Translation", {5.0, 6.0, 7.0}}}));

	// Euler edits keep each object's other angles.
	FieldChange euler;
	euler.Path = {"Rotation"};
	euler.Component = 0;
	euler.EulerDegrees = glm::vec3(30.0f, 0.0f, 0.0f);
	glm::quat const yawed = Math::EulerDegreesToQuaternion({0.0f, 45.0f, 0.0f});
	Json const rotated = Json::object({{"Rotation", {yawed.x, yawed.y, yawed.z, yawed.w}}});
	Json const patch = euler.MakePatch(rotated);
	glm::quat const result = glm::quat::wxyz(patch["Rotation"][3].get<float>(), patch["Rotation"][0].get<float>(),
	                                         patch["Rotation"][1].get<float>(), patch["Rotation"][2].get<float>());
	glm::vec3 const degrees = Math::QuaternionToEulerDegrees(result);
	CHECK(degrees.x == doctest::Approx(30.0f).epsilon(1e-3));
	CHECK(degrees.y == doctest::Approx(45.0f).epsilon(1e-3));
	CHECK(degrees.z == doctest::Approx(0.0f).epsilon(1e-3));

	// Fields of nested structs patch only the nested field.
	FieldChange nested;
	nested.Path = {"Renderer", "Bloom"};
	nested.Value = false;
	Json const settings = Json::object({{"Renderer", {{"Bloom", true}, {"EV100", 1.0}}}});
	CHECK(nested.MakePatch(settings) == Json::object({{"Renderer", {{"Bloom", false}, {"EV100", 1.0}}}}));

	CHECK(FieldChange().MakePatch(values) == Json::object());
}

TEST_CASE("EntityPresets: every preset creates its entity, placed at the spawn point or under a parent")
{
	EditorContext context;
	EditorOperations operations(context);
	// Labels are unique across the menus ("3D Text" in 3D Object, "Text" in UI).
	std::set<std::string_view> labels;
	glm::vec3 const spawn(1.0f, 2.0f, 3.0f);
	for (EntityPreset const& preset : EntityPresets::GetAll())
	{
		CAPTURE(preset.Label);
		CHECK(labels.insert(preset.Label).second);
		Result<UUID> created = EntityPresets::Create(operations, preset, UUID::Invalid(), spawn);
		REQUIRE(created.IsOk());
		Entity const entity = context.GetScene().GetEntityByUUID(created.GetValue());
		CHECK(entity.GetName() == preset.EntityName);
		if (preset.NeedsCanvas)
		{
			// UI elements go into the scene's canvas (the Canvas preset comes first).
			CHECK(context.GetScene().GetParent(entity).HasComponent<UICanvasComponent>());
		}
		else
		{
			CHECK(entity.GetComponent<TransformComponent>().Translation == spawn);
		}
	}
	// Categories are contiguous, so menus group them.
	std::span<EntityPreset const> const presets = EntityPresets::GetAll();
	std::set<std::string_view> finished;
	for (size_t i = 0; i < presets.size(); i++)
	{
		if (i > 0 && presets[i].Category != presets[i - 1].Category)
		{
			CHECK(finished.insert(presets[i - 1].Category).second);
		}
		CHECK_FALSE(finished.contains(presets[i].Category));
	}

	auto const find = [&](std::string_view label)
	{
		auto const it = std::find_if(presets.begin(), presets.end(),
		                             [label](EntityPreset const& preset)
		                             {
										 return preset.Label == label;
									 });
		REQUIRE(it != presets.end());
		return *it;
	};
	Result<UUID> const cube = EntityPresets::Create(operations, find("Cube"), UUID::Invalid(), spawn);
	REQUIRE(cube.IsOk());
	Entity const cubeEntity = context.GetScene().GetEntityByUUID(cube.GetValue());
	CHECK(cubeEntity.GetComponent<MeshComponent>().Mesh == GetBuiltInHandle(BuiltInAsset::CubeMesh));

	// A character draws the capsule mesh its controller's default capsule matches.
	Result<UUID> const character = EntityPresets::Create(operations, find("Character Controller"), UUID::Invalid(), spawn);
	REQUIRE(character.IsOk());
	Entity const characterEntity = context.GetScene().GetEntityByUUID(character.GetValue());
	CHECK(characterEntity.GetComponent<MeshComponent>().Mesh == GetBuiltInHandle(BuiltInAsset::CapsuleMesh));
	CharacterControllerComponent const& controller = characterEntity.GetComponent<CharacterControllerComponent>();
	CHECK(controller.Radius == 0.5f);
	CHECK(controller.Height == 2.0f);

	// Children start at their parent's origin; lights pointing down keep their rotation.
	Result<UUID> const spot = EntityPresets::Create(operations, find("Spot Light"), cube.GetValue(), spawn);
	REQUIRE(spot.IsOk());
	Entity const spotEntity = context.GetScene().GetEntityByUUID(spot.GetValue());
	CHECK(context.GetScene().GetParent(spotEntity) == cubeEntity);
	CHECK(spotEntity.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f));
	glm::vec3 const forward = spotEntity.GetComponent<TransformComponent>().Rotation * glm::vec3(0.0f, 0.0f, -1.0f);
	CHECK(forward.y == doctest::Approx(-1.0f).epsilon(1e-4));

	// Post-process volumes start global, with a 10 m box for when they are made local.
	Result<UUID> const volume = EntityPresets::Create(operations, find("Post Process Volume"), UUID::Invalid(), spawn);
	REQUIRE(volume.IsOk());
	Entity const volumeEntity = context.GetScene().GetEntityByUUID(volume.GetValue());
	REQUIRE(volumeEntity.HasComponent<PostProcessComponent>());
	CHECK(volumeEntity.GetComponent<PostProcessComponent>().Global);
	CHECK(volumeEntity.GetComponent<TransformComponent>().Scale == glm::vec3(10.0f));
}

TEST_CASE("EntityPresets: the create menus offer Core, 3D Object, 2D Object, Rendering, Effects, Physics, Audio and UI")
{
	using Labels = std::vector<std::string_view>;
	std::span<std::string_view const> const categories = EntityPresets::GetCategories();
	CHECK(std::vector<std::string_view>(categories.begin(), categories.end()) ==
	      Labels{"Core", "3D Object", "2D Object", "Rendering", "Effects", "Physics", "Audio", "UI"});

	std::vector<EntityPresetMenuSection> const menu = EntityPresets::GetMenu();
	REQUIRE(menu.size() == categories.size());
	auto const labels = [](EntityPresetMenuSection const& section)
	{
		Labels result;
		for (EntityPreset const* preset : section.Presets)
		{
			result.push_back(preset->Label);
		}
		return result;
	};
	// Core comes first, directly in the menu; every other category is a submenu.
	CHECK(menu[0].Category == EntityPresets::CoreCategory);
	CHECK_FALSE(menu[0].Submenu);
	CHECK(labels(menu[0]) == Labels{"Create Empty"});
	CHECK(labels(menu[1]) == Labels{"Cube", "Sphere", "Plane", "Cylinder", "Capsule", "Cone", "Quad", "3D Text"});
	CHECK(labels(menu[2]) == Labels{"Sprite"});
	// Post-process volumes are rendering settings, next to the lights; the character is a physics object.
	CHECK(labels(menu[3]) == Labels{"Camera", "Directional Light", "Point Light", "Spot Light", "Sky Light", "Post Process Volume"});
	CHECK(labels(menu[4]) == Labels{"Particle System", "Fire", "Smoke", "Sparks"});
	CHECK(labels(menu[5]) == Labels{"Character Controller"});
	CHECK(labels(menu[6]) == Labels{"Audio Source", "Audio Listener"});
	CHECK(labels(menu[7]) == Labels{"Canvas", "Panel", "Image", "Text", "Button"});
	// Every preset is in the menus once, in the section of its category.
	size_t count = 0;
	for (size_t i = 0; i < menu.size(); i++)
	{
		CAPTURE(menu[i].Category);
		CHECK(menu[i].Category == categories[i]);
		CHECK(menu[i].Submenu == (i != 0));
		for (EntityPreset const* preset : menu[i].Presets)
		{
			CHECK(preset->Category == menu[i].Category);
		}
		count += menu[i].Presets.size();
	}
	CHECK(count == EntityPresets::GetAll().size());

	// What the new entries create.
	EditorContext context;
	EditorOperations operations(context);
	Result<UUID> const empty = EntityPresets::Create(operations, *menu[0].Presets[0], UUID::Invalid(), glm::vec3(0.0f));
	REQUIRE(empty.IsOk());
	Entity const emptyEntity = context.GetScene().GetEntityByUUID(empty.GetValue());
	CHECK(emptyEntity.GetName() == "Entity");
	CHECK_FALSE(emptyEntity.HasComponent<MeshComponent>());
	Result<UUID> const listener = EntityPresets::Create(operations, *menu[6].Presets[1], UUID::Invalid(), glm::vec3(0.0f));
	REQUIRE(listener.IsOk());
	Entity const listenerEntity = context.GetScene().GetEntityByUUID(listener.GetValue());
	CHECK(listenerEntity.GetName() == "Audio Listener");
	CHECK(listenerEntity.HasComponent<AudioListenerComponent>());
	CHECK(listenerEntity.GetComponent<AudioListenerComponent>().Active);
}

TEST_CASE("HierarchyEditing: drops resolve to a parent and an anchor, never into an own subtree")
{
	EditorContext context;
	EditorOperations operations(context);
	UUID const a = Create(operations, "A");
	UUID const b = Create(operations, "B");
	UUID const c = Create(operations, "C");
	UUID const child = Create(operations, "Child", a);
	Scene& scene = context.GetScene();

	std::vector<UUID> const draggedC = {c};
	CHECK(HierarchyEditing::ResolveDrop(scene, draggedC, a, DropPosition::Before) == MoveTarget{UUID::Invalid(), a});
	CHECK(HierarchyEditing::ResolveDrop(scene, draggedC, a, DropPosition::After) == MoveTarget{UUID::Invalid(), b});
	CHECK(HierarchyEditing::ResolveDrop(scene, draggedC, a, DropPosition::Inside) == MoveTarget{a, UUID::Invalid()});
	CHECK(HierarchyEditing::ResolveDrop(scene, draggedC, child, DropPosition::After) == MoveTarget{a, UUID::Invalid()});
	// The anchor skips dragged entities.
	std::vector<UUID> const draggedBC = {b, c};
	CHECK(HierarchyEditing::ResolveDrop(scene, draggedBC, a, DropPosition::After) == MoveTarget{UUID::Invalid(), UUID::Invalid()});

	std::vector<UUID> const draggedA = {a};
	CHECK_FALSE(HierarchyEditing::ResolveDrop(scene, draggedA, a, DropPosition::Inside));
	CHECK_FALSE(HierarchyEditing::ResolveDrop(scene, draggedA, child, DropPosition::Inside));
	CHECK_FALSE(HierarchyEditing::ResolveDrop(scene, draggedA, child, DropPosition::Before));
	CHECK_FALSE(HierarchyEditing::ResolveDrop(scene, draggedA, UUID(999), DropPosition::Inside));
	std::vector<UUID> const unknown = {UUID(999)};
	CHECK_FALSE(HierarchyEditing::ResolveDrop(scene, unknown, a, DropPosition::Inside));
	CHECK_FALSE(HierarchyEditing::ResolveDrop(scene, {}, a, DropPosition::Inside));

	// Applying a resolved drop moves the entities there.
	std::optional<MoveTarget> const move = HierarchyEditing::ResolveDrop(scene, draggedC, a, DropPosition::Before);
	REQUIRE(move);
	REQUIRE(operations.MoveEntities(draggedC, move->Parent, move->InsertBefore).IsOk());
	CHECK(scene.GetRootEntityIDs() == std::vector<UUID>{c, a, b});
}

TEST_CASE("HierarchyEditing: ranges, name search and ancestors")
{
	std::vector<UUID> const displayed = {UUID(1), UUID(2), UUID(3), UUID(4)};
	CHECK(HierarchyEditing::GetRange(displayed, UUID(2), UUID(4)) == std::vector<UUID>{UUID(2), UUID(3), UUID(4)});
	CHECK(HierarchyEditing::GetRange(displayed, UUID(3), UUID(1)) == std::vector<UUID>{UUID(1), UUID(2), UUID(3)});
	CHECK(HierarchyEditing::GetRange(displayed, UUID(9), UUID(2)) == std::vector<UUID>{UUID(2)});

	EditorContext context;
	EditorOperations operations(context);
	UUID const level = Create(operations, "Level");
	UUID const lamp = Create(operations, "Desk Lamp", level);
	UUID const bulb = Create(operations, "Bulb", lamp);
	UUID const other = Create(operations, "Floor LAMP");
	Scene& scene = context.GetScene();
	CHECK(HierarchyEditing::FindByName(scene, "lamp") == std::vector<UUID>{lamp, other});
	CHECK(HierarchyEditing::FindByName(scene, "nothing").empty());
	CHECK(HierarchyEditing::GetAncestors(scene, bulb) == std::vector<UUID>{level, lamp});
	CHECK(HierarchyEditing::GetAncestors(scene, level).empty());
	CHECK(HierarchyEditing::GetAncestors(scene, UUID(12345)).empty());
}

TEST_CASE("ComponentInspection: shared and addable components, mixed fields and defaults")
{
	EditorContext context;
	EditorOperations operations(context);
	UUID const a = Create(operations, "A", UUID::Invalid(), Json::object({{"PointLight", {{"Range", 5.0}}}, {"Camera", Json::object()}}));
	UUID const b = Create(operations, "B", UUID::Invalid(), Json::object({{"PointLight", {{"Range", 8.0}}}}));
	Scene& scene = context.GetScene();
	std::vector<UUID> const both = {a, b};

	CHECK(Names(ComponentInspection::GetCommonComponents(scene, both)) == std::vector<std::string_view>{"Transform", "PointLight"});
	std::vector<UUID> const onlyA = {a};
	CHECK(Names(ComponentInspection::GetCommonComponents(scene, onlyA)) ==
	      std::vector<std::string_view>{"Transform", "Camera", "PointLight"});
	CHECK(ComponentInspection::GetCommonComponents(scene, {}).empty());

	std::vector<std::string_view> const addable = Names(ComponentInspection::GetAddableComponents(scene, both));
	// Camera is missing on B; PointLight is on both; core and internal components never appear.
	CHECK(std::find(addable.begin(), addable.end(), "Camera") != addable.end());
	CHECK(std::find(addable.begin(), addable.end(), "PointLight") == addable.end());
	CHECK(std::find(addable.begin(), addable.end(), "Transform") == addable.end());
	CHECK(std::find(addable.begin(), addable.end(), "Prefab") == addable.end());
	CHECK(ComponentInspection::GetEntitiesWithout(scene, both, ComponentRegistry::Get<CameraComponent>()) == std::vector<UUID>{b});

	ComponentInfo const& light = ComponentRegistry::Get<PointLightComponent>();
	std::vector<Json> const values = {light.Serialize(scene.GetRegistry(), scene.GetEntityByUUID(a).GetHandle()),
	                                  light.Serialize(scene.GetRegistry(), scene.GetEntityByUUID(b).GetHandle())};
	CHECK(ComponentInspection::FindMixedFields(values) == std::vector<std::string>{"Range"});
	CHECK(ComponentInspection::FindMixedFields(std::span<Json const>(values).first(1)).empty());

	Json const defaults = ComponentInspection::GetDefaultValues(light);
	CHECK(defaults["Range"] == 10.0f);
	CHECK(defaults.size() == light.Fields.size());
}

TEST_CASE("ComponentInspection: the Add Component menu groups components by category and finds them by name")
{
	EditorContext context;
	EditorOperations operations(context);
	UUID const entity = Create(operations, "Lamp", UUID::Invalid(), Json::object({{"PointLight", Json::object()}}));
	std::vector<UUID> const selection = {entity};
	std::vector<ComponentInfo const*> const addable = ComponentInspection::GetAddableComponents(context.GetScene(), selection);

	// Categories in menu order without the empty Core one (nothing core can be added), components by display name; every
	// addable component is listed exactly once. The expected groups come from the registry, so components added later only
	// need their category.
	std::vector<ComponentInspection::ComponentGroup> const groups = ComponentInspection::GroupByCategory(addable);
	std::vector<std::pair<std::string_view, std::vector<std::string>>> expected;
	for (std::string_view const category : ComponentCategory::All)
	{
		std::vector<std::string> names;
		for (ComponentInfo const* component : addable)
		{
			if (component->Category == category)
			{
				names.push_back(UI::FormatDisplayName(component->Name));
			}
		}
		std::sort(names.begin(), names.end());
		if (!names.empty())
		{
			expected.emplace_back(category, std::move(names));
		}
	}
	REQUIRE(groups.size() == expected.size());
	for (size_t i = 0; i < groups.size(); i++)
	{
		CAPTURE(groups[i].Category);
		CHECK(groups[i].Category == expected[i].first);
		std::vector<std::string> names;
		for (ComponentInfo const* component : groups[i].Components)
		{
			names.push_back(UI::FormatDisplayName(component->Name));
		}
		CHECK(names == expected[i].second);
	}
	REQUIRE_FALSE(groups.empty());
	CHECK(groups.front().Category == ComponentCategory::Rendering);
	auto const findGroup = [&groups](std::string_view category) -> std::vector<std::string_view>
	{
		for (ComponentInspection::ComponentGroup const& group : groups)
		{
			if (group.Category == category)
			{
				return Names(group.Components);
			}
		}
		return {};
	};
	auto const contains = [](std::vector<std::string_view> const& names, std::string_view name)
	{
		return std::find(names.begin(), names.end(), name) != names.end();
	};
	// The lamp has its point light already; the others are where users look for them.
	CHECK_FALSE(contains(findGroup(ComponentCategory::Rendering), "PointLight"));
	CHECK(contains(findGroup(ComponentCategory::Rendering), "Mesh"));
	CHECK(contains(findGroup(ComponentCategory::Rendering), "Text"));
	CHECK(contains(findGroup(ComponentCategory::Physics), "BoxCollider"));
	CHECK(contains(findGroup(ComponentCategory::Scripting), "Script"));
	CHECK(contains(findGroup(ComponentCategory::UI), "UIButton"));
	CHECK(ComponentInspection::GroupByCategory({}).empty());

	// The menu searches what can be added: the lamp's point light is not offered.
	std::vector<std::string_view> const lights = Names(ComponentInspection::SearchComponents(addable, "light"));
	CHECK(contains(lights, "DirectionalLight"));
	CHECK_FALSE(contains(lights, "PointLight"));
	CHECK(ComponentInspection::SearchComponents(addable, "").size() == addable.size());

	// Names starting with the text come first, then names with a word starting with it, then the others; case, spaces
	// around the text and the spelling of the name do not matter. These searches run over a fixed set of components, so
	// components added later cannot change their matches.
	std::vector<ComponentInfo const*> searched;
	for (std::string_view const name :
	     {"AudioListener", "AudioSource", "BoxCollider", "Camera", "CapsuleCollider", "DirectionalLight", "Mesh", "MeshCollider",
	      "ParticleSystem", "RigidBody", "Script", "SkyLight", "SphereCollider", "SpotLight", "SpriteRenderer", "Text"})
	{
		ComponentInfo const* component = ComponentRegistry::Find(name);
		REQUIRE(component != nullptr);
		searched.push_back(component);
	}
	CHECK(Names(ComponentInspection::SearchComponents(searched, "light")) ==
	      std::vector<std::string_view>{"DirectionalLight", "SkyLight", "SpotLight"});
	CHECK(Names(ComponentInspection::SearchComponents(searched, "  COLL ")) ==
	      std::vector<std::string_view>{"BoxCollider", "CapsuleCollider", "MeshCollider", "SphereCollider"});
	CHECK(Names(ComponentInspection::SearchComponents(searched, "sphere col")) == std::vector<std::string_view>{"SphereCollider"});
	CHECK(Names(ComponentInspection::SearchComponents(searched, "spherecol")) == std::vector<std::string_view>{"SphereCollider"});
	CHECK(Names(ComponentInspection::SearchComponents(searched, "li")) ==
	      std::vector<std::string_view>{"AudioListener", "DirectionalLight", "SkyLight", "SpotLight", "BoxCollider", "CapsuleCollider",
	                                    "MeshCollider", "SphereCollider"});
	CHECK(Names(ComponentInspection::SearchComponents(searched, "s")) ==
	      std::vector<std::string_view>{"Script", "SkyLight", "SphereCollider", "SpotLight", "SpriteRenderer", "AudioSource",
	                                    "ParticleSystem", "AudioListener", "CapsuleCollider", "Mesh", "MeshCollider"});
	CHECK(Names(ComponentInspection::SearchComponents(searched, "urce")) == std::vector<std::string_view>{"AudioSource"});
	CHECK(ComponentInspection::SearchComponents(searched, "teleporter").empty());
	CHECK(ComponentInspection::SearchComponents(searched, "").size() == searched.size());
	CHECK(ComponentInspection::SearchComponents({}, "light").empty());
}

TEST_CASE(
	"SetVisualScriptCommand: an edit is one step of its own description that gives the asset the document itself and records its writes")
{
	AssetStepFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Player.sgraph");
	Ref<VisualScriptAsset> const asset = fixture.GetGraph(graph);
	Ref<VisualScriptDocument const> const original = asset->GetSharedDocument();
	std::string const originalBytes = fixture.ReadFile("Scripts/Player.sgraph");
	CommandHistory const& history = fixture.Context.GetHistory();
	size_t const steps = history.GetUndoCount();
	CHECK_FALSE(fixture.Context.IsDirty());

	// The file takes the canonical text, then the asset the committed object itself; the scene is not modified.
	Ref<VisualScriptDocument const> const first = fixture.Edit(graph, MakeGraphDocument(), "Add Sequence in Player");
	CHECK(history.GetUndoCount() == steps + 1);
	CHECK(history.GetUndoDescription() == "Add Sequence in Player");
	CHECK_FALSE(fixture.Context.IsDirty());
	CHECK(asset->GetSharedDocument().get() == first.get());
	CHECK(fixture.ReadFile("Scripts/Player.sgraph") == Canonical(*first));
	// The write is recorded: a refresh keeps the asset object.
	CHECK(fixture.Refresh().empty());
	CHECK(fixture.GetGraph(graph).get() == asset.get());

	// Consecutive steps share their documents: undoing the second gives the asset the first one's object back.
	Ref<VisualScriptDocument const> const second = fixture.Edit(graph, Moved(*first, 64.0f), "Move Node in Player");
	CHECK(history.GetUndoCount() == steps + 2);
	CHECK(asset->GetSharedDocument().get() == second.get());
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(asset->GetSharedDocument().get() == first.get());
	CHECK(fixture.ReadFile("Scripts/Player.sgraph") == Canonical(*first));

	// Undo gives the file its bytes back, and the asset its document; an undo then a refresh keeps the asset object.
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(asset->GetSharedDocument().get() == original.get());
	CHECK(fixture.ReadFile("Scripts/Player.sgraph") == originalBytes);
	CHECK(fixture.Refresh().empty());
	CHECK(fixture.GetGraph(graph).get() == asset.get());
	CHECK_FALSE(fixture.Context.IsDirty());

	// Redo writes again, recorded too.
	REQUIRE(fixture.Operations.Redo().IsOk());
	CHECK(asset->GetSharedDocument().get() == first.get());
	CHECK(fixture.ReadFile("Scripts/Player.sgraph") == Canonical(*first));
	CHECK(fixture.Refresh().empty());
	CHECK(fixture.GetGraph(graph).get() == asset.get());
	CHECK(history.GetRedoDescription() == "Move Node in Player");
	CHECK_FALSE(fixture.Context.IsDirty());

	// A change of the file made after the editor's write is still one.
	std::error_code error;
	std::filesystem::path const file = fixture.GetFile("Scripts/Player.sgraph");
	std::filesystem::last_write_time(file, std::filesystem::last_write_time(file, error) + std::chrono::hours(1), error);
	REQUIRE_FALSE(error);
	CHECK(fixture.Refresh() == std::vector<AssetHandle>{graph});
}

TEST_CASE(
	"SetVisualScriptCommand: the document the file has already makes no step and writes nothing, and an integer written as a float is another")
{
	AssetStepFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Player.sgraph");
	fixture.Edit(graph, MakeGraphDocument());
	Ref<VisualScriptAsset> const asset = fixture.GetGraph(graph);
	Ref<VisualScriptDocument const> const stored = asset->GetSharedDocument();
	CommandHistory const& history = fixture.Context.GetHistory();
	size_t const steps = history.GetUndoCount();
	uint64_t const version = asset->GetVersion();

	// The file's time of writing moved back, as read back on this file system: a write would change it.
	std::error_code error;
	std::filesystem::path const file = fixture.GetFile("Scripts/Player.sgraph");
	std::filesystem::last_write_time(file, std::filesystem::last_write_time(file, error) - std::chrono::hours(2), error);
	REQUIRE_FALSE(error);
	std::filesystem::file_time_type const written = std::filesystem::last_write_time(file, error);
	REQUIRE_FALSE(error);

	// The same object, an equal copy, and graph.set's path.
	CHECK(fixture.Operations.EditVisualScript(graph, stored, "Edit Graph").IsOk());
	CHECK(fixture.Operations.EditVisualScript(graph, CreateRef<VisualScriptDocument const>(*stored), "Edit Graph").IsOk());
	CHECK(fixture.Operations.SetVisualScriptDocument(graph, *stored).IsOk());
	CHECK(history.GetUndoCount() == steps);
	CHECK(asset->GetVersion() == version);
	CHECK(asset->GetSharedDocument().get() == stored.get());
	CHECK(std::filesystem::last_write_time(file, error) == written);

	// A count of 3 written as 3.0 equals the stored document by operator==, but the runtime needs an integer: it is a change.
	VisualScriptDocument floating = *stored;
	floating.Graph.Nodes[1].Settings["Count"] = 3.0;
	REQUIRE(floating == *stored);
	Ref<VisualScriptDocument const> const changed = fixture.Edit(graph, floating, "Set Count on Sequence in Player");
	CHECK(history.GetUndoCount() == steps + 1);
	CHECK(asset->GetSharedDocument().get() == changed.get());
	CHECK(fixture.ReadFile("Scripts/Player.sgraph").find("\"Count\": 3.0") != std::string::npos);
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.ReadFile("Scripts/Player.sgraph").find("\"Count\": 3\n") != std::string::npos);
}

TEST_CASE("SetVisualScriptCommand: edits of one graph with one merge key are one step, which disappears when it ends where it started")
{
	AssetStepFixture fixture;
	AssetHandle const player = fixture.CreateGraph("Scripts/Player.sgraph");
	AssetHandle const enemy = fixture.CreateGraph("Scripts/Enemy.sgraph");
	fixture.Edit(player, MakeGraphDocument());
	fixture.Edit(enemy, MakeGraphDocument());
	CommandHistory& history = fixture.Context.GetHistory();
	history.BreakMerge();
	Ref<VisualScriptDocument const> const start = fixture.GetGraph(player)->GetSharedDocument();
	std::string const startBytes = fixture.ReadFile("Scripts/Player.sgraph");
	size_t const steps = history.GetUndoCount();
	constexpr uint64_t Key = 0x5445535400000001ull;

	// One interaction: the step keeps its first description and takes the last document.
	fixture.Edit(player, Moved(*start, 16.0f), "Move Node in Player", Key);
	Ref<VisualScriptDocument const> const dragged = fixture.Edit(player, Moved(*start, 32.0f), "Move Node in Player (again)", Key);
	CHECK(history.GetUndoCount() == steps + 1);
	CHECK(history.GetUndoDescription() == "Move Node in Player");
	CHECK(fixture.ReadFile("Scripts/Player.sgraph") == Canonical(*dragged));
	// Another graph with the same key, and edits without a key, are steps of their own.
	fixture.Edit(enemy, Moved(*start, 16.0f), "Move Node in Enemy", Key);
	CHECK(history.GetUndoCount() == steps + 2);
	fixture.Edit(enemy, Moved(*start, 48.0f), "Move Node in Enemy");
	fixture.Edit(enemy, Moved(*start, 64.0f), "Move Node in Enemy");
	CHECK(history.GetUndoCount() == steps + 4);
	for (int step = 0; step < 4; step++)
	{
		REQUIRE(fixture.Operations.Undo().IsOk());
	}
	CHECK(fixture.ReadFile("Scripts/Player.sgraph") == startBytes);
	CHECK(fixture.GetGraph(player)->GetSharedDocument().get() == start.get());

	// An interaction that comes back to where it started leaves no step, and the file its bytes.
	constexpr uint64_t Back = 0x5445535400000002ull;
	fixture.Edit(player, Moved(*start, 16.0f), "Move Node in Player", Back);
	fixture.Edit(player, VisualScriptDocument(*start), "Move Node in Player", Back);
	CHECK(history.GetUndoCount() == steps);
	CHECK(fixture.ReadFile("Scripts/Player.sgraph") == startBytes);
	CHECK(fixture.GetGraph(player)->GetDocument() == *start);
	CHECK_FALSE(history.CanRedo());
}

TEST_CASE(
	"SetVisualScriptCommand: undo gives the file back byte for byte, a version-1 file, a repaired file and a byte order mark included")
{
	AssetStepFixture fixture;
	struct File
	{
		std::string Path;
		std::string Bytes;
	};
	std::vector<File> const files = {
		{"Scripts/Version1.sgraph", ReadVersion1Example()},
		// The second node repeats the first one's ID: the loader drops it.
		{"Scripts/Repaired.sgraph",
	     "{\"Strada\": {\"Version\": 3, \"Type\": \"VisualScript\"}, \"VisualScript\": {\"Graph\": {\"Nodes\": [{\"ID\": 1, \"Type\": "
	     "\"Event.Update\", \"Position\": [0, 0]}, {\"ID\": 1, \"Type\": \"Flow.Branch\"}]}}}\n"},
		{"Scripts/Marked.sgraph", "\xEF\xBB\xBF" + Canonical(MakeGraphDocument())},
	};
	for (File const& file : files)
	{
		CAPTURE(file.Path);
		AssetHandle const graph = fixture.AddFile(file.Path, file.Bytes);
		Ref<VisualScriptAsset> const asset = fixture.GetGraph(graph);
		Ref<VisualScriptDocument const> const loaded = asset->GetSharedDocument();
		REQUIRE(Canonical(*loaded) != file.Bytes);

		Ref<VisualScriptDocument const> const edited = fixture.Edit(graph, Described(*loaded, "Edited"));
		std::string const canonical = Canonical(*edited);
		CHECK(fixture.ReadFile(file.Path) == canonical);
		CHECK(canonical.find("\"Version\": 3,") != std::string::npos);
		REQUIRE(fixture.Operations.Undo().IsOk());
		CHECK(fixture.ReadFile(file.Path) == file.Bytes);
		CHECK(asset->GetSharedDocument().get() == loaded.get());
		// The bytes given back load as the document the asset holds: a refresh keeps it.
		CHECK(fixture.Refresh().empty());
		CHECK(fixture.GetGraph(graph).get() == asset.get());
		REQUIRE(fixture.Operations.Redo().IsOk());
		CHECK(fixture.ReadFile(file.Path) == canonical);
		REQUIRE(fixture.Operations.Undo().IsOk());
		CHECK(fixture.ReadFile(file.Path) == file.Bytes);
	}

	// A file that does not load: it is replaced, and undo gives its bytes back, which fail to load again.
	std::string const damaged = "{ \"Strada\": { \"Version\": 4, \"Type\": \"VisualScript\" }, \"VisualScript\": {} }\n";
	AssetHandle const graph = fixture.AddFile("Scripts/Damaged.sgraph", damaged);
	REQUIRE(AssetManager::TryGetAsset<VisualScriptAsset>(graph).IsError());
	Ref<VisualScriptDocument const> const repaired = fixture.Edit(graph, MakeGraphDocument());
	CHECK(fixture.ReadFile("Scripts/Damaged.sgraph") == Canonical(*repaired));
	CHECK(fixture.GetGraph(graph)->GetDocument() == *repaired);
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.ReadFile("Scripts/Damaged.sgraph") == damaged);
	CHECK(AssetManager::TryGetAsset<VisualScriptAsset>(graph).IsError());
	REQUIRE(fixture.Operations.Redo().IsOk());
	CHECK(fixture.ReadFile("Scripts/Damaged.sgraph") == Canonical(*repaired));
	CHECK(fixture.GetGraph(graph)->GetDocument() == *repaired);
	CHECK_FALSE(fixture.Context.IsDirty());
}

TEST_CASE(
	"SetVisualScriptCommand: a step keeps the text it wrote and the bytes it gives back, and on a version-1 file even a round trip stays")
{
	AssetStepFixture fixture;
	std::string const version1 = ReadVersion1Example();
	AssetHandle const graph = fixture.AddFile("Scripts/Version1.sgraph", version1);
	Ref<VisualScriptDocument const> const loaded = fixture.GetGraph(graph)->GetSharedDocument();
	constexpr uint64_t Key = 0x5445535400000003ull;

	// What the step reports: its graph, no scene change, and the memory of the text written and of the bytes kept.
	SetVisualScriptCommand first(graph, CreateRef<VisualScriptDocument const>(Described(*loaded, "First")), "Describe Player", Key);
	REQUIRE(first.Execute(fixture.Context).IsOk());
	CHECK(first.GetEditedAsset() == graph);
	CHECK_FALSE(first.ModifiesScene());
	CHECK(first.HasEffect());
	CHECK(first.GetMemoryUse() == fixture.ReadFile("Scripts/Version1.sgraph").size() + version1.size());

	// A merge takes the later document and its text; the step keeps its description and the bytes from before its own
	// first execution.
	SetVisualScriptCommand second(graph, CreateRef<VisualScriptDocument const>(Described(*loaded, "Second and longer")), "Again", Key);
	REQUIRE(second.Execute(fixture.Context).IsOk());
	CHECK(first.CanMergeWith(second));
	SetVisualScriptCommand otherKey(graph, loaded, "Describe Player", Key + 1);
	CHECK_FALSE(first.CanMergeWith(otherKey));
	SetVisualScriptCommand withoutKey(graph, loaded, "Describe Player");
	CHECK_FALSE(withoutKey.CanMergeWith(withoutKey));
	SetMaterialCommand material(graph, Json::object(), Key);
	CHECK_FALSE(first.CanMergeWith(material));
	first.MergeWith(second);
	CHECK(first.GetDescription() == "Describe Player");
	CHECK(first.GetMemoryUse() == fixture.ReadFile("Scripts/Version1.sgraph").size() + version1.size());
	REQUIRE(first.Undo(fixture.Context).IsOk());
	CHECK(fixture.ReadFile("Scripts/Version1.sgraph") == version1);
	CHECK(fixture.GetGraph(graph)->GetSharedDocument().get() == loaded.get());

	// Through the history: an interaction that comes back to where it started is one step that gives the version-1 bytes back.
	CommandHistory const& history = fixture.Context.GetHistory();
	size_t const steps = history.GetUndoCount();
	fixture.Edit(graph, Described(*loaded, "Edited"), "Describe Player", Key);
	fixture.Edit(graph, VisualScriptDocument(*loaded), "Describe Player", Key);
	CHECK(history.GetUndoCount() == steps + 1);
	CHECK(fixture.ReadFile("Scripts/Version1.sgraph") == Canonical(*loaded));
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.ReadFile("Scripts/Version1.sgraph") == version1);
}

TEST_CASE("SetVisualScriptCommand: a read-only file is refused by edits and by undo, which stays on the stack until the file is writable")
{
	AssetStepFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Player.sgraph");
	fixture.Edit(graph, MakeGraphDocument(), "Add Sequence in Player");
	Ref<VisualScriptAsset> const asset = fixture.GetGraph(graph);
	Ref<VisualScriptDocument const> const current = asset->GetSharedDocument();
	std::string const bytes = fixture.ReadFile("Scripts/Player.sgraph");
	CommandHistory const& history = fixture.Context.GetHistory();
	size_t const steps = history.GetUndoCount();
	{
		Testing::ReadOnlyFileScope const readOnly(fixture.GetFile("Scripts/Player.sgraph"));
		Result<void> const refused = fixture.Operations.EditVisualScript(
			graph, CreateRef<VisualScriptDocument const>(Moved(*current, 64.0f)), "Move Node in Player");
		REQUIRE(refused.IsError());
		CHECK(refused.GetError() == "the file of visual script 'Scripts/Player.sgraph' is read-only");
		CHECK(history.GetUndoCount() == steps);
		CHECK(asset->GetSharedDocument().get() == current.get());

		Result<void> const undone = fixture.Operations.Undo();
		REQUIRE(undone.IsError());
		CHECK(undone.GetError().find("is read-only") != std::string::npos);
		CHECK(history.GetUndoCount() == steps);
		CHECK(history.GetUndoDescription() == "Add Sequence in Player");
		CHECK(asset->GetSharedDocument().get() == current.get());
		CHECK(fixture.ReadFile("Scripts/Player.sgraph") == bytes);
	}
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(history.GetUndoCount() == steps - 1);
	CHECK(fixture.ReadFile("Scripts/Player.sgraph") != bytes);
}

TEST_CASE("SetVisualScriptCommand: undo after the file changed outside the editor gives that change back, for the next refresh to load")
{
	AssetStepFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Player.sgraph");
	Ref<VisualScriptAsset> const asset = fixture.GetGraph(graph);
	// The editor wrote the file last, and remembers the text it wrote.
	Ref<VisualScriptDocument const> const shown = fixture.Edit(graph, Described(*asset->GetSharedDocument(), "Shown"));
	REQUIRE(fixture.Context.FindVisualScriptText(graph, shown.get()) != nullptr);
	std::filesystem::path const file = fixture.GetFile("Scripts/Player.sgraph");
	// Another program writes the file; no refresh has seen it yet.
	std::string const outside = Canonical(MakeGraphDocument());
	REQUIRE(FileSystem::WriteTextFile(file, outside).IsOk());

	// An edit of what the editor shows replaces the file.
	fixture.Edit(graph, Described(*shown, "Edited"));
	CHECK(fixture.ReadFile("Scripts/Player.sgraph") != outside);
	// The step's write is recorded an hour back, so that the undo's write is later even on file systems with coarse times.
	std::error_code error;
	std::filesystem::last_write_time(file, std::filesystem::last_write_time(file, error) - std::chrono::hours(1), error);
	REQUIRE_FALSE(error);
	AssetManager::RecordFileWrite(graph);

	// Undo gives the file the other program's bytes and the asset what it showed; the next refresh loads what the file has.
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.ReadFile("Scripts/Player.sgraph") == outside);
	CHECK(asset->GetSharedDocument().get() == shown.get());
	CHECK(fixture.Refresh() == std::vector<AssetHandle>{graph});
	CHECK(fixture.GetGraph(graph)->GetDocument() == MakeGraphDocument());
}

TEST_CASE("SetVisualScriptCommand: a file emptied outside the editor is given back empty, by one step even when the edit comes back")
{
	AssetStepFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Player.sgraph");
	Ref<VisualScriptDocument const> const shown = fixture.Edit(graph, MakeGraphDocument());
	Ref<VisualScriptAsset> const asset = fixture.GetGraph(graph);
	// Another program empties the file; no refresh has seen it yet.
	std::filesystem::path const file = fixture.GetFile("Scripts/Player.sgraph");
	REQUIRE(FileSystem::WriteTextFile(file, "").IsOk());

	// An edit of what the editor shows replaces the file, and its undo gives it back empty: no bytes are bytes too.
	Ref<VisualScriptDocument const> const edited = fixture.Edit(graph, Described(*shown, "Edited"));
	CHECK(fixture.ReadFile("Scripts/Player.sgraph") == Canonical(*edited));
	// The step's write is recorded an hour back, so that the undo's write is later even on file systems with coarse times.
	std::error_code error;
	std::filesystem::last_write_time(file, std::filesystem::last_write_time(file, error) - std::chrono::hours(1), error);
	REQUIRE_FALSE(error);
	AssetManager::RecordFileWrite(graph);
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.ReadFile("Scripts/Player.sgraph").empty());
	CHECK(asset->GetSharedDocument().get() == shown.get());
	// The write is not recorded, since the file no longer holds what the asset shows: the next refresh loads it, and fails.
	CHECK(fixture.Refresh() == std::vector<AssetHandle>{graph});
	CHECK(AssetManager::TryGetAsset<VisualScriptAsset>(graph).IsError());
	CHECK(fixture.Context.FindVisualScriptText(graph, shown.get()) == nullptr);

	// An interaction that comes back to where it started on an emptied file stays one step, whose undo empties it again.
	AssetHandle const enemy = fixture.CreateGraph("Scripts/Enemy.sgraph");
	Ref<VisualScriptDocument const> const start = fixture.Edit(enemy, MakeGraphDocument());
	REQUIRE(FileSystem::WriteTextFile(fixture.GetFile("Scripts/Enemy.sgraph"), "").IsOk());
	CommandHistory const& history = fixture.Context.GetHistory();
	size_t const steps = history.GetUndoCount();
	constexpr uint64_t Key = 0x5445535400000004ull;
	fixture.Edit(enemy, Moved(*start, 16.0f), "Move Node in Enemy", Key);
	fixture.Edit(enemy, VisualScriptDocument(*start), "Move Node in Enemy", Key);
	CHECK(history.GetUndoCount() == steps + 1);
	CHECK(fixture.ReadFile("Scripts/Enemy.sgraph") == Canonical(*start));
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.ReadFile("Scripts/Enemy.sgraph").empty());
	CHECK(history.GetUndoCount() == steps);
}

TEST_CASE("EditorContext: the text the editor wrote last to a visual script counts only for the document object it is the text of")
{
	EditorContext context;
	AssetHandle const graph = AssetHandle::Generate();
	AssetHandle const other = AssetHandle::Generate();
	Ref<VisualScriptDocument const> written = CreateRef<VisualScriptDocument const>(MakeGraphDocument());
	Ref<VisualScriptDocument const> const equal = CreateRef<VisualScriptDocument const>(MakeGraphDocument());
	CHECK(context.FindVisualScriptText(graph, written.get()) == nullptr);
	context.RememberVisualScriptText(graph, written, Canonical(*written));
	std::string const* const found = context.FindVisualScriptText(graph, written.get());
	REQUIRE(found != nullptr);
	CHECK(*found == Canonical(*written));

	// Another object, even an equal one, another graph or no document: not its text.
	CHECK(context.FindVisualScriptText(graph, equal.get()) == nullptr);
	CHECK(context.FindVisualScriptText(other, written.get()) == nullptr);
	CHECK(context.FindVisualScriptText(graph, nullptr) == nullptr);
	// Forgotten, and gone with its document (another object may take its address).
	context.ForgetVisualScriptText(graph);
	CHECK(context.FindVisualScriptText(graph, written.get()) == nullptr);
	context.RememberVisualScriptText(graph, written, "text");
	VisualScriptDocument const* const address = written.get();
	written.reset();
	CHECK(context.FindVisualScriptText(graph, address) == nullptr);
	// The texts belong to the project's files: another project forgets them.
	context.RememberVisualScriptText(graph, equal, "text");
	REQUIRE(context.FindVisualScriptText(graph, equal.get()) != nullptr);
	context.SetProject(nullptr);
	CHECK(context.FindVisualScriptText(graph, equal.get()) == nullptr);
}

TEST_CASE("SetVisualScriptAndComponentsCommand: a graph and the components of entities that run it change in one step, all or nothing")
{
	AssetStepFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Player.sgraph");
	fixture.Edit(graph, MakeGraphDocument());
	Ref<VisualScriptAsset> const asset = fixture.GetGraph(graph);
	Ref<VisualScriptDocument const> const original = asset->GetSharedDocument();
	std::string const originalBytes = fixture.ReadFile("Scripts/Player.sgraph");
	UUID const door = Create(fixture.Operations, "Door");
	UUID const window = Create(fixture.Operations, "Window");
	UUID const player = Create(fixture.Operations, "Player", UUID::Invalid(),
	                           Json::object({{"VisualScript", Json::object({{"Graph", graph.ToString()}})}}));
	CommandHistory& history = fixture.Context.GetHistory();
	history.MarkSaved();
	size_t const steps = history.GetUndoCount();
	auto const variables = [&]
	{
		Entity const entity = fixture.Operations.FindEntity(player);
		REQUIRE(entity);
		return entity.GetComponent<VisualScriptComponent>().Variables;
	};

	// The dropped door becomes a variable of the graph, and the player's value of it.
	VisualScriptDocument referencing = *original;
	VisualScriptVariable doorVariable;
	doorVariable.ID = 12;
	doorVariable.Name = "Door";
	doorVariable.Type = "Strada.Entity";
	doorVariable.Public = true;
	referencing.Variables.push_back(doorVariable);
	VisualScriptNode get;
	get.ID = 13;
	get.Type = "Variable.Get";
	get.Position = glm::vec2(0.0f, 160.0f);
	get.Settings = Json::object({{"Variable", 12}});
	referencing.Graph.Nodes.push_back(get);
	// The edit that gives an entity's VisualScript component the value of Door.
	auto const reference = [](UUID entity, UUID target)
	{
		Json const value = Json::object({{"Type", "Entity"}, {"Value", std::to_string(target.GetValue())}});
		return std::vector<ComponentEdit>{{entity, "VisualScript", Json::object({{"Variables", Json::object({{"Door", value}})}})}};
	};
	Ref<VisualScriptDocument const> const referenced = CreateRef<VisualScriptDocument const>(referencing);
	REQUIRE(
		fixture.Operations.EditVisualScriptAndComponents(graph, referenced, "Reference Door in Player", reference(player, door)).IsOk());
	CHECK(history.GetUndoCount() == steps + 1);
	CHECK(history.GetUndoDescription() == "Reference Door in Player");
	CHECK(fixture.Context.IsDirty());
	CHECK(asset->GetSharedDocument().get() == referenced.get());
	CHECK(fixture.ReadFile("Scripts/Player.sgraph") == Canonical(*referenced));
	CHECK(variables() == ScriptFieldMap{{"Door", ScriptFieldValue::FromEntity(door)}});

	// Undo takes back both, components first; redo makes both again.
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(variables().empty());
	CHECK(asset->GetSharedDocument().get() == original.get());
	CHECK(fixture.ReadFile("Scripts/Player.sgraph") == originalBytes);
	CHECK_FALSE(fixture.Context.IsDirty());
	REQUIRE(fixture.Operations.Redo().IsOk());
	CHECK(variables() == ScriptFieldMap{{"Door", ScriptFieldValue::FromEntity(door)}});
	CHECK(asset->GetSharedDocument().get() == referenced.get());

	// All or nothing: components that cannot be edited leave the graph as it was, and a graph that cannot be written the
	// components.
	Ref<VisualScriptDocument const> const described = CreateRef<VisualScriptDocument const>(Described(*referenced, "Opens doors"));
	CHECK(fixture.Operations.EditVisualScriptAndComponents(graph, described, "Reference Door in Player", reference(UUID(77), door))
	          .IsError());
	CHECK(asset->GetSharedDocument().get() == referenced.get());
	CHECK(fixture.ReadFile("Scripts/Player.sgraph") == Canonical(*referenced));
	{
		Testing::ReadOnlyFileScope const readOnly(fixture.GetFile("Scripts/Player.sgraph"));
		REQUIRE(fixture.Operations.SetComponentFields(player, "VisualScript", Json::object({{"Variables", Json::object()}})).IsOk());
		CHECK(fixture.Operations.EditVisualScriptAndComponents(graph, described, "Reference Door in Player", reference(player, door))
		          .IsError());
		CHECK(variables().empty());
		REQUIRE(fixture.Operations.Undo().IsOk());
	}
	CHECK(history.GetUndoCount() == steps + 1);
	CHECK(asset->GetSharedDocument().get() == referenced.get());

	// Once its graph is forgotten (the graph was deleted), the step undoes and redoes the components alone.
	SetVisualScriptAndComponentsCommand command(graph, described, "Reference Window in Player", reference(player, window));
	REQUIRE(command.Execute(fixture.Context).IsOk());
	std::string const describedBytes = fixture.ReadFile("Scripts/Player.sgraph");
	CHECK(describedBytes == Canonical(*described));
	CHECK(variables() == ScriptFieldMap{{"Door", ScriptFieldValue::FromEntity(window)}});
	CHECK(command.GetDescription() == "Reference Window in Player");
	CHECK(command.GetEditedAsset() == graph);
	CHECK(command.GetMemoryUse() == describedBytes.size());
	CHECK(command.HasEffect());
	CHECK(command.ModifiesScene());
	command.ForgetEditedAsset();
	CHECK_FALSE(command.GetEditedAsset().IsValid());
	CHECK(command.GetMemoryUse() == 0);
	REQUIRE(command.Undo(fixture.Context).IsOk());
	CHECK(variables() == ScriptFieldMap{{"Door", ScriptFieldValue::FromEntity(door)}});
	CHECK(fixture.ReadFile("Scripts/Player.sgraph") == describedBytes);
	REQUIRE(command.Execute(fixture.Context).IsOk());
	CHECK(variables() == ScriptFieldMap{{"Door", ScriptFieldValue::FromEntity(window)}});
	CHECK(fixture.ReadFile("Scripts/Player.sgraph") == describedBytes);
}

TEST_CASE("SetMaterialCommand: a material edit names its material and records its writes, so a refresh keeps the asset")
{
	AssetStepFixture fixture;
	Result<AssetHandle> const created = fixture.Operations.CreateMaterial("Materials/Rock.smat");
	REQUIRE(created.IsOk());
	AssetHandle const material = created.GetValue();
	Ref<MaterialAsset> const asset = AssetManager::GetAsset<MaterialAsset>(material);
	REQUIRE(asset != nullptr);

	REQUIRE(fixture.Operations.SetMaterialFields(material, Json::object({{"Roughness", 0.25}})).IsOk());
	CHECK(fixture.Refresh().empty());
	CHECK(AssetManager::GetAsset<MaterialAsset>(material).get() == asset.get());
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.Refresh().empty());
	REQUIRE(fixture.Operations.Redo().IsOk());
	CHECK(fixture.Refresh().empty());
	CHECK(AssetManager::GetAsset<MaterialAsset>(material).get() == asset.get());
	CHECK(asset->GetData().Roughness == doctest::Approx(0.25f));

	SetMaterialCommand const command(material, Json::object({{"Roughness", 0.75}}));
	CHECK(command.GetEditedAsset() == material);
	CHECK(command.GetMemoryUse() == 0);
	CHECK_FALSE(command.ModifiesScene());
}

TEST_CASE("CommandHistory: undo steps keep at most DefaultMaxMemory, the oldest dropped first, but the last MinStepsKept always stay")
{
	EditorContext context;
	std::vector<std::string> log;
	AssetHandle const graph = AssetHandle::Generate();
	constexpr size_t Budget = CommandHistory::DefaultMaxMemory;

	// Steps of a 64th of the budget: 64 fit, and each one more drops the oldest.
	CommandHistory history;
	for (int step = 0; step < 70; step++)
	{
		Record(history, context, log, {"Edit " + std::to_string(step), false, graph, Budget / 64});
	}
	CHECK(history.GetUndoCount() == 64);
	UndoAll(history, context);
	CHECK(log.back() == "undo Edit 6");

	// Scene steps keep nothing, but they are steps: the oldest go first whatever they keep.
	CommandHistory mixed;
	Record(mixed, context, log, {"Move"});
	for (int step = 0; step < 64; step++)
	{
		Record(mixed, context, log, {"Edit " + std::to_string(step), false, graph, Budget / 64});
	}
	CHECK(mixed.GetUndoCount() == 65);
	Record(mixed, context, log, {"Rename"});
	Record(mixed, context, log, {"Edit 64", false, graph, Budget / 64});
	CHECK(mixed.GetUndoCount() == 65);
	UndoAll(mixed, context);
	CHECK(log.back() == "undo Edit 1");

	// Steps larger than the budget: the last MinStepsKept stay all the same.
	CommandHistory large;
	for (int step = 0; step < 20; step++)
	{
		Record(large, context, log, {"Large " + std::to_string(step), false, graph, Budget / 2});
	}
	CHECK(large.GetUndoCount() == CommandHistory::MinStepsKept);
	UndoAll(large, context);
	CHECK(log.back() == "undo Large 4");

	// A merged step keeps the memory of its last change: the steps before it make room when it grows.
	CommandHistory merging;
	constexpr uint64_t Key = 0x5445535400000004ull;
	for (int step = 0; step < 64; step++)
	{
		Record(merging, context, log, {"Edit " + std::to_string(step), false, graph, Budget / 64});
	}
	Record(merging, context, log, {"Drag", false, graph, Budget / 64, Key});
	CHECK(merging.GetUndoCount() == 64);
	Record(merging, context, log, {"Drag", false, graph, 3 * (Budget / 64), Key});
	CHECK(merging.GetUndoCount() == 62);
	CHECK(merging.GetUndoDescription() == "Drag");
	UndoAll(merging, context);
	CHECK(log.back() == "undo Edit 3");
}

TEST_CASE("CommandHistory: steps of asset files move to another history, whose scene keeps its state and whose next edit starts a step")
{
	EditorContext context;
	std::vector<std::string> log;
	AssetHandle const graph = AssetHandle::Generate();
	AssetHandle const material = AssetHandle::Generate();
	constexpr uint64_t Key = 0x5445535400000005ull;

	// The history of a scene that played: scene steps and steps of asset files, the last one undone.
	CommandHistory played;
	Record(played, context, log, {"Move"});
	Record(played, context, log, {"Edit Graph", false, graph});
	Record(played, context, log, {"Rename"});
	Record(played, context, log, {"Edit Material", false, material, 0, Key});
	Record(played, context, log, {"Edit Graph Again", false, graph});
	REQUIRE(played.Undo(context).IsOk());
	std::vector<Scope<EditorCommand>> steps = played.TakeAssetSteps();

	// The undo steps of asset files, oldest first; the undone one is not taken, and the redo stack is cleared. The scene
	// steps that stay still lead back to where the scene started.
	REQUIRE(steps.size() == 2);
	CHECK(steps[0]->GetDescription() == "Edit Graph");
	CHECK(steps[1]->GetDescription() == "Edit Material");
	CHECK(played.GetUndoCount() == 2);
	CHECK_FALSE(played.CanRedo());
	CHECK(played.GetUndoDescription() == "Rename");
	CHECK(played.IsDirty());
	UndoAll(played, context);
	CHECK(log.back() == "undo Move");
	CHECK_FALSE(played.IsDirty());

	// The history of the edited scene: saved after a step, and a later step undone.
	CommandHistory edited;
	Record(edited, context, log, {"Create"});
	edited.MarkSaved();
	Record(edited, context, log, {"Delete"});
	REQUIRE(edited.Undo(context).IsOk());
	CHECK_FALSE(edited.IsDirty());

	// No steps change nothing: the step undone can still be redone.
	edited.AppendAssetSteps({});
	CHECK(edited.GetUndoCount() == 1);
	CHECK(edited.GetRedoDescription() == "Delete");

	// The steps become the newest undo steps. They leave the scene as it is, so it stays saved; the redo stack is cleared.
	edited.AppendAssetSteps(std::move(steps));
	CHECK(edited.GetUndoCount() == 3);
	CHECK(edited.GetUndoDescription() == "Edit Material");
	CHECK_FALSE(edited.CanRedo());
	CHECK_FALSE(edited.IsDirty());

	// The next edit starts a step of its own, even under the merge key of the last step taken.
	Record(edited, context, log, {"Edit Material", false, material, 0, Key});
	CHECK(edited.GetUndoCount() == 4);
	for (int step = 0; step < 3; step++)
	{
		REQUIRE(edited.Undo(context).IsOk());
		CHECK_FALSE(edited.IsDirty());
	}
	CHECK(log.back() == "undo Edit Graph");
	REQUIRE(edited.Undo(context).IsOk());
	CHECK(log.back() == "undo Create");
	CHECK(edited.IsDirty());

	// Also when the history would have merged the next edit into its last step before the steps came.
	CommandHistory merging;
	Record(merging, context, log, {"Edit Graph", false, graph, 0, Key});
	std::vector<Scope<EditorCommand>> taken;
	taken.push_back(CreateScope<FakeStep>(FakeStepInfo{"Edit Material", false, material, 0, Key}, log));
	merging.AppendAssetSteps(std::move(taken));
	Record(merging, context, log, {"Edit Material", false, material, 0, Key});
	CHECK(merging.GetUndoCount() == 3);

	// Appended steps are bounded like recorded ones.
	CommandHistory small(2);
	std::vector<Scope<EditorCommand>> many;
	for (int step = 0; step < 3; step++)
	{
		many.push_back(CreateScope<FakeStep>(FakeStepInfo{"Step " + std::to_string(step), false, graph}, log));
	}
	small.AppendAssetSteps(std::move(many));
	CHECK(small.GetUndoCount() == 2);
	UndoAll(small, context);
	CHECK(log.back() == "undo Step 1");
}

TEST_CASE("CommandHistory: the steps of a deleted asset leave both stacks, and a step that also changed the scene stays and forgets it")
{
	EditorContext context;
	std::vector<std::string> log;
	AssetHandle const graph = AssetHandle::Generate();
	AssetHandle const material = AssetHandle::Generate();
	CommandHistory history;
	CHECK_FALSE(history.GetUndoEditedAsset().IsValid());
	CHECK_FALSE(history.GetRedoEditedAsset().IsValid());

	Record(history, context, log, {"Create"});
	Record(history, context, log, {"Edit Graph", false, graph});
	// A step of the scene that edits the graph too, as an entity dropped into a graph does.
	Record(history, context, log, {"Reference Door", true, graph});
	Record(history, context, log, {"Edit Material", false, material});
	Record(history, context, log, {"Edit Graph Again", false, graph});
	Record(history, context, log, {"Edit Graph Last", false, graph});
	REQUIRE(history.Undo(context).IsOk());
	// The assets the next undo and redo steps edit, invalid for a step of the scene alone.
	CHECK(history.GetUndoEditedAsset() == graph);
	CHECK(history.GetRedoEditedAsset() == graph);
	CHECK(history.IsDirty());

	// Neither an invalid handle nor an asset no step edits changes anything.
	history.DiscardStepsOf(AssetHandle());
	history.DiscardStepsOf(AssetHandle::Generate());
	CHECK(history.GetUndoCount() == 5);
	CHECK(history.GetRedoCount() == 1);

	history.DiscardStepsOf(graph);
	CHECK(history.GetUndoCount() == 3);
	CHECK_FALSE(history.CanRedo());
	CHECK(history.GetUndoDescription() == "Edit Material");
	CHECK(history.GetUndoEditedAsset() == material);
	CHECK(log.back() == "forget Reference Door");
	CHECK(history.IsDirty());

	// Undo goes on with the steps before; the step that stayed edits the scene alone now.
	REQUIRE(history.Undo(context).IsOk());
	CHECK(history.GetUndoDescription() == "Reference Door");
	CHECK_FALSE(history.GetUndoEditedAsset().IsValid());
	CHECK(history.GetRedoEditedAsset() == material);
	REQUIRE(history.Undo(context).IsOk());
	REQUIRE(history.Undo(context).IsOk());
	CHECK(log.back() == "undo Create");
	CHECK_FALSE(history.CanUndo());
	CHECK_FALSE(history.IsDirty());

	// Steps waiting to be redone go too.
	history.DiscardStepsOf(material);
	CHECK(history.GetRedoCount() == 2);
	REQUIRE(history.Redo(context).IsOk());
	REQUIRE(history.Redo(context).IsOk());
	CHECK(log.back() == "do Reference Door");
	CHECK_FALSE(history.CanRedo());

	// The step before a discarded one does not take the next edit: it starts a step of its own.
	constexpr uint64_t Key = 0x5445535400000006ull;
	CommandHistory merging;
	Record(merging, context, log, {"Edit Material", false, material, 0, Key});
	Record(merging, context, log, {"Edit Graph", false, graph});
	merging.DiscardStepsOf(graph);
	Record(merging, context, log, {"Edit Material", false, material, 0, Key});
	CHECK(merging.GetUndoCount() == 2);
}
