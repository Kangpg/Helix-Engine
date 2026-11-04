// =============================================================================
// 크로스노드 라우팅 샘플 — 공개 helix SDK (Phase 6: scale-out 검증)
//
// scale-out 동작을 노드 투명하게 시연한다. 같은 바이너리를 여러 노드로 띄우고
// (각자 다른 node_id/port, 공용 Redis), 한 노드에 붙은 클라이언트가 다른 노드에
// 붙은 클라이언트의 SessionId로 메시지를 보낸다.
//
// 프로토콜(길이 프리픽스 프레이밍 — FrameCodec/봇 기본값 4B big-endian):
//   - 접속 시 서버 → 클라: 8바이트(big-endian) = 그 클라의 전역 SessionId.
//   - 클라 → 서버 메시지: [8B big-endian 대상 SessionId][임의 페이로드]
//       서버는 Server::RouteToSession(target, payload) 로 전달한다(로컬이면 직접,
//       타 노드면 Redis pub/sub). 대상 클라는 payload 프레임을 그대로 수신.
//
// 한 노드만 띄우면 같은 노드 내 라우팅을, 두 노드를 띄우면 노드 간 라우팅을 검증.
// 검증: tools/cluster_route_bot.py
// =============================================================================

#include "helix/helix.h"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using namespace helix;

namespace
{
	Server* g_server = nullptr;
	std::atomic<bool> g_run{ true };

	void OnSignal(int)
	{
		g_run.store(false);
		if (g_server != nullptr) { g_server->Stop(); }
	}

	void PutBe64(std::uint64_t v, std::byte* out)
	{
		for (int i = 0; i < 8; ++i)
		{
			out[i] = static_cast<std::byte>((v >> (8 * (7 - i))) & 0xFF);
		}
	}

	std::uint64_t GetBe64(const std::byte* p)
	{
		std::uint64_t v = 0;
		for (int i = 0; i < 8; ++i)
		{
			v = (v << 8) | static_cast<std::uint8_t>(p[i]);
		}
		return v;
	}
}

class RouteApp final : public IServerApp
{
public:
	void OnStart(Server& server) override { g_server = &server; }

	void OnConnect(Session& s) override
	{
		// 접속 클라에게 자신의 전역 SessionId(노드 비트 포함)를 알려준다.
		std::byte id[8];
		PutBe64(static_cast<std::uint64_t>(s.Id()), id);
		s.Send(ByteSpan(id, sizeof(id)));
		std::printf("[+] sid=%llu (node=%u)\n",
		            static_cast<unsigned long long>(s.Id()),
		            static_cast<unsigned>(NodeOf(s.Id())));
	}

	void OnMessage(Session& s, ByteSpan msg) override
	{
		if (msg.size() < 8 || g_server == nullptr)
		{
			return; // 형식 위반 — 무시
		}
		const SessionId target = static_cast<SessionId>(GetBe64(msg.data()));
		const ByteSpan payload(msg.data() + 8, msg.size() - 8);

		const Status st = g_server->RouteToSession(target, payload);
		if (st != Status::Ok)
		{
			std::printf("[route] sid=%llu -> %llu failed (status=%d)\n",
			            static_cast<unsigned long long>(s.Id()),
			            static_cast<unsigned long long>(target),
			            static_cast<int>(st));
		}
	}

	void OnDisconnect(Session&, SessionCloseReason) override {}
};

int main()
{
	auto server = Server::LoadConfig("./config/helix.toml");
	g_server = &server;
	std::signal(SIGINT, OnSignal);

	std::printf("Helix route sample (Phase 6 scale-out) — Ctrl-C로 graceful 종료\n");
	return server.Run(std::make_unique<RouteApp>());
}
