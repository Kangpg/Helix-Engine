// Phase 4 — EpollBackend 유닛테스트 (Linux). INetBackend 계약을 실제 루프백
// 소켓으로 직접 검증한다: accept → recv → send(부분 포함) → peer-close → Wake.
//
// 빌드(샌드박스):
//   g++ -std=c++20 -O1 -pthread -I include -I src \
//       src/net/EpollBackend.cpp tests/epoll_backend_test.cpp -o /tmp/epoll_test
//   /tmp/epoll_test
//
// 의존성 최소화를 위해 코어(Acceptor/Session)는 끌어오지 않고 백엔드만 검증한다.

#include "net/EpollBackend.h"
#include "net/PlatformNet.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace helix::net;

namespace
{
	int g_fail = 0;
	void Check(bool cond, const char* what)
	{
		std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
		if (!cond) { ++g_fail; }
	}

	// 블로킹 listen 소켓 생성 → 실제 바인드된 포트 반환.
	int MakeListener(std::uint16_t& outPort)
	{
		int s = ::socket(AF_INET, SOCK_STREAM, 0);
		assert(s >= 0);
		int one = 1;
		::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
		a.sin_port = 0;   // 임의 포트
		assert(::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
		assert(::listen(s, 16) == 0);
		sockaddr_in bound{};
		socklen_t bl = sizeof(bound);
		::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &bl);
		outPort = ::ntohs(bound.sin_port);
		return s;
	}

	int Connect(std::uint16_t port)
	{
		int c = ::socket(AF_INET, SOCK_STREAM, 0);
		assert(c >= 0);
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
		a.sin_port = ::htons(port);
		assert(::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
		return c;
	}

	// out 에서 ctx 종류별 완료를 한 건 대기(여러 번 Poll 시도).
	IoCompletion PollOne(EpollBackend& be, int tries = 200)
	{
		IoCompletion c[8];
		for (int t = 0; t < tries; ++t)
		{
			const int n = be.Poll(c, 8, 25);
			if (n > 0) { return c[0]; }
		}
		return IoCompletion{ nullptr, 0, std::make_error_code(std::errc::timed_out) };
	}
}

int main()
{
	std::printf("== EpollBackend contract test ==\n");

	EpollBackend be;
	BackendConfig cfg;
	cfg.kind = IoBackendKind::Epoll;
	cfg.maxConnections = 1024;
	Check(be.Init(cfg), "Init");
	Check(std::string(be.Name()) == "epoll", "Name()==epoll");

	std::uint16_t port = 0;
	const int listenFd = MakeListener(port);
	Check(be.Register(static_cast<NativeHandle>(listenFd)), "Register(listen)");

	// accept 게시.
	IoContext* acc = be.CreateContext(IoOp::Accept);
	acc->handle = static_cast<NativeHandle>(listenFd);
	Check(be.PostAccept(acc), "PostAccept");

	// 클라이언트 접속.
	std::atomic<int> clientFd{ -1 };
	std::thread client([&] { clientFd.store(Connect(port)); });

	IoCompletion ac = PollOne(be);
	Check(ac.ctx == acc && ac.ctx->op == IoOp::Accept && !ac.error, "accept 완료");
	Check(acc->acceptSocket != kInvalidHandle, "acceptSocket 채워짐");
	client.join();
	const int conn = static_cast<int>(acc->acceptSocket);

	// 수락 소켓 등록 + recv 게시.
	Check(be.Register(acc->acceptSocket), "Register(accepted)");
	IoContext* rcv = be.CreateContext(IoOp::Recv);
	rcv->handle = acc->acceptSocket;
	std::vector<std::byte> rbuf(256);
	rcv->buffer = rbuf.data();
	rcv->length = static_cast<std::uint32_t>(rbuf.size());

	// 클라이언트가 "hello" 송신.
	const char* msg = "hello";
	::send(clientFd.load(), msg, 5, 0);
	Check(be.PostRecv(rcv), "PostRecv");

	IoCompletion rc = PollOne(be);
	Check(rc.ctx == rcv && rc.ctx->op == IoOp::Recv && !rc.error, "recv 완료");
	Check(rc.bytesTransferred == 5, "recv 5바이트");
	Check(std::memcmp(rbuf.data(), "hello", 5) == 0, "recv 내용 일치");

	// 서버 → 클라이언트 echo 송신.
	IoContext* snd = be.CreateContext(IoOp::Send);
	snd->handle = acc->acceptSocket;
	const char* reply = "world!";
	std::vector<std::byte> sbuf(6);
	std::memcpy(sbuf.data(), reply, 6);
	snd->buffer = sbuf.data();
	snd->length = 6;
	Check(be.PostSend(snd), "PostSend");

	IoCompletion sc = PollOne(be);
	Check(sc.ctx == snd && sc.ctx->op == IoOp::Send && !sc.error, "send 완료");
	Check(sc.bytesTransferred == 6, "send 6바이트(전체)");

	char rb[16] = {};
	const ssize_t got = ::recv(clientFd.load(), rb, sizeof(rb), 0);
	Check(got == 6 && std::memcmp(rb, "world!", 6) == 0, "클라이언트 echo 수신");

	// peer close → recv 0.
	::close(clientFd.load());
	Check(be.PostRecv(rcv), "PostRecv(after close)");
	IoCompletion pc = PollOne(be);
	Check(pc.ctx == rcv && pc.bytesTransferred == 0 && !pc.error, "peer-closed → bytes=0");

	// Wake → null ctx 완료.
	be.Wake(1);
	IoCompletion wc = PollOne(be);
	Check(wc.ctx == nullptr, "Wake → null ctx 완료");

	// 정리.
	::close(conn);
	be.DestroyContext(acc);
	be.DestroyContext(rcv);
	be.DestroyContext(snd);
	be.Shutdown();

	std::printf("== %s ==\n", g_fail == 0 ? "ALL PASS" : "FAILURES PRESENT");
	return g_fail == 0 ? 0 : 1;
}
