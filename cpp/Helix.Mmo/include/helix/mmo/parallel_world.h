#pragma once
#include "world_instance.h"
#include "world_executor.h"

namespace helix::mmo
{
	struct ParallelWorldOptions
	{
		std::size_t batchSize;
		double cellSize;
		double visibilityRadius;
		std::size_t maxHitsPerTick;
	};
	struct AreaAttackRequest { EntityId attacker; Position center; double radius; };

	// C++ experiment: lifecycle -> movement -> spatial index -> hit evaluation ->
	// target-partitioned application -> publication. Each stage may have MANY
	// simultaneous state writers, exclusively to disjoint entity slots.
	// Movement/combat policies MUST be pure, deterministic, concurrent-call safe.
	// Tick coordinates a DAG; overlapping/reentrant Tick on one world is rejected.
	class ParallelWorldInstance final
	{
	public:
		ParallelWorldInstance(std::uint64_t id, WorldLimits limits, WorldBehaviors behaviors,
			ParallelWorldOptions options, std::shared_ptr<IWorldExecutor> executor);
		~ParallelWorldInstance();
		ParallelWorldInstance(const ParallelWorldInstance&) = delete;
		ParallelWorldInstance& operator=(const ParallelWorldInstance&) = delete;
		std::uint64_t Id() const noexcept;
		bool QueueAdd(EntityState entity);
		bool QueueRemove(EntityId entity);
		bool QueueMove(MoveRequest request);
		bool QueueAttack(AttackRequest request);
		bool QueueAreaAttack(AreaAttackRequest request);
		void ReplaceMovement(std::shared_ptr<const IMovementPolicy> policy);
		void ReplaceCombat(std::shared_ptr<const ICombatRules> rules);
		void ReplaceVisibility(std::shared_ptr<const IVisibilityPolicy> policy);
		TickResult Tick();
		std::shared_ptr<const WorldSnapshot> Snapshot() const;
		std::optional<EntityState> Find(EntityId id) const;
		std::vector<EntityId> VisibleTo(EntityId observer) const;
	private:
		struct Impl;
		std::unique_ptr<Impl> _impl;
	};
}
