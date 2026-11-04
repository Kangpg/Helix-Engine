// =============================================================================
// 에코 서버 샘플 — 공개 helix SDK (Phase 5: 런타임 데이터 핫스왑 데모)
//
// 시연 포인트:
//  1) IServerApp + Server.Run 만으로 서버 구동(저수준 비노출).
//  2) helix::Snapshot<T> 로 런타임 데이터 무중단 교체 — 백그라운드 스레드가
//     배너 스냅샷을 주기적으로 Store, 핸들러(워커 스레드)는 Load로 락 없이 읽음.
//     동시 교체/읽기에서 torn read·크래시가 없어야 한다(echo_bot churn으로 검증).
//  3) graceful drain — Ctrl-C 시 진행 세션을 정리한 뒤 종료.
//
// 에코 프로토콜은 그대로(받은 메시지만 되돌림) → 봇은 변경 없이 통과.
// =============================================================================

#include "helix/helix.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

using namespace helix;

namespace
{
	// 런타임 핫스왑 데이터(불변 스냅샷). 백그라운드에서 교체, 핸들러에서 읽기.
	Snapshot<std::string> g_banner;

	Server* g_server = nullptr;
	std::atomic<bool> g_run{ true };

	void OnSignal(int)
	{
		g_run.store(false);
		if (g_server != nullptr) { g_server->Stop(); }
	}
}

class EchoApp final : public IServerApp
{
public:
	void OnConnect(Session& s) override
	{
		// 핫패스에서 현재 스냅샷을 락 없이 읽는다(동시 Store와 경쟁해도 안전).
		auto banner = g_banner.Load();
		std::printf("[+] sid=%llu  banner=\"%s\"\n",
		            static_cast<unsigned long long>(s.Id()),
		            banner ? banner->c_str() : "(none)");
	}

	void OnMessage(Session& s, ByteSpan msg) override
	{
		auto banner = g_banner.Load();  // 워커 스레드에서 동시 읽기(스왑과 경쟁)
		(void)banner;
		s.Send(msg);                    // 에코(엔진이 프레이밍)
	}

	void OnDisconnect(Session&, SessionCloseReason) override {}
};

int main()
{
	g_banner.Store(std::make_shared<const std::string>("helix banner v0"));

	// 데이터 핫스왑 스레드: 2초마다 새 불변 스냅샷으로 원자 교체(무중단).
	std::thread swapper([]
	{
		int v = 1;
		while (g_run.load())
		{
			std::this_thread::sleep_for(std::chrono::seconds(2));
			g_banner.Store(std::make_shared<const std::string>("helix banner v" + std::to_string(v++)));
		}
	});

	auto server = Server::LoadConfig("./config/helix.toml");
	g_server = &server;
	std::signal(SIGINT, OnSignal);
	std::signal(SIGTERM, OnSignal);   // 컨테이너 graceful 종료(docker stop) → 드레이닝

	std::printf("Helix echo + hot-swap (Phase 5) — Ctrl-C/SIGTERM으로 graceful 종료\n");
	const int rc = server.Run(std::make_unique<EchoApp>());

	g_run.store(false);
	if (swapper.joinable()) { swapper.join(); }
	return rc;
}
