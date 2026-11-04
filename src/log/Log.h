#pragma once

// =============================================================================
// Phase 3 — 로거 (레벨 · 스레드 안전 · stdout/파일)
//
// 컨테이너 표준에 맞춰 stdout 출력을
// 1순위로 한다. Phase 3은 mutex 기반 동기 구현(정확·단순). 비동기 큐는 후속.
//
// 사용: HLOG_INFO("listening on :5000");  (레벨 필터는 매크로에서 선검사)
// =============================================================================

#include <string>
#include <string_view>

namespace helix::log
{
	enum class Level : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };

	[[nodiscard]] Level ParseLevel(std::string_view s) noexcept;

	struct LogConfig
	{
		Level level = Level::Info;
		bool toStdout = true;
		bool toFile = false;
		std::string file = "helix.log";
	};

	void Init(const LogConfig& cfg);
	void Write(Level lvl, std::string_view msg);
	[[nodiscard]] Level CurrentLevel() noexcept;
}

#define HLOG(lvl, msg)                                                   \
	do {                                                                 \
		if (static_cast<int>(lvl) >= static_cast<int>(::helix::log::CurrentLevel())) \
			::helix::log::Write((lvl), (msg));                           \
	} while (0)

#define HLOG_DEBUG(msg) HLOG(::helix::log::Level::Debug, msg)
#define HLOG_INFO(msg)  HLOG(::helix::log::Level::Info,  msg)
#define HLOG_WARN(msg)  HLOG(::helix::log::Level::Warn,  msg)
#define HLOG_ERROR(msg) HLOG(::helix::log::Level::Error, msg)
