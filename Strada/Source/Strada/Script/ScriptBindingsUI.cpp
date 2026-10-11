#include "stpch.h"
#include "Strada/Script/ScriptGlue.h"

namespace Strada::ScriptGlue
{
	namespace
	{
		// The running scene's entity when it has a UIButton; invalid (with a logged script error) otherwise.
		Entity FindButton(uint64_t id)
		{
			return FindComponent<UIButtonComponent>(id) != nullptr ? ScriptEngine::GetSceneContext()->GetEntityByUUID(UUID(id)) : Entity();
		}

		uint8_t UIButtonComponent_WasClicked(uint64_t id)
		{
			Entity const entity = FindButton(id);
			return entity && entity.GetScene()->WasUIButtonClicked(entity) ? 1 : 0;
		}

		int32_t UIButtonComponent_GetState(uint64_t id)
		{
			Entity const entity = FindButton(id);
			return static_cast<int32_t>(entity ? entity.GetScene()->GetUIButtonState(entity) : UIButtonState::Normal);
		}

		void UIButtonComponent_Click(uint64_t id)
		{
			if (Entity const entity = FindButton(id))
			{
				entity.GetScene()->ClickUIButton(entity);
			}
		}
	}

	void RegisterUIBindings(BindingTable& table)
	{
		AddFieldBindings<&UICanvasComponent::ScaleMode, "UICanvasComponent", "ScaleMode">(table);
		AddFieldBindings<&UICanvasComponent::ReferenceResolution, "UICanvasComponent", "ReferenceResolution">(table);

		AddFieldBindings<&UIRectComponent::AnchorMin, "UIRectComponent", "AnchorMin">(table);
		AddFieldBindings<&UIRectComponent::AnchorMax, "UIRectComponent", "AnchorMax">(table);
		AddFieldBindings<&UIRectComponent::Position, "UIRectComponent", "Position">(table);
		AddFieldBindings<&UIRectComponent::Size, "UIRectComponent", "Size">(table);
		AddFieldBindings<&UIRectComponent::Pivot, "UIRectComponent", "Pivot">(table);
		AddFieldBindings<&UIRectComponent::Visible, "UIRectComponent", "Visible">(table);
		AddFieldBindings<&UIRectComponent::Opacity, "UIRectComponent", "Opacity">(table);

		AddFieldBindings<&UIImageComponent::Color, "UIImageComponent", "Color">(table);
		AddFieldBindings<&UIImageComponent::Texture, "UIImageComponent", "Texture">(table);
		AddFieldBindings<&UIImageComponent::PreserveAspect, "UIImageComponent", "PreserveAspect">(table);
		AddFieldBindings<&UIImageComponent::CornerRadius, "UIImageComponent", "CornerRadius">(table);

		table.Add("UITextComponent_GetText", &GetText<UITextComponent, &UITextComponent::Text>);
		table.Add("UITextComponent_SetText", &SetText<UITextComponent, &UITextComponent::Text>);
		AddFieldBindings<&UITextComponent::Font, "UITextComponent", "Font">(table);
		AddFieldBindings<&UITextComponent::Color, "UITextComponent", "Color">(table);
		AddFieldBindings<&UITextComponent::FontSize, "UITextComponent", "FontSize">(table);
		AddFieldBindings<&UITextComponent::Alignment, "UITextComponent", "Alignment">(table);
		AddFieldBindings<&UITextComponent::VerticalAlignment, "UITextComponent", "VerticalAlignment">(table);
		AddFieldBindings<&UITextComponent::Wrap, "UITextComponent", "Wrap">(table);
		AddFieldBindings<&UITextComponent::LineSpacing, "UITextComponent", "LineSpacing">(table);

		AddFieldBindings<&UIButtonComponent::Interactable, "UIButtonComponent", "Interactable">(table);
		AddFieldBindings<&UIButtonComponent::NormalColor, "UIButtonComponent", "NormalColor">(table);
		AddFieldBindings<&UIButtonComponent::HoveredColor, "UIButtonComponent", "HoveredColor">(table);
		AddFieldBindings<&UIButtonComponent::PressedColor, "UIButtonComponent", "PressedColor">(table);
		AddFieldBindings<&UIButtonComponent::DisabledColor, "UIButtonComponent", "DisabledColor">(table);
		table.Add("UIButtonComponent_WasClicked", &UIButtonComponent_WasClicked);
		table.Add("UIButtonComponent_GetState", &UIButtonComponent_GetState);
		table.Add("UIButtonComponent_Click", &UIButtonComponent_Click);
	}
}
