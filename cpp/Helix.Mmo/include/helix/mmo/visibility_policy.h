#pragma once
#include "types.h"

namespace helix::mmo
{
	// 불변 스냅샷 위의 조회. 동시 조회가 가능하므로 구현/캡처는 thread-safe해야 한다.
	class IVisibilityPolicy
	{
	public:
		virtual ~IVisibilityPolicy() = default;
		virtual std::vector<EntityId> Select(EntityId observer, std::span<const EntityState> entities) const = 0;
	};
}
