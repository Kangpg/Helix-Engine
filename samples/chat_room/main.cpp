// =============================================================================
// 채팅 룸 샘플 — 다세션 브로드캐스트 (공개 helix SDK)
//
// 도메인 예제: 한 클라가 보낸 메시지를 접속 중인 모든 클라에게 전달한다.
// 시연 포인트:
//   - 멤버 집합을 앱이 직접 관리(OnConnect 추가 / OnDisconnect 제거).
//   - 콜백 밖의 세션은 SessionId로 Server::FindSession 재조회 후 Send.
//   - 전송 무관(TCP/KCP) 동일 동작 — config의 transport만 바꾸면 됨.
//
// 프로토콜: 길이 프리픽스 프레이밍(서버 FrameCodec 기본 = echo_bot 기본과 동일).
// 스모크 테스트: 클라 1개면 발신자도 수신하므로 echo_bot 으로 통과 확인 가능
//   python tools\echo_bot.py --port 5000 --clients 1 --messages 20
// (멀티 클라 브로드캐스트는 telnet 류로 수동 확인 — echo_bot은 1:1 에코 가정)
// =============================================================================

#include "helix/helix.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

using namespace helix;

namespace
{
	Server* g_server = nullptr;
	void OnSignal(int) { if (g_server != nullptr) { g_server->Stop(); } }
}

class ChatApp final : public IServerApp
{
public:
	void OnStart(Server& s) override { g_server = &s; }

	void OnConnect(Session& s) override
	{
		std::size_t n;
		{
			std::lock_guard<std::mutex> lk(_mx);
			_members.insert(s.Id());
			n = _members.size();
		}
		std::printf("[+] join sid=%llu (members=%zu)\n",
		            static_cast<unsigned long long>(s.Id()), n);
	}

	void OnMessage(Session& /*from*/, ByteSpan msg) override
	{
		// 멤버 스냅샷을 락 안에서 복사 → 락 밖에서 송신(Send는 스레드 안전).
		std::vector<SessionId> ids;
		{
			std::lock_guard<std::mutex> lk(_mx);
			ids.assign(_members.begin(), _members.end());
		}
		if (g_server == nullptr) { return; }
		for (const SessionId id : ids)
		{
			if (Session* peer = g_server->FindSession(id)) { peer->Send(msg); }
		}
	}

	void OnDisconnect(Session& s, SessionCloseReason) override
	{
		{
			std::lock_guard<std::mutex> lk(_mx);
			_members.erase(s.Id());
		}
		std::printf("[-] leave sid=%llu\n", static_cast<unsigned long long>(s.Id()));
	}

private:
	std::mutex _mx;
	std::set<SessionId> _members;
};

int main()
{
	auto server = Server::LoadConfig("./config/helix.toml");
	g_server = &server;
	std::signal(SIGINT, OnSignal);
	std::signal(SIGTERM, OnSignal);   // 컨테이너 graceful 종료(docker stop) → 드레이닝

	std::printf("Helix chat room — broadcast to all members. Ctrl-C/SIGTERM으로 종료\n");
	return server.Run(std::make_unique<ChatApp>());
}
