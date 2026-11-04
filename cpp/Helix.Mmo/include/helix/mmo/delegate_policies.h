#pragma once
#include "movement_policy.h"
#include "combat_rules.h"
#include "visibility_policy.h"
#include <functional>
#include <stdexcept>
#include <utility>

namespace helix::mmo
{
	class DelegateMovementPolicy final : public IMovementPolicy
	{
	public:
		using Function = std::function<std::optional<Position>(const MovementContext&)>;
		explicit DelegateMovementPolicy(Function function) : _function(std::move(function))
		{
			if (!_function) { throw std::invalid_argument("movement function is required"); }
		}
		std::optional<Position> Resolve(const MovementContext& context) const override { return _function(context); }
	private:
		Function _function;
	};

	class DelegateCombatRules final : public ICombatRules
	{
	public:
		using Function = std::function<std::optional<std::int32_t>(const CombatContext&)>;
		explicit DelegateCombatRules(Function function) : _function(std::move(function))
		{
			if (!_function) { throw std::invalid_argument("combat function is required"); }
		}
		std::optional<std::int32_t> Resolve(const CombatContext& context) const override { return _function(context); }
	private:
		Function _function;
	};

	class DelegateVisibilityPolicy final : public IVisibilityPolicy
	{
	public:
		using Function = std::function<std::vector<EntityId>(EntityId, std::span<const EntityState>)>;
		explicit DelegateVisibilityPolicy(Function function) : _function(std::move(function))
		{
			if (!_function) { throw std::invalid_argument("visibility function is required"); }
		}
		std::vector<EntityId> Select(EntityId observer, std::span<const EntityState> entities) const override
		{
			return _function(observer, entities);
		}
	private:
		Function _function;
	};
}
