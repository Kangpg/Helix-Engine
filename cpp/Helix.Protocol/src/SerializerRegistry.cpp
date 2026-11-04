#include "helix/protocol/serializer.h"
#include <stdexcept>

namespace helix::protocol
{
	SerializerRegistry::SerializerRegistry() { Register("protobuf", 1, [] { return std::make_shared<ProtobufSerializer>(); }); }
	void SerializerRegistry::Register(std::string name, std::uint8_t id, Factory factory)
	{
		if (name.empty() || id == 0 || !factory || _factories.contains(name)) { throw std::invalid_argument("Duplicate/invalid serializer registration"); }
		for (const auto& [_, e] : _factories) { if (e.id == id) { throw std::invalid_argument("Duplicate serializer ID"); } }
		_factories.emplace(std::move(name), Entry{ id, std::move(factory) });
	}
	std::shared_ptr<const IPacketSerializer> SerializerRegistry::Create(const std::string& name) const
	{
		const auto it = _factories.find(name);
		if (it == _factories.end()) { throw std::invalid_argument("Unsupported serializer: " + name); }
		auto codec = it->second.factory();
		if (!codec || codec->FormatId() != it->second.id) { throw std::logic_error("Serializer factory violated its format ID"); }
		return std::make_shared<WireSerializer>(std::move(codec));
	}
	WireSerializer::WireSerializer(std::shared_ptr<const IPacketSerializer> serializer) : _serializer(std::move(serializer))
	{
		if (!_serializer || _serializer->FormatId() == 0) { throw std::invalid_argument("Invalid serializer"); }
	}
	void WireSerializer::Wrap(std::string& bytes, std::uint8_t direction) const
	{
		if (FormatId() == 1) { return; }
		const char header[] { 'H', 'X', 'S', '1', static_cast<char>(FormatId()), 1, static_cast<char>(direction), 0 };
		bytes.insert(0, header, sizeof(header));
	}
	bool WireSerializer::Unwrap(std::span<const std::byte>& bytes, std::uint8_t direction) const
	{
		const bool magic = bytes.size() >= 4 && std::string_view(reinterpret_cast<const char*>(bytes.data()), 4) == "HXS1";
		if (FormatId() == 1) { return !magic; }
		if (bytes.size() < 8 || !magic || bytes[4] != std::byte{FormatId()} || bytes[5] != std::byte{1} || bytes[6] != std::byte{direction} || bytes[7] != std::byte{0}) { return false; }
		bytes = bytes.subspan(8); return true;
	}
	bool WireSerializer::EncodeClient(const messages::ClientPacket& p, std::string& b) const { b.clear(); if (!_serializer->EncodeClient(p, b)) { b.clear(); return false; } Wrap(b, 1); return true; }
	bool WireSerializer::EncodeServer(const messages::ServerPacket& p, std::string& b) const { b.clear(); if (!_serializer->EncodeServer(p, b)) { b.clear(); return false; } Wrap(b, 2); return true; }
	bool WireSerializer::DecodeClient(std::span<const std::byte> b, messages::ClientPacket& p) const { p = {}; return Unwrap(b, 1) && _serializer->DecodeClient(b, p); }
	bool WireSerializer::DecodeServer(std::span<const std::byte> b, messages::ServerPacket& p) const { p = {}; return Unwrap(b, 2) && _serializer->DecodeServer(b, p); }
}
