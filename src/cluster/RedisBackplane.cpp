#include "RedisBackplane.h"
#include "../log/Log.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace helix::cluster
{
	namespace
	{
		// "redis://host:port/db" → host/port/db (모든 부분 선택적).
		void ParseUrl(const std::string& url, std::string& host, std::uint16_t& port, int& db)
		{
			std::string s = url;
			const std::string scheme = "redis://";
			if (s.rfind(scheme, 0) == 0) { s = s.substr(scheme.size()); }

			// db
			const std::size_t slash = s.find('/');
			if (slash != std::string::npos)
			{
				const std::string dbStr = s.substr(slash + 1);
				if (!dbStr.empty()) { db = std::atoi(dbStr.c_str()); }
				s = s.substr(0, slash);
			}
			// host:port
			const std::size_t colon = s.find(':');
			if (colon != std::string::npos)
			{
				host = s.substr(0, colon);
				const std::string portStr = s.substr(colon + 1);
				if (!portStr.empty()) { port = static_cast<std::uint16_t>(std::atoi(portStr.c_str())); }
			}
			else if (!s.empty())
			{
				host = s;
			}
		}

		std::string Be64(std::uint64_t v)
		{
			std::string s(8, '\0');
			for (int i = 0; i < 8; ++i)
			{
				s[static_cast<std::size_t>(i)] = static_cast<char>((v >> (8 * (7 - i))) & 0xFF);
			}
			return s;
		}

		std::uint64_t Rd64(const char* p)
		{
			std::uint64_t v = 0;
			for (int i = 0; i < 8; ++i)
			{
				v = (v << 8) | static_cast<std::uint8_t>(p[i]);
			}
			return v;
		}
	}

	RedisBackplane::~RedisBackplane()
	{
		Disconnect();
	}

	std::string RedisBackplane::SessKey(helix::SessionId sid) const
	{
		return "helix:sess:" + std::to_string(sid);
	}

	bool RedisBackplane::Connect(const BackplaneConfig& cfg, IBackplaneSink& sink)
	{
		_cfg = cfg;
		_sink = &sink;

		ParseUrl(cfg.url, _host, _port, _db);
		_routeChannel = "helix:route:" + std::to_string(cfg.nodeId);
		_dataChannel = "helix:dataver";

		// 명령 연결 수립(수신 타임아웃으로 영구 블로킹 방지).
		{
			std::lock_guard<std::mutex> lk(_cmdMx);
			if (!_cmd.Connect(_host, _port, 5000))
			{
				HLOG_ERROR("backplane: redis connect failed " + _host + ":" + std::to_string(_port));
				return false;
			}
			if (_db != 0)
			{
				RespValue r;
				_cmd.Command({ "SELECT", std::to_string(_db) }, r);
			}
		}

		_running.store(true, std::memory_order_release);
		_subThread = std::thread([this] { SubscribeLoop(); });

		HLOG_INFO("backplane: redis connected " + _host + ":" + std::to_string(_port)
		          + " node=" + std::to_string(cfg.nodeId)
		          + " channel=" + _routeChannel);
		return true;
	}

	void RedisBackplane::Disconnect()
	{
		if (!_running.exchange(false, std::memory_order_acq_rel))
		{
			return;
		}
		_sub.Close();   // 구독 스레드의 블로킹 recv 를 깨운다.
		if (_subThread.joinable()) { _subThread.join(); }

		std::lock_guard<std::mutex> lk(_cmdMx);
		_cmd.Close();
	}

	bool RedisBackplane::EnsureCmd()
	{
		// _cmdMx 보유 가정.
		if (_cmd.IsOpen()) { return true; }
		if (!_cmd.Connect(_host, _port, 5000)) { return false; }
		if (_db != 0)
		{
			RespValue r;
			_cmd.Command({ "SELECT", std::to_string(_db) }, r);
		}
		return true;
	}

	void RedisBackplane::RegisterSession(helix::SessionId sid)
	{
		{
			std::lock_guard<std::mutex> lk(_localMx);
			_local.insert(sid);
		}
		std::lock_guard<std::mutex> lk(_cmdMx);
		if (!EnsureCmd()) { return; }
		RespValue r;
		if (!_cmd.Command({ "SET", SessKey(sid), std::to_string(_cfg.nodeId),
		                    "EX", std::to_string(_cfg.sessionTtlSec) }, r))
		{
			_cmd.Close();
		}
	}

	void RedisBackplane::UnregisterSession(helix::SessionId sid)
	{
		{
			std::lock_guard<std::mutex> lk(_localMx);
			_local.erase(sid);
		}
		std::lock_guard<std::mutex> lk(_cmdMx);
		if (!EnsureCmd()) { return; }
		RespValue r;
		if (!_cmd.Command({ "DEL", SessKey(sid) }, r))
		{
			_cmd.Close();
		}
	}

	std::optional<NodeId> RedisBackplane::GetNodeLocked(helix::SessionId sid)
	{
		// _cmdMx 보유 가정.
		if (!EnsureCmd()) { return std::nullopt; }
		RespValue r;
		if (!_cmd.Command({ "GET", SessKey(sid) }, r))
		{
			_cmd.Close();
			return std::nullopt;
		}
		if (r.type != RespValue::Type::Bulk || r.IsNil() || r.str.empty())
		{
			return std::nullopt;
		}
		return static_cast<NodeId>(std::strtoul(r.str.c_str(), nullptr, 10));
	}

	std::optional<NodeId> RedisBackplane::Locate(helix::SessionId sid)
	{
		std::lock_guard<std::mutex> lk(_cmdMx);
		return GetNodeLocked(sid);
	}

	void RedisBackplane::Heartbeat()
	{
		std::vector<helix::SessionId> snapshot;
		{
			std::lock_guard<std::mutex> lk(_localMx);
			snapshot.assign(_local.begin(), _local.end());
		}
		if (snapshot.empty()) { return; }

		std::lock_guard<std::mutex> lk(_cmdMx);
		if (!EnsureCmd()) { return; }

		// 파이프라인: EXPIRE 일괄 전송 후 응답 일괄 회수(라운드트립 1회).
		const std::string ttl = std::to_string(_cfg.sessionTtlSec);
		for (const auto sid : snapshot)
		{
			if (!_cmd.SendCommand({ "EXPIRE", SessKey(sid), ttl })) { _cmd.Close(); return; }
		}
		for (std::size_t i = 0; i < snapshot.size(); ++i)
		{
			RespValue r;
			if (!_cmd.ReadReply(r)) { _cmd.Close(); return; }
		}
	}

	bool RedisBackplane::RouteToSession(helix::SessionId target, helix::ByteSpan payload)
	{
		std::lock_guard<std::mutex> lk(_cmdMx);

		const auto node = GetNodeLocked(target);
		if (!node.has_value())
		{
			return false; // 디렉터리에 없음(만료/미등록)
		}

		std::string msg = Be64(static_cast<std::uint64_t>(target));
		msg.append(reinterpret_cast<const char*>(payload.data()), payload.size());

		const std::string chan = "helix:route:" + std::to_string(*node);
		RespValue r;
		if (!_cmd.Command({ "PUBLISH", chan, msg }, r))
		{
			_cmd.Close();
			return false;
		}
		return true;
	}

	void RedisBackplane::PublishDataVersion(std::string_view channel, std::uint64_t version)
	{
		std::string msg = Be64(version);
		msg.append(channel.data(), channel.size());

		std::lock_guard<std::mutex> lk(_cmdMx);
		if (!EnsureCmd()) { return; }
		RespValue r;
		if (!_cmd.Command({ "PUBLISH", _dataChannel, msg }, r))
		{
			_cmd.Close();
		}
	}

	bool RedisBackplane::KvSet(std::string_view key, std::string_view value, std::uint32_t ttlSec)
	{
		std::lock_guard<std::mutex> lk(_cmdMx);
		if (!EnsureCmd()) { return false; }
		RespValue r;
		std::vector<std::string> args{ "SET", "helix:kv:" + std::string(key), std::string(value) };
		if (ttlSec > 0)
		{
			args.emplace_back("EX");
			args.emplace_back(std::to_string(ttlSec));
		}
		if (!_cmd.Command(args, r))
		{
			_cmd.Close();
			return false;
		}
		return r.type == RespValue::Type::Simple && r.str == "OK";
	}

	std::optional<std::string> RedisBackplane::KvGet(std::string_view key)
	{
		std::lock_guard<std::mutex> lk(_cmdMx);
		if (!EnsureCmd()) { return std::nullopt; }
		RespValue r;
		if (!_cmd.Command({ "GET", "helix:kv:" + std::string(key) }, r))
		{
			_cmd.Close();
			return std::nullopt;
		}
		if (r.type != RespValue::Type::Bulk || r.IsNil())
		{
			return std::nullopt;
		}
		return r.str;
	}

	bool RedisBackplane::OpenSubscriber()
	{
		if (!_sub.Connect(_host, _port, 0)) { return false; }  // 블로킹(타임아웃 없음)
		RespValue r;
		if (_db != 0)
		{
			_sub.Command({ "SELECT", std::to_string(_db) }, r);
		}
		// 두 채널 구독(라우팅 + 데이터 버전). 구독 확인 응답은 무시.
		if (!_sub.SendCommand({ "SUBSCRIBE", _routeChannel, _dataChannel })) { _sub.Close(); return false; }
		for (int i = 0; i < 2; ++i)
		{
			RespValue ack;
			if (!_sub.ReadReply(ack)) { _sub.Close(); return false; }
		}
		return true;
	}

	void RedisBackplane::Dispatch(const std::string& channel, const std::string& payload)
	{
		if (_sink == nullptr) { return; }

		if (channel == _routeChannel)
		{
			if (payload.size() < 8) { return; }
			const helix::SessionId sid = static_cast<helix::SessionId>(Rd64(payload.data()));
			const auto* bytes = reinterpret_cast<const std::byte*>(payload.data() + 8);
			_sink->OnRouted(sid, helix::ByteSpan(bytes, payload.size() - 8));
		}
		else if (channel == _dataChannel)
		{
			if (payload.size() < 8) { return; }
			const std::uint64_t version = Rd64(payload.data());
			_sink->OnDataVersion(std::string_view(payload.data() + 8, payload.size() - 8), version);
		}
	}

	void RedisBackplane::SubscribeLoop()
	{
		while (_running.load(std::memory_order_acquire))
		{
			if (!OpenSubscriber())
			{
				if (!_running.load(std::memory_order_acquire)) { break; }
				HLOG_WARN("backplane: subscriber connect failed — retrying");
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				continue;
			}

			HLOG_DEBUG("backplane: subscribed " + _routeChannel + " + " + _dataChannel);

			while (_running.load(std::memory_order_acquire))
			{
				RespValue r;
				if (!_sub.ReadReply(r))
				{
					break; // 연결 끊김 → 바깥 루프에서 재연결
				}
				// pub/sub 푸시: ["message", channel, payload]
				if (r.type == RespValue::Type::Array && r.elements.size() >= 3
				    && r.elements[0].str == "message")
				{
					Dispatch(r.elements[1].str, r.elements[2].str);
				}
			}

			_sub.Close();
			if (_running.load(std::memory_order_acquire))
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
			}
		}
	}
}
