#pragma once

// =============================================================================
// 백플레인 팩토리 (DIP/OCP)
//
// 설정(BackplaneKind)에 따라 IBackplane 구현을 생성한다. 
// 코어는 IBackplane만 의존하고 구체 타입(RedisBackplane)을 모른다. 
// NATS/Kafka 추가 시 새 구현 + 이 팩토리에 분기만 더하면 된다(코어 무변경).
// =============================================================================

#include "IBackplane.h"

#include <memory>

namespace helix::cluster
{
	enum class BackplaneKind : uint8
	{
		None = 0,  // 단일 노드(백플레인 비활성)

		Redis,     // 기본 scale-out 구현
	};

	[[nodiscard]] BackplaneKind ParseBackplaneKind(std::string_view s) noexcept;

	[[nodiscard]] std::unique_ptr<IBackplane> CreateBackplane(BackplaneKind kind);
}
