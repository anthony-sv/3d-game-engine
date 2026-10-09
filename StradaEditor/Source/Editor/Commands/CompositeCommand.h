#pragma once

#include "Editor/Commands/EditorCommand.h"

#include "Strada/Core/Base.h"

#include <string>
#include <vector>

namespace Strada
{
	// Runs several commands as one undo step (for example adding a component to every selected entity), all or nothing:
	// when one fails, the ones already applied are reverted and the failure is returned.
	class CompositeCommand final : public EditorCommand
	{
	public:
		CompositeCommand(std::string description, std::vector<Scope<EditorCommand>> commands);

		Result<void> Execute(EditorContext& context) override;
		Result<void> Undo(EditorContext& context) override;
		std::string GetDescription() const override { return m_Description; }
		bool HasEffect() const override;

	private:
		std::string m_Description;
		std::vector<Scope<EditorCommand>> m_Commands;
	};
}
