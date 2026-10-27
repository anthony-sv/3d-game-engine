#pragma once

#include <imgui.h>

#include <cstdint>

struct ImTextureData;

namespace Strada
{
	// The canvas's soft shapes: two white alpha masks kept in the UI font atlas as custom rectangles, so they draw with the
	// atlas texture every other canvas primitive uses (no texture change, no extra draw command):
	// - a soft rounded rectangle, drawn as a 9-slice around a rectangle: node shadows, the selection glow, the flash;
	// - a soft dot (a radial Gaussian): pin glows and link pulses.
	// The atlas may grow or be repacked at any time (a text drawn at a new size can do it in the middle of a frame), which
	// moves the rectangles: their UVs are read again before each draw whenever the atlas's texture changed. When the atlas
	// cannot take them, nothing is drawn and the canvas stays fully usable.
	//
	// One object serves one ImGui context: it adds its rectangles to the current context's atlas when first drawn there, and
	// again when that context uses another atlas. Its owner must not outlive the context. Main thread only.
	class VisualScriptBrushes
	{
	public:
		// The soft rectangle's mask: SoftRectSize pixels square, a rounded rectangle SoftRectInset inside its edges with corners
		// of SoftRectRadius, its edge blurred as 0.5 × erfc(d / (σ √2)) with σ = SoftRectSigma pixels (d: the distance from the
		// rounded rectangle, positive outside), so the alpha is 0.5 on the shape's edge.
		static constexpr int SoftRectSize = 64;
		static constexpr int SoftRectInset = 20;
		static constexpr int SoftRectRadius = 8;
		static constexpr float SoftRectSigma = 7.0f;
		// The soft dot's mask: SoftDotSize pixels square, exp(-r² / (2σ²)) around its centre with σ = SoftDotSigma pixels.
		static constexpr int SoftDotSize = 32;
		static constexpr float SoftDotSigma = 6.0f;

		VisualScriptBrushes() = default;
		// Gives the rectangles back to the atlas when its context is current.
		~VisualScriptBrushes();
		VisualScriptBrushes(VisualScriptBrushes const&) = delete;
		VisualScriptBrushes& operator=(VisualScriptBrushes const&) = delete;

		// Makes the brushes drawable in the current ImGui context: adds the masks to its font atlas when it does not hold them
		// (the first use, another atlas, or rectangles the atlas lost) and reads their UVs again when the atlas's texture
		// changed. Returns whether they can be drawn: false without a context, with a renderer that cannot update textures
		// (ImGuiBackendFlags_RendererHasTextures), or when the atlas has no room for them (it is then not asked again until the
		// context uses another atlas). The draw functions call it; it is cheap when nothing changed.
		bool Prepare();
		bool IsReady() const { return m_Ready; }

		// Draws the soft rectangle around [min, max] (screen pixels): its 0.5 edge on the rectangle, fading out over reach
		// pixels beyond it, in color (the mask's alpha multiplies the colour's). 16 vertices. Returns false (and draws
		// nothing) when the brushes cannot be drawn.
		bool DrawSoftRect(ImDrawList* drawList, ImVec2 lower, ImVec2 upper, float reach, ImU32 color);
		// Draws the soft dot centred on center, its mask radius pixels across in each direction (the Gaussian's σ is
		// radius × SoftDotSigma / (SoftDotSize / 2)). 4 vertices. Returns false (and draws nothing) when the brushes cannot
		// be drawn.
		bool DrawSoftDot(ImDrawList* drawList, ImVec2 center, float radius, ImU32 color);

		// The UVs the masks are drawn with now (read after Prepare); for tests.
		ImVec2 GetSoftRectUV0() const { return m_SoftRectUV0; }
		ImVec2 GetSoftRectUV1() const { return m_SoftRectUV1; }
		ImVec2 GetSoftDotUV0() const { return m_SoftDotUV0; }
		ImVec2 GetSoftDotUV1() const { return m_SoftDotUV1; }
		// How many times the masks were added to an atlas; for tests.
		uint32_t GetAddCount() const { return m_AddCount; }

	private:
		// Adds both masks to the atlas and writes their pixels; false when the atlas has no room.
		bool Add(ImFontAtlas* atlas);
		// Reads the rectangles' UVs for the atlas's current texture; false when the atlas no longer holds them.
		bool ReadRects(ImFontAtlas* atlas);

		ImFontAtlas* m_Atlas = nullptr;
		ImTextureData* m_Texture = nullptr;
		// ImFontAtlasRectId values (ImFontAtlasRectId_Invalid: none).
		int m_SoftRect = -1;
		int m_SoftDot = -1;
		ImVec2 m_SoftRectUV0{0.0f, 0.0f};
		ImVec2 m_SoftRectUV1{0.0f, 0.0f};
		ImVec2 m_SoftDotUV0{0.0f, 0.0f};
		ImVec2 m_SoftDotUV1{0.0f, 0.0f};
		bool m_Ready = false;
		// The atlas m_Atlas had no room: not asked again.
		bool m_Failed = false;
		uint32_t m_AddCount = 0;
	};
}
