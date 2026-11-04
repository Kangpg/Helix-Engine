#pragma once
#include "helix_mmo.pb.h"
#include "messages.h"
#include <cstddef>
#include <span>
#include <string>

namespace helix::protocol
{
	inline constexpr std::uint32_t kProtocolVersion = messages::Version;
	// 파싱과 의미 검증은 별개다. 전송 프레이밍은 엔진이 소유한다.
	bool TryDecodeClient(std::span<const std::byte> bytes, wire::ClientPacket& packet);
	bool IsValid(const wire::ClientPacket& packet);
	bool TryEncode(const wire::ServerPacket& packet, std::string& bytes);
	wire::ServerPacket MakeReply(std::uint64_t requestId, wire::ReplyStatus status);
}
