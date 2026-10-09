#include "Editor/Automation/CommandResult.h"

namespace Strada
{
	std::string_view GetAutomationErrorCodeName(AutomationErrorCode code)
	{
		switch (code)
		{
			case AutomationErrorCode::ParseError:
				return "ParseError";
			case AutomationErrorCode::InvalidRequest:
				return "InvalidRequest";
			case AutomationErrorCode::MethodNotFound:
				return "MethodNotFound";
			case AutomationErrorCode::InvalidParams:
				return "InvalidParams";
			case AutomationErrorCode::InternalError:
				return "InternalError";
			case AutomationErrorCode::Unauthenticated:
				return "Unauthenticated";
			case AutomationErrorCode::ServerBusy:
				return "ServerBusy";
			case AutomationErrorCode::MessageTooLarge:
				return "MessageTooLarge";
			case AutomationErrorCode::EntityNotFound:
				return "EntityNotFound";
			case AutomationErrorCode::ComponentNotFound:
				return "ComponentNotFound";
			case AutomationErrorCode::InvalidOperation:
				return "InvalidOperation";
			case AutomationErrorCode::FileError:
				return "FileError";
			case AutomationErrorCode::Unavailable:
				return "Unavailable";
			case AutomationErrorCode::Cancelled:
				return "Cancelled";
			case AutomationErrorCode::UnsavedChanges:
				return "UnsavedChanges";
		}
		return "Unknown";
	}
}
