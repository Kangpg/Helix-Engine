#include "helix/protocol/codec.h"
#include <cmath>
#include <limits>

namespace helix::protocol
{
	bool TryDecodeClient(std::span<const std::byte> bytes, wire::ClientPacket& packet)
	{
		if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) { return false; }
		return packet.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()));
	}
	bool IsValid(const wire::ClientPacket& packet)
	{
		if (packet.protocol_version() != kProtocolVersion) { return false; }
		switch (packet.body_case())
		{
		case wire::ClientPacket::kMove:
			return packet.move().has_destination() && std::isfinite(packet.move().destination().x()) &&
				std::isfinite(packet.move().destination().y()) && std::isfinite(packet.move().destination().z());
		case wire::ClientPacket::kAttack: return packet.attack().target_entity_id() != 0;
		case wire::ClientPacket::kLook: return true;
		default: return false;
		}
	}
	bool TryEncode(const wire::ServerPacket& packet, std::string& bytes) { return packet.SerializeToString(&bytes); }
	wire::ServerPacket MakeReply(std::uint64_t requestId, wire::ReplyStatus status)
	{
		wire::ServerPacket packet;
		packet.set_protocol_version(kProtocolVersion);
		packet.set_request_id(requestId);
		packet.mutable_reply()->set_status(status);
		return packet;
	}
}
