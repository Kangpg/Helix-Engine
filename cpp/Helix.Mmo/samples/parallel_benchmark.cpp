#include "helix/mmo/parallel_world.h"
#include "helix/mmo/delegate_policies.h"
#include "helix/engine_config.h"
#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace helix::mmo;
namespace
{
	std::size_t Number(const char* text)
	{
		std::size_t result = 0; const auto end = text + std::char_traits<char>::length(text);
		const auto parsed = std::from_chars(text, end, result);
		if (parsed.ec != std::errc{} || parsed.ptr != end) { throw std::invalid_argument("Invalid integer argument"); }
		return result;
	}
	Position Start(std::size_t i, const std::string& scenario)
	{
		const auto spacing = scenario == "distributed" ? 4.0 : 0.05;
		return { double(i % 64) * spacing, 0, double(i / 64) * spacing };
	}
	std::uint64_t Hash(const WorldSnapshot& snapshot)
	{
		std::uint64_t hash = 14695981039346656037ull;
		const auto add = [&](std::uint64_t value) { for (int b = 0; b < 8; ++b) { hash ^= (value >> (b * 8)) & 255; hash *= 1099511628211ull; } };
		add(snapshot.tick);
		for (const auto& e : snapshot.entities)
		{
			add(e.id); add(std::bit_cast<std::uint64_t>(e.position.x)); add(std::bit_cast<std::uint64_t>(e.position.y));
			add(std::bit_cast<std::uint64_t>(e.position.z)); add(static_cast<std::uint64_t>(e.health));
		}
		return hash;
	}
}

int main(int argc, char** argv)
{
	try
	{
#ifndef NDEBUG
		throw std::runtime_error("Run this benchmark in Release");
#endif
		if (argc != 9) { throw std::invalid_argument("Usage: benchmark config entities ticks warmup workers(0=inline) distributed|dense|boss area_radius query_count"); }
		helix::config::ConfigLoader loader;
		const auto config = loader.Load(argv[1]);
		if (!config) { throw std::runtime_error(loader.GetError()); }
		const auto entities = Number(argv[2]), ticks = Number(argv[3]), warmup = Number(argv[4]), workers = Number(argv[5]);
		const std::string scenario = argv[6];
		double radius = 0;
		const std::string radiusText = argv[7];
		const auto parsed = std::from_chars(radiusText.data(), radiusText.data() + radiusText.size(), radius);
		const auto queries = Number(argv[8]);
		if (parsed.ec != std::errc{} || parsed.ptr != radiusText.data() + radiusText.size() || !std::isfinite(radius) || radius < 0 ||
			entities == 0 || ticks == 0 || warmup > std::numeric_limits<std::size_t>::max() - ticks ||
			entities > config->worldMaxEntities || entities > config->worldMaxPendingCommands / 2 || entities > config->worldMaxCommandsPerTick / 2 ||
			(scenario != "distributed" && scenario != "dense" && scenario != "boss")) { throw std::invalid_argument("Invalid workload/configuration"); }
		auto executor = workers ? CreateTaskflowWorldExecutor(workers) : CreateSequentialWorldExecutor();
		WorldBehaviors behaviors{
			std::make_shared<DelegateMovementPolicy>([](const MovementContext& c) { return std::optional(c.request.destination); }),
			std::make_shared<DelegateCombatRules>([](const CombatContext& c)
			{
				const auto distance = std::hypot(c.attacker.position.x - c.target.position.x, c.attacker.position.z - c.target.position.z);
				return std::optional<std::int32_t>(7 + static_cast<std::int32_t>(std::fmod(distance, 5.0)));
			}),
			std::make_shared<DelegateVisibilityPolicy>([](EntityId observer, std::span<const EntityState> states)
			{
				std::vector<EntityId> ids; ids.reserve(states.size());
				for (const auto& e : states) { if (e.id != observer) { ids.push_back(e.id); } }
				return ids;
			})
		};
		ParallelWorldInstance world(1, { config->worldMaxEntities, config->worldMaxPendingCommands, config->worldMaxCommandsPerTick }, behaviors,
			{ config->worldBatchSize, config->worldCellSize, config->worldVisibilityRadius, config->worldMaxHitsPerTick }, executor);
		for (std::size_t i = 0; i < entities; ++i) { if (!world.QueueAdd({ i + 1, Start(i, scenario), std::numeric_limits<std::int32_t>::max() })) { throw std::runtime_error("Seed queue full"); } }
		world.Tick();
		std::vector<double> times; times.reserve(ticks);
		std::size_t accepted = 0, rejected = 0, visible = 0;
		double queryUs = 0;
		for (std::size_t tick = 0; tick < warmup + ticks; ++tick)
		{
			for (std::size_t i = 0; i < entities; ++i)
			{
				auto position = Start(i, scenario); position.x += double(tick % 16) * .001;
				if (!world.QueueMove({ i + 1, position })) { throw std::runtime_error("Movement queue full"); }
				const bool queued = scenario != "boss" && i % 64 == 0 ? world.QueueAreaAttack({ i + 1, position, radius }) :
					world.QueueAttack({ i + 1, scenario == "boss" ? 1 : ((i + 1) % entities + 1) });
				if (!queued) { throw std::runtime_error("Combat queue full"); }
			}
			const auto start = std::chrono::steady_clock::now();
			const auto result = world.Tick();
			const auto end = std::chrono::steady_clock::now();
			const auto queryStart = end;
			std::size_t visibleNow = 0;
			for (std::size_t i = 0; i < queries; ++i) { visibleNow += world.VisibleTo(i % entities + 1).size(); }
			const auto queryEnd = std::chrono::steady_clock::now();
			if (tick >= warmup)
			{
				times.push_back(std::chrono::duration<double, std::micro>(end - start).count());
				queryUs += std::chrono::duration<double, std::micro>(queryEnd - queryStart).count();
				accepted += result.applied; rejected += result.rejected; visible += visibleNow;
			}
		}
		std::sort(times.begin(), times.end());
		const auto percentile = [&](double q) { return times[static_cast<std::size_t>(std::ceil(q * times.size())) - 1]; };
		std::cout << "scenario,workers,entities,ticks,warmup,batch_size,cell_size,area_radius,max_hits,queries,p50_us,p95_us,p99_us,query_mean_us,applied,rejected,visible,hash\n";
		std::cout << std::setprecision(12) << scenario << ',' << workers << ',' << entities << ',' << ticks << ',' << warmup << ',' << config->worldBatchSize << ','
			<< config->worldCellSize << ',' << radius << ',' << config->worldMaxHitsPerTick << ',' << queries << ',' << percentile(.5) << ',' << percentile(.95) << ',' << percentile(.99)
			<< ',' << queryUs / ticks << ',' << accepted << ',' << rejected << ',' << visible << ',' << Hash(*world.Snapshot()) << '\n';
		return 0;
	}
	catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
