#pragma once
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace helix::protocol::messages
{
	inline constexpr std::uint32_t Version = 1;
	struct Position { double x = 0, y = 0, z = 0; };
	struct Move { std::optional<Position> destination; };
	struct Attack { std::uint64_t targetEntityId = 0; };
	struct Look {};
	struct Welcome { std::uint64_t entityId = 0, worldId = 0; };
	enum class ReplyStatus : std::uint8_t { Unspecified, Queued, Busy, Invalid, Pending };
	struct Reply { ReplyStatus status = ReplyStatus::Unspecified; };
	struct State
	{
		std::uint64_t entityId = 0;
		std::optional<Position> position;
		std::int32_t health = 0;
		std::vector<std::uint64_t> visibleEntityIds;
		std::uint64_t tick = 0;
	};
	struct ClientPacket
	{
		std::uint32_t protocolVersion = Version;
		std::uint64_t requestId = 0;
		std::variant<std::monostate, Move, Attack, Look> body;
	};
	struct ServerPacket
	{
		std::uint32_t protocolVersion = Version;
		std::uint64_t requestId = 0;
		std::variant<std::monostate, Welcome, Reply, State> body;
	};
	bool IsValid(const ClientPacket& packet);
	bool IsValid(const ServerPacket& packet);
	inline ServerPacket MakeReply(std::uint64_t id, ReplyStatus status) { return { Version, id, Reply{ status } }; }
}
