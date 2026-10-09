#include "Editor/Commands/CompositeCommand.h"

#include <utility>

namespace Strada
{
	CompositeCommand::CompositeCommand(std::string description, std::vector<Scope<EditorCommand>> commands)
		: m_Description(std::move(description)),
		  m_Commands(std::move(commands))
	{
	}

	Result<void> CompositeCommand::Execute(EditorContext& context)
	{
		if (m_Commands.empty())
		{
			return Error{"nothing to do"};
		}
		for (size_t i = 0; i < m_Commands.size(); i++)
		{
			if (Result<void> result = m_Commands[i]->Execute(context); !result)
			{
				// Each command reverts its own last execution, so undoing the applied ones in reverse restores the start state.
				for (size_t j = i; j-- > 0;)
				{
					(void)m_Commands[j]->Undo(context);
				}
				return result;
			}
		}
		return {};
	}

	Result<void> CompositeCommand::Undo(EditorContext& context)
	{
		for (size_t i = m_Commands.size(); i-- > 0;)
		{
			if (Result<void> result = m_Commands[i]->Undo(context); !result)
			{
				for (size_t j = i + 1; j < m_Commands.size(); j++)
				{
					(void)m_Commands[j]->Execute(context);
				}
				return result;
			}
		}
		return {};
	}

	bool CompositeCommand::HasEffect() const
	{
		for (Scope<EditorCommand> const& command : m_Commands)
		{
			if (command->HasEffect())
			{
				return true;
			}
		}
		return false;
	}
}
