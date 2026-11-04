#include "helix/protocol/codec.h"
#include "world_state_generated.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
	struct State { std::uint64_t id = 1234567890123, tick = 987654; double x = 12.25, y = -34.5, z = 100.75; std::int32_t health = 87; std::vector<std::uint64_t> visible; };
	using Bytes = std::vector<std::byte>;
	struct Codec { const char* name; std::function<Bytes(const State&)> encode; std::function<State(const Bytes&)> decode; };
	Bytes ProtobufEncode(const State& state)
	{
		helix::wire::WorldState packet; packet.set_entity_id(state.id); packet.set_tick(state.tick); packet.set_health(state.health);
		packet.mutable_position()->set_x(state.x); packet.mutable_position()->set_y(state.y); packet.mutable_position()->set_z(state.z);
		packet.mutable_visible_entity_ids()->Reserve(static_cast<int>(state.visible.size()));
		for (auto id : state.visible) packet.add_visible_entity_ids(id);
		Bytes bytes(packet.ByteSizeLong()); if (!packet.SerializeToArray(bytes.data(), static_cast<int>(bytes.size()))) throw std::runtime_error("Protobuf encode"); return bytes;
	}
	State ProtobufDecode(const Bytes& bytes)
	{
		helix::wire::WorldState packet; if (!packet.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) throw std::runtime_error("Protobuf decode");
		return { packet.entity_id(), packet.tick(), packet.position().x(), packet.position().y(), packet.position().z(), packet.health(),
			{packet.visible_entity_ids().begin(), packet.visible_entity_ids().end()} };
	}
	Bytes FlatEncode(const State& state)
	{
		flatbuffers::FlatBufferBuilder builder(128 + state.visible.size() * 8);
		const auto ids = builder.CreateVector(state.visible);
		const auto packet = Helix::SerializationBenchmarks::Flat::CreateWorldState(builder, state.id, state.x, state.y, state.z, state.health, state.tick, ids);
		Helix::SerializationBenchmarks::Flat::FinishWorldStateBuffer(builder, packet);
		const auto* first = reinterpret_cast<const std::byte*>(builder.GetBufferPointer());
		return {first, first + builder.GetSize()};
	}
	State FlatDecode(const Bytes& bytes)
	{
		const auto* packet = Helix::SerializationBenchmarks::Flat::GetWorldState(bytes.data());
		return {packet->entity_id(), packet->tick(), packet->x(), packet->y(), packet->z(), packet->health(),
			{packet->visible_entity_ids()->begin(), packet->visible_entity_ids()->end()} };
	}
	// The same fixed LE layout as the managed benchmark, independent of native struct padding.
	template<class T> void Write(Bytes& bytes, std::size_t at, T value)
	{
		auto data = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
		if constexpr (std::endian::native == std::endian::big) std::reverse(data.begin(), data.end());
		std::memcpy(bytes.data() + at, data.data(), data.size());
	}
	template<class T> T Read(const Bytes& bytes, std::size_t at)
	{
		std::array<std::byte, sizeof(T)> data; std::memcpy(data.data(), bytes.data() + at, data.size());
		if constexpr (std::endian::native == std::endian::big) std::reverse(data.begin(), data.end());
		return std::bit_cast<T>(data);
	}
	Bytes BinaryEncode(const State& state)
	{
		Bytes bytes(48 + state.visible.size() * 8); Write(bytes, 0, state.id); Write(bytes, 8, state.x); Write(bytes, 16, state.y); Write(bytes, 24, state.z);
		Write(bytes, 32, state.health); Write(bytes, 36, state.tick); Write(bytes, 44, static_cast<std::uint32_t>(state.visible.size()));
		for (std::size_t i = 0; i < state.visible.size(); i++) Write(bytes, 48 + i * 8, state.visible[i]);
		return bytes;
	}
	State BinaryDecode(const Bytes& bytes)
	{
		if (bytes.size() < 48) throw std::runtime_error("Truncated binary");
		const auto count = Read<std::uint32_t>(bytes, 44);
		if (bytes.size() != 48ull + count * 8ull) throw std::runtime_error("Invalid binary count");
		State state { Read<std::uint64_t>(bytes, 0), Read<std::uint64_t>(bytes, 36), Read<double>(bytes, 8), Read<double>(bytes, 16), Read<double>(bytes, 24), Read<std::int32_t>(bytes, 32), {} };
		state.visible.resize(count); for (std::size_t i = 0; i < count; i++) state.visible[i] = Read<std::uint64_t>(bytes, 48 + i * 8); return state;
	}
	volatile std::uint64_t sink = 0;
	template<class F> double Measure(F operation, int iterations, int samples)
	{
		for (int i = 0; i < 2000; i++) sink = operation();
		std::vector<double> times;
		for (int batch = 0; batch < samples; batch++)
		{
			const auto start = std::chrono::steady_clock::now();
			for (int i = 0; i < iterations; i++) sink = operation();
			times.push_back(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / iterations);
		}
		std::sort(times.begin(), times.end()); return times[samples / 2];
	}
}
int main(int argc, char** argv)
{
	try
	{
		const int iterations = argc > 1 ? std::stoi(argv[1]) : 10000, samples = argc > 2 ? std::stoi(argv[2]) : 9;
		if (iterations < 1 || samples < 3) throw std::runtime_error("iterations > 0, samples >= 3");
		std::ofstream file; if (argc > 3) { file.open(argv[3]); if (!file) throw std::runtime_error("output path"); }
		const std::string header = "AOI,Codec,Bytes,EncodeNs,DecodeNs\n";
		std::cout << "C++ Release batch median; logical CPUs=" << std::thread::hardware_concurrency() << "; operations=" << iterations << "; samples=" << samples << '\n';
		std::cout << header; if (file) file << header;
		const std::vector<Codec> codecs { {"Protobuf", ProtobufEncode, ProtobufDecode}, {"FlatBuffers", FlatEncode, FlatDecode}, {"Binary", BinaryEncode, BinaryDecode} };
		for (auto count : {0, 16, 256, 4096}) for (const auto& codec : codecs)
		{
			State state; for (int i = 0; i < count; i++) state.visible.push_back(1000000000000ull + i);
			const auto bytes = codec.encode(state); const auto decoded = codec.decode(bytes);
			if (decoded.id != state.id || decoded.tick != state.tick || decoded.health != state.health || decoded.x != state.x || decoded.y != state.y || decoded.z != state.z || decoded.visible != state.visible) throw std::runtime_error("round-trip mismatch");
			const auto encode = Measure([&] { auto value = codec.encode(state); return value.size() + std::to_integer<unsigned>(value[0]); }, iterations, samples);
			const auto decode = Measure([&] { auto value = codec.decode(bytes); return value.id + value.visible.size() + value.health; }, iterations, samples);
			std::cout << count << ',' << codec.name << ',' << bytes.size() << ',' << encode << ',' << decode << '\n';
			if (file) file << count << ',' << codec.name << ',' << bytes.size() << ',' << encode << ',' << decode << '\n';
		}
	}
	catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
