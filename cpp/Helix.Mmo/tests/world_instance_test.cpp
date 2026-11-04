#include "helix/mmo/delegate_policies.h"
#include "helix/mmo/world_instance.h"
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace helix::mmo;
void Check(bool condition, const char* message) { if (!condition) { throw std::runtime_error(message); } }

class FixedDamage final : public ICombatRules
{
public:
	std::optional<std::int32_t> Resolve(const CombatContext&) const override { return 7; }
};

WorldBehaviors Behaviors()
{
	return {
		std::make_shared<DelegateMovementPolicy>([](const MovementContext& c) { return std::optional(c.request.destination); }),
		std::make_shared<FixedDamage>(),
		std::make_shared<DelegateVisibilityPolicy>([](EntityId, std::span<const EntityState>) { return std::vector<EntityId>{ 1, 2, 2, 99 }; })
	};
}

int main()
{
	try
	{
		WorldInstance field(1, { 10, 20, 20 }, Behaviors());
		WorldInstance arena(2, { 10, 20, 20 }, Behaviors());
		for (auto* world : { &field, &arena })
		{
			Check(world->QueueAdd({ 1, {}, 100 }) && world->QueueAdd({ 2, {}, 100 }), "enqueue initial entities");
			Check(!world->Find(1), "commands must wait for Tick");
			Check(world->Tick().applied == 2, "initial Tick");
		}
		const auto oldSnapshot = field.Snapshot();
		field.ReplaceMovement(std::make_shared<DelegateMovementPolicy>([](const MovementContext&) { return std::optional(Position{ 3, 0, 0 }); }));
		field.ReplaceCombat(std::make_shared<DelegateCombatRules>([](const CombatContext&) { return std::optional<std::int32_t>(20); }));
		field.QueueMove({ 1, { 9, 0, 0 } });
		field.QueueAttack({ 1, 2 });
		arena.QueueMove({ 1, { 9, 0, 0 } });
		arena.QueueAttack({ 1, 2 });
		field.Tick(); arena.Tick();
		Check(field.Find(1)->position.x == 3 && arena.Find(1)->position.x == 9, "movement replacement must be instance-local");
		Check(field.Find(2)->health == 80 && arena.Find(2)->health == 93, "class and delegate combat contracts");
		Check(oldSnapshot->entities[1].health == 100, "old snapshot must remain immutable");
		Check(field.VisibleTo(1) == std::vector<EntityId>{ 2 }, "AOI must filter self, duplicates and invalid ids");
		field.ReplaceVisibility(std::make_shared<DelegateVisibilityPolicy>([](EntityId, std::span<const EntityState>) { return std::vector<EntityId>{}; }));
		Check(field.VisibleTo(1).size() == 1, "AOI replacement must wait for Tick");
		field.Tick();
		Check(field.VisibleTo(1).empty() && arena.VisibleTo(1).size() == 1, "AOI replacement must be instance-local");

		std::promise<void> entered, resume;
		auto resumed = resume.get_future().share();
		WorldInstance busy(3, { 10, 20, 20 }, Behaviors());
		busy.QueueAdd({ 1, {}, 100 }); busy.Tick();
		busy.ReplaceMovement(std::make_shared<DelegateMovementPolicy>([&](const MovementContext& c)
		{
			entered.set_value(); resumed.wait(); return std::optional(c.request.destination);
		}));
		busy.QueueMove({ 1, { 1, 0, 0 } });
		std::thread ticker([&] { busy.Tick(); });
		entered.get_future().wait();
		busy.ReplaceMovement(std::make_shared<DelegateMovementPolicy>([](const MovementContext&) { return std::optional(Position{ 8, 0, 0 }); }));
		busy.QueueMove({ 1, { 2, 0, 0 } });
		bool overlapRejected = false;
		try { busy.Tick(); } catch (const std::logic_error&) { overlapRejected = true; }
		resume.set_value(); ticker.join();
		Check(overlapRejected && busy.Find(1)->position.x == 1, "active Tick must retain its original policy and batch");
		busy.Tick();
		Check(busy.Find(1)->position.x == 8, "during-Tick replacement must apply next Tick");

		WorldInstance bounded(4, { 1, 2, 1 }, Behaviors());
		Check(bounded.QueueAdd({ 1, {}, 100 }) && bounded.QueueAdd({ 2, {}, 100 }), "queue capacity");
		Check(!bounded.QueueRemove(1), "queue backpressure");
		Check(bounded.Tick().processed == 1 && bounded.Tick().rejected == 1, "command budget and entity limit");
		bounded.ReplaceMovement(std::make_shared<DelegateMovementPolicy>([](const MovementContext&) { return std::optional(Position{ std::numeric_limits<double>::quiet_NaN(), 0, 0 }); }));
		bounded.QueueMove({ 1, {} });
		Check(bounded.Tick().rejected == 1 && bounded.Find(1)->position.x == 0, "invalid rule output cannot mutate state");
		bounded.ReplaceMovement(std::make_shared<DelegateMovementPolicy>([](const MovementContext&) -> std::optional<Position> { throw std::runtime_error("user rule"); }));
		bounded.QueueMove({ 1, {} });
		Check(bounded.Tick().rejected == 1, "throwing rule must reject command");
		std::cout << "PASS: instance isolation, interface/delegate parity, all replacements, tick boundary, immutable snapshots, limits and rule failures\n";
		return 0;
	}
	catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
