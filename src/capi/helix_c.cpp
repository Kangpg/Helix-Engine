// =============================================================================
// Phase 2 마무리 — 안정적 C ABI 구현 (helix_c.h)
//
// 공개 C++ SDK(Server/IServerApp/Session) 위에 C ABI를 구현한다.
// C# SDK(P/Invoke)와 C++ 외 언어 바인딩이 이 위에 올라간다.
//
// 규약: 예외는 경계를 넘지 않는다(전부 helix_status). 콜백은 워커 스레드에서
// 호출될 수 있다. helix_bytes.data 는 콜백 동안만 유효.
//
// 미검증: Windows/MSVC 전용(엔진이 Windows-only인 동안).
// =============================================================================

#include "helix/helix_c.h"
#include "helix/server.h"
#include "helix/server_app.h"
#include "helix/session.h"

#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>

namespace
{
	// helix_callbacks → IServerApp 어댑터.
	class CallbackApp final : public helix::IServerApp
	{
	public:
		explicit CallbackApp(const helix_callbacks& cb) : _cb(cb) {}

		void OnConnect(helix::Session& s) override
		{
			if (_cb.on_connect) { _cb.on_connect(_cb.ctx, s.Id()); }
		}

		void OnMessage(helix::Session& s, helix::ByteSpan msg) override
		{
			helix_bytes b{ reinterpret_cast<const std::uint8_t*>(msg.data()), msg.size() };
			if (_cb.on_message) { _cb.on_message(_cb.ctx, s.Id(), b); }
		}

		void OnDisconnect(helix::Session& s, helix::SessionCloseReason r) override
		{
			if (_cb.on_disconnect)
			{
				_cb.on_disconnect(_cb.ctx, s.Id(), static_cast<helix_close_reason>(r));
			}
		}

		void OnDataVersion(std::string_view channel, std::uint64_t version) override
		{
			if (_cb.on_data_version)
			{
				// channel 을 NUL 종료 보장(콜백 동안만 유효).
				const std::string ch(channel);
				_cb.on_data_version(_cb.ctx, ch.c_str(), version);
			}
		}

	private:
		helix_callbacks _cb;
	};

	helix_status MapStatus(helix::Status st) noexcept
	{
		switch (st)
		{
		case helix::Status::Ok:              return HELIX_OK;
		case helix::Status::ConfigError:     return HELIX_ERR_CONFIG;
		case helix::Status::AlreadyRunning:  return HELIX_ERR_ALREADY_RUNNING;
		case helix::Status::NotRunning:      return HELIX_ERR_NOT_RUNNING;
		case helix::Status::InvalidSession:  return HELIX_ERR_INVALID_SESSION;
		case helix::Status::BackpressureFull:return HELIX_ERR_BACKPRESSURE_FULL;
		case helix::Status::TransportError:  return HELIX_ERR_TRANSPORT;
		default:                             return HELIX_ERR_INTERNAL;
		}
	}
}

// 불투명 핸들 정의: 공개 Server + 저장된 콜백.
struct helix_server
{
	helix::Server server;
	helix_callbacks cb{};
	bool has_cb = false;
};

