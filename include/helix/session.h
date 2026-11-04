#pragma once

// =============================================================================
// Helix public SDK — Session
//
// 하나의 클라이언트 연결을 나타내는 애플리케이션 측 핸들.
// 엔진이 구현하며, 업체 코드는 이 인터페이스만 통해 연결을 제어한다.
// 인스턴스 수명은 엔진이 소유한다(OnConnect ~ OnDisconnect 구간에서 유효).
// 콜백 밖에서 보관하려면 SessionId를 저장하고 Server로 다시 조회할 것.
// =============================================================================

#include "types.h"

namespace helix
{
	class Session
	{
	public:
		virtual ~Session() = default;

		[[nodiscard]] virtual SessionId Id() const noexcept = 0;

		// 메시지 1건 전송. 프레이밍(길이 프리픽스)은 엔진이 자동 부착한다.
		// 송신은 세션별로 직렬화되고, 큐가 가득 차면 BackpressureFull을 반환.
		// 스레드 안전: 어느 스레드에서 호출해도 안전.
		virtual Status Send(ByteSpan message) = 0;

		// 연결 종료 요청. 진행 중 송신을 비운 뒤(graceful) 닫는다.
		virtual void Close(SessionCloseReason reason = SessionCloseReason::Kicked) = 0;

		[[nodiscard]] virtual Endpoint Remote() const = 0;

		[[nodiscard]] virtual TransportType Transport() const noexcept = 0;

		// 애플리케이션 측 per-session 상태 포인터. 엔진은 내용을 해석하지 않는다.
		virtual void  SetUserData(void* ptr) noexcept = 0;
		[[nodiscard]] virtual void* GetUserData() const noexcept = 0;

	protected:
		Session() = default;
	};
}
