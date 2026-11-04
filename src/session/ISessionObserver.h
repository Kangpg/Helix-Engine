#pragma once

// =============================================================================
// Phase 6 — 세션 수명 옵저버 (ISP/DIP)
//
// SessionManager는 세션 수명/조회만 책임진다(SRP). scale-out 디렉터리 등록은
// 별개의 관심사이므로, 뚱뚱한 결합 대신 이 작은 옵저버 인터페이스로 알린다.
// ServerCore가 구현해 백플레인(IBackplane)으로 위임한다. 옵저버가 없으면
// (단일 노드) SessionManager는 디렉터리를 전혀 모른다.
// =============================================================================

#include "helix/types.h"

namespace helix::core
{
	class ISessionObserver
	{
	public:
		virtual ~ISessionObserver() = default;

		// 세션 생성 직후(디렉터리 등록). 호출 컨텍스트: accept 완료(워커 스레드).
		virtual void OnSessionOpened(helix::SessionId id) = 0;

		// 세션 최종 해제 시(디렉터리 제거). 호출 컨텍스트: 마지막 Release.
		virtual void OnSessionClosed(helix::SessionId id) = 0;
	};
}
