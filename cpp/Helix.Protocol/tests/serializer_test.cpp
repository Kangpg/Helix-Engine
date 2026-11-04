#include "helix/protocol/serializer.h"
#ifdef HELIX_HAS_FLATBUFFERS
#include "helix/protocol/flatbuffers_serializer.h"
#endif
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace p = helix::protocol;
namespace m = p::messages;
namespace
{
	class CustomSerializer final : public p::IPacketSerializer
	{
	public:
		std::uint8_t FormatId() const noexcept override { return 42; }
		bool EncodeClient(const m::ClientPacket& v, std::string& b) const override { return _inner.EncodeClient(v, b); }
		bool EncodeServer(const m::ServerPacket& v, std::string& b) const override { return _inner.EncodeServer(v, b); }
		bool DecodeClient(std::span<const std::byte> b, m::ClientPacket& v) const override { return _inner.DecodeClient(b, v); }
		bool DecodeServer(std::span<const std::byte> b, m::ServerPacket& v) const override { return _inner.DecodeServer(b, v); }
	private:
		p::ProtobufSerializer _inner;
	};
	void Check(bool ok, const char* message) { if (!ok) { throw std::runtime_error(message); } }
	std::span<const std::byte> Bytes(const std::string& s) { return { reinterpret_cast<const std::byte*>(s.data()), s.size() }; }
	std::vector<m::ClientPacket> Clients()
	{
		return { { 1, UINT64_MAX, m::Move{ m::Position{ 12.25, -34.5, 100.75 } } },
			{ 1, 123, m::Attack{ UINT64_MAX } }, { 1, 999, m::Look{} } };
	}
	std::vector<m::ServerPacket> Servers()
	{
		std::vector<m::ServerPacket> result{ { 1, 0, m::Welcome{ UINT64_MAX, 2 } } };
		for (int status = 1; status <= 4; ++status) { result.push_back(m::MakeReply(UINT64_MAX, static_cast<m::ReplyStatus>(status))); }
		for (const auto count : { 0, 16, 256, 4096 })
		{
			m::State state{ UINT64_MAX, m::Position{ 12.25, -34.5, 100.75 }, INT32_MIN, {}, UINT64_MAX };
			for (int i = 0; i < count; ++i) { state.visibleEntityIds.push_back(UINT64_MAX - i); }
			result.push_back({ 1, UINT64_MAX, std::move(state) });
		}
		return result;
	}
	void Fixtures(const p::IPacketSerializer& codec, const std::filesystem::path& dir, bool write)
	{
		if (write) { std::filesystem::create_directories(dir); }
		int index = 0;
		for (const auto& expected : Clients())
		{
			std::string local; Check(codec.EncodeClient(expected, local), "encode client");
			const auto path = dir / ("client-" + std::to_string(index++) + ".bin");
			if (write) { std::ofstream(path, std::ios::binary).write(local.data(), local.size()); }
			else
			{
				std::ifstream file(path, std::ios::binary); Check(file.good(), "fixture file");
				std::string data((std::istreambuf_iterator<char>(file)), {}); m::ClientPacket decoded;
				Check(codec.DecodeClient(Bytes(data), decoded) && m::IsValid(decoded), "foreign client semantic decode");
				std::string normalized; Check(codec.EncodeClient(decoded, normalized) && local == normalized, "foreign client fields");
			}
		}
		index = 0;
		for (const auto& expected : Servers())
		{
			std::string local; Check(codec.EncodeServer(expected, local), "encode server");
			const auto path = dir / ("server-" + std::to_string(index++) + ".bin");
			if (write) { std::ofstream(path, std::ios::binary).write(local.data(), local.size()); }
			else
			{
				std::ifstream file(path, std::ios::binary); Check(file.good(), "fixture file");
				std::string data((std::istreambuf_iterator<char>(file)), {}); m::ServerPacket decoded;
				Check(codec.DecodeServer(Bytes(data), decoded) && m::IsValid(decoded), "foreign server semantic decode");
				std::string normalized; Check(codec.EncodeServer(decoded, normalized) && local == normalized, "foreign server fields");
			}
		}
		if (!write && codec.FormatId() == 2 && std::filesystem::exists(dir / "future-client.bin"))
		{
			std::ifstream file(dir / "future-client.bin", std::ios::binary); std::string data((std::istreambuf_iterator<char>(file)), {});
			m::ClientPacket decoded; Check(codec.DecodeClient(Bytes(data), decoded) && m::IsValid(decoded) && decoded.requestId == 999 && std::holds_alternative<m::Look>(decoded.body), "foreign appended FlatBuffers field");
		}
	}
}
int main(int argc, char** argv)
{
	p::SerializerRegistry registry;
#ifdef HELIX_HAS_FLATBUFFERS
	p::RegisterFlatBuffers(registry);
#endif
	if (argc == 4)
	{
		const std::string operation = argv[1]; Check(operation == "--write" || operation == "--verify", "fixture operation");
		Fixtures(*registry.Create(argv[3]), argv[2], operation == "--write");
		std::cout << "PASS: cross-language " << argv[3] << " " << argv[1] << '\n'; return 0;
	}
	std::vector<std::string> names{ "protobuf" };
#ifdef HELIX_HAS_FLATBUFFERS
	names.push_back("flatbuffers");
#endif
	for (const auto& name : names)
	{
		const auto codec = registry.Create(name);
		for (const auto& packet : Clients())
		{
			std::string bytes; Check(codec->EncodeClient(packet, bytes), "encode"); m::ClientPacket decoded;
			Check(codec->DecodeClient(Bytes(bytes), decoded) && m::IsValid(decoded), "client round-trip");
			std::string again; Check(codec->EncodeClient(decoded, again) && bytes == again, "all client fields");
			if (codec->FormatId() != 1)
			{
				m::ServerPacket server; Check(!codec->DecodeServer(Bytes(bytes), server), "direction mismatch");
				for (const auto field : { 4, 5, 6, 7, 8, 12 }) { auto bad = bytes; bad[field] = static_cast<char>(0xFF); Check(!codec->DecodeClient(Bytes(bad), decoded), "bad envelope/FlatBuffers"); }
				Check(!registry.Create()->DecodeClient(Bytes(bytes), decoded), "format mismatch");
			}
			Check(!codec->DecodeClient(Bytes(bytes.substr(0, 3)), decoded), "truncation");
		}
		for (const auto& packet : Servers())
		{
			std::string bytes; Check(codec->EncodeServer(packet, bytes), "server encode"); m::ServerPacket decoded;
			Check(codec->DecodeServer(Bytes(bytes), decoded) && m::IsValid(decoded), "server round-trip");
			std::string again; Check(codec->EncodeServer(decoded, again) && bytes == again, "all server fields");
		}
		for (const auto& packet : std::vector<m::ClientPacket>{ { 2, 45, m::Look{} }, { 1, 45, m::Attack{0} }, { 1, 45, m::Move{} }, { 1, 45, m::Move{m::Position{NAN, 0, 0}} } })
		{
			std::string bytes; m::ClientPacket decoded; Check(codec->EncodeClient(packet, bytes) && codec->DecodeClient(Bytes(bytes), decoded), "invalid semantic parse");
			Check(decoded.requestId == 45 && !m::IsValid(decoded), "shared semantic validation");
		}
	}
	bool unknown = false; try { registry.Create("memorypack"); } catch (const std::invalid_argument&) { unknown = true; }
	Check(unknown, "unsupported native MemoryPack must fail");
	bool duplicate = false; try { registry.Register("another", 1, [] { return std::make_shared<p::ProtobufSerializer>(); }); } catch (const std::invalid_argument&) { duplicate = true; }
	Check(duplicate, "duplicate format ID");
	registry.Register("custom", 42, [] { return std::make_shared<CustomSerializer>(); });
	const auto custom = registry.Create("custom"); std::string extension; m::ClientPacket decoded;
	Check(custom->EncodeClient(Clients()[2], extension) && custom->DecodeClient(Bytes(extension), decoded) && decoded.requestId == 999 && m::IsValid(decoded), "extension without core changes");
	std::cout << "PASS: serializer registry, messages, codec/metadata/direction mismatch, verifier, all DTO fields\n";
}
