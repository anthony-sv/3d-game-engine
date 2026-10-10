#include "stpch.h"
#include "Strada/Script/ScriptBindings.h"

#include "Strada/Script/ScriptGlue.h"

namespace Strada
{
	ScriptGlue::BindingTable::BindingTable()
	{
		RegisterEntityBindings(*this);
		RegisterComponentBindings(*this);
		RegisterRuntimeBindings(*this);
	}

	std::span<ScriptBinding const> GetScriptBindings()
	{
		// Built once; the names each binding points to live as long as the table.
		static ScriptGlue::BindingTable const s_Table;
		return s_Table.GetBindings();
	}
}