extern "C" {

uint32_t helix_abi_version(void)
{
	return HELIX_ABI_VERSION;
}

helix_status helix_server_create(const char* config_path, helix_server** out)
{
	if (out == nullptr) { return HELIX_ERR_INVALID_ARG; }
	*out = nullptr;
	try
	{
		auto* hs = new helix_server{ helix::Server::LoadConfig(config_path ? config_path : ""), helix_callbacks{}, false };
		*out = hs;
		return HELIX_OK;
	}
	catch (...)
	{
		return HELIX_ERR_INTERNAL;
	}
}

helix_status helix_server_set_callbacks(helix_server* s, helix_callbacks cb)
{
	if (s == nullptr) { return HELIX_ERR_INVALID_ARG; }
	if (cb.on_message == nullptr) { return HELIX_ERR_INVALID_ARG; }
	s->cb = cb;
	s->has_cb = true;
	return HELIX_OK;
}

helix_status helix_server_run(helix_server* s)
{
	if (s == nullptr) { return HELIX_ERR_INVALID_ARG; }
	if (!s->has_cb) { return HELIX_ERR_INVALID_ARG; }
	try
	{
		const int rc = s->server.Run(std::make_unique<CallbackApp>(s->cb));
		return rc == 0 ? HELIX_OK : HELIX_ERR_TRANSPORT;
	}
	catch (...)
	{
		return HELIX_ERR_INTERNAL;
	}
}

helix_status helix_server_stop(helix_server* s)
{
	if (s == nullptr) { return HELIX_ERR_INVALID_ARG; }
	s->server.Stop();
	return HELIX_OK;
}

void helix_server_destroy(helix_server* s)
{
	delete s;
}

helix_status helix_session_send(helix_server* s, helix_session_id sid, helix_bytes msg)
{
	if (s == nullptr) { return HELIX_ERR_INVALID_ARG; }
	helix::Session* sess = s->server.FindSession(sid);
	if (sess == nullptr) { return HELIX_ERR_INVALID_SESSION; }
	const auto span = helix::ByteSpan(reinterpret_cast<const std::byte*>(msg.data), msg.len);
	return MapStatus(sess->Send(span));
}

helix_status helix_session_close(helix_server* s, helix_session_id sid, helix_close_reason reason)
{
	if (s == nullptr) { return HELIX_ERR_INVALID_ARG; }
	helix::Session* sess = s->server.FindSession(sid);
	if (sess == nullptr) { return HELIX_ERR_INVALID_SESSION; }
	sess->Close(static_cast<helix::SessionCloseReason>(reason));
	return HELIX_OK;
}

helix_status helix_session_remote(helix_server* s, helix_session_id sid,
                                  char* ip_buf, size_t ip_buf_len, uint16_t* port)
{
	if (s == nullptr || ip_buf == nullptr || ip_buf_len == 0) { return HELIX_ERR_INVALID_ARG; }
	helix::Session* sess = s->server.FindSession(sid);
	if (sess == nullptr) { return HELIX_ERR_INVALID_SESSION; }
	const helix::Endpoint ep = sess->Remote();
	std::strncpy(ip_buf, ep.ip.c_str(), ip_buf_len - 1);
	ip_buf[ip_buf_len - 1] = '\0';
	if (port != nullptr) { *port = ep.port; }
	return HELIX_OK;
}

size_t helix_server_session_count(helix_server* s)
{
	return s == nullptr ? 0 : s->server.SessionCount();
}

/* --- scale-out (v2) -------------------------------------------------------- */

helix_status helix_session_route(helix_server* s, helix_session_id target, helix_bytes msg)
{
	if (s == nullptr) { return HELIX_ERR_INVALID_ARG; }
	const auto span = helix::ByteSpan(reinterpret_cast<const std::byte*>(msg.data), msg.len);
	return MapStatus(s->server.RouteToSession(target, span));
}

helix_status helix_cluster_publish_data_version(helix_server* s, const char* channel, uint64_t version)
{
	if (s == nullptr || channel == nullptr) { return HELIX_ERR_INVALID_ARG; }
	s->server.PublishDataVersion(channel, version);
	return HELIX_OK;
}

helix_status helix_cluster_kv_set(helix_server* s, const char* key, helix_bytes value, uint32_t ttl_sec)
{
	if (s == nullptr || key == nullptr) { return HELIX_ERR_INVALID_ARG; }
	const std::string_view val(reinterpret_cast<const char*>(value.data), value.len);
	return s->server.ClusterKvSet(key, val, ttl_sec) ? HELIX_OK : HELIX_ERR_NOT_RUNNING;
}

helix_status helix_cluster_kv_get(helix_server* s, const char* key,
                                  uint8_t* buf, size_t buf_len, size_t* out_len)
{
	if (s == nullptr || key == nullptr || out_len == nullptr) { return HELIX_ERR_INVALID_ARG; }
	*out_len = 0;
	std::optional<std::string> v = s->server.ClusterKvGet(key);
	if (!v.has_value()) { return HELIX_ERR_NOT_FOUND; }
	*out_len = v->size();
	if (buf != nullptr && buf_len > 0)
	{
		const size_t n = (v->size() < buf_len) ? v->size() : buf_len;
		std::memcpy(buf, v->data(), n);
	}
	return HELIX_OK;  // *out_len > buf_len 이면 호출자가 잘림 감지
}

} // extern "C"
