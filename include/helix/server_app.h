#pragma once

// =============================================================================
// Helix public SDK — IServerApp
//
// 구매 업체가 구현하는 유일한 핵심 인터페이스.
// OnMessage만 필수이며 나머지는 선택적 훅이다.
//
// 스레딩: 기본 설정에서 콜백은 워커 스레드 풀에서 호출된다(앱 로직이 I/O를
// 막지 않도록). 동일 세션의 콜백은 순서가 보장된다. 저지연이 필요하면 설정에서
// I/O 스레드 직접 디스패치로 전환할 수 있다.
// =============================================================================

#include "types.h"

namespace helix
{
	class Session;
	class Server;

	class IServerApp
	{
	public:
		virtual ~IServerApp() = default;

		// 서버 기동 직후 1회. 설정 로드/리스닝 시작 후 호출.
		virtual void OnStart(Server& /*server*/) {}

		// 새 연결 수립.
		virtual void OnConnect(Session& /*session*/) {}

		// 완결 메시지 1건 수신(프레이밍 후). 필수 구현.
		// message는 콜백 동안만 유효 — 보관하려면 복사할 것.
		virtual void OnMessage(Session& session, ByteSpan message) = 0;

		// 연결 종료(어떤 사유든).
		virtual void OnDisconnect(Session& /*session*/, SessionCloseReason /*reason*/) {}

		// (Phase 6, scale-out) 런타임 데이터 버전 갱신 통지.
		// 백플레인 pub/sub로 전 노드에 브로드캐스트된 버전이 도착하면 호출된다.
		// 앱은 해당 channel의 데이터를 다시 로드해 helix::Snapshot<T>::Store로 교체한다.
		// 단일 노드(클러스터 비활성)에서도 Server::PublishDataVersion 호출 시 로컬로 통지된다.
		virtual void OnDataVersion(std::string_view /*channel*/, std::uint64_t /*version*/) {}

		// 서버 graceful shutdown 완료 직전 1회.
		virtual void OnStop() {}

	protected:
		IServerApp() = default;
	};
}
