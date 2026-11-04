#include "helix/mmo/world_instance.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <variant>

namespace helix::mmo
{
	namespace
	{
		struct Remove { EntityId id; };
		using Command = std::variant<EntityState, Remove, MoveRequest, AttackRequest>;
		bool IsFinite(Position p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
		const EntityState* Locate(std::span<const EntityState> entities, EntityId id)
		{
			const auto it = std::lower_bound(entities.begin(), entities.end(), id,
				[](const EntityState& e, EntityId value) { return e.id < value; });
			return it != entities.end() && it->id == id ? &*it : nullptr;
		}
	}

	struct WorldInstance::Impl
	{
		struct Published { std::shared_ptr<const WorldSnapshot> snapshot; WorldBehaviors behaviors; };
		std::uint64_t id;
		WorldLimits limits;
		WorldBehaviors active;
		WorldBehaviors pending;
		std::mutex queueLock;
		std::mutex tickLock;
		std::deque<Command> commands;
		std::map<EntityId, EntityState> entities;
		std::atomic<std::shared_ptr<const Published>> published;
		std::uint64_t tick = 0;

		Impl(std::uint64_t value, WorldLimits config, WorldBehaviors behaviors)
			: id(value), limits(config), active(std::move(behaviors))
		{
			if (id == 0 || limits.maxEntities == 0 || limits.maxPendingCommands == 0 || limits.maxCommandsPerTick == 0)
				{ throw std::invalid_argument("world id and limits must be positive"); }
			if (!active.movement || !active.combat || !active.visibility)
				{ throw std::invalid_argument("all world behaviors are required"); }
			Publish();
		}

		std::vector<EntityState> States() const
		{
			std::vector<EntityState> result;
			result.reserve(entities.size());
			for (const auto& [key, entity] : entities) { result.push_back(entity); }
			return result;
		}

		void Publish()
		{
			auto snapshot = std::make_shared<const WorldSnapshot>(WorldSnapshot{ tick, States() });
			published.store(std::make_shared<const Published>(Published{ std::move(snapshot), active }));
		}

		bool Queue(Command command)
		{
			std::lock_guard lock(queueLock);
			if (commands.size() >= limits.maxPendingCommands) { return false; }
			commands.push_back(std::move(command));
			return true;
		}

		bool Apply(EntityState entity)
		{
			if (entity.id == 0 || entity.health <= 0 || !IsFinite(entity.position) || entities.size() >= limits.maxEntities)
				{ return false; }
			return entities.emplace(entity.id, entity).second;
		}
		bool Apply(Remove command) { return entities.erase(command.id) != 0; }
		bool Apply(MoveRequest command)
		{
			const auto it = entities.find(command.entity);
			if (it == entities.end() || it->second.health <= 0 || !IsFinite(command.destination)) { return false; }
			const auto states = States();
			const auto next = active.movement->Resolve(MovementContext{ it->second, command, states });
			if (!next || !IsFinite(*next)) { return false; }
			it->second.position = *next;
			return true;
		}
		bool Apply(AttackRequest command)
		{
			const auto attacker = entities.find(command.attacker);
			const auto target = entities.find(command.target);
			if (attacker == entities.end() || target == entities.end() || attacker->second.health <= 0 || target->second.health <= 0)
				{ return false; }
			const auto states = States();
			const auto damage = active.combat->Resolve(CombatContext{ attacker->second, target->second, command, states });
			if (!damage || *damage < 0) { return false; }
			target->second.health -= std::min(target->second.health, *damage);
			return true;
		}
	};

	WorldInstance::WorldInstance(std::uint64_t id, WorldLimits limits, WorldBehaviors behaviors)
		: _impl(std::make_unique<Impl>(id, limits, std::move(behaviors))) {}
	WorldInstance::~WorldInstance() = default;
	std::uint64_t WorldInstance::Id() const noexcept { return _impl->id; }
	bool WorldInstance::QueueAdd(EntityState entity) { return _impl->Queue(entity); }
	bool WorldInstance::QueueRemove(EntityId entity) { return _impl->Queue(Remove{ entity }); }
	bool WorldInstance::QueueMove(MoveRequest request) { return _impl->Queue(request); }
	bool WorldInstance::QueueAttack(AttackRequest request) { return _impl->Queue(request); }

	void WorldInstance::ReplaceMovement(std::shared_ptr<const IMovementPolicy> policy)
	{
		if (!policy) { throw std::invalid_argument("movement policy is required"); }
		std::lock_guard lock(_impl->queueLock);
		_impl->pending.movement.swap(policy);
	}
	void WorldInstance::ReplaceCombat(std::shared_ptr<const ICombatRules> rules)
	{
		if (!rules) { throw std::invalid_argument("combat rules are required"); }
		std::lock_guard lock(_impl->queueLock);
		_impl->pending.combat.swap(rules);
	}
	void WorldInstance::ReplaceVisibility(std::shared_ptr<const IVisibilityPolicy> policy)
	{
		if (!policy) { throw std::invalid_argument("visibility policy is required"); }
		std::lock_guard lock(_impl->queueLock);
		_impl->pending.visibility.swap(policy);
	}

	TickResult WorldInstance::Tick()
	{
		std::unique_lock tickGuard(_impl->tickLock, std::try_to_lock);
		if (!tickGuard.owns_lock()) { throw std::logic_error("Tick must have a single non-reentrant owner"); }
		std::deque<Command> batch;
		WorldBehaviors previous;
		{
			std::lock_guard lock(_impl->queueLock);
			previous = _impl->active; // 이전 구현의 파괴는 queue 락 밖에서.
			if (_impl->pending.movement) { _impl->active.movement = std::move(_impl->pending.movement); }
			if (_impl->pending.combat) { _impl->active.combat = std::move(_impl->pending.combat); }
			if (_impl->pending.visibility) { _impl->active.visibility = std::move(_impl->pending.visibility); }
			while (! _impl->commands.empty() && batch.size() < _impl->limits.maxCommandsPerTick)
			{
				batch.push_back(std::move(_impl->commands.front()));
				_impl->commands.pop_front();
			}
		}
		TickResult result;
		for (const auto& command : batch)
		{
			++result.processed;
			try
			{
				if (std::visit([this](const auto& value) { return _impl->Apply(value); }, command)) { ++result.applied; }
				else { ++result.rejected; }
			}
			catch (...) { ++result.rejected; } // 사용자 규칙 실패는 해당 명령만 거부한다.
		}
		++_impl->tick;
		_impl->Publish();
		return result;
	}

	std::shared_ptr<const WorldSnapshot> WorldInstance::Snapshot() const { return _impl->published.load()->snapshot; }
	std::optional<EntityState> WorldInstance::Find(EntityId id) const
	{
		const auto snapshot = Snapshot();
		const auto* entity = Locate(snapshot->entities, id);
		return entity ? std::optional<EntityState>(*entity) : std::nullopt;
	}
	std::vector<EntityId> WorldInstance::VisibleTo(EntityId observer) const
	{
		const auto view = _impl->published.load(); // 데이터와 AOI 구현이 같은 tick에 고정된다.
		if (!Locate(view->snapshot->entities, observer)) { return {}; }
		auto ids = view->behaviors.visibility->Select(observer, view->snapshot->entities);
		std::erase_if(ids, [&](EntityId id) { return id == observer || !Locate(view->snapshot->entities, id); });
		std::sort(ids.begin(), ids.end());
		ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
		return ids;
	}
}
