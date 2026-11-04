#pragma once
#include "types.h"
#include <optional>

namespace helix::mmo
{
	// 반환값은 비음수 피해량. 실제 HP 변경은 WorldInstance가 소유한다.
	class ICombatRules
	{
	public:
		virtual ~ICombatRules() = default;
		virtual std::optional<std::int32_t> Resolve(const CombatContext& context) const = 0;
	};
}
