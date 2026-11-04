#include "KcpTransport.h"
#include "../../log/Log.h"

#include "../../net/PlatformNet.h"

#include "ikcp.h"

#include <chrono>
#include <cstring>
#include <vector>

namespace helix::transport
{
	namespace { constexpr std::uintptr_t kInvalidSock = static_cast<std::uintptr_t>(INVALID_SOCKET); }

	// 전송 수준 연결 1개: ikcpcb + 원격 주소 + output용 소켓 핸들 복사본.
	struct KcpConn
	{
		ConnId id = kInvalidConn;
		ikcpcb* kcp = nullptr;
		std::uintptr_t sock = kInvalidSock;   // output 콜백이 sendto에 사용(소유 아님)
		sockaddr_storage addr{};
		socklen_t addrLen = 0;
		std::string key;
		helix::Endpoint remote;
		std::uint32_t lastRecvMs = 0;
	};

	namespace
	{
		// ikcp output 콜백: 한 datagram을 원격으로 송신. user = KcpConn*.
		int KcpOutput(const char* buf, int len, ikcpcb* /*kcp*/, void* user)
		{
			auto* c = static_cast<KcpConn*>(user);
			if (c == nullptr) { return -1; }
			::sendto(static_cast<SOCKET>(c->sock), buf, len, 0,
			         reinterpret_cast<const sockaddr*>(&c->addr), c->addrLen);
			return 0;
		}

		helix::Endpoint EndpointOf(const sockaddr_storage& sa)
		{
			char ip[INET6_ADDRSTRLEN] = { 0 };
			std::uint16_t port = 0;
			if (sa.ss_family == AF_INET)
			{
				const auto* s4 = reinterpret_cast<const sockaddr_in*>(&sa);
				::inet_ntop(AF_INET, &s4->sin_addr, ip, sizeof(ip));
				port = ::ntohs(s4->sin_port);
			}
			else if (sa.ss_family == AF_INET6)
			{
				const auto* s6 = reinterpret_cast<const sockaddr_in6*>(&sa);
				::inet_ntop(AF_INET6, &s6->sin6_addr, ip, sizeof(ip));
				port = ::ntohs(s6->sin6_port);
			}
			return helix::Endpoint{ std::string(ip), port };
		}

		std::uint64_t NowTickMs()
		{
			using namespace std::chrono;
			return static_cast<std::uint64_t>(
				duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
		}
	}

	KcpTransport::KcpTransport()
		: _sock(kInvalidSock)
	{
	}

	KcpTransport::~KcpTransport()
	{
		Stop();
	}

	std::uint32_t KcpTransport::NowMs() const noexcept
	{
		return static_cast<std::uint32_t>(NowTickMs() - _startTick);
	}

	bool KcpTransport::Init(const TransportConfig& cfg, ITransportSink& sink, helix::net::INetBackend* /*backend*/)
	{
		_cfg = cfg;
		_sink = &sink;
		_startTick = NowTickMs();

		SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (s == INVALID_SOCKET) { return false; }

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = ::htons(cfg.bind.port);
		if (::inet_pton(AF_INET, cfg.bind.ip.c_str(), &addr.sin_addr) != 1)
		{
			addr.sin_addr.s_addr = INADDR_ANY;
		}
		if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
		{
			::closesocket(s);
			return false;
		}

		int rcvbuf = 1 << 21;  // 2 MiB 수신 버퍼(돌발 트래픽 흡수)
		::setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));

		// 수신 타임아웃: RecvLoop가 종료 신호를 주기적으로 확인하고 빠져나오게 한다.
		// (Windows는 closesocket이 recvfrom을 깨우지만, Linux에서는 close가
		//  블로킹 recvfrom을 안정적으로 깨우지 못하므로 타임아웃이 필수다.)
		{
			const int timeoutMs = 200;
#if defined(_WIN32)
			DWORD tv = static_cast<DWORD>(timeoutMs);
			::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
			struct timeval tv{};
			tv.tv_sec = timeoutMs / 1000;
			tv.tv_usec = (timeoutMs % 1000) * 1000;
			::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif
		}

