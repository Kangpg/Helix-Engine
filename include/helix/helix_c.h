#ifndef HELIX_C_H
#define HELIX_C_H

/* ===========================================================================
 * Helix — Stable C ABI
 *
 * 엔진 코어(C++)를 C ABI로 노출하는 단일 경계.
 * - C++ SDK 와 C# SDK(P/Invoke) 가 공통으로 이 위를 얇게 감싼다.
 * - 규칙:
 *     1) 예외는 이 경계를 넘지 않는다. 모든 실패는 helix_status로 반환.
 *     2) 모든 객체는 불투명 핸들(포인터)로만 다룬다.
 *     3) 콜백은 엔진 워커 스레드에서 호출될 수 있다. C# 측은 델리게이트
 *        수명을 GCHandle로 고정(pinning)하고, ctx로 인스턴스를 전달할 것.
 *     4) 콜백에 전달된 helix_bytes.data 는 콜백 동안만 유효(복사 필요).
 *     5) ABI 안정성: 구조체에 필드를 추가할 때는 끝에만 추가하고
 *        helix_abi_version() 으로 런타임 호환성을 확인한다.
 * =========================================================================== */

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(HELIX_BUILD_DLL)
#    define HELIX_API __declspec(dllexport)
#  elif defined(HELIX_USE_DLL)
#    define HELIX_API __declspec(dllimport)
#  else
#    define HELIX_API
#  endif
#else
#  define HELIX_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --- 버전 ------------------------------------------------------------------ */
/* v2: scale-out(route/kv/data-version) + on_data_version 콜백 추가(모두 끝에 append). */
#define HELIX_ABI_VERSION 2
HELIX_API uint32_t helix_abi_version(void);

/* --- 기본 타입 ------------------------------------------------------------- */
typedef uint64_t helix_session_id;

typedef enum helix_status
{
	HELIX_OK = 0,
	HELIX_ERR_CONFIG = 1,
	HELIX_ERR_ALREADY_RUNNING = 2,
	HELIX_ERR_NOT_RUNNING = 3,
	HELIX_ERR_INVALID_SESSION = 4,
	HELIX_ERR_BACKPRESSURE_FULL = 5,
	HELIX_ERR_TRANSPORT = 6,
	HELIX_ERR_INTERNAL = 7,
	HELIX_ERR_INVALID_ARG = 8,
	HELIX_ERR_NOT_FOUND = 9,        /* v2: KvGet 키 없음 / 클러스터 비활성 */
} helix_status;

typedef enum helix_close_reason
{
	HELIX_CLOSE_NORMAL = 0,
	HELIX_CLOSE_PEER = 1,
	HELIX_CLOSE_IDLE_TIMEOUT = 2,
	HELIX_CLOSE_IO_ERROR = 3,
	HELIX_CLOSE_PROTOCOL_ERROR = 4,
	HELIX_CLOSE_SERVER_SHUTDOWN = 5,
	HELIX_CLOSE_KICKED = 6,
} helix_close_reason;

/* 소유권 없는 바이트 뷰 (콜백 동안만 유효) */
typedef struct helix_bytes
{
	const uint8_t* data;
	size_t         len;
} helix_bytes;

/* 불투명 핸들 */
typedef struct helix_server helix_server;

/* --- 콜백 ------------------------------------------------------------------ */
typedef void (*helix_on_connect_fn)(void* ctx, helix_session_id sid);
typedef void (*helix_on_message_fn)(void* ctx, helix_session_id sid, helix_bytes msg);
typedef void (*helix_on_disconnect_fn)(void* ctx, helix_session_id sid, helix_close_reason reason);
/* v2: 런타임 데이터 버전 브로드캐스트 수신(scale-out). channel은 콜백 동안만 유효. */
typedef void (*helix_on_data_version_fn)(void* ctx, const char* channel, uint64_t version);

typedef struct helix_callbacks
{
	helix_on_connect_fn      on_connect;       /* nullable */
	helix_on_message_fn      on_message;       /* required */
	helix_on_disconnect_fn   on_disconnect;    /* nullable */
	void*                    ctx;              /* 콜백마다 그대로 전달 */
	helix_on_data_version_fn on_data_version;  /* v2, nullable (끝에 append) */
} helix_callbacks;

/* --- 수명/실행 ------------------------------------------------------------- */
/* 설정 파일(TOML) + 환경변수 오버라이드로 서버 생성. 실패 시 *out = NULL. */
HELIX_API helix_status helix_server_create(const char* config_path, helix_server** out);

HELIX_API helix_status helix_server_set_callbacks(helix_server* s, helix_callbacks cb);

/* Stop() 또는 시그널까지 블로킹 실행. */
HELIX_API helix_status helix_server_run(helix_server* s);

/* 다른 스레드에서 graceful shutdown 트리거. */
HELIX_API helix_status helix_server_stop(helix_server* s);

HELIX_API void helix_server_destroy(helix_server* s);

/* --- 세션 조작 ------------------------------------------------------------- */
/* 메시지 전송(프레이밍 자동). 큐 가득 차면 HELIX_ERR_BACKPRESSURE_FULL. */
HELIX_API helix_status helix_session_send(helix_server* s, helix_session_id sid, helix_bytes msg);

HELIX_API helix_status helix_session_close(helix_server* s, helix_session_id sid, helix_close_reason reason);

/* 원격 주소 조회. ip_buf에 NUL 종료 문자열 기록, *port 설정. */
HELIX_API helix_status helix_session_remote(helix_server* s, helix_session_id sid,
                                            char* ip_buf, size_t ip_buf_len, uint16_t* port);

HELIX_API size_t helix_server_session_count(helix_server* s);

/* --- scale-out (v2) -------------------------------------------------------- */
/* SessionId로 노드 투명 전송(로컬→직접, 타 노드→백플레인). 프레이밍 자동. */
HELIX_API helix_status helix_session_route(helix_server* s, helix_session_id target, helix_bytes msg);

/* 런타임 데이터 버전을 전 노드에 브로드캐스트(자기 노드 포함, on_data_version 콜백). */
HELIX_API helix_status helix_cluster_publish_data_version(helix_server* s, const char* channel, uint64_t version);

/* 공유 KV. 클러스터 비활성 시 set 실패. ttl_sec=0 이면 만료 없음. */
HELIX_API helix_status helix_cluster_kv_set(helix_server* s, const char* key, helix_bytes value, uint32_t ttl_sec);

/* 공유 KV 조회. 값을 buf(최대 buf_len)로 복사하고 *out_len 에 실제 길이 기록.
 * 반환 HELIX_OK=발견(*out_len>buf_len 이면 잘림 → 더 큰 버퍼로 재호출),
 *      HELIX_ERR_NOT_FOUND=키 없음/클러스터 비활성. */
HELIX_API helix_status helix_cluster_kv_get(helix_server* s, const char* key,
                                            uint8_t* buf, size_t buf_len, size_t* out_len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HELIX_C_H */
