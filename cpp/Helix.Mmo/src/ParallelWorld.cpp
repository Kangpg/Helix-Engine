#include "helix/mmo/parallel_world.h"
#include "SpatialIndex.h"
#include <atomic>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <variant>

namespace helix::mmo
{
	namespace
	{
		struct Remove { EntityId id; };
		using Command = std::variant<EntityState, Remove, MoveRequest, AttackRequest, AreaAttackRequest>;
		std::size_t Locate(std::span<const EntityState> states, EntityId id)
		{
			const auto it = std::lower_bound(states.begin(), states.end(), id, [](const auto& e, EntityId value) { return e.id < value; });
			return it != states.end() && it->id == id ? static_cast<std::size_t>(it - states.begin()) : states.size();
		}
	}

	struct ParallelWorldInstance::Impl
	{
		struct Published
		{
			std::shared_ptr<const WorldSnapshot> snapshot;
			WorldBehaviors behaviors;
			detail::SpatialIndex index;
		};
		std::uint64_t id;
		WorldLimits limits;
		ParallelWorldOptions options;
		std::shared_ptr<IWorldExecutor> executor;
		std::mutex queueLock;
		std::atomic_flag ticking;
		std::vector<Command> commands;
		WorldBehaviors active, pending;
		std::atomic<std::shared_ptr<const Published>> published;

		Impl(std::uint64_t value, WorldLimits bounds, WorldBehaviors behaviors, ParallelWorldOptions settings,
			std::shared_ptr<IWorldExecutor> runner)
			: id(value), limits(bounds), options(settings), executor(std::move(runner)), active(std::move(behaviors))
		{
			if (id == 0 || limits.maxEntities == 0 || limits.maxPendingCommands == 0 || limits.maxCommandsPerTick == 0 ||
				options.batchSize == 0 || options.maxHitsPerTick == 0 || !std::isfinite(options.cellSize) || options.cellSize <= 0 ||
				!std::isfinite(options.visibilityRadius) || options.visibilityRadius < 0 || !executor ||
				!active.movement || !active.combat || !active.visibility)
				{ throw std::invalid_argument("Invalid parallel world configuration"); }
			published.store(std::make_shared<const Published>(Published{ std::make_shared<const WorldSnapshot>(), active,
				detail::SpatialIndex(options.cellSize) }));
		}

		bool Queue(Command command)
		{
			std::lock_guard lock(queueLock);
			if (commands.size() >= limits.maxPendingCommands) { return false; }
			commands.push_back(std::move(command));
			return true;
		}
	};

	ParallelWorldInstance::ParallelWorldInstance(std::uint64_t id, WorldLimits limits, WorldBehaviors behaviors,
		ParallelWorldOptions options, std::shared_ptr<IWorldExecutor> executor)
		: _impl(std::make_unique<Impl>(id, limits, std::move(behaviors), options, std::move(executor))) {}
	ParallelWorldInstance::~ParallelWorldInstance() = default;
	std::uint64_t ParallelWorldInstance::Id() const noexcept { return _impl->id; }
	bool ParallelWorldInstance::QueueAdd(EntityState e) { return _impl->Queue(e); }
	bool ParallelWorldInstance::QueueRemove(EntityId e) { return _impl->Queue(Remove{ e }); }
	bool ParallelWorldInstance::QueueMove(MoveRequest r) { return _impl->Queue(r); }
	bool ParallelWorldInstance::QueueAttack(AttackRequest r) { return _impl->Queue(r); }
	bool ParallelWorldInstance::QueueAreaAttack(AreaAttackRequest r) { return _impl->Queue(r); }
	void ParallelWorldInstance::ReplaceMovement(std::shared_ptr<const IMovementPolicy> p)
	{
		if (!p) { throw std::invalid_argument("Missing movement policy"); }
		std::lock_guard lock(_impl->queueLock); _impl->pending.movement.swap(p);
	}
	void ParallelWorldInstance::ReplaceCombat(std::shared_ptr<const ICombatRules> p)
	{
		if (!p) { throw std::invalid_argument("Missing combat rules"); }
		std::lock_guard lock(_impl->queueLock); _impl->pending.combat.swap(p);
	}
	void ParallelWorldInstance::ReplaceVisibility(std::shared_ptr<const IVisibilityPolicy> p)
	{
		if (!p) { throw std::invalid_argument("Missing visibility policy"); }
		std::lock_guard lock(_impl->queueLock); _impl->pending.visibility.swap(p);
	}

