#pragma once
#include "types.h"
#include <optional>

namespace helix::mmo
{
	// 규칙은 월드 상태를 직접 변경하지 않는다. nullopt = 요청 거부.
	class IMovementPolicy
	{
	public:
		virtual ~IMovementPolicy() = default;
		virtual std::optional<Position> Resolve(const MovementContext& context) const = 0;
	};
}
