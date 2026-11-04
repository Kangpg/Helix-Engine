#include "helix/protocol/flatbuffers_serializer.h"
#include "proto/FrameCodec.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <thread>

#ifdef HELIX_COUNT_NEW
namespace allocation
{
	thread_local bool enabled = false;
	thread_local std::uint64_t bytes = 0, count = 0;
	void Reset() { bytes = 0; count = 0; }
}
#endif

namespace p = helix::protocol;
namespace m = p::messages;
namespace
{
	using Frame = std::vector<std::byte>;
	using Packet = std::variant<m::ClientPacket, m::ServerPacket>;
	volatile std::uint64_t sink = 0;
	class WireCase
	{
	public:
		WireCase(const std::string& name, std::shared_ptr<const p::IPacketSerializer> codec) : _codec(std::move(codec))
		{
			const m::Position position{ 12.25, -34.5, 100.75 };
			if (name == "Move") { _packet = m::ClientPacket{ 1, 123, m::Move{ position } }; }
			else if (name == "Attack") { _packet = m::ClientPacket{ 1, 123, m::Attack{1234567890123} }; }
			else if (name == "Look") { _packet = m::ClientPacket{ 1, 123, m::Look{} }; }
			else if (name == "Welcome") { _packet = m::ServerPacket{ 1, 0, m::Welcome{1234567890123, 2} }; }
			else if (name == "Reply") { _packet = m::MakeReply(123, m::ReplyStatus::Queued); }
			else
			{
				if (!name.starts_with("State")) { throw std::invalid_argument("case"); }
				const int count = std::stoi(name.substr(5)); m::State state{ 1234567890123, position, 87, {}, 987654 };
				for (int i = 0; i < count; ++i) { state.visibleEntityIds.push_back(1000000000000ull + i); }
				_packet = m::ServerPacket{ 1, 123, std::move(state) };
			}
			input = Encode(); const auto decoded = Decode();
			if (EncodePacket(decoded) != input) { throw std::runtime_error("wire fields changed"); }
		}
		Frame Encode() const { return EncodePacket(_packet); }
		Packet Decode() const
		{
			if (input.size() < 4) { throw std::runtime_error("short frame"); }
			const auto size = helix::proto::DecodeHeader(_framing, input.data());
			if (size > _framing.maxMessageSize || size != input.size() - 4) { throw std::runtime_error("frame length"); }
			const auto payload = std::span(input).subspan(4);
			if (std::holds_alternative<m::ClientPacket>(_packet))
			{
				m::ClientPacket decoded; if (!_codec->DecodeClient(payload, decoded) || !m::IsValid(decoded)) { throw std::runtime_error("client validation"); }
				return decoded;
			}
			m::ServerPacket decoded; if (!_codec->DecodeServer(payload, decoded) || !m::IsValid(decoded)) { throw std::runtime_error("server validation"); }
			return decoded;
		}
		std::uint64_t DecodeSink() const
		{
			const auto value = Decode();
			if (const auto* client = std::get_if<m::ClientPacket>(&value)) { return client->requestId; }
			const auto& server = std::get<m::ServerPacket>(value);
			if (const auto* state = std::get_if<m::State>(&server.body)) { return server.requestId + state->entityId + state->visibleEntityIds.size() + state->tick; }
			return server.requestId;
		}
		Frame input;
	private:
		Frame EncodePacket(const Packet& value) const
		{
			std::string payload;
			const bool ok = std::holds_alternative<m::ClientPacket>(value) ? _codec->EncodeClient(std::get<m::ClientPacket>(value), payload)
				: _codec->EncodeServer(std::get<m::ServerPacket>(value), payload);
			if (!ok) { throw std::runtime_error("packet encode"); }
			Frame frame(4 + payload.size());
			if (!helix::proto::EncodeHeader(_framing, static_cast<std::uint32_t>(payload.size()), frame.data())) { throw std::runtime_error("frame encode"); }
			std::memcpy(frame.data() + 4, payload.data(), payload.size()); return frame;
		}
		std::shared_ptr<const p::IPacketSerializer> _codec;
		helix::proto::FrameCodec _framing;
		Packet _packet;
	};
	template<class F> std::vector<double> Timings(F operation, int iterations, int samples)
	{
		for (int i = 0; i < 2000; ++i) { sink = operation(); }
		std::vector<double> timings;
		for (int batch = 0; batch < samples; ++batch)
		{
			const auto start = std::chrono::steady_clock::now();
			for (int i = 0; i < iterations; ++i) { sink = operation(); }
			timings.push_back(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / iterations);
		}
		return timings;
	}
	template<class F> void Row(std::ostream& file, const std::string& name, const std::string& format, const std::string& operation, const WireCase& item, F action, int iterations, int samples)
	{
		file << name << ',' << format << ',' << operation << ',' << item.input.size() << ',' << (format == "protobuf" ? 0 : 8) << ',';
#ifdef HELIX_COUNT_NEW
		for (int i = 0; i < 2000; ++i) { sink = action(); }
		allocation::Reset(); allocation::enabled = true;
		for (int i = 0; i < iterations; ++i) { sink = action(); }
		allocation::enabled = false;
		file << allocation::bytes / static_cast<double>(iterations) << ',' << allocation::count / static_cast<double>(iterations) << '\n';
#else
		const auto raw = Timings(action, iterations, samples); auto sorted = raw; std::sort(sorted.begin(), sorted.end());
		file << sorted[samples / 2] << ',' << std::accumulate(raw.begin(), raw.end(), 0.0) / samples << ',';
		for (int i = 0; i < samples; ++i) { if (i) { file << ';'; } file << raw[i]; }
		file << '\n';
#endif
	}
}
int main(int argc, char** argv)
{
	try
	{
		const int iterations = argc > 1 ? std::stoi(argv[1]) : 10000, samples = argc > 2 ? std::stoi(argv[2]) : 9, run = argc > 4 ? std::stoi(argv[4]) : 0;
		if (iterations < 1 || samples < 3 || samples % 2 == 0) { throw std::invalid_argument("positive iterations, odd samples >=3"); }
		if (argc < 4) { throw std::invalid_argument("iterations samples output.csv [run-order]"); }
		std::ofstream file(argv[3]); if (!file) { throw std::runtime_error("output file"); }
		file << std::setprecision(12);
#ifdef HELIX_COUNT_NEW
		file << "Message,Format,Operation,WireBytes,EnvelopeBytes,AllocatedBytes,AllocationCount\n";
#else
		file << "Message,Format,Operation,WireBytes,EnvelopeBytes,MedianNs,MeanNs,SamplesNs\n";
#endif
		p::SerializerRegistry registry; p::RegisterFlatBuffers(registry);
		std::vector<std::string> formats{ "protobuf", "flatbuffers" }; if (run % 2) { std::reverse(formats.begin(), formats.end()); }
		for (const auto* name : { "Move", "Attack", "Look", "Welcome", "Reply", "State0", "State16", "State256", "State4096" })
		for (const auto& format : formats)
		{
			const WireCase item(name, registry.Create(format));
			Row(file, name, format, "EncodeFramed", item, [&] { const auto value = item.Encode(); return value.size() + std::to_integer<unsigned>(value[0]); }, iterations, samples);
			Row(file, name, format, "DecodeValidated", item, [&] { return item.DecodeSink(); }, iterations, samples);
		}
		std::cout << "PASS: 18 native production-codec cases; run=" << run << ", ops=" << iterations << ", samples=" << samples << ", logical CPUs=" << std::thread::hardware_concurrency() << ", compiler=";
#ifdef _MSC_FULL_VER
		std::cout << "MSVC " << _MSC_FULL_VER;
#else
		std::cout << __VERSION__;
#endif
		std::cout << '\n';
	}
	catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
