#pragma once

// =============================================================================
// Helix public SDK — 런타임 데이터 핫스왑 (불변 스냅샷 + atomic 포인터 교체)
//
// 설정/게임 데이터 테이블 등을 무중단으로 실시간 갱신한다.
//   읽기(핫패스): auto snap = data.Load();   // 락 없음
//                 snap->...                    // 스냅샷은 불변
//   쓰기        : data.Store(BuildNewSnapshot());  // 통째 교체
//
// 진행 중 reader는 옛 스냅샷, 신규는 새 스냅샷 → torn read 없음.
// 회수: shared_ptr refcount가 마지막 reader 종료 시 자동. 고경합이면 RCU/hazard로.
//
// scale-out(Phase 6): 데이터 버전을 백플레인 pub/sub로 브로드캐스트 → 각 노드가
// 새 스냅샷을 Load 후 Store → 전 클러스터 동시 갱신.
//
// 주의: std::atomic<std::shared_ptr<T>>는 C++20. (MSVC VS2022 / Clang 최신 지원)
// =============================================================================

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

namespace helix
{
	template <class T>
	class Snapshot
	{
	public:
		using Ptr = std::shared_ptr<const T>;

		Snapshot() = default;
		explicit Snapshot(Ptr initial) : _ptr(std::move(initial)) {}

		// 핫패스. 현재 스냅샷의 소유 복사본(사용 중 회수 안전).
		[[nodiscard]] Ptr Load() const noexcept
		{
			return _ptr.load(std::memory_order_acquire);
		}

		// 새 스냅샷으로 원자 교체. next는 게시 후 불변으로 취급할 것.
		void Store(Ptr next) noexcept
		{
			_ptr.store(std::move(next), std::memory_order_release);
		}

		// 이전 스냅샷을 돌려받으며 교체(버전 비교/롤백용).
		[[nodiscard]] Ptr Exchange(Ptr next) noexcept
		{
			return _ptr.exchange(std::move(next), std::memory_order_acq_rel);
		}

	private:
		std::atomic<Ptr> _ptr;
	};

	// 버전 태그를 곁들인 스냅샷 — 백플레인 브로드캐스트와 맞물려
	// "이미 최신이면 스킵" 처리에 쓴다.
	template <class T>
	struct Versioned
	{
		std::uint64_t version = 0;
		std::shared_ptr<const T> data;
	};
}
