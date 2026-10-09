#include "Editor/HierarchyEditing.h"

#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace Strada
{
	namespace HierarchyEditing
	{
		std::optional<MoveTarget> ResolveDrop(Scene& scene, std::span<UUID const> dragged, UUID target, DropPosition position)
		{
			Entity const targetEntity = scene.GetEntityByUUID(target);
			if (!targetEntity || dragged.empty())
			{
				return std::nullopt;
			}
			std::vector<Entity> draggedEntities;
			for (UUID const id : dragged)
			{
				Entity const entity = scene.GetEntityByUUID(id);
				if (!entity)
				{
					return std::nullopt;
				}
				draggedEntities.push_back(entity);
			}
			auto const isDragged = [&dragged](UUID id)
			{
				return std::find(dragged.begin(), dragged.end(), id) != dragged.end();
			};

			Entity const parent = position == DropPosition::Inside ? targetEntity : scene.GetParent(targetEntity);
			// Nothing may end up inside its own subtree.
			for (Entity const entity : draggedEntities)
			{
				if (parent && (parent == entity || scene.IsDescendantOf(parent, entity)))
				{
					return std::nullopt;
				}
			}

			MoveTarget move;
			move.Parent = parent ? parent.GetUUID() : UUID::Invalid();
			if (position == DropPosition::Inside)
			{
				return move;
			}

			// The anchor is the first sibling from the drop point on that is not moved itself.
			std::vector<UUID> const siblings = [&]
			{
				std::vector<UUID> ids;
				std::vector<Entity> const entities = parent ? scene.GetChildren(parent) : scene.GetRootEntities();
				for (Entity const entity : entities)
				{
					ids.push_back(entity.GetUUID());
				}
				return ids;
			}();
			auto it = std::find(siblings.begin(), siblings.end(), target);
			if (it != siblings.end() && position == DropPosition::After)
			{
				++it;
			}
			while (it != siblings.end() && isDragged(*it))
			{
				++it;
			}
			move.InsertBefore = it != siblings.end() ? *it : UUID::Invalid();
			return move;
		}

		std::vector<UUID> GetRange(std::span<UUID const> displayed, UUID anchor, UUID clicked)
		{
			auto const anchorIt = std::find(displayed.begin(), displayed.end(), anchor);
			auto const clickedIt = std::find(displayed.begin(), displayed.end(), clicked);
			if (anchorIt == displayed.end() || clickedIt == displayed.end())
			{
				return {clicked};
			}
			auto const first = std::min(anchorIt, clickedIt);
			auto const last = std::max(anchorIt, clickedIt);
			return std::vector<UUID>(first, last + 1);
		}

		std::vector<UUID> FindByName(Scene& scene, std::string_view filter)
		{
			auto const lower = [](std::string_view text)
			{
				std::string result(text);
				std::transform(result.begin(), result.end(), result.begin(),
				               [](unsigned char c)
				               {
								   return static_cast<char>(std::tolower(c));
							   });
				return result;
			};
			std::string const needle = lower(filter);
			std::vector<UUID> matches;
			scene.ForEachEntityInHierarchyOrder(
				[&](Entity entity)
				{
					if (lower(entity.GetName()).find(needle) != std::string::npos)
					{
						matches.push_back(entity.GetUUID());
					}
				});
			return matches;
		}

		std::vector<UUID> GetAncestors(Scene& scene, UUID entity)
		{
			std::vector<UUID> ancestors;
			Entity current = scene.GetEntityByUUID(entity);
			if (!current)
			{
				return ancestors;
			}
			// Bounded by the entity count so a corrupted hierarchy cannot loop forever.
			for (size_t guard = 0; guard < scene.GetEntityCount(); guard++)
			{
				current = scene.GetParent(current);
				if (!current)
				{
					break;
				}
				ancestors.push_back(current.GetUUID());
			}
			std::reverse(ancestors.begin(), ancestors.end());
			return ancestors;
		}
	}
}
