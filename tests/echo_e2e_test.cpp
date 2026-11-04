// Phase 4 — Linux TCP 경로 end-to-end 검증.
// 실제 EpollBackend + Acceptor + SessionManager + Session + FrameCodec 를 엮어
// 에코 서버를 띄우고(ServerCore::WorkerLoop 와 동일한 디스패치), 클라이언트가
// 길이 프리픽스 프레임을 주고받아 검증한다. (cluster/transport 미포함)
//
// 빌드:
//   g++ -std=c++20 -O1 -pthread -I include -I src \
//     src/net/EpollBackend.cpp src/net/Acceptor.cpp \
//     src/session/Session.cpp src/session/SessionManager.cpp \
//     tests/echo_e2e_test.cpp -o /tmp/echo_e2e && /tmp/echo_e2e

#include "net/EpollBackend.h"
#include "net/Acceptor.h"
#include "session/SessionManager.h"
#include "session/Session.h"
#include "proto/FrameCodec.h"
#include "helix/server_app.h"
#include "helix/session.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace helix;

namespace
{
	int g_fail = 0;
	void Check(bool c, const char* what)
	{
		std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what);
		if (!c) { ++g_fail; }
	}

	// 에코 앱: 받은 메시지를 그대로 되돌린다. 연결 수를 센다.
	class EchoApp final : public IServerApp
	{
	public:
		std::atomic<int> connects{ 0 };
		std::atomic<int> messages{ 0 };
		std::atomic<int> disconnects{ 0 };

		void OnConnect(Session&) override { connects.fetch_add(1); }
		void OnMessage(Session& s, ByteSpan msg) override
		{
			messages.fetch_add(1);
			s.Send(msg);
		}
		void OnDisconnect(Session&, SessionCloseReason) override { disconnects.fetch_add(1); }
	};

	// 4바이트 빅엔디안 길이 프리픽스 프레임 송신.
	void SendFramed(int fd, const std::string& payload)
	{
		const std::uint32_t n = static_cast<std::uint32_t>(payload.size());
		unsigned char hdr[4] = {
			static_cast<unsigned char>((n >> 24) & 0xFF),
			static_cast<unsigned char>((n >> 16) & 0xFF),
			static_cast<unsigned char>((n >> 8) & 0xFF),
			static_cast<unsigned char>(n & 0xFF) };
		::send(fd, hdr, 4, 0);
		::send(fd, payload.data(), payload.size(), 0);
	}

	// 정확히 n바이트 수신.
	bool RecvAll(int fd, char* buf, std::size_t n)
	{
		std::size_t got = 0;
		while (got < n)
		{
			const ssize_t r = ::recv(fd, buf + got, n - got, 0);
			if (r <= 0) { return false; }
			got += static_cast<std::size_t>(r);
		}
		return true;
	}

	// 한 프레임 수신 → payload 반환.
	std::string RecvFramed(int fd)
	{
		unsigned char hdr[4];
		if (!RecvAll(fd, reinterpret_cast<char*>(hdr), 4)) { return {}; }
		const std::uint32_t n = (std::uint32_t(hdr[0]) << 24) | (std::uint32_t(hdr[1]) << 16)
		                      | (std::uint32_t(hdr[2]) << 8) | std::uint32_t(hdr[3]);
		std::string out(n, '\0');
		if (!RecvAll(fd, out.data(), n)) { return {}; }
		return out;
	}

	int ClientConnect(std::uint16_t port)
	{
		int c = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
		a.sin_port = ::htons(port);
		if (::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { ::close(c); return -1; }
		return c;
	}
}