		_sock = static_cast<std::uintptr_t>(s);
		return true;
	}

	bool KcpTransport::Listen()
	{
		if (_sock == kInvalidSock) { return false; }
		_running.store(true, std::memory_order_release);
		_recvThread = std::thread([this] { RecvLoop(); });
		_updateThread = std::thread([this] { UpdateLoop(); });
		HLOG_INFO("kcp transport listening on " + _cfg.bind.ip + ":" + std::to_string(_cfg.bind.port) + "/udp");
		return true;
	}

	void KcpTransport::Stop()
	{
		if (!_running.exchange(false, std::memory_order_acq_rel)) { return; }

		if (_sock != kInvalidSock)
		{
			::closesocket(static_cast<SOCKET>(_sock));  // recvfrom 블로킹 해제(Win) / 타임아웃 백스톱(Linux)
			_sock = kInvalidSock;
		}
		if (_recvThread.joinable()) { _recvThread.join(); }
		if (_updateThread.joinable()) { _updateThread.join(); }

		std::lock_guard<std::mutex> lk(_mx);
		for (auto& [id, c] : _conns)
		{
			if (c->kcp != nullptr) { ikcp_release(c->kcp); }
		}
		_conns.clear();
		_byKey.clear();
	}

	void KcpTransport::RecvLoop()
	{
		std::vector<char> buf(65536);

		while (_running.load(std::memory_order_acquire))
		{
			sockaddr_storage from{};
			socklen_t fromLen = static_cast<socklen_t>(sizeof(from));
			const int n = static_cast<int>(::recvfrom(static_cast<SOCKET>(_sock), buf.data(),
			                                          static_cast<int>(buf.size()), 0,
			                                          reinterpret_cast<sockaddr*>(&from), &fromLen));
			if (n <= 0)
			{
				if (!_running.load(std::memory_order_acquire)) { break; }
				continue;  // 타임아웃/일시 오류 — 계속
			}
			// ikcp 헤더(IKCP_OVERHEAD=24)보다 작으면 무시.
			if (n < 24) { continue; }

			const std::string key(reinterpret_cast<const char*>(&from), static_cast<std::size_t>(fromLen));

			ConnId connId = kInvalidConn;
			ConnId newId = kInvalidConn;
			helix::Endpoint newRemote;
			std::vector<std::vector<std::byte>> msgs;

			{
				std::lock_guard<std::mutex> lk(_mx);
				KcpConn* conn = nullptr;
				const auto it = _byKey.find(key);
				if (it == _byKey.end())
				{
					if (_conns.size() >= _cfg.maxConnections) { continue; }  // 용량 초과 드롭

					const IUINT32 conv = ikcp_getconv(buf.data());
					auto c = std::make_unique<KcpConn>();
					c->id = _nextConn.fetch_add(1, std::memory_order_relaxed);
					c->sock = _sock;
					std::memcpy(&c->addr, &from, static_cast<std::size_t>(fromLen));
					c->addrLen = fromLen;
					c->key = key;
					c->remote = EndpointOf(from);
					c->kcp = ikcp_create(conv, c.get());
					ikcp_setoutput(c->kcp, &KcpOutput);
					ikcp_nodelay(c->kcp, 1, 10, 2, 1);  // 저지연 모드(nodelay/interval10/resend2/nc1)
					ikcp_wndsize(c->kcp, 256, 256);
					c->lastRecvMs = NowMs();

					conn = c.get();
					connId = c->id;
					newId = c->id;
					newRemote = c->remote;
					_byKey.emplace(key, c->id);
					_conns.emplace(c->id, std::move(c));
				}
				else
				{
					connId = it->second;
					conn = _conns[connId].get();
				}

				ikcp_input(conn->kcp, buf.data(), static_cast<long>(n));
				conn->lastRecvMs = NowMs();

				for (;;)
				{
					const int sz = ikcp_peeksize(conn->kcp);
					if (sz <= 0) { break; }
					std::vector<std::byte> m(static_cast<std::size_t>(sz));
					const int got = ikcp_recv(conn->kcp, reinterpret_cast<char*>(m.data()), sz);
					if (got <= 0) { break; }
					m.resize(static_cast<std::size_t>(got));
					msgs.push_back(std::move(m));
				}
			}

			// 콜백은 락 밖에서(코어가 다시 Send/Close를 호출할 수 있으므로).
			if (newId != kInvalidConn) { _sink->OnAccepted(newId, newRemote); }
			for (const auto& m : msgs)
			{
				_sink->OnData(connId, helix::ByteSpan(m.data(), m.size()));
			}
		}
	}

	void KcpTransport::UpdateLoop()
	{
		while (_running.load(std::memory_order_acquire))
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));

			std::vector<ConnId> timedOut;
			{
				std::lock_guard<std::mutex> lk(_mx);
				const std::uint32_t now = NowMs();
				for (auto& [id, c] : _conns)
				{
					ikcp_update(c->kcp, now);
					if (now - c->lastRecvMs > _idleTimeoutMs) { timedOut.push_back(id); }
				}
				for (const ConnId id : timedOut)
				{
					const auto it = _conns.find(id);
					if (it != _conns.end())
					{
						_byKey.erase(it->second->key);
						if (it->second->kcp != nullptr) { ikcp_release(it->second->kcp); }
						_conns.erase(it);
					}
				}
			}
			for (const ConnId id : timedOut)
			{
				_sink->OnClosed(id, helix::SessionCloseReason::IdleTimeout);
			}
		}
	}

	bool KcpTransport::Send(ConnId conn, helix::ByteSpan bytes)
	{
		std::lock_guard<std::mutex> lk(_mx);
		const auto it = _conns.find(conn);
		if (it == _conns.end()) { return false; }
		if (ikcp_send(it->second->kcp,
		              reinterpret_cast<const char*>(bytes.data()),
		              static_cast<int>(bytes.size())) < 0)
		{
			return false;
		}
		ikcp_flush(it->second->kcp);  // 즉시 송신(지연 최소화)
		return true;
	}

	void KcpTransport::Close(ConnId conn, helix::SessionCloseReason reason)
	{
		bool erased = false;
		{
			std::lock_guard<std::mutex> lk(_mx);
			const auto it = _conns.find(conn);
			if (it != _conns.end())
			{
				_byKey.erase(it->second->key);
				if (it->second->kcp != nullptr) { ikcp_release(it->second->kcp); }
				_conns.erase(it);
				erased = true;
			}
		}
		if (erased) { _sink->OnClosed(conn, reason); }
	}
}
