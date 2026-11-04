#pragma once
#include "messages.h"
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>

namespace helix::protocol
{
	// Independent of framing, sessions, generated Protobuf/FlatBuffers types and MMO state.
	// Codecs own no mutable per-call state; share them safely between session callbacks.
	// TryDecode resets output on parse failure; semantic validation is messages::IsValid.
	class IClientPacketSerializer
	{
	public:
		virtual ~IClientPacketSerializer() = default;
		virtual bool EncodeClient(const messages::ClientPacket&, std::string&) const = 0;
		virtual bool DecodeServer(std::span<const std::byte>, messages::ServerPacket&) const = 0;
	};
	class IServerPacketSerializer
	{
	public:
		virtual ~IServerPacketSerializer() = default;
		virtual bool DecodeClient(std::span<const std::byte>, messages::ClientPacket&) const = 0;
		virtual bool EncodeServer(const messages::ServerPacket&, std::string&) const = 0;
	};
	// Full adapters implement both roles; apps can depend on just their endpoint's role.
	class IPacketSerializer : public IClientPacketSerializer, public IServerPacketSerializer
	{
	public:
		~IPacketSerializer() override = default;
		virtual std::uint8_t FormatId() const noexcept = 0;
	};
	class ProtobufSerializer final : public IPacketSerializer
	{
	public:
		std::uint8_t FormatId() const noexcept override { return 1; }
		bool EncodeClient(const messages::ClientPacket&, std::string&) const override;
		bool DecodeClient(std::span<const std::byte>, messages::ClientPacket&) const override;
		bool EncodeServer(const messages::ServerPacket&, std::string&) const override;
		bool DecodeServer(std::span<const std::byte>, messages::ServerPacket&) const override;
	};
	// Format 1 stays legacy raw Protobuf. Others: HXS1 + format + schema + direction + reserved.
	class WireSerializer final : public IPacketSerializer
	{
	public:
		explicit WireSerializer(std::shared_ptr<const IPacketSerializer> serializer);
		std::uint8_t FormatId() const noexcept override { return _serializer->FormatId(); }
		bool EncodeClient(const messages::ClientPacket&, std::string&) const override;
		bool DecodeClient(std::span<const std::byte>, messages::ClientPacket&) const override;
		bool EncodeServer(const messages::ServerPacket&, std::string&) const override;
		bool DecodeServer(std::span<const std::byte>, messages::ServerPacket&) const override;
	private:
		void Wrap(std::string&, std::uint8_t) const;
		bool Unwrap(std::span<const std::byte>&, std::uint8_t) const;
		std::shared_ptr<const IPacketSerializer> _serializer;
	};
	class SerializerRegistry
	{
		// Register during composition, then publish. Concurrent registration/Create is not supported.
	public:
		using Factory = std::function<std::shared_ptr<const IPacketSerializer>()>;
		SerializerRegistry(); // default Protobuf; optional implementations register at composition root.
		void Register(std::string name, std::uint8_t formatId, Factory factory);
		std::shared_ptr<const IPacketSerializer> Create(const std::string& name = "protobuf") const;
	private:
		struct Entry { std::uint8_t id; Factory factory; };
		std::unordered_map<std::string, Entry> _factories;
	};
}
