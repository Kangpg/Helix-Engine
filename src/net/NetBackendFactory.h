#pragma once

// =============================================================================
// 백엔드 팩토리 — 설정(IoBackendKind)에 따라 플랫폼 백엔드를 생성한다.
// Windows auto/iocp는 일반 IOCP. 명시적 iocp_rio는 실제 RIO를 요구한다.
//
// 플러그인(DLL) 방향으로 확장 시: 이 팩토리가 모듈 레지스트리를 조회해
// 동적 로드된 백엔드를 반환하도록 바꾸면 된다(코어 무변경).
// =============================================================================

#include "INetBackend.h"

#include <memory>

namespace helix::net
{
	// 현재 플랫폼/설정에 맞는 백엔드를 생성. 실패 시 nullptr.
	// Windows: Iocp / IocpRio(→ 폴백 가능) / Auto
	// Linux  : Epoll (추후 Phase 4)
	std::unique_ptr<INetBackend> CreateNetBackend(const BackendConfig& cfg);
}