	TickResult ParallelWorldInstance::Tick()
	{
		if (_impl->ticking.test_and_set(std::memory_order_acquire)) { throw std::logic_error("Overlapping/reentrant parallel world Tick"); }
		struct TickGuard
		{
			std::atomic_flag& flag;
			~TickGuard() { flag.clear(std::memory_order_release); }
		} tickGuard{ _impl->ticking };
		std::vector<Command> batch;
		WorldBehaviors retired;
		{
			std::lock_guard lock(_impl->queueLock);
			retired = _impl->active; // User destructors run after the queue lock is released.
			if (_impl->pending.movement) { _impl->active.movement = std::move(_impl->pending.movement); }
			if (_impl->pending.combat) { _impl->active.combat = std::move(_impl->pending.combat); }
			if (_impl->pending.visibility) { _impl->active.visibility = std::move(_impl->pending.visibility); }
			const auto count = std::min(_impl->commands.size(), _impl->limits.maxCommandsPerTick);
			batch.assign(_impl->commands.begin(), _impl->commands.begin() + count);
			_impl->commands.erase(_impl->commands.begin(), _impl->commands.begin() + count);
		}
		const auto behaviors = _impl->active;
		const auto previous = _impl->published.load();
		if (previous->snapshot->tick == std::numeric_limits<std::uint64_t>::max()) { throw std::overflow_error("World tick exhausted"); }
		// All mutable buffers belong to this Tick. Infrastructure failure consumes
		// its input batch but never publishes a partially changed world.
		std::vector<unsigned char> applied(batch.size(), 0);
		detail::SpatialIndex index(_impl->options.cellSize);
		auto initial = previous->snapshot->entities;
		if (std::any_of(batch.begin(), batch.end(), [](const auto& c) { return std::holds_alternative<EntityState>(c) || std::holds_alternative<Remove>(c); }))
		{
			std::map<EntityId, EntityState> lifecycle;
			for (const auto& e : initial) { lifecycle.emplace(e.id, e); }
			for (std::size_t c = 0; c < batch.size(); ++c)
			{
				if (const auto* add = std::get_if<EntityState>(&batch[c]))
				{
					if (add->id && add->health > 0 && index.CanIndex(add->position) && lifecycle.size() < _impl->limits.maxEntities)
						{ applied[c] = lifecycle.emplace(add->id, *add).second; }
				}
				else if (const auto* remove = std::get_if<Remove>(&batch[c])) { applied[c] = lifecycle.erase(remove->id) != 0; }
			}
			initial.clear(); initial.reserve(lifecycle.size());
			for (const auto& [id, e] : lifecycle) { initial.push_back(e); }
		}
		auto moved = initial;
		std::vector<std::vector<std::size_t>> moves(initial.size());
		for (std::size_t c = 0; c < batch.size(); ++c)
		{
			if (const auto* move = std::get_if<MoveRequest>(&batch[c]))
			{
				const auto i = Locate(initial, move->entity);
				if (i != initial.size()) { moves[i].push_back(c); }
			}
		}

		struct Hit { std::size_t command, attacker, target; std::int32_t damage = -1; };
		std::vector<Hit> hits;
		std::vector<std::vector<std::size_t>> targets(initial.size());
		std::vector<EntityState> next;
		std::vector<WorldTask> tasks;
		const auto addStage = [&](std::size_t count, const std::vector<std::size_t>& dependencies, auto work, std::size_t grain)
		{
			std::vector<std::size_t> nodes;
			// At least one node preserves dependencies for empty stages.
			for (std::size_t begin = 0;;)
			{
				const auto end = begin + std::min(grain, count - begin);
				nodes.push_back(tasks.size());
				tasks.push_back({ [work, begin, end] { work(begin, end); }, dependencies });
				if (end == count) { break; }
				begin = end;
			}
			return nodes;
		};
		const auto moveNodes = addStage(initial.size(), {}, [&](std::size_t begin, std::size_t end)
		{
			for (auto i = begin; i < end; ++i)
			{
				for (auto c : moves[i])
				{
					const auto& request = std::get<MoveRequest>(batch[c]);
					if (moved[i].health <= 0 || !index.CanIndex(request.destination)) { continue; }
					try
					{
						const auto p = behaviors.movement->Resolve({ moved[i], request, initial });
						if (p && index.CanIndex(*p)) { moved[i].position = *p; applied[c] = 1; }
					}
					catch (...) { /* A user rule failure rejects this command only. */ }
				}
			}
		}, _impl->options.batchSize);
		const auto planNode = tasks.size();
		tasks.push_back({ [&]
		{
			index.Build(moved);
			next = moved;
			for (std::size_t c = 0; c < batch.size(); ++c)
			{
				EntityId attackerId = 0;
				std::vector<std::size_t> candidates;
				if (const auto* attack = std::get_if<AttackRequest>(&batch[c]))
				{
					attackerId = attack->attacker;
					const auto target = Locate(moved, attack->target);
					if (target != moved.size() && moved[target].health > 0) { candidates.push_back(target); }
				}
				else if (const auto* area = std::get_if<AreaAttackRequest>(&batch[c]))
				{
					attackerId = area->attacker;
					if (!detail::Finite(area->center) || !std::isfinite(area->radius) || area->radius < 0) { continue; }
					candidates = index.Query(moved, area->center, area->radius);
					std::erase_if(candidates, [&](auto i) { return moved[i].id == attackerId || moved[i].health <= 0; });
				}
				else { continue; }
				const auto attacker = Locate(moved, attackerId);
				if (attacker == moved.size() || moved[attacker].health <= 0 ||
					candidates.size() > _impl->options.maxHitsPerTick - hits.size()) { continue; }
				for (auto target : candidates) { hits.push_back({ c, attacker, target }); }
			}
		}, moveNodes });
		// The planner determines hit count at runtime. Partition hits into
		// contiguous lanes to avoid interleaved writes to neighboring cache lines.
		const auto workCount = std::max(initial.size(), batch.size());
		const auto lanes = std::max<std::size_t>(1, workCount / _impl->options.batchSize + (workCount % _impl->options.batchSize != 0));
		const auto hitNodes = addStage(lanes, { planNode }, [&](std::size_t begin, std::size_t end)
		{
			for (auto lane = begin; lane < end; ++lane)
			{
				const auto laneBegin = hits.size() / lanes * lane + std::min(lane, hits.size() % lanes);
				const auto laneEnd = laneBegin + hits.size() / lanes + (lane < hits.size() % lanes);
				for (auto h = laneBegin; h < laneEnd; ++h)
				{
					auto& hit = hits[h];
					try
					{
						const auto damage = behaviors.combat->Resolve({ moved[hit.attacker], moved[hit.target],
							{ moved[hit.attacker].id, moved[hit.target].id }, moved });
						if (damage && *damage >= 0) { hit.damage = *damage; }
					}
					catch (...) { /* Rejected hit; no writes to published state. */ }
				}
			}
		}, 1);
		const auto groupNode = tasks.size();
		tasks.push_back({ [&]
		{
			for (std::size_t h = 0; h < hits.size(); ++h)
			{
				if (hits[h].damage >= 0) { targets[hits[h].target].push_back(h); applied[hits[h].command] = 1; }
			}
		}, hitNodes });
		addStage(initial.size(), { groupNode }, [&](std::size_t begin, std::size_t end)
		{
			for (auto i = begin; i < end; ++i)
			{
				for (auto h : targets[i]) { next[i].health -= std::min(next[i].health, hits[h].damage); }
			}
		}, _impl->options.batchSize);
		// Publication is deliberately outside the executor: executor failure may
		// not expose a half-completed graph, even if some tasks already finished.
		_impl->executor->Execute(tasks);
		auto snapshot = std::make_shared<const WorldSnapshot>(WorldSnapshot{ previous->snapshot->tick + 1, std::move(next) });
		_impl->published.store(std::make_shared<const Impl::Published>(Impl::Published{ std::move(snapshot), behaviors, std::move(index) }));
		TickResult result{ batch.size(), 0, 0 };
		for (auto accepted : applied) { result.applied += accepted; }
		result.rejected = result.processed - result.applied;
		return result;
	}

	std::shared_ptr<const WorldSnapshot> ParallelWorldInstance::Snapshot() const { return _impl->published.load()->snapshot; }
	std::optional<EntityState> ParallelWorldInstance::Find(EntityId id) const
	{
		const auto snapshot = Snapshot();
		const auto i = Locate(snapshot->entities, id);
		return i == snapshot->entities.size() ? std::nullopt : std::optional(snapshot->entities[i]);
	}
	std::vector<EntityId> ParallelWorldInstance::VisibleTo(EntityId observer) const
	{
		const auto view = _impl->published.load();
		const auto& states = view->snapshot->entities;
		const auto i = Locate(states, observer);
		if (i == states.size()) { return {}; }
		std::vector<EntityState> candidates;
		for (auto candidate : view->index.Query(states, states[i].position, _impl->options.visibilityRadius)) { candidates.push_back(states[candidate]); }
		auto ids = view->behaviors.visibility->Select(observer, candidates);
		std::erase_if(ids, [&](EntityId id) { return id == observer || Locate(candidates, id) == candidates.size(); });
		std::sort(ids.begin(), ids.end());
		ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
		return ids;
	}
}
