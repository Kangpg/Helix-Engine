#include "helix/protocol/codec.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
	void Check(bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
}

int main()
{
	using namespace helix;
	wire::ClientPacket request;
	request.set_protocol_version(protocol::kProtocolVersion); request.set_request_id(123);
	request.mutable_move()->mutable_destination()->set_x(3.5);
	auto bytes = request.SerializeAsString();
	// Future field 100, varint 1: unknown fields must survive round-trip.
	bytes.append("\xA0\x06\x01", 3);
	wire::ClientPacket decoded;
	Check(protocol::TryDecodeClient({ reinterpret_cast<const std::byte*>(bytes.data()), bytes.size() }, decoded), "decode");
	Check(protocol::IsValid(decoded) && decoded.request_id() == 123 && decoded.move().destination().x() == 3.5, "move round-trip");
	Check(decoded.GetReflection()->GetUnknownFields(decoded).field_count() == 1, "unknown field retained");
	decoded.set_protocol_version(2); Check(!protocol::IsValid(decoded), "unsupported version");
	decoded.set_protocol_version(1); decoded.mutable_move()->mutable_destination()->set_x(NAN);
	Check(!protocol::IsValid(decoded), "finite coordinates");
	decoded.mutable_attack()->set_target_entity_id(0); Check(!protocol::IsValid(decoded), "zero target");
	decoded.mutable_look(); Check(protocol::IsValid(decoded) && !decoded.has_attack(), "oneof replacement");
	decoded.clear_body(); Check(!protocol::IsValid(decoded), "missing body");
	const std::byte truncated[] { std::byte{0x52}, std::byte{0x05}, std::byte{0x08} };
	Check(!protocol::TryDecodeClient(truncated, decoded), "truncated message");
	std::string output;
	Check(protocol::TryEncode(protocol::MakeReply(123, wire::REPLY_STATUS_QUEUED), output), "encode reply");
	wire::ServerPacket reply; Check(reply.ParseFromString(output) && reply.request_id() == 123 && reply.reply().status() == wire::REPLY_STATUS_QUEUED, "reply round-trip");
	std::cout << "PASS: Protobuf validation, unknown fields, oneof, malformed input and reply correlation\n";
}
