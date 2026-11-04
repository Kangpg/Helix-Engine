#pragma once

// =============================================================================
// 백플레인 (scale-out)
//
// 동질 노드 + L4 LB 구성에서 노드 간 협업을 담당. 기본 구현은 Redis.
//   - 세션 디렉터리: sessionId/userId → 어느 NodeId 에 있는지 (외부화)
//   - pub/sub 라우팅: 다른 노드에 붙은 세션으로 메시지 전달
//   - 공유 KV     : 룸/매치 등 노드 간 공유 상태
//   - 런타임 데이터 버전 브로드캐스트(2.5절 핫스왑 트리거)
//
// 엔진은 토폴로지를 강요하지 않는다 — 인터페이스만 제공하고 Redis 기본 구현을
// 끼운다. 구매 업체는 NATS/Kafka 등으로 교체 가능.
// =============================================================================

#include "helix/types.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace helix::cluster
{
	using NodeId = uint32;

	struct BackplaneConfig
	{
		std::string url;                 // 예: "redis://127.0.0.1:6379/0"
		NodeId nodeId = 0;               // 이 노드의 ID(설정/환경변수)
		uint32 sessionTtlSec = 30; // 디렉터리 엔트리 TTL(좀비 정리)
	};

	// 다른 노드 → 이 노드 방향 수신.
	class IBackplaneSink
	{
	public:
		virtual ~IBackplaneSink() = default;
		// 이 노드가 보유한 세션으로 전달할 메시지.
		virtual void OnRouted(helix::SessionId target, helix::ByteSpan payload) = 0;
		// 런타임 데이터 갱신 알림(채널, 새 버전). 코어가 로드 후 atomic 스왑.
		virtual void OnDataVersion(std::string_view channel, std::uint64_t version) = 0;
	};

	class IBackplane
	{
	public:
		virtual ~IBackplane() = default;

		virtual bool Connect(const BackplaneConfig& cfg, IBackplaneSink& sink) = 0;
		virtual void Disconnect() = 0;

		// 세션 디렉터리
		virtual void RegisterSession(helix::SessionId sid) = 0;        // 이 노드에 등록
		virtual void UnregisterSession(helix::SessionId sid) = 0;
		[[nodiscard]] virtual std::optional<NodeId> Locate(helix::SessionId sid) = 0;
		virtual void Heartbeat() = 0;                                  // TTL 갱신(주기 호출)

		// 크로스노드 전달: target이 다른 노드면 그 노드로 publish.
		// 반환 false면 대상 미발견.
		virtual bool RouteToSession(helix::SessionId target, helix::ByteSpan payload) = 0;

		// 런타임 데이터 버전 브로드캐스트(전 노드 핫스왑 트리거).
		virtual void PublishDataVersion(std::string_view channel, std::uint64_t version) = 0;

		// 공유 KV (룸/매치 등)
		virtual bool KvSet(std::string_view key, std::string_view value, std::uint32_t ttlSec) = 0;
		[[nodiscard]] virtual std::optional<std::string> KvGet(std::string_view key) = 0;
	};
}
