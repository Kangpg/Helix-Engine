// =============================================================================
// KCP 에코 검증 클라이언트 (Phase 7) — 원본 ikcp 사용
//
// helix 에코 서버를 KCP 전송으로 띄운 뒤(아래) 이 클라가 붙어 N개의 프레임을
// 보내고 에코를 대조한다. 순수 UDP 위에서 ikcp가 신뢰·순서를 보장하는지 검증.
//
// 서버 기동(같은 에코 앱을 KCP로):
//   set HELIX_NETWORK__TRANSPORT=kcp   (PowerShell: $env:HELIX_NETWORK__TRANSPORT="kcp")
//   helix_echo_phase1
// 클라:
//   helix_kcp_echo_client [host=127.0.0.1] [port=5000] [count=200] [size=128]
//
// 프레이밍: 서버 FrameCodec 기본값과 일치(4바이트 big-endian 길이 프리픽스).
// 종료코드: PASS=0 / FAIL=1.
// =============================================================================

#include <WinSock2.h>
#include <WS2tcpip.h>

#include "ikcp.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{
	struct Ctx { SOCKET sock = INVALID_SOCKET; };

	// ikcp → UDP 송신.
	int UdpOutput(const char* buf, int len, ikcpcb* /*kcp*/, void* user)
	{
		auto* c = static_cast<Ctx*>(user);
		return ::send(c->sock, buf, len, 0);
	}

	std::uint32_t NowMs(std::chrono::steady_clock::time_point start)
	{
		using namespace std::chrono;
		return static_cast<std::uint32_t>(
			duration_cast<milliseconds>(steady_clock::now() - start).count());
	}

	void PutBe32(std::uint32_t v, char* out)
	{
		out[0] = static_cast<char>((v >> 24) & 0xFF);
		out[1] = static_cast<char>((v >> 16) & 0xFF);
		out[2] = static_cast<char>((v >> 8) & 0xFF);
		out[3] = static_cast<char>(v & 0xFF);
	}
	std::uint32_t GetBe32(const unsigned char* p)
	{
		return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16)
		     | (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
	}
}

int main(int argc, char** argv)
{
	const char* host = (argc > 1) ? argv[1] : "127.0.0.1";
	const int port = (argc > 2) ? std::atoi(argv[2]) : 5000;
	const int count = (argc > 3) ? std::atoi(argv[3]) : 200;
	const int size = (argc > 4) ? std::atoi(argv[4]) : 128;

	WSADATA wsa{};
	if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { std::printf("WSAStartup failed\n"); return 1; }

	Ctx ctx;
	ctx.sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (ctx.sock == INVALID_SOCKET) { std::printf("socket failed\n"); return 1; }

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = ::htons(static_cast<u_short>(port));
	::inet_pton(AF_INET, host, &addr.sin_addr);
	if (::connect(ctx.sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
	{
		std::printf("connect failed\n"); return 1;
	}
	u_long nb = 1;
	::ioctlsocket(ctx.sock, FIONBIO, &nb);  // 논블로킹 recv

	const auto start = std::chrono::steady_clock::now();
	ikcpcb* kcp = ikcp_create(0x68656C78 /*'helx'*/, &ctx);
	ikcp_setoutput(kcp, &UdpOutput);
	ikcp_nodelay(kcp, 1, 10, 2, 1);
	ikcp_wndsize(kcp, 256, 256);

	// 보낼 페이로드 준비(랜덤) + 전부 ikcp_send 큐잉.
	std::vector<std::string> expected;
	expected.reserve(count);
	for (int i = 0; i < count; ++i)
	{
		std::string p(static_cast<std::size_t>(size), '\0');
		for (int j = 0; j < size; ++j) { p[j] = static_cast<char>(std::rand() & 0xFF); }
		expected.push_back(p);

		std::string frame(4, '\0');
		PutBe32(static_cast<std::uint32_t>(p.size()), frame.data());
		frame += p;
		ikcp_send(kcp, frame.data(), static_cast<int>(frame.size()));
	}

	// 펌프 루프: update + UDP 수신 → ikcp_input → ikcp_recv → 디프레이밍/대조.
	std::string acc;          // 수신 스트림 누적(디프레이밍용)
	std::vector<char> udp(65536);
	int recvd = 0, mismatches = 0;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

	while (recvd < count && std::chrono::steady_clock::now() < deadline)
	{
		ikcp_update(kcp, NowMs(start));

		for (;;)
		{
			const int n = ::recv(ctx.sock, udp.data(), static_cast<int>(udp.size()), 0);
			if (n <= 0) { break; }
			ikcp_input(kcp, udp.data(), n);
		}
		for (;;)
		{
			const int sz = ikcp_peeksize(kcp);
			if (sz <= 0) { break; }
			std::string tmp(static_cast<std::size_t>(sz), '\0');
			const int got = ikcp_recv(kcp, tmp.data(), sz);
			if (got <= 0) { break; }
			acc.append(tmp.data(), static_cast<std::size_t>(got));
		}

		// 디프레이밍: [4B len][payload] 반복.
		std::size_t pos = 0;
		while (acc.size() - pos >= 4)
		{
			const std::uint32_t plen = GetBe32(reinterpret_cast<const unsigned char*>(acc.data() + pos));
			if (acc.size() - pos < 4 + plen) { break; }
			const std::string got(acc.data() + pos + 4, plen);
			if (recvd < count && got != expected[static_cast<std::size_t>(recvd)]) { ++mismatches; }
			++recvd;
			pos += 4 + plen;
		}
		if (pos > 0) { acc.erase(0, pos); }

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	ikcp_release(kcp);
	::closesocket(ctx.sock);
	::WSACleanup();

	const bool pass = (recvd == count && mismatches == 0);
	std::printf("============================================================\n");
	std::printf("Helix KCP echo client  target=%s:%d  count=%d size=%d\n", host, port, count, size);
	std::printf("  echoed   : %d / %d\n", recvd, count);
	std::printf("  mismatch : %d\n", mismatches);
	std::printf("  RESULT   : %s\n", pass ? "PASS" : "FAIL");
	std::printf("============================================================\n");
	return pass ? 0 : 1;
}
