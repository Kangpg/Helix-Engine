#pragma once

#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>

namespace helix::mmo
{
	using EntityId = std::uint64_t;
	struct Position { double x = 0; double y = 0; double z = 0; };
	struct EntityState { EntityId id; Position position; std::int32_t health; };
	struct MoveRequest { EntityId entity; Position destination; };
	struct AttackRequest { EntityId attacker; EntityId target; };
	struct MovementContext { EntityState entity; MoveRequest request; std::span<const EntityState> entities; };
	struct CombatContext { EntityState attacker; EntityState target; AttackRequest request; std::span<const EntityState> entities; };
	struct WorldLimits { std::size_t maxEntities; std::size_t maxPendingCommands; std::size_t maxCommandsPerTick; };
	struct TickResult { std::size_t processed = 0; std::size_t applied = 0; std::size_t rejected = 0; };
	struct WorldSnapshot { std::uint64_t tick = 0; std::vector<EntityState> entities; };
}
