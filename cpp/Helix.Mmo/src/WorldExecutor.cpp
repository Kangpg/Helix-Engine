#include "helix/mmo/world_executor.h"
#include <taskflow/taskflow.hpp>
#include <stdexcept>

namespace helix::mmo
{
	namespace
	{
		std::vector<std::size_t> Validate(std::span<const WorldTask> tasks)
		{
			std::vector<std::size_t> pending(tasks.size()), ready;
			std::vector<std::vector<std::size_t>> successors(tasks.size());
			for (std::size_t i = 0; i < tasks.size(); ++i)
			{
				if (!tasks[i].run) { throw std::invalid_argument("World task has no work"); }
				pending[i] = tasks[i].predecessors.size();
				for (auto predecessor : tasks[i].predecessors)
				{
					if (predecessor >= tasks.size()) { throw std::invalid_argument("Invalid world task dependency"); }
					successors[predecessor].push_back(i);
				}
				if (pending[i] == 0) { ready.push_back(i); }
			}
			for (std::size_t i = 0; i < ready.size(); ++i)
			{
				for (auto successor : successors[ready[i]])
				{
					if (--pending[successor] == 0) { ready.push_back(successor); }
				}
			}
			if (ready.size() != tasks.size()) { throw std::invalid_argument("Cyclic world task graph"); }
			return ready;
		}

		class SequentialWorldExecutor final : public IWorldExecutor
		{
		public:
			void Execute(std::span<const WorldTask> tasks) override
			{
				for (auto index : Validate(tasks)) { tasks[index].run(); }
			}
		};

		class TaskflowWorldExecutor final : public IWorldExecutor
		{
		public:
			explicit TaskflowWorldExecutor(std::size_t workers) : _executor(workers) {}
			void Execute(std::span<const WorldTask> tasks) override
			{
				Validate(tasks);
				tf::Taskflow graph;
				std::vector<tf::Task> nodes;
				nodes.reserve(tasks.size());
				for (const auto& task : tasks) { nodes.push_back(graph.emplace(task.run)); }
				for (std::size_t i = 0; i < tasks.size(); ++i)
				{
					for (auto predecessor : tasks[i].predecessors) { nodes[predecessor].precede(nodes[i]); }
				}
				_executor.run(graph).get();
			}
		private:
			tf::Executor _executor;
		};
	}

	std::shared_ptr<IWorldExecutor> CreateSequentialWorldExecutor() { return std::make_shared<SequentialWorldExecutor>(); }
	std::shared_ptr<IWorldExecutor> CreateTaskflowWorldExecutor(std::size_t workers)
	{
		if (workers == 0) { throw std::invalid_argument("World worker count must be positive"); }
		return std::make_shared<TaskflowWorldExecutor>(workers);
	}
}
