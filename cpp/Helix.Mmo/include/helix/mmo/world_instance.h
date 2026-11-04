#pragma once
#include "movement_policy.h"
#include "combat_rules.h"
#include "visibility_policy.h"
#include <memory>
#include <optional>

namespace helix::mmo
{
	struct WorldBehaviors
	{
		std::shared_ptr<const IMovementPolicy> movement;
		std::shared_ptr<const ICombatRules> combat;
		std::shared_ptr<const IVisibilityPolicy> visibility;
	};

	// 상태의 단일 writer는 Tick이다. 네트워크 콜백/외부 스레드는 큐에 요청만 넣는다.
	class WorldInstance final
	{
	public:
		WorldInstance(std::uint64_t id, WorldLimits limits, WorldBehaviors behaviors);
		~WorldInstance();
		WorldInstance(const WorldInstance&) = delete;
		WorldInstance& operator=(const WorldInstance&) = delete;

		[[nodiscard]] std::uint64_t Id() const noexcept;
		bool QueueAdd(EntityState entity);
		bool QueueRemove(EntityId entity);
		bool QueueMove(MoveRequest request);
		bool QueueAttack(AttackRequest request);
		void ReplaceMovement(std::shared_ptr<const IMovementPolicy> policy);
		void ReplaceCombat(std::shared_ptr<const ICombatRules> rules);
		void ReplaceVisibility(std::shared_ptr<const IVisibilityPolicy> policy);
		// 동시/재진입 Tick은 거부. Tick 중 들어온 요청과 교체는 다음 Tick에서 반영.
		TickResult Tick();
		[[nodiscard]] std::shared_ptr<const WorldSnapshot> Snapshot() const;
		[[nodiscard]] std::optional<EntityState> Find(EntityId id) const;
		[[nodiscard]] std::vector<EntityId> VisibleTo(EntityId observer) const;

	private:
		struct Impl;
		std::unique_ptr<Impl> _impl;
	};
}
