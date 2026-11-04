#pragma once
#include "helix/protocol/serializer.h"

namespace helix::protocol
{
	class FlatBuffersSerializer final : public IPacketSerializer
	{
	public:
		std::uint8_t FormatId() const noexcept override { return 2; }
		bool EncodeClient(const messages::ClientPacket&, std::string&) const override;
		bool DecodeClient(std::span<const std::byte>, messages::ClientPacket&) const override;
		bool EncodeServer(const messages::ServerPacket&, std::string&) const override;
		bool DecodeServer(std::span<const std::byte>, messages::ServerPacket&) const override;
	};
	inline void RegisterFlatBuffers(SerializerRegistry& registry)
	{
		registry.Register("flatbuffers", 2, [] { return std::make_shared<FlatBuffersSerializer>(); });
	}
}
