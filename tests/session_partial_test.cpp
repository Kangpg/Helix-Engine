#include "session/Session.h"
#include "session/SessionManager.h"
#include <algorithm>
#include <deque>
#include <iostream>
#include <stdexcept>

namespace
{
	void Check(bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
	struct Backend final : helix::net::INetBackend
	{
		helix::net::IoContext* receive = nullptr;
		std::deque<helix::net::IoCompletion> completions;
		std::vector<std::byte> sent;
		int failAt = 0, posts = 0, contexts = 0;
		bool Init(const helix::net::BackendConfig&) override { return true; }
		void Shutdown() override {}
		bool Register(helix::net::NativeHandle) override { return true; }
		helix::net::NativeHandle CreateAcceptSocket() override { return 42; }
		void CloseSocket(helix::net::NativeHandle) override {}
		helix::net::IoContext* CreateContext(helix::net::IoOp op) override { ++contexts; auto* context = new helix::net::IoContext; context->op = op; return context; }
		void DestroyContext(helix::net::IoContext* context) override { --contexts; delete context; }
		bool PostAccept(helix::net::IoContext*) override { return false; }
		bool PostRecv(helix::net::IoContext* context) override { receive = context; return true; }
		bool PostSend(helix::net::IoContext* context) override
		{
			if (++posts == failAt) return false;
			const auto count = std::min(context->length, 3u);
			sent.insert(sent.end(), context->buffer, context->buffer + count);
			completions.push_back({ context, count, {} }); return true;
		}
		int Poll(helix::net::IoCompletion*, int, int) override { return 0; }
		void Wake(int) override {}
		const char* Name() const noexcept override { return "partial-test"; }
	};
	void Run(int failAt)
	{
		Backend backend; backend.failAt = failAt;
		helix::core::SessionFactoryConfig config;
		helix::core::SessionManager manager(backend, 10, config, nullptr);
		auto* session = manager.Create(42, {}); Check(session->Start(), "start");
		const std::byte payload[] { std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5} };
		Check(session->Send(payload) == helix::Status::Ok, "queue send");
		if (!failAt) Check(session->Send(payload) == helix::Status::Ok, "queue second send");
		while (!backend.completions.empty())
		{
			const auto completion = backend.completions.front(); backend.completions.pop_front();
			static_cast<helix::core::Session*>(completion.ctx->user)->OnSendComplete(completion.bytesTransferred);
		}
		if (!failAt)
		{
			Check(backend.sent.size() == 18 && backend.sent[3] == std::byte{5} && backend.sent[12] == std::byte{5}, "complete frames preserve order");
			session->Close(helix::SessionCloseReason::Normal);
		}
		static_cast<helix::core::Session*>(backend.receive->user)->OnRecvComplete(0);
		Check(manager.Count() == 0 && backend.contexts == 0, "single refcount cleans up after canceled receive and failed partial post");
	}
}
int main()
{
	try { Run(0); Run(2); std::cout << "PASS: partial send framing/order, failed re-post and context/refcount cleanup\n"; }
	catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
