#pragma once

// =============================================================================
// Phase 4 — 플랫폼 소켓 호환 셰임 (내부 전용)
//
// SRP: "Winsock 스타일 호출부가 POSIX에서도 그대로 컴파일되도록" 하는 단일 지점.
// 코어/세션/Acceptor 는 SOCKET/closesocket/WSA* 표기를 유지하고, 플랫폼 차이는
// 오직 이 헤더에서만 흡수한다(다른 곳에 #ifdef _WIN32 를 흩뿌리지 않는다).
//
// 공개 헤더(include/helix/)에는 절대 포함하지 않는다 — 저수준 타입 비노출 규칙.
// =============================================================================

#if defined(_WIN32)

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <WinSock2.h>
#  include <WS2tcpip.h>
#  include <MSWSock.h>

#else  // ----------------------------------------------------------------- POSIX

#  include <sys/socket.h>
#  include <sys/types.h>
#  include <sys/time.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <cerrno>

// Winsock 핸들/상수 대응.
using SOCKET = int;
inline constexpr SOCKET INVALID_SOCKET = -1;
inline constexpr int    SOCKET_ERROR   = -1;
inline constexpr int    WSA_FLAG_OVERLAPPED = 0;

// AcceptEx 전용 옵션. POSIX 에선 의미 없음(호출 자체를 _WIN32 가드로 막지만,
// 혹시 참조돼도 컴파일되도록 상수만 둔다).
#  ifndef SO_UPDATE_ACCEPT_CONTEXT
inline constexpr int SO_UPDATE_ACCEPT_CONTEXT = 0;
#  endif

inline int closesocket(SOCKET s) noexcept { return ::close(s); }
inline int WSAGetLastError() noexcept { return errno; }

// WSASocketW(af, type, proto, protocolInfo, group, flags) → socket(af,type,proto).
// 오버랩 플래그/프로토콜 정보/그룹은 POSIX 에 대응물이 없어 무시한다.
inline SOCKET WSASocketW(int af, int type, int proto,
                         void* /*lpProtocolInfo*/, unsigned int /*group*/,
                         unsigned int /*flags*/) noexcept
{
	return ::socket(af, type, proto);
}

// Winsock 시작/종료는 POSIX 에선 불필요 → no-op.
struct WSADATA { int _unused; };
inline int WSAStartup(unsigned short /*version*/, WSADATA* /*data*/) noexcept { return 0; }
inline int WSACleanup() noexcept { return 0; }

#  ifndef MAKEWORD
#    define MAKEWORD(a, b) \
	((unsigned short)(((unsigned char)(a)) | ((unsigned short)((unsigned char)(b))) << 8))
#  endif

#endif  // _WIN32
