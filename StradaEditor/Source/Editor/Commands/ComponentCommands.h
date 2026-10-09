#pragma once

#include "Editor/Commands/EditorCommand.h"

#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"
#include "Strada/Scene/ComponentRegistry.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace Strada
{
	enum class ComponentAccess : uint8_t
	{
		// Reading or changing fields (core components such as Transform are allowed).
		Modify = 0,
		// Adding or removing the component (core components are present on every entity).
		AddRemove
	};

	// Looks up a registered component that users may edit: internal components (ID, Relationship, Prefab) are managed by
	// the engine, and core components cannot be added or removed.
	[[nodiscard]] Result<ComponentInfo const*> FindEditableComponent(std::string_view name, ComponentAccess access);

	// Adds a component, initialized from defaults plus the given fields.
	class AddComponentCommand final : public EditorCommand
	{
	public:
		AddComponentCommand(UUID entity, std::string component, Json fields);

		Result<void> Execute(EditorContext& context) override;
		Result<void> Undo(EditorContext& context) override;
		std::string GetDescription() const override;

	private:
		UUID m_Entity;
		std::string m_Component;
		Json m_Fields;
		// Every field as added, recorded by the first execution and reapplied by redo.
		Json m_Snapshot;
	};

	class RemoveComponentCommand final : public EditorCommand
	{
	public:
		RemoveComponentCommand(UUID entity, std::string component);

		Result<void> Execute(EditorContext& context) override;
		Result<void> Undo(EditorContext& context) override;
		std::string GetDescription() const override;

	private:
		UUID m_Entity;
		std::string m_Component;
		Json m_Snapshot;
	};

	// Applies a partial JSON patch to a component (transactional: every field is validated before anything changes).
	// Commands with the same entity, component and non-zero merge key merge into one undo step.
	class SetComponentCommand final : public EditorCommand
	{
	public:
		SetComponentCommand(UUID entity, std::string component, Json patch, uint64_t mergeKey = 0, std::string description = {});

		Result<void> Execute(EditorContext& context) override;
		Result<void> Undo(EditorContext& context) override;
		std::string GetDescription() const override;
		bool HasEffect() const override { return m_Before != m_After; }
		bool CanMergeWith(EditorCommand const& next) const override;
		void MergeWith(EditorCommand& next) override;

	private:
		UUID m_Entity;
		std::string m_Component;
		Json m_Patch;
		uint64_t m_MergeKey;
		std::string m_Description;
		// Complete component state before and after, recorded by the first execution.
		Json m_Before;
		Json m_After;
	};
}
