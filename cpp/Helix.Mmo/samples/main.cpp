#include "helix/engine_config.h"
#include "helix/mmo/delegate_policies.h"
#include "helix/mmo/world_instance.h"
#include <iostream>

int main(int argc, char** argv)
{
	helix::config::ConfigLoader loader;
	const auto config = loader.Load(argc > 1 ? argv[1] : "config/helix.toml");
	if (!config) { std::cerr << loader.GetError() << '\n'; return 1; }
	using namespace helix::mmo;
	WorldBehaviors behaviors{
		std::make_shared<DelegateMovementPolicy>([](const MovementContext& c) { return std::optional(c.request.destination); }),
		std::make_shared<DelegateCombatRules>([](const CombatContext&) { return std::optional<std::int32_t>(0); }),
		std::make_shared<DelegateVisibilityPolicy>([](EntityId observer, std::span<const EntityState> entities)
		{
			std::vector<EntityId> ids;
			for (const auto& entity : entities) { if (entity.id != observer) { ids.push_back(entity.id); } }
			return ids;
		})
	};
	WorldLimits limits{ config->worldMaxEntities, config->worldMaxPendingCommands, config->worldMaxCommandsPerTick };
	WorldInstance field(1, limits, behaviors);
	WorldInstance arena(2, limits, behaviors);
	field.QueueAdd({ 1, {}, 100 });
	field.QueueAdd({ 2, {}, 100 });
	field.Tick();
	field.ReplaceCombat(std::make_shared<DelegateCombatRules>([](const CombatContext&) { return std::optional<std::int32_t>(10); }));
	field.QueueAttack({ 1, 2 });
	field.Tick();
	std::cout << "field target HP=" << field.Find(2)->health << "; independent arena entities=" << arena.Snapshot()->entities.size() << '\n';
}
