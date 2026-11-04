#pragma once

// =============================================================================
// Phase 4 — Linux epoll 백엔드 (INetBackend 구현)
//
// epoll 은 reactor(준비 통지)지만, 코어는 proactor(완료 통지)를 전제로 한다.
// 따라서 이 백엔드가 reactor→proactor 를 에뮬레이션한다:
//   - Post{Accept,Recv,Send}() 는 해당 fd 에 관심(EPOLLIN/EPOLLOUT)을 EPOLLONESHOT
//     으로 무장만 한다.
//   - Poll() 이 epoll_wait 으로 준비된 fd 를 받아, 그 자리에서 실제
//     accept4()/recv()/send() 를 수행하고 "완료"로 포장해 돌려준다.
//
// 계약 일치(매우 중요):
//   - Recv: 1 Post = 1 recv() = 1 완료. recv()==0 → bytes=0(상위가 PeerClosed).
//   - Send: WSASend 처럼 "버퍼 전체 송신 후" 완료한다. 부분 송신이면 진행분을
//     기록하고 EPOLLOUT 재무장(완료 미발생) → 다 보내야 완료.
//   - Accept: accept4() 결과를 ctx->acceptSocket 에 채워 완료(에러 없음).
//   - Wake(): ctx==nullptr 완료 1건(종료/깨우기 신호). eventfd(SEMAPHORE) 사용.
//
// 스레드 안전: 다수 워커가 Poll() 동시 호출. EPOLLONESHOT 으로 한 fd 의 준비는
// 정확히 한 스레드에만 배달되고, 처리 후 재무장(다음 Post* 또는 부분 송신 재무장)
// 될 때까지 다시 안 뜬다. 레지스트리/무장 변경만 _mtx 로 보호(syscall 은 락 밖).
// =============================================================================

#include "INetBackend.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace helix::net
{
	class EpollBackend final : public INetBackend
	{
	public:
		EpollBackend() = default;
		~EpollBackend() override;

		bool Init(const BackendConfig& cfg) override;
		void Shutdown() override;

		bool Register(NativeHandle handle) override;
		NativeHandle CreateAcceptSocket() override { return kInvalidHandle; }
		void CloseSocket(NativeHandle handle) override;

		IoContext* CreateContext(IoOp op) override;
		void DestroyContext(IoContext* ctx) override;

		bool PostAccept(IoContext* ctx) override;
		bool PostRecv(IoContext* ctx) override;
		bool PostSend(IoContext* ctx) override;

		int Poll(IoCompletion* out, int maxOut, int timeoutMs) override;
		void Wake(int count = 1) override;

		[[nodiscard]] const char* Name() const noexcept override { return "epoll"; }

	private:
		// 등록된 fd 1개의 비동기 상태. recv/accept 는 한 슬롯(둘은 같은 fd 에서
		// 동시 발생하지 않음 — listen=accept, conn=recv)을 공유한다.
		struct FdState
		{
			int fd = -1;
			IoContext* pendingRecv = nullptr;  // Recv 또는 Accept 대기 ctx
			IoContext* pendingSend = nullptr;  // Send 대기 ctx
		};

		// 호출자가 _mtx 를 잡은 상태에서 호출. pending 상태로부터 관심 이벤트를
		// 재계산해 EPOLL_CTL_MOD 한다.
		bool RearmLocked(FdState& st);

		// _mtx 잡고 ctx 를 pending 슬롯에 넣고 재무장. (Post* / 재시도 공통 경로)
		bool ArmRecv(IoContext* ctx);
		bool ArmSend(IoContext* ctx);

		int _epfd = -1;
		int _wakefd = -1;                 // eventfd (EFD_SEMAPHORE)
		std::uint32_t _maxConnections = 0;

		std::mutex _mtx;
		std::unordered_map<int, std::unique_ptr<FdState>> _fds;  // fd → 상태
	};
}
