#include "Editor/Panels/StatisticsPanel.h"

#include "Strada/Core/Application.h"
#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/RHI/Swapchain.h"

#include <imgui.h>

#include <algorithm>
#include <numeric>

namespace Strada
{
	void StatisticsPanel::OnImGuiRender(bool& isOpen)
	{
		Application const& application = Application::Get();
		float const frameMilliseconds = application.GetFrameTime().GetMilliseconds();
		m_FrameTimes[m_FrameTimeOffset] = frameMilliseconds;
		m_FrameTimeOffset = (m_FrameTimeOffset + 1) % m_FrameTimes.size();

		if (!ImGui::Begin("Statistics", &isOpen))
		{
			ImGui::End();
			return;
		}

		float const average = std::accumulate(m_FrameTimes.begin(), m_FrameTimes.end(), 0.0f) / static_cast<float>(m_FrameTimes.size());
		ImGui::Text("Frame time: %.2f ms (%.0f FPS)", average, average > 0.0f ? 1000.0f / average : 0.0f);
		ImGui::PlotLines("##FrameTimes", m_FrameTimes.data(), static_cast<int>(m_FrameTimes.size()), static_cast<int>(m_FrameTimeOffset),
		                 nullptr, 0.0f, std::max(33.3f, *std::max_element(m_FrameTimes.begin(), m_FrameTimes.end())), ImVec2(-1.0f, 60.0f));
		ImGui::Text("Frames: %llu", static_cast<unsigned long long>(application.GetFrameCount()));

		ImGui::SeparatorText("GPU");
		if (GraphicsDevice::IsInitialized())
		{
			AdapterInfo const& adapter = GraphicsDevice::GetAdapterInfo();
			ImGui::Text("%s (%s)", adapter.Name.c_str(), AdapterTypeToString(adapter.Type));
			ImGui::Text("Vulkan %s, %s", adapter.ApiVersion.c_str(), adapter.Driver.c_str());
			ImGui::Text("Validation: %s", GraphicsDevice::IsValidationEnabled() ? "enabled" : "disabled");
			if (Swapchain const* swapchain = application.GetSwapchain())
			{
				ImGui::Text("Swapchain: %ux%u, vsync %s", swapchain->GetWidth(), swapchain->GetHeight(),
				            swapchain->IsVSync() ? "on" : "off");
			}
		}
		else
		{
			ImGui::TextUnformatted("No GPU device");
		}
		ImGui::End();
	}
}
