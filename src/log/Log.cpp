#include "Log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>

namespace helix::log
{
	namespace
	{
		std::mutex g_lock;
		LogConfig g_cfg;
		std::ofstream g_file;
		bool g_inited = false;

		const char* LevelName(Level l) noexcept
		{
			switch (l)
			{
			case Level::Debug: return "DEBUG";
			case Level::Info:  return "INFO";
			case Level::Warn:  return "WARN";
			case Level::Error: return "ERROR";
			}
			return "?";
		}

		std::string Timestamp()
		{
			const auto now = std::chrono::system_clock::now();
			const std::time_t t = std::chrono::system_clock::to_time_t(now);
			std::tm tm{};
#if defined(_WIN32)
			localtime_s(&tm, &t);
#else
			localtime_r(&t, &tm);
#endif
			char buf[32];
			std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
			return std::string(buf);
		}
	}

	Level ParseLevel(std::string_view s) noexcept
	{
		if (s == "debug") { return Level::Debug; }
		if (s == "warn")  { return Level::Warn; }
		if (s == "error") { return Level::Error; }
		return Level::Info;
	}

	void Init(const LogConfig& cfg)
	{
		std::lock_guard<std::mutex> guard(g_lock);
		g_cfg = cfg;
		if (cfg.toFile)
		{
			g_file.open(cfg.file, std::ios::out | std::ios::app);
		}
		g_inited = true;
	}

	Level CurrentLevel() noexcept
	{
		return g_inited ? g_cfg.level : Level::Info;
	}

	void Write(Level lvl, std::string_view msg)
	{
		// "2026-06-01T12:00:00 INFO  message"
		std::string line = Timestamp();
		line += ' ';
		line += LevelName(lvl);
		line += "  ";
		line.append(msg.data(), msg.size());
		line += '\n';

		std::lock_guard<std::mutex> guard(g_lock);
		const bool toStdout = !g_inited || g_cfg.toStdout;
		if (toStdout)
		{
			std::fputs(line.c_str(), stdout);
			std::fflush(stdout);
		}
		if (g_inited && g_cfg.toFile && g_file.is_open())
		{
			g_file << line;
			g_file.flush();
		}
	}
}
