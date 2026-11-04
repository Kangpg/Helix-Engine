#pragma once

// =============================================================================
// 내부 추상화 #1 — 플랫폼 I/O 백엔드 (proactor 스타일로 통일)
//
// Windows = IOCP / IOCP+RIO (proactor 네이티브)
// Linux   = epoll (reactor를 내부에서 proactor로 에뮬레이션)
//
// 백엔드는 설정으로 선택한다(IoBackendKind). RIO는 Windows 전용 고속 경로이며
// iocp_rio를 명시하면 실제 RIO 초기화를 요구하며 실패를 숨기지 않는다.
//
// "proactor"로 통일: Post*() 로 비동기 작업을 건 뒤, Poll()이 "이미 완료된"
// 작업을 돌려준다(요청한 바이트 전송이 끝난 상태). epoll 백엔드는 내부에서
// readiness를 받아 실제 recv/send를 수행한 후 완료로 포장한다.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <system_error>

namespace helix::net
{
	// 플랫폼 소켓 핸들의 불투명 표현 (Windows SOCKET / POSIX fd).
	using NativeHandle = std::uintptr_t;
	inline constexpr NativeHandle kInvalidHandle = static_cast<NativeHandle>(~0ull);

	// 사용자가 설정으로 고르는 I/O 백엔드 종류.
	//   Auto     : OS 기본값 (Windows=Iocp / Linux=Epoll)
	//   Iocp     : 일반 IOCP (기본, 호환·단순)
	//   IocpRio  : IOCP AcceptEx + RIO 송수신. 미지원/자원 부족은 시작 실패로 보고.
	//   Epoll    : Linux epoll
	enum class IoBackendKind : std::uint8_t
	{
		Auto = 0,
		Iocp,
		IocpRio,
		Epoll,
	};

	[[nodiscard]] constexpr const char* ToString(IoBackendKind k) noexcept
	{
		switch (k)
		{
		case IoBackendKind::Auto:    return "auto";
		case IoBackendKind::Iocp:    return "iocp";
		case IoBackendKind::IocpRio: return "iocp+rio";
		case IoBackendKind::Epoll:   return "epoll";
		}
		return "unknown";
	}

	enum class IoOp : std::uint8_t { Accept, Recv, Send, Connect };

	// 한 비동기 작업의 컨텍스트. 플랫폼 백엔드가 실제 타입을 소유한다
	// (Windows는 OVERLAPPED를 내부에 품은 파생 타입). 호출자(세션 계층)는
	// CreateContext()로 받아 buffer/length/user를 채운 뒤 Post*()에 넘긴다.
	struct IoContext
	{
		IoOp op = IoOp::Recv;
		NativeHandle handle = kInvalidHandle;  // 대상 소켓
		void* user = nullptr;                  // 세션 포인터 등 (백엔드는 해석 안 함)
		std::byte* buffer = nullptr;
		std::uint32_t length = 0;

		// Accept 전용: 백엔드가 미리 만들어 둔 수락 소켓을 여기 채워 PostAccept에 넘긴다.
		NativeHandle acceptSocket = kInvalidHandle;

		virtual ~IoContext() = default;
	};

	// Poll()이 돌려주는 완료 1건. ctx == nullptr 이면 Wake()에 의한 깨우기 신호.
	struct IoCompletion
	{
		IoContext* ctx = nullptr;
		std::uint32_t bytesTransferred = 0;
		std::error_code error{};   // 성공이면 비어 있음
	};

	struct BackendConfig
	{
		IoBackendKind kind = IoBackendKind::Auto;
		std::uint32_t maxConnections = 10000;
		std::uint32_t rioBufferSize = 16384;
		std::uint32_t rioCompletionQueueSize = 32768;
	};

	class INetBackend
	{
	public:
		virtual ~INetBackend() = default;

		virtual bool Init(const BackendConfig& cfg) = 0;
		virtual void Shutdown() = 0;

		// 소켓을 백엔드에 등록(IOCP associate / epoll add).
		virtual bool Register(NativeHandle handle) = 0;
		// 수락 소켓 생성/종료도 백엔드가 맡아 등록 I/O의 생성 옵션과 수명을 유지.
		virtual NativeHandle CreateAcceptSocket() = 0;
		virtual void CloseSocket(NativeHandle handle) = 0;

		// 비동기 작업 컨텍스트 수명: 백엔드가 플랫폼 세부(OVERLAPPED 등)를 소유.
		virtual IoContext* CreateContext(IoOp op) = 0;
		virtual void DestroyContext(IoContext* ctx) = 0;

		// 비동기 작업 게시. 완료는 추후 Poll()로 회수된다.
		virtual bool PostAccept(IoContext* ctx) = 0;
		virtual bool PostRecv(IoContext* ctx) = 0;
		virtual bool PostSend(IoContext* ctx) = 0;

		// 완료를 최대 maxOut개 회수. 반환: 채운 개수. 0 = 타임아웃.
		// timeoutMs < 0 이면 무한 대기. 여러 I/O 스레드가 동시 호출 가능.
		virtual int Poll(IoCompletion* out, int maxOut, int timeoutMs) = 0;

		// 대기 중인 Poll들을 깨우기 위한 신호 게시(graceful shutdown). count회 게시.
		virtual void Wake(int count = 1) = 0;

		// 실제로 활성화된 백엔드 이름("iocp" / "iocp+rio" / "epoll").
		// (요청이 IocpRio여도 폴백되면 "iocp"를 반환할 수 있다)
		[[nodiscard]] virtual const char* Name() const noexcept = 0;
	};
}
