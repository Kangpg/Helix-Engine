#include "RespClient.h"

#include "../net/PlatformNet.h"

#include <cstdlib>
#include <cstring>

namespace helix::cluster
{
	namespace
	{
		constexpr std::uintptr_t kInvalid = static_cast<std::uintptr_t>(INVALID_SOCKET);
	}

	RespConn::~RespConn()
	{
		Close();
	}

	bool RespConn::IsOpen() const noexcept
	{
		return _sock != kInvalid;
	}

	void RespConn::Close() noexcept
	{
		if (_sock != kInvalid)
		{
			::closesocket(static_cast<SOCKET>(_sock));
			_sock = kInvalid;
		}
		_rx.clear();
		_pos = 0;
	}

	bool RespConn::Connect(const std::string& host, std::uint16_t port, int recvTimeoutMs)
	{
		Close();

		addrinfo hints{};
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		addrinfo* res = nullptr;
		const std::string portStr = std::to_string(port);
		if (::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || res == nullptr)
		{
			return false;
		}

		SOCKET s = INVALID_SOCKET;
		for (addrinfo* p = res; p != nullptr; p = p->ai_next)
		{
			s = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
			if (s == INVALID_SOCKET) { continue; }
			if (::connect(s, p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0) { break; }
			::closesocket(s);
			s = INVALID_SOCKET;
		}
		::freeaddrinfo(res);

		if (s == INVALID_SOCKET)
		{
			return false;
		}

		// Nagle 비활성(소량 명령 지연 최소화).
		int one = 1;
		::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));

		if (recvTimeoutMs > 0)
		{
#if defined(_WIN32)
			DWORD tv = static_cast<DWORD>(recvTimeoutMs);
			::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
			// POSIX: SO_RCVTIMEO 는 struct timeval (Windows 는 DWORD ms).
			struct timeval tv{};
			tv.tv_sec = recvTimeoutMs / 1000;
			tv.tv_usec = (recvTimeoutMs % 1000) * 1000;
			::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif
		}

		_sock = static_cast<std::uintptr_t>(s);
		_rx.clear();
		_pos = 0;
		return true;
	}

	bool RespConn::SendAll(const char* data, std::size_t len)
	{
		if (_sock == kInvalid) { return false; }
		std::size_t sent = 0;
		while (sent < len)
		{
			const int n = static_cast<int>(::send(static_cast<SOCKET>(_sock),
			                                      data + sent, static_cast<int>(len - sent), 0));
			if (n == SOCKET_ERROR || n == 0)
			{
				return false;
			}
			sent += static_cast<std::size_t>(n);
		}
		return true;
	}

	bool RespConn::Fill()
	{
		if (_sock == kInvalid) { return false; }

		// 소비한 접두부 정리(버퍼 무한 증가 방지).
		if (_pos > 0)
		{
			_rx.erase(0, _pos);
			_pos = 0;
		}

		char tmp[8192];
		const int n = static_cast<int>(::recv(static_cast<SOCKET>(_sock), tmp, sizeof(tmp), 0));
		if (n <= 0)
		{
			return false; // 타임아웃/종료/오류 — 호출자가 연결을 닫는다.
		}
		_rx.append(tmp, static_cast<std::size_t>(n));
		return true;
	}

	bool RespConn::ReadLine(std::string& line)
	{
		for (;;)
		{
			const std::size_t nl = _rx.find("\r\n", _pos);
			if (nl != std::string::npos)
			{
				line.assign(_rx, _pos, nl - _pos);
				_pos = nl + 2;
				return true;
			}
			if (!Fill()) { return false; }
		}
	}

	bool RespConn::ReadBytes(std::size_t n, std::string& out)
	{
		// n바이트 + 후행 CRLF가 모두 도착할 때까지 채운다.
		while (_rx.size() - _pos < n + 2)
		{
			if (!Fill()) { return false; }
		}
		out.assign(_rx, _pos, n);
		_pos += n + 2; // 페이로드 + CRLF 소비
		return true;
	}

	bool RespConn::ParseValue(RespValue& out)
	{
		std::string line;
		if (!ReadLine(line) || line.empty()) { return false; }

		const char tag = line[0];
		const std::string rest = line.substr(1);

		switch (tag)
		{
		case '+':
			out.type = RespValue::Type::Simple;
			out.str = rest;
			return true;
		case '-':
			out.type = RespValue::Type::Error;
			out.str = rest;
			return true;
		case ':':
			out.type = RespValue::Type::Integer;
			out.integer = std::strtoll(rest.c_str(), nullptr, 10);
			return true;
		case '$':
		{
			const long long n = std::strtoll(rest.c_str(), nullptr, 10);
			if (n < 0)
			{
				out.type = RespValue::Type::Nil;
				return true;
			}
			out.type = RespValue::Type::Bulk;
			return ReadBytes(static_cast<std::size_t>(n), out.str);
		}
		case '*':
		{
			const long long n = std::strtoll(rest.c_str(), nullptr, 10);
			if (n < 0)
			{
				out.type = RespValue::Type::Nil;
				return true;
			}
			out.type = RespValue::Type::Array;
			out.elements.resize(static_cast<std::size_t>(n));
			for (long long i = 0; i < n; ++i)
			{
				if (!ParseValue(out.elements[static_cast<std::size_t>(i)])) { return false; }
			}
			return true;
		}
		default:
			return false; // 프로토콜 위반
		}
	}

	bool RespConn::SendCommand(const std::vector<std::string>& args)
	{
		if (args.empty()) { return false; }

		std::string req;
		req.reserve(64);
		req += '*';
		req += std::to_string(args.size());
		req += "\r\n";
		for (const auto& a : args)
		{
			req += '$';
			req += std::to_string(a.size());
			req += "\r\n";
			req += a;            // 바이너리 안전(길이 프리픽스)
			req += "\r\n";
		}
		return SendAll(req.data(), req.size());
	}

	bool RespConn::ReadReply(RespValue& out)
	{
		out = RespValue{};
		return ParseValue(out);
	}

	bool RespConn::Command(const std::vector<std::string>& args, RespValue& out)
	{
		if (!SendCommand(args)) { return false; }
		return ReadReply(out);
	}
}
