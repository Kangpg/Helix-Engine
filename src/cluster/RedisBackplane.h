#pragma once

// =============================================================================
// Phase 6 — IBackplane 의 Redis 기본 구현
//
// scale-out 협업(동질 노드 + L4 LB)을 Redis로 외부화한다.
//   - 세션 디렉터리 : SET helix:sess:{sid} {nodeId} EX ttl  /  GET  /  DEL
//   - 크로스노드 라우팅 : PUBLISH helix:route:{nodeId}  (8B sid + payload)
//   - 데이터 버전 브로드캐스트 : PUBLISH helix:dataver  (8B version + channel)
//   - 공유 KV : SET/GET helix:kv:{key}
//   - heartbeat : 로컬 보유 세션 키 TTL 갱신(좀비 정리)
//
// 연결 2개:
//   (1) 명령 연결(_cmd)  — 요청/응답. 어느 스레드에서나 _cmdMx로 직렬화해 사용.
//   (2) 구독 연결(_sub)  — 전용 스레드가 SUBSCRIBE 후 푸시 수신 → IBackplaneSink.
// 구독 스레드는 끊기면 자동 재연결 + 재구독한다(at-most-once, 계획서 5절 합의).
//
// SOLID: 코어는 IBackplane만 의존(DIP). 이 구현은 팩토리(CreateBackplane)로
// 주입되며 NATS/Kafka 등으로 교체 가능(OCP).
// =============================================================================

#include "IBackplane.h"
#include "RespClient.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

namespace helix::cluster
{
	class RedisBackplane final : public IBackplane
	{
	public:
		RedisBackplane() = default;
		~RedisBackplane() override;

		bool Connect(const BackplaneConfig& cfg, IBackplaneSink& sink) override;
		void Disconnect() override;

		void RegisterSession(helix::SessionId sid) override;
		void UnregisterSession(helix::SessionId sid) override;
		[[nodiscard]] std::optional<NodeId> Locate(helix::SessionId sid) override;
		void Heartbeat() override;

		bool RouteToSession(helix::SessionId target, helix::ByteSpan payload) override;
		void PublishDataVersion(std::string_view channel, std::uint64_t version) override;

		bool KvSet(std::string_view key, std::string_view value, std::uint32_t ttlSec) override;
		[[nodiscard]] std::optional<std::string> KvGet(std::string_view key) override;

	private:
		void SubscribeLoop();              // 구독 스레드 본체(재연결 포함)
		bool OpenSubscriber();             // _sub connect + SELECT + SUBSCRIBE
		void Dispatch(const std::string& channel, const std::string& payload);
		bool EnsureCmd();                  // _cmd 미연결 시 재연결 + SELECT (_cmdMx 보유 가정)
		[[nodiscard]] std::optional<NodeId> GetNodeLocked(helix::SessionId sid); // _cmdMx 보유 가정

		[[nodiscard]] std::string SessKey(helix::SessionId sid) const;

		BackplaneConfig _cfg;
		IBackplaneSink* _sink = nullptr;

		std::string _host = "127.0.0.1";
		std::uint16_t _port = 6379;
		int _db = 0;

		std::string _routeChannel;   // helix:route:{nodeId}
		std::string _dataChannel;    // helix:dataver

		RespConn _cmd;
		std::mutex _cmdMx;

		RespConn _sub;               // 구독 전용 연결(구독 스레드 단독, Disconnect가 Close로 깨움)
		std::thread _subThread;
		std::atomic<bool> _running{ false };

		std::mutex _localMx;
		std::unordered_set<helix::SessionId> _local; // heartbeat 대상(이 노드 보유 세션)
	};
}
