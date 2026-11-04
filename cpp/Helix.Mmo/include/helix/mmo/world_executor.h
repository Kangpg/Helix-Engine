#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace helix::mmo
{
	struct WorldTask
	{
		std::function<void()> run;
		std::vector<std::size_t> predecessors;
	};

	// Execute joins ALL work before returning or propagating a task exception.
	// Calls from independent worlds may overlap. Tasks must not recursively call
	// this executor or wait on a different world. Invalid DAGs fail before work.
	class IWorldExecutor
	{
	public:
		virtual ~IWorldExecutor() = default;
		virtual void Execute(std::span<const WorldTask> tasks) = 0;
	};

	// Both adapters execute the same staged rules, not legacy command semantics.
	std::shared_ptr<IWorldExecutor> CreateSequentialWorldExecutor();
	std::shared_ptr<IWorldExecutor> CreateTaskflowWorldExecutor(std::size_t workers);
}
