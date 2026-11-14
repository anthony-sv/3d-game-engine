#include "Editor/UI/FieldEditor.h"

namespace Strada
{
	std::vector<std::pair<std::string_view, FieldStructDrawer>> GetFieldStructDrawers()
	{
		// The drawers of the reflected structs that have one, wherever they appear in a field table (a curve inside a vehicle
		// definition), one line per struct, sorted by name:
		//   drawers.emplace_back("<StructName>", &Draw<StructName>);
		std::vector<std::pair<std::string_view, FieldStructDrawer>> drawers;
		return drawers;
	}
}
