#include "core/ServerCore.h"
#include "net/PlatformNet.h"
#include "net/NetBackendFactory.h"
#include "helix/server_app.h"
#include "helix/session.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
	void Check(bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
	struct Echo final : helix::IServerApp
	{
		void OnMessage(helix::Session& session, helix::ByteSpan bytes) override { Check(session.Send(bytes) == helix::Status::Ok, "send"); }
	};
	SOCKET Socket() { const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); Check(socket != INVALID_SOCKET, "socket"); return socket; }
	std::uint16_t FreePort()
	{
		const auto socket = Socket(); sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		Check(::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind probe");
		int size = sizeof(address); Check(::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size) == 0, "getsockname");
		::closesocket(socket); return ntohs(address.sin_port);
	}
	void Run(helix::net::IoBackendKind kind)
	{
		helix::core::ServerCoreConfig config;
		config.backend = kind; config.bind = { "127.0.0.1", FreePort() }; config.ioThreads = 4;
		config.maxConnections = 16; config.concurrentAccepts = 4; config.rioBufferSize = 1024; config.rioCompletionQueueSize = 64;
		helix::core::ServerCore server(config); Echo app;
		Check(server.Start(&app), "requested backend must start");
		Check(std::strcmp(server.BackendName(), kind == helix::net::IoBackendKind::IocpRio ? "iocp+rio" : "iocp") == 0, "actual backend (no fallback)");
		std::thread runner([&] { server.Run(); });
		SOCKET client = INVALID_SOCKET;
		try
		{
			client = Socket(); DWORD timeout = 5000;
			::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
			::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
			sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(config.bind.port); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			Check(::connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connect");
			std::vector<char> frame(200004);
			const auto length = htonl(200000); std::memcpy(frame.data(), &length, 4);
			for (std::size_t i = 4; i < frame.size(); i++) frame[i] = static_cast<char>(i % 251);
			std::size_t sent = 0;
			while (sent < frame.size()) { const int count = ::send(client, frame.data() + sent, static_cast<int>(frame.size() - sent), 0); Check(count > 0, "send client"); sent += count; }
			std::vector<char> echoed(frame.size()); std::size_t received = 0;
			while (received < echoed.size()) { const int count = ::recv(client, echoed.data() + received, static_cast<int>(echoed.size() - received), 0); if (count <= 0) { std::cerr << "recv progress=" << received << " error=" << ::WSAGetLastError() << '\n'; } Check(count > 0, "recv client"); received += count; }
			Check(echoed == frame, "large frame intact");
			// Outstanding recv and AcceptEx must drain before their owners are destroyed.
			server.Stop(); runner.join(); Check(server.SessionCount() == 0, "graceful cancellation drain");
			::closesocket(client); client = INVALID_SOCKET;
			std::cout << "PASS: " << helix::net::ToString(kind) << " actual backend, 200KB echo, partial sends and graceful pending-I/O drain\n";
		}
		catch (...) { if (client != INVALID_SOCKET) ::closesocket(client); server.Stop(); if (runner.joinable()) runner.join(); throw; }
	}
}
int main()
{
	try
	{
		WSADATA data{}; Check(::WSAStartup(MAKEWORD(2, 2), &data) == 0, "Winsock");
		helix::net::BackendConfig invalid;
		invalid.kind = helix::net::IoBackendKind::IocpRio;
		invalid.maxConnections = 16; invalid.rioCompletionQueueSize = 31;
		Check(!helix::net::CreateNetBackend(invalid), "invalid RIO CQ must fail without fallback");
		invalid.rioCompletionQueueSize = 64; invalid.rioBufferSize = 0;
		Check(!helix::net::CreateNetBackend(invalid), "invalid RIO buffer must fail without fallback");
		Run(helix::net::IoBackendKind::Iocp); Run(helix::net::IoBackendKind::IocpRio); ::WSACleanup();
	}
	catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << " Winsock=" << ::WSAGetLastError() << '\n'; return 1; }
}
