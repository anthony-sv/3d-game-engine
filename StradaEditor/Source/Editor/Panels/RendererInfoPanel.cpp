#include "Editor/Panels/RendererInfoPanel.h"

#include "Strada/Core/Application.h"
#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/RHI/Swapchain.h"
#include "Strada/Renderer/SceneRenderer.h"

#include <imgui.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <numeric>

namespace Strada
{
	namespace
	{
		constexpr ImGuiTableFlags TableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;

		bool BeginSection(char const* name, int columns)
		{
			ImGui::SeparatorText(name);
			if (!ImGui::BeginTable(name, columns, TableFlags))
			{
				return false;
			}
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 10.0f);
			for (int column = 1; column < columns; column++)
			{
				ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
			}
			return true;
		}

		void NameCell(char const* name)
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextDisabled("%s", name);
		}

		void Row(char const* name, char const* format, ...) IM_FMTARGS(2);

		void Row(char const* name, char const* format, ...)
		{
			NameCell(name);
			ImGui::TableNextColumn();
			va_list arguments;
			va_start(arguments, format);
			ImGui::TextV(format, arguments);
			va_end(arguments);
		}

		// One row with the value for each view; views without a renderer show a dash.
		template<typename TFormat>
		void ViewRow(char const* name, SceneRendererStatistics const* sceneView, SceneRendererStatistics const* gameView,
		             TFormat const& format)
		{
			NameCell(name);
			for (SceneRendererStatistics const* statistics : {sceneView, gameView})
			{
				ImGui::TableNextColumn();
				char text[96] = "-";
				if (statistics != nullptr)
				{
					format(*statistics, text, sizeof(text));
				}
				ImGui::TextUnformatted(text);
			}
		}
	}

	void RendererInfoPanel::OnImGuiRender(bool& isOpen, SceneRendererStatistics const* sceneView, SceneRendererStatistics const* gameView)
	{
		Application const& application = Application::Get();
		m_FrameTimes[m_FrameTimeOffset] = application.GetFrameTime().GetMilliseconds();
		m_FrameTimeOffset = (m_FrameTimeOffset + 1) % m_FrameTimes.size();

		if (!ImGui::Begin("Renderer Info", &isOpen))
		{
			ImGui::End();
			return;
		}

		if (BeginSection("Device", 2))
		{
			if (GraphicsDevice::IsInitialized())
			{
				AdapterInfo const& adapter = GraphicsDevice::GetAdapterInfo();
				Row("Adapter", "%s", adapter.Name.c_str());
				Row("Type", "%s", AdapterTypeToString(adapter.Type));
				Row("Vulkan", "%s", adapter.ApiVersion.c_str());
				Row("Driver", "%s", adapter.Driver.c_str());
				Row("GPU timing", "%s", adapter.SupportsTimerQueries ? "Supported" : "Unsupported");
				Row("Validation", "%s", GraphicsDevice::IsValidationEnabled() ? "On" : "Off");
				Row("Validation errors", "%llu", static_cast<unsigned long long>(GraphicsDevice::GetValidationErrorCount()));
			}
			else
			{
				Row("Adapter", "No GPU device");
			}
			ImGui::EndTable();
		}

		float const average = std::accumulate(m_FrameTimes.begin(), m_FrameTimes.end(), 0.0f) / static_cast<float>(m_FrameTimes.size());
		float const worst = *std::max_element(m_FrameTimes.begin(), m_FrameTimes.end());
		if (BeginSection("Frame", 2))
		{
			Row("Frame time", "%.2f ms (%.0f FPS)", static_cast<double>(average),
			    average > 0.0f ? 1000.0 / static_cast<double>(average) : 0.0);
			Row("Slowest frame", "%.2f ms", static_cast<double>(worst));
			Row("Frames", "%llu", static_cast<unsigned long long>(application.GetFrameCount()));
			if (Swapchain const* swapchain = application.GetSwapchain())
			{
				Row("Back buffer", "%ux%u, %s", swapchain->GetWidth(), swapchain->GetHeight(), swapchain->IsVSync() ? "vsync" : "no vsync");
			}
			ImGui::EndTable();
		}
		char overlay[48];
		std::snprintf(overlay, sizeof(overlay), "%.1f ms",
		              static_cast<double>(m_FrameTimes[(m_FrameTimeOffset + m_FrameTimes.size() - 1) % m_FrameTimes.size()]));
		ImGui::PlotLines("##FrameTimes", m_FrameTimes.data(), static_cast<int>(m_FrameTimes.size()), static_cast<int>(m_FrameTimeOffset),
		                 overlay, 0.0f, std::max(33.3f, worst), ImVec2(-1.0f, 70.0f));

		if (BeginSection("Views", 3))
		{
			ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
			ImGui::TableNextColumn();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted("Scene View");
			ImGui::TableNextColumn();
			ImGui::TextUnformatted("Game View");
			ViewRow("GPU time", sceneView, gameView,
			        [](SceneRendererStatistics const& statistics, char* text, size_t size)
			        {
						if (statistics.GpuMilliseconds)
						{
							std::snprintf(text, size, "%.2f ms", static_cast<double>(*statistics.GpuMilliseconds));
						}
					});
			ViewRow("Draw calls", sceneView, gameView,
			        [](SceneRendererStatistics const& statistics, char* text, size_t size)
			        {
						std::snprintf(text, size, "%u", statistics.DrawCalls);
					});
			ViewRow("Shadow draws", sceneView, gameView,
			        [](SceneRendererStatistics const& statistics, char* text, size_t size)
			        {
						std::snprintf(text, size, "%u into %u maps", statistics.ShadowDrawCalls, statistics.ShadowMapViews);
					});
			ViewRow("Triangles", sceneView, gameView,
			        [](SceneRendererStatistics const& statistics, char* text, size_t size)
			        {
						std::snprintf(text, size, "%u", statistics.Triangles);
					});
			ViewRow("Culled submeshes", sceneView, gameView,
			        [](SceneRendererStatistics const& statistics, char* text, size_t size)
			        {
						std::snprintf(text, size, "%u", statistics.Culled);
					});
			ViewRow("Lights", sceneView, gameView,
			        [](SceneRendererStatistics const& statistics, char* text, size_t size)
			        {
						std::snprintf(text, size, "%u (%u culled)", statistics.Lights, statistics.CulledLights);
					});
			ViewRow("Shadows dropped", sceneView, gameView,
			        [](SceneRendererStatistics const& statistics, char* text, size_t size)
			        {
						std::snprintf(text, size, "%u", statistics.ShadowsDropped);
					});
			ViewRow("Sprite, text and UI quads", sceneView, gameView,
			        [](SceneRendererStatistics const& statistics, char* text, size_t size)
			        {
						std::snprintf(text, size, "%u", statistics.Quads);
					});
			ImGui::EndTable();
		}
		ImGui::End();
	}
}
