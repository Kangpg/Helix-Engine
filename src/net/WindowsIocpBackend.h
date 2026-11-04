#pragma once

// =============================================================================
// Phase 1 — Windows IOCP 백엔드 (INetBackend 구현)
//
// 일반 IOCP 경로를 구현한다(AcceptEx + WSARecv + WSASend).
// RIO 데이터 경로는 별도 WindowsRioBackend/RioDataPlane에서 조립한다.
//
// Windows CTest가 일반 IOCP와 하이브리드의 송수신/취소 계약을 검증한다.
// =============================================================================

#include "INetBackend.h"

#include <WinSock2.h>
#include <MSWSock.h>
#include <Windows.h>

#include <atomic>

namespace helix::net
{
	class IIocpCompletionSource
	{
	public:
		virtual ~IIocpCompletionSource() = default;
		virtual int Drain(IoCompletion* out, int capacity) = 0;
	};
	class WindowsIocpBackend final : public INetBackend
	{
	public:
		WindowsIocpBackend() = default;
		~WindowsIocpBackend() override;

		bool Init(const BackendConfig& cfg) override;
		void Shutdown() override;

		bool Register(NativeHandle handle) override;
		NativeHandle CreateAcceptSocket() override;
		void CloseSocket(NativeHandle handle) override;
		HANDLE CompletionPort() const noexcept { return _iocp; }
		void SetCompletionSource(ULONG_PTR key, IIocpCompletionSource* source) noexcept { _sourceKey = key; _source = source; }

		IoContext* CreateContext(IoOp op) override;
		void DestroyContext(IoContext* ctx) override;

		bool PostAccept(IoContext* ctx) override;
		bool PostRecv(IoContext* ctx) override;
		bool PostSend(IoContext* ctx) override;

		int Poll(IoCompletion* out, int maxOut, int timeoutMs) override;
		void Wake(int count = 1) override;

		[[nodiscard]] const char* Name() const noexcept override { return _name; }

		// Acceptor가 listen 소켓에 AcceptEx를 걸 때, 수락 소켓을 listen 소켓의
		// 컨텍스트로 갱신할 수 있도록 listen 소켓 핸들을 공개.
		[[nodiscard]] LPFN_ACCEPTEX AcceptExFn() const noexcept { return _acceptEx; }

	private:
		bool LoadAcceptEx();

		HANDLE _iocp = nullptr;
		LPFN_ACCEPTEX _acceptEx = nullptr;
		ULONG_PTR _sourceKey = 0;
		IIocpCompletionSource* _source = nullptr;
		const char* _name = "iocp";
		std::atomic<bool> _initialized{ false };
	};
}
