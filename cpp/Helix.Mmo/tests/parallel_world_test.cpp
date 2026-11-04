#include "helix/mmo/parallel_world.h"
#include "helix/mmo/delegate_policies.h"
#include "helix/engine_config.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace helix::mmo;
using namespace std::chrono_literals;
namespace
{
	void Check(bool value, const char* text) { if (!value) { throw std::runtime_error(text); } }
	template<class F> void Throws(F f)
	{
		bool thrown = false; try { f(); } catch (const std::exception&) { thrown = true; }
		Check(thrown, "Expected failure");
	}
	WorldBehaviors Behaviors()
	{
		return {
			std::make_shared<DelegateMovementPolicy>([](const MovementContext& c) { return std::optional(c.request.destination); }),
			std::make_shared<DelegateCombatRules>([](const CombatContext&) { return std::optional<std::int32_t>(7); }),
			std::make_shared<DelegateVisibilityPolicy>([](EntityId observer, std::span<const EntityState> states)
			{
				std::vector<EntityId> ids{ observer, 999999 };
				for (const auto& e : states) { ids.push_back(e.id); ids.push_back(e.id); }
				return ids;
			})
		};
	}
	ParallelWorldOptions Options() { return { 8, 4, 10, 65536 }; }
	void Equal(const WorldSnapshot& a, const WorldSnapshot& b)
	{
		Check(a.tick == b.tick && a.entities.size() == b.entities.size(), "Snapshot size/tick mismatch");
		for (std::size_t i = 0; i < a.entities.size(); ++i)
		{
			const auto& x = a.entities[i]; const auto& y = b.entities[i];
			Check(x.id == y.id && x.health == y.health && x.position.x == y.position.x && x.position.y == y.position.y &&
				x.position.z == y.position.z, "Worker count changed world result");
		}
	}
	void GraphContract(std::shared_ptr<IWorldExecutor> executor)
	{
		std::atomic<int> value = 0;
		std::vector<WorldTask> tasks{
			{ [&] { Check(value == 0, "Root order"); value = 1; }, {} },
			{ [&] { value.fetch_add(2); }, { 0 } },
			{ [&] { value.fetch_add(4); }, { 0 } },
			{ [&] { Check(value == 7, "Join order"); }, { 1, 2 } }
		};
		executor->Execute(tasks);
		executor->Execute({});
		Throws([&] { std::vector<WorldTask> bad{ { [] {}, { 1 } }, { [] {}, { 0 } } }; executor->Execute(bad); });
		Throws([&] { std::vector<WorldTask> bad{ { [] {}, { 2 } } }; executor->Execute(bad); });
		Throws([&] { std::vector<WorldTask> bad{ { {}, {} } }; executor->Execute(bad); });
		Throws([&] { std::vector<WorldTask> bad{ { [] { throw std::runtime_error("task"); }, {} } }; executor->Execute(bad); });
		value = 0;
		executor->Execute(tasks); // Exceptions do not poison the executor.
	}
	void Parity(std::shared_ptr<IWorldExecutor> shared)
	{
		ParallelWorldInstance reference(1, { 128, 1024, 1024 }, Behaviors(), Options(), CreateSequentialWorldExecutor());
		ParallelWorldInstance parallel(1, { 128, 1024, 1024 }, Behaviors(), Options(), shared);
		for (auto* world : { &reference, &parallel })
		{
			for (EntityId i = 1; i <= 128; ++i) { Check(world->QueueAdd({ i, { double(i % 16) - 8, 0, double(i / 16) - 4 }, 100000 }), "Seed"); }
			Check(world->Tick().applied == 128, "Seed count");
		}
		const auto old = parallel.Snapshot();
		for (int tick = 0; tick < 24; ++tick)
		{
			for (auto* world : { &reference, &parallel })
			{
				for (EntityId i = 1; i <= 128; ++i)
				{
					world->QueueMove({ i, { double(i % 16) - 8 + tick * .25, 0, double(i / 16) - 4 } });
					world->QueueAttack({ i, tick % 2 ? 1 : i % 128 + 1 });
					if (i % 16 == 0) { world->QueueAreaAttack({ i, { 0, 0, 0 }, 4 }); }
				}
			}
			const auto a = reference.Tick(), b = parallel.Tick();
			Check(a.processed == b.processed && a.applied == b.applied && a.rejected == b.rejected, "Command result parity");
			Equal(*reference.Snapshot(), *parallel.Snapshot());
			for (EntityId id : { 1ull, 64ull, 128ull }) { Check(reference.VisibleTo(id) == parallel.VisibleTo(id), "AOI parity"); }
		}
		Check(old->tick == 1 && old->entities[0].health == 100000 && old->entities[0].position.x == -7, "Old snapshot immutable");
	}
	void CombatSemantics(std::shared_ptr<IWorldExecutor> executor)
	{
		ParallelWorldInstance world(1, { 8, 32, 32 }, Behaviors(), { 1, 2, 3, 2 }, executor);
		for (EntityId i = 1; i <= 4; ++i) { world.QueueAdd({ i, { double(i) - 2, 0, 0 }, 7 }); }
		world.Tick();
		world.QueueAreaAttack({ 1, {}, 100 }); // Three hits exceed the entire-command budget.
		world.QueueAttack({ 1, 2 }); world.QueueAttack({ 2, 1 });
		const auto result = world.Tick();
		Check(result.applied == 2 && result.rejected == 1, "Bounded whole area command admission");
		Check(world.Find(1)->health == 0 && world.Find(2)->health == 0, "Same-phase mutual kills");
		world.QueueAttack({ 1, 3 }); world.QueueMove({ 1, {} });
		Check(world.Tick().rejected == 2, "Dead entities cannot act on subsequent ticks");
		world.QueueAreaAttack({ 3, {}, -1 }); world.QueueAreaAttack({ 3, {}, std::numeric_limits<double>::infinity() });
		Check(world.Tick().rejected == 2, "Malformed area inputs");
		world.QueueAdd({ 5, { 1e200, 0, 0 }, 10 });
		world.QueueMove({ 3, { std::numeric_limits<double>::quiet_NaN(), 0, 0 } });
		Check(world.Tick().rejected == 2, "Unindexable/nonfinite positions");
		Check(world.VisibleTo(4) == std::vector<EntityId>({ 1, 2, 3 }), "Distance/self/duplicate/missing filtering");
		world.ReplaceCombat(std::make_shared<DelegateCombatRules>([](const CombatContext&) -> std::optional<std::int32_t> { throw std::runtime_error("rule"); }));
		world.QueueAttack({ 3, 4 });
		Check(world.Tick().rejected == 1 && world.Find(4)->health == 7, "Throwing combat rule");
		world.ReplaceMovement(std::make_shared<DelegateMovementPolicy>([](const MovementContext&) { return std::optional(Position{ 1e200, 0, 0 }); }));
		world.QueueMove({ 3, {} });
		Check(world.Tick().rejected == 1 && world.Find(3)->position.x == 1, "Invalid policy position");
		world.QueueRemove(4); world.QueueMove({ 4, {} });
		Check(world.Tick().rejected == 1 && !world.Find(4), "Lifecycle precedes movement");
	}
	void ConcurrentExecution(std::shared_ptr<IWorldExecutor> executor)
	{
		std::promise<void> twoEntered, release;
		auto resume = release.get_future().share();
		std::atomic<int> entered = 0;
		auto behaviors = Behaviors();
		behaviors.movement = std::make_shared<DelegateMovementPolicy>([&](const MovementContext& c)
		{
			if (entered.fetch_add(1) == 1) { twoEntered.set_value(); }
			resume.wait();
			return std::optional(c.request.destination);
		});
		ParallelWorldInstance world(1, { 8, 32, 32 }, behaviors, { 1, 4, 10, 32 }, executor);
		world.QueueAdd({ 1, {}, 100 }); world.QueueAdd({ 2, {}, 100 }); world.Tick();
		world.QueueMove({ 1, { 1, 0, 0 } }); world.QueueMove({ 2, { 2, 0, 0 } });
		auto ticker = std::async(std::launch::async, [&] { return world.Tick(); });
		const bool overlapped = twoEntered.get_future().wait_for(5s) == std::future_status::ready;
		// Always release workers before assertions so failures cannot hang teardown.
		world.QueueMove({ 1, { 9, 0, 0 } });
		world.ReplaceMovement(Behaviors().movement);
		bool refused = false;
		try { world.Tick(); } catch (const std::logic_error&) { refused = true; }
		const bool unpublished = world.Snapshot()->tick == 1 && world.Find(1)->position.x == 0;
		release.set_value(); ticker.get();
		Check(overlapped, "A single world must execute movement policies simultaneously");
		Check(refused && unpublished && world.Find(1)->position.x == 1, "Tick exclusion and coherent publication");
		world.Tick();
		Check(world.Find(1)->position.x == 9, "Next tick input and replacement");
		ParallelWorldInstance other(2, { 8, 32, 32 }, Behaviors(), Options(), executor);
		other.QueueAdd({ 99, {}, 100 });
		std::atomic<bool> running = true, coherent = true;
		auto reader = std::async(std::launch::async, [&]
		{
			while (running.load())
			{
				auto snapshot = world.Snapshot();
				if (snapshot->entities.size() != 2 || snapshot->entities[0].id != 1) { coherent = false; }
				world.VisibleTo(1);
			}
		});
		auto a = std::async(std::launch::async, [&] { for (int i = 0; i < 20; ++i) { world.Tick(); } });
		auto b = std::async(std::launch::async, [&] { for (int i = 0; i < 20; ++i) { other.Tick(); } });
		a.get(); b.get(); running = false; reader.get();
		Check(coherent && other.Find(99) && !world.Find(99), "Shared executor worlds and concurrent readers");
	}
	class FailExecutor final : public IWorldExecutor
	{
	public:
		bool fail = false;
		void Execute(std::span<const WorldTask> tasks) override
		{
			if (fail) { tasks.front().run(); throw std::runtime_error("infrastructure failure"); }
			CreateSequentialWorldExecutor()->Execute(tasks);
		}
	};
	void FailureAndLimits()
	{
		auto executor = std::make_shared<FailExecutor>();
		ParallelWorldInstance world(1, { 1, 2, 1 }, Behaviors(), Options(), executor);
		world.QueueAdd({ 1, {}, 100 }); world.QueueAdd({ 2, {}, 100 });
		Check(!world.QueueRemove(1), "Bounded queue");
		Check(world.Tick().applied == 1 && world.Tick().rejected == 1, "Entity/command budget");
		const auto before = world.Snapshot();
		world.QueueMove({ 1, { 9, 0, 0 } }); executor->fail = true;
		Throws([&] { world.Tick(); });
		Check(world.Snapshot() == before, "No partial publication after executor failure");
		executor->fail = false; world.Tick();
		Check(world.Find(1)->position.x == 0, "Infrastructure failure batch is consumed, not replayed");
		Throws([&] { ParallelWorldInstance invalid(0, { 1, 1, 1 }, Behaviors(), Options(), executor); });
		Throws([&] { ParallelWorldInstance invalid(1, { 1, 1, 1 }, Behaviors(), { 0, 4, 10, 32 }, executor); });
		Throws([&] { CreateTaskflowWorldExecutor(0); });
		bool reentryRefused = false;
		world.ReplaceMovement(std::make_shared<DelegateMovementPolicy>([&](const MovementContext& c)
		{
			try { world.Tick(); } catch (const std::logic_error&) { reentryRefused = true; }
			return std::optional(c.request.destination);
		}));
		world.QueueMove({ 1, { 2, 0, 0 } });
		Check(world.Tick().applied == 1 && reentryRefused, "Same-thread Tick reentry must fail without locking recursively");
	}
	void SpatialOracle()
	{
		for (double radius : { 0.0, 2.5, 10.0, 1e300 })
		{
			ParallelWorldInstance world(1, { 64, 128, 128 }, Behaviors(), { 3, 2, radius, 128 }, CreateTaskflowWorldExecutor(3));
			for (EntityId id = 1; id <= 64; ++id)
			{
				world.QueueAdd({ id, { double(id % 8) - 4, double(id % 3), double(id / 8) - 4 }, 100 });
			}
			world.Tick();
			const auto snapshot = world.Snapshot();
			for (const auto& observer : snapshot->entities)
			{
				std::vector<EntityId> expected;
				for (const auto& target : snapshot->entities)
				{
					if (target.id != observer.id && std::hypot(target.position.x - observer.position.x,
						target.position.y - observer.position.y, target.position.z - observer.position.z) <= radius) { expected.push_back(target.id); }
				}
				Check(world.VisibleTo(observer.id) == expected, "Grid query must match independent all-entity distance oracle");
			}
		}
	}
	class ScopedEnv
	{
	public:
		explicit ScopedEnv(const char* key) : _key(key)
		{
			if (const auto* value = std::getenv(key)) { _old = value; }
			Set("");
		}
		~ScopedEnv() { Set(_old.value_or("")); }
		void Set(const std::string& value)
		{
#ifdef _WIN32
			_putenv_s(_key, value.c_str());
#else
			if (value.empty()) { unsetenv(_key); } else { setenv(_key, value.c_str(), 1); }
#endif
		}
	private:
		const char* _key;
		std::optional<std::string> _old;
	};
	void Configuration()
	{
		ScopedEnv execution("HELIX_MMO__EXECUTION"), workers("HELIX_MMO__WORKER_THREADS"), batch("HELIX_MMO__BATCH_SIZE"),
			cell("HELIX_MMO__CELL_SIZE"), radius("HELIX_MMO__VISIBILITY_RADIUS"), hits("HELIX_MMO__MAX_HITS_PER_TICK");
		const auto file = std::filesystem::temp_directory_path() / ("helix-parallel-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".toml");
		{ std::ofstream out(file); out << "[mmo]\nexecution=parallel\nworker_threads=3\nbatch_size=7\ncell_size=2.5\nvisibility_radius=0\nmax_hits_per_tick=123\n"; }
		helix::config::ConfigLoader loader;
		const auto config = loader.Load(file.string());
		Check(config && config->worldExecution == "parallel" && config->worldWorkerThreads == 3 && config->worldBatchSize == 7 &&
			config->worldCellSize == 2.5 && config->worldVisibilityRadius == 0 && config->worldMaxHitsPerTick == 123, "Parallel settings consumed");
		batch.Set("12");
		Check(loader.Load(file.string())->worldBatchSize == 12, "Environment must override file");
		batch.Set("bad"); Check(!loader.Load(file.string()), "Invalid environment must not silently fall back"); batch.Set("");
		for (const auto* invalid : { "worker_threads=0", "batch_size=-1", "max_hits_per_tick=1x", "cell_size=nan", "visibility_radius=-1", "execution=magic" })
		{
			{ std::ofstream out(file); out << "[mmo]\n" << invalid << '\n'; }
			Check(!loader.Load(file.string()), "Invalid parallel setting must fail");
		}
		std::filesystem::remove(file);
	}
}

int main()
{
	try
	{
		GraphContract(CreateSequentialWorldExecutor());
		// Graph root expects a clean value on every run; reset it within root.
		for (auto workers : { 1u, 2u, 4u, 8u })
		{
			auto executor = CreateTaskflowWorldExecutor(workers);
			GraphContract(executor); Parity(executor); CombatSemantics(executor);
			if (workers > 1) { ConcurrentExecution(executor); }
		}
		FailureAndLimits(); SpatialOracle(); Configuration();
		std::cout << "PASS: staged parity (1/2/4/8), real parallel execution, area/hot-target combat, bounded inputs, policy replacement, publication, shared executor, failures and configuration\n";
		return 0;
	}
	catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
