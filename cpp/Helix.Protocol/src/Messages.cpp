#include "helix/protocol/messages.h"
#include <cmath>

namespace helix::protocol::messages
{
	namespace { bool Finite(const std::optional<Position>& p) { return p && std::isfinite(p->x) && std::isfinite(p->y) && std::isfinite(p->z); } }
	bool IsValid(const ClientPacket& p)
	{
		if (p.protocolVersion != Version) { return false; }
		if (const auto* m = std::get_if<Move>(&p.body)) { return Finite(m->destination); }
		if (const auto* a = std::get_if<Attack>(&p.body)) { return a->targetEntityId != 0; }
		return std::holds_alternative<Look>(p.body);
	}
	bool IsValid(const ServerPacket& p)
	{
		if (p.protocolVersion != Version) { return false; }
		if (const auto* w = std::get_if<Welcome>(&p.body)) { return w->entityId != 0 && w->worldId != 0; }
		if (const auto* r = std::get_if<Reply>(&p.body)) { return r->status >= ReplyStatus::Queued && r->status <= ReplyStatus::Pending; }
		if (const auto* s = std::get_if<State>(&p.body)) { return s->entityId != 0 && Finite(s->position); }
		return false;
	}
}
