#include "Editor/Panels/ViewportPanel.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/MeshSource.h"
#include "Strada/ImGui/ImGuiRenderer.h"
#include "Strada/Renderer/Renderer.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/SceneRendering.h"

#include <imgui.h>

namespace Strada
{
	ViewportPanel::ViewportPanel()
	{
		if (Renderer::IsInitialized())
		{
			m_Renderer = CreateScope<SceneRenderer>();
		}
		m_Camera.SetView(glm::vec3(0.0f, 0.5f, 0.0f), 7.0f, 30.0f, -20.0f);
	}

	void ViewportPanel::OnImGuiRender(EditorContext& context, bool& open)
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		bool const visible = ImGui::Begin("Viewport", &open);
		ImGui::PopStyleVar();
		if (!visible)
		{
			ImGui::End();
			return;
		}
		if (!m_Renderer)
		{
			ImGui::TextDisabled("Rendering is unavailable (no GPU).");
			ImGui::End();
			return;
		}

		ImVec2 const size = ImGui::GetContentRegionAvail();
		uint32_t const width = static_cast<uint32_t>(std::max(size.x, 1.0f));
		uint32_t const height = static_cast<uint32_t>(std::max(size.y, 1.0f));

		ImGuiIO const& io = ImGui::GetIO();
		bool const hovered = ImGui::IsWindowHovered();
		// A drag that starts in the viewport keeps controlling the camera until its button is released.
		auto const updateCapture = [hovered](bool& captured, ImGuiMouseButton button, bool condition)
		{
			if (hovered && condition && ImGui::IsMouseClicked(button))
			{
				captured = true;
			}
			if (!ImGui::IsMouseDown(button))
			{
				captured = false;
			}
		};
		updateCapture(m_LookCaptured, ImGuiMouseButton_Right, true);
		updateCapture(m_PanCaptured, ImGuiMouseButton_Middle, true);
		updateCapture(m_OrbitCaptured, ImGuiMouseButton_Left, io.KeyAlt);

		EditorCameraInput input;
		input.Look = m_LookCaptured;
		input.Pan = m_PanCaptured;
		input.Orbit = m_OrbitCaptured;
		if (input.Look || input.Pan || input.Orbit)
		{
			input.MouseDelta = glm::vec2(io.MouseDelta.x, io.MouseDelta.y);
		}
		if (input.Look)
		{
			input.Move.x = (ImGui::IsKeyDown(ImGuiKey_D) ? 1.0f : 0.0f) - (ImGui::IsKeyDown(ImGuiKey_A) ? 1.0f : 0.0f);
			input.Move.y = (ImGui::IsKeyDown(ImGuiKey_E) ? 1.0f : 0.0f) - (ImGui::IsKeyDown(ImGuiKey_Q) ? 1.0f : 0.0f);
			input.Move.z = (ImGui::IsKeyDown(ImGuiKey_W) ? 1.0f : 0.0f) - (ImGui::IsKeyDown(ImGuiKey_S) ? 1.0f : 0.0f);
			input.Fast = io.KeyShift;
		}
		if (hovered)
		{
			input.Scroll = io.MouseWheel;
			if (!input.Look && ImGui::IsKeyPressed(ImGuiKey_F, false))
			{
				FocusSelection(context);
			}
		}
		m_Camera.Update(input, io.DeltaTime);

		m_Camera.SetViewportSize(width, height);
		m_Renderer->SetViewportSize(width, height);
		RenderScene(context.GetScene(), *m_Renderer, m_Camera.GetRendererCamera());
		ImGui::Image(ImGuiRenderer::GetTextureID(m_Renderer->GetFinalImage()),
		             ImVec2(static_cast<float>(width), static_cast<float>(height)));
		ImGui::End();
	}

	void ViewportPanel::FocusSelection(EditorContext& context)
	{
		Scene& scene = context.GetScene();
		AABB bounds;
		for (UUID const id : context.GetSelection().GetEntities())
		{
			Entity entity = scene.GetEntityByUUID(id);
			if (!entity)
			{
				continue;
			}
			glm::mat4 const world = scene.GetWorldTransform(entity);
			MeshComponent const* mesh = entity.TryGetComponent<MeshComponent>();
			Ref<MeshSource> const source = mesh != nullptr ? AssetManager::GetAsset<MeshSource>(mesh->Mesh) : nullptr;
			if (source)
			{
				bounds.Expand(source->GetBounds().Transform(world));
			}
			else
			{
				bounds.Expand(glm::vec3(world[3]));
			}
		}
		m_Camera.Focus(bounds);
	}
}
