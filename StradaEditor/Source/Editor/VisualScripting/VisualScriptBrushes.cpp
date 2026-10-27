#include "Editor/VisualScripting/VisualScriptBrushes.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace Strada
{
	namespace
	{
		// Where the soft rectangle's 9-slice is cut, in mask pixels from each edge: the inset plus the corner radius, so the
		// corners hold the whole rounded corner with its blur and the edges between them vary only across.
		constexpr int SoftRectSlice = VisualScriptBrushes::SoftRectInset + VisualScriptBrushes::SoftRectRadius;

		uint8_t ToByte(float alpha)
		{
			return static_cast<uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
		}

		// The soft rectangle's alpha at the centre of mask pixel (x, y).
		float GetSoftRectAlpha(int x, int y)
		{
			constexpr float Center = static_cast<float>(VisualScriptBrushes::SoftRectSize) * 0.5f;
			constexpr float HalfExtent = Center - static_cast<float>(VisualScriptBrushes::SoftRectInset);
			constexpr float Radius = static_cast<float>(VisualScriptBrushes::SoftRectRadius);
			// The signed distance from a rounded rectangle: positive outside, negative inside.
			float const qx = std::abs(static_cast<float>(x) + 0.5f - Center) - (HalfExtent - Radius);
			float const qy = std::abs(static_cast<float>(y) + 0.5f - Center) - (HalfExtent - Radius);
			float const outside = std::sqrt(std::max(qx, 0.0f) * std::max(qx, 0.0f) + std::max(qy, 0.0f) * std::max(qy, 0.0f));
			float const distance = outside + std::min(std::max(qx, qy), 0.0f) - Radius;
			return 0.5f * std::erfc(distance / (VisualScriptBrushes::SoftRectSigma * std::sqrt(2.0f)));
		}

		// The soft dot's alpha at the centre of mask pixel (x, y).
		float GetSoftDotAlpha(int x, int y)
		{
			constexpr float Center = static_cast<float>(VisualScriptBrushes::SoftDotSize) * 0.5f;
			float const dx = static_cast<float>(x) + 0.5f - Center;
			float const dy = static_cast<float>(y) + 0.5f - Center;
			constexpr float Sigma = VisualScriptBrushes::SoftDotSigma;
			return std::exp(-(dx * dx + dy * dy) / (2.0f * Sigma * Sigma));
		}

		// Writes a mask into the atlas's texture as white with the mask's alpha (or the alpha alone in an Alpha8 texture).
		template<typename TAlpha>
		void WriteMask(ImTextureData* texture, ImFontAtlasRect const& rect, TAlpha&& alpha)
		{
			for (int y = 0; y < rect.h; y++)
			{
				for (int x = 0; x < rect.w; x++)
				{
					uint8_t const value = ToByte(alpha(x, y));
					void* const pixel = texture->GetPixelsAt(rect.x + x, rect.y + y);
					if (texture->Format == ImTextureFormat_Alpha8)
					{
						*static_cast<uint8_t*>(pixel) = value;
					}
					else
					{
						ImU32 const color = IM_COL32(255, 255, 255, value);
						std::memcpy(pixel, &color, sizeof(color));
					}
				}
			}
		}
	}

	VisualScriptBrushes::~VisualScriptBrushes()
	{
		// The atlas is only touched while its context is current: an atlas of a destroyed context is gone with it.
		if (m_Atlas == nullptr || ImGui::GetCurrentContext() == nullptr || ImGui::GetIO().Fonts != m_Atlas)
		{
			return;
		}
		// Identifiers carry a generation, so removing one the atlas no longer holds does nothing (ImFontAtlasPackGetRectSafe).
		if (m_SoftRect != ImFontAtlasRectId_Invalid)
		{
			m_Atlas->RemoveCustomRect(m_SoftRect);
		}
		if (m_SoftDot != ImFontAtlasRectId_Invalid)
		{
			m_Atlas->RemoveCustomRect(m_SoftDot);
		}
	}

	bool VisualScriptBrushes::Prepare()
	{
		if (ImGui::GetCurrentContext() == nullptr)
		{
			m_Ready = false;
			return false;
		}
		ImFontAtlas* const atlas = ImGui::GetIO().Fonts;
		if (atlas != m_Atlas)
		{
			// Another context or atlas: its rectangles are not this one's.
			m_Atlas = atlas;
			m_Texture = nullptr;
			m_SoftRect = ImFontAtlasRectId_Invalid;
			m_SoftDot = ImFontAtlasRectId_Invalid;
			m_Failed = false;
			m_Ready = false;
		}
		if (atlas == nullptr || m_Failed || (ImGui::GetIO().BackendFlags & ImGuiBackendFlags_RendererHasTextures) == 0)
		{
			// Without texture updates the pixels written after the atlas was built would never reach the GPU.
			m_Ready = false;
			return false;
		}
		if (m_Ready && atlas->TexData == m_Texture)
		{
			return true;
		}
		if (m_SoftRect != ImFontAtlasRectId_Invalid && ReadRects(atlas))
		{
			return true;
		}
		if (!Add(atlas))
		{
			m_Failed = true;
			m_Ready = false;
			return false;
		}
		return ReadRects(atlas);
	}

	bool VisualScriptBrushes::Add(ImFontAtlas* atlas)
	{
		if (m_SoftRect != ImFontAtlasRectId_Invalid)
		{
			atlas->RemoveCustomRect(m_SoftRect);
		}
		if (m_SoftDot != ImFontAtlasRectId_Invalid)
		{
			atlas->RemoveCustomRect(m_SoftDot);
		}
		m_SoftRect = atlas->AddCustomRect(SoftRectSize, SoftRectSize);
		m_SoftDot = m_SoftRect != ImFontAtlasRectId_Invalid ? atlas->AddCustomRect(SoftDotSize, SoftDotSize) : ImFontAtlasRectId_Invalid;
		if (m_SoftDot == ImFontAtlasRectId_Invalid)
		{
			if (m_SoftRect != ImFontAtlasRectId_Invalid)
			{
				atlas->RemoveCustomRect(m_SoftRect);
			}
			m_SoftRect = ImFontAtlasRectId_Invalid;
			return false;
		}
		// Both are read after both were added: adding the second may grow the atlas and move the first. The pixels are
		// writable at once; AddCustomRect queued their upload (imgui.h, "Custom Rectangles/Glyphs API"), and a texture made
		// by a growth is uploaded whole.
		ImFontAtlasRect softRect;
		ImFontAtlasRect softDot;
		if (!atlas->GetCustomRect(m_SoftRect, &softRect) || !atlas->GetCustomRect(m_SoftDot, &softDot))
		{
			return false;
		}
		WriteMask(atlas->TexData, softRect, GetSoftRectAlpha);
		WriteMask(atlas->TexData, softDot, GetSoftDotAlpha);
		m_AddCount++;
		return true;
	}

	bool VisualScriptBrushes::ReadRects(ImFontAtlas* atlas)
	{
		ImFontAtlasRect softRect;
		ImFontAtlasRect softDot;
		// A rectangle of another size under the same identifier is not ours (the atlas was cleared and refilled).
		if (!atlas->GetCustomRect(m_SoftRect, &softRect) || !atlas->GetCustomRect(m_SoftDot, &softDot) || softRect.w != SoftRectSize ||
		    softRect.h != SoftRectSize || softDot.w != SoftDotSize || softDot.h != SoftDotSize)
		{
			m_Ready = false;
			return false;
		}
		m_SoftRectUV0 = softRect.uv0;
		m_SoftRectUV1 = softRect.uv1;
		m_SoftDotUV0 = softDot.uv0;
		m_SoftDotUV1 = softDot.uv1;
		m_Texture = atlas->TexData;
		m_Ready = true;
		return true;
	}

	bool VisualScriptBrushes::DrawSoftRect(ImDrawList* drawList, ImVec2 lower, ImVec2 upper, float reach, ImU32 color)
	{
		if ((color & IM_COL32_A_MASK) == 0 || !(reach > 0.0f) || !Prepare())
		{
			return false;
		}
		// The mask's SoftRectInset pixels of blur outside the shape span reach screen pixels.
		float const scale = reach / static_cast<float>(SoftRectInset);
		ImVec2 const outerMin(lower.x - reach, lower.y - reach);
		ImVec2 const outerMax(upper.x + reach, upper.y + reach);
		float const sliceX = std::min(static_cast<float>(SoftRectSlice) * scale, (outerMax.x - outerMin.x) * 0.5f);
		float const sliceY = std::min(static_cast<float>(SoftRectSlice) * scale, (outerMax.y - outerMin.y) * 0.5f);
		if (!(sliceX > 0.0f) || !(sliceY > 0.0f))
		{
			return false;
		}
		std::array<float, 4> const xs = {outerMin.x, outerMin.x + sliceX, outerMax.x - sliceX, outerMax.x};
		std::array<float, 4> const ys = {outerMin.y, outerMin.y + sliceY, outerMax.y - sliceY, outerMax.y};
		float const sliceU = (m_SoftRectUV1.x - m_SoftRectUV0.x) * static_cast<float>(SoftRectSlice) / static_cast<float>(SoftRectSize);
		float const sliceV = (m_SoftRectUV1.y - m_SoftRectUV0.y) * static_cast<float>(SoftRectSlice) / static_cast<float>(SoftRectSize);
		std::array<float, 4> const us = {m_SoftRectUV0.x, m_SoftRectUV0.x + sliceU, m_SoftRectUV1.x - sliceU, m_SoftRectUV1.x};
		std::array<float, 4> const vs = {m_SoftRectUV0.y, m_SoftRectUV0.y + sliceV, m_SoftRectUV1.y - sliceV, m_SoftRectUV1.y};

		// The first vertex's index is read after PrimReserve, which may start a new vertex offset (imgui_draw.cpp, PrimReserve).
		drawList->PrimReserve(9 * 6, 16);
		unsigned int const base = drawList->_VtxCurrentIdx;
		for (size_t row = 0; row < 4; row++)
		{
			for (size_t column = 0; column < 4; column++)
			{
				drawList->PrimWriteVtx(ImVec2(xs[column], ys[row]), ImVec2(us[column], vs[row]), color);
			}
		}
		for (unsigned int row = 0; row < 3; row++)
		{
			for (unsigned int column = 0; column < 3; column++)
			{
				unsigned int const topLeft = base + row * 4 + column;
				drawList->PrimWriteIdx(static_cast<ImDrawIdx>(topLeft));
				drawList->PrimWriteIdx(static_cast<ImDrawIdx>(topLeft + 1));
				drawList->PrimWriteIdx(static_cast<ImDrawIdx>(topLeft + 5));
				drawList->PrimWriteIdx(static_cast<ImDrawIdx>(topLeft));
				drawList->PrimWriteIdx(static_cast<ImDrawIdx>(topLeft + 5));
				drawList->PrimWriteIdx(static_cast<ImDrawIdx>(topLeft + 4));
			}
		}
		return true;
	}

	bool VisualScriptBrushes::DrawSoftDot(ImDrawList* drawList, ImVec2 center, float radius, ImU32 color)
	{
		if ((color & IM_COL32_A_MASK) == 0 || !(radius > 0.0f) || !Prepare())
		{
			return false;
		}
		drawList->PrimReserve(6, 4);
		drawList->PrimRectUV(ImVec2(center.x - radius, center.y - radius), ImVec2(center.x + radius, center.y + radius), m_SoftDotUV0,
		                     m_SoftDotUV1, color);
		return true;
	}
}