int main()
{
	std::printf("== TCP echo end-to-end (epoll) ==\n");

	net::EpollBackend backend;
	net::BackendConfig bcfg;
	bcfg.kind = net::IoBackendKind::Epoll;
	bcfg.maxConnections = 1024;
	Check(backend.Init(bcfg), "backend Init");

	EchoApp app;
	core::SessionFactoryConfig fcfg;             // 기본 프레이밍(4바이트 BE, 1MiB)
	fcfg.recvBufSize = 4096;
	core::SessionManager mgr(backend, 1024, fcfg, &app, /*nodeId*/ 0, /*observer*/ nullptr);
	net::Acceptor acceptor(backend, mgr);

	// 임의 포트 바인드를 위해 0번 시도 후 실제 포트 조회는 listen 소켓이 내부라
	// 여기선 고정 포트 범위를 시도한다.
	std::uint16_t port = 0;
	bool listening = false;
	for (std::uint16_t p = 28000; p < 28050; ++p)
	{
		helix::Endpoint ep{ "127.0.0.1", p };
		if (acceptor.Listen(ep, 128, 16)) { port = p; listening = true; break; }
	}
	Check(listening, "acceptor Listen");

	// 워커 풀(ServerCore::WorkerLoop 와 동일 디스패치).
	std::atomic<bool> running{ true };
	auto worker = [&]
	{
		constexpr int kBatch = 64;
		net::IoCompletion comps[kBatch];
		while (running.load(std::memory_order_acquire))
		{
			const int n = backend.Poll(comps, kBatch, 200);
			for (int i = 0; i < n; ++i)
			{
				net::IoCompletion& c = comps[i];
				if (c.ctx == nullptr) { continue; }
				switch (c.ctx->op)
				{
				case net::IoOp::Accept:
					acceptor.OnAcceptComplete(c.ctx, c.error);
					break;
				case net::IoOp::Recv:
					static_cast<core::Session*>(c.ctx->user)
						->OnRecvComplete(c.error ? 0u : c.bytesTransferred);
					break;
				case net::IoOp::Send:
					static_cast<core::Session*>(c.ctx->user)
						->OnSendComplete(c.error ? 0u : c.bytesTransferred);
					break;
				default: break;
				}
			}
		}
	};
	std::vector<std::thread> workers;
	for (int i = 0; i < 3; ++i) { workers.emplace_back(worker); }

	// --- 단일 클라이언트, 3개 프레임 echo ---
	int c1 = ClientConnect(port);
	Check(c1 >= 0, "client connect");
	const char* msgs[3] = { "hello", "helix-engine", "epoll-proactor-emulation" };
	bool echoOk = true;
	for (const char* m : msgs)
	{
		SendFramed(c1, m);
		const std::string got = RecvFramed(c1);
		if (got != m) { echoOk = false; }
	}
	Check(echoOk, "3개 프레임 echo 일치");

	// --- 큰 메시지(부분 송신 유발 가능) echo ---
	std::string big(200000, 'x');
	SendFramed(c1, big);
	Check(RecvFramed(c1) == big, "200KB 메시지 echo(부분 송신 경로)");

	// --- 다중 클라이언트 동시 ---
	std::atomic<int> okCount{ 0 };
	std::vector<std::thread> clients;
	for (int i = 0; i < 20; ++i)
	{
		clients.emplace_back([&, i]
		{
			int c = ClientConnect(port);
			if (c < 0) { return; }
			const std::string payload = "client-" + std::to_string(i);
			SendFramed(c, payload);
			if (RecvFramed(c) == payload) { okCount.fetch_add(1); }
			::close(c);
		});
	}
	for (auto& t : clients) { t.join(); }
	Check(okCount.load() == 20, "20개 동시 클라이언트 echo");

	// --- 정리 ---
	::close(c1);
	std::this_thread::sleep_for(std::chrono::milliseconds(100));  // 종료 완료 처리
	Check(app.connects.load() == 21, "OnConnect 21회");
	Check(app.messages.load() == 24, "OnMessage 24회(3+1+20)");

	running.store(false, std::memory_order_release);
	backend.Wake(static_cast<int>(workers.size()));
	for (auto& t : workers) { t.join(); }
	acceptor.Stop();
	mgr.CloseAll();
	backend.Shutdown();

	std::printf("== %s ==\n", g_fail == 0 ? "ALL PASS" : "FAILURES PRESENT");
	return g_fail == 0 ? 0 : 1;
}
