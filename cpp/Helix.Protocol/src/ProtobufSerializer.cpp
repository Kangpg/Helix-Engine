#include "helix/protocol/serializer.h"
#include "helix_mmo.pb.h"
#include <limits>

namespace helix::protocol
{
	namespace
	{
		void WritePosition(const messages::Position& p, wire::Position& w) { w.set_x(p.x); w.set_y(p.y); w.set_z(p.z); }
		messages::Position ReadPosition(const wire::Position& p) { return { p.x(), p.y(), p.z() }; }
		template<class T> bool Parse(std::span<const std::byte> bytes, T& p) { return bytes.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()) && p.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())); }
	}
	bool ProtobufSerializer::EncodeClient(const messages::ClientPacket& p, std::string& b) const
	{
		wire::ClientPacket w; w.set_protocol_version(p.protocolVersion); w.set_request_id(p.requestId);
		if (const auto* m = std::get_if<messages::Move>(&p.body)) { auto* move = w.mutable_move(); if (m->destination) { WritePosition(*m->destination, *move->mutable_destination()); } }
		else if (const auto* a = std::get_if<messages::Attack>(&p.body)) { w.mutable_attack()->set_target_entity_id(a->targetEntityId); }
		else if (std::holds_alternative<messages::Look>(p.body)) { w.mutable_look(); }
		else { b.clear(); return false; }
		return w.SerializeToString(&b);
	}
	bool ProtobufSerializer::DecodeClient(std::span<const std::byte> bytes, messages::ClientPacket& p) const
	{
		p = {}; wire::ClientPacket w; if (!Parse(bytes, w)) { return false; }
		messages::ClientPacket value{ w.protocol_version(), w.request_id(), {} };
		switch (w.body_case())
		{
		case wire::ClientPacket::kMove: value.body = messages::Move{ w.move().has_destination() ? std::optional(ReadPosition(w.move().destination())) : std::nullopt }; break;
		case wire::ClientPacket::kAttack: value.body = messages::Attack{ w.attack().target_entity_id() }; break;
		case wire::ClientPacket::kLook: value.body = messages::Look{}; break;
		default: break; // A parsed missing/unknown body retains request ID for semantic INVALID.
		}
		p = std::move(value); return true;
	}
	bool ProtobufSerializer::EncodeServer(const messages::ServerPacket& p, std::string& b) const
	{
		wire::ServerPacket w; w.set_protocol_version(p.protocolVersion); w.set_request_id(p.requestId);
		if (const auto* v = std::get_if<messages::Welcome>(&p.body)) { auto* x = w.mutable_welcome(); x->set_entity_id(v->entityId); x->set_world_id(v->worldId); }
		else if (const auto* v = std::get_if<messages::Reply>(&p.body)) { w.mutable_reply()->set_status(static_cast<wire::ReplyStatus>(v->status)); }
		else if (const auto* v = std::get_if<messages::State>(&p.body))
		{
			auto* s = w.mutable_state(); s->set_entity_id(v->entityId); s->set_health(v->health); s->set_tick(v->tick);
			if (v->position) { WritePosition(*v->position, *s->mutable_position()); }
			for (const auto id : v->visibleEntityIds) { s->add_visible_entity_ids(id); }
		}
		else { b.clear(); return false; }
		return w.SerializeToString(&b);
	}
	bool ProtobufSerializer::DecodeServer(std::span<const std::byte> bytes, messages::ServerPacket& p) const
	{
		p = {}; wire::ServerPacket w; if (!Parse(bytes, w)) { return false; }
		messages::ServerPacket value{ w.protocol_version(), w.request_id(), {} };
		switch (w.body_case())
		{
		case wire::ServerPacket::kWelcome: value.body = messages::Welcome{ w.welcome().entity_id(), w.welcome().world_id() }; break;
		case wire::ServerPacket::kReply:
			if (w.reply().status() < 0 || w.reply().status() > 4) { return false; }
			value.body = messages::Reply{ static_cast<messages::ReplyStatus>(w.reply().status()) }; break;
		case wire::ServerPacket::kState:
		{
			const auto& s = w.state(); messages::State state{ s.entity_id(), s.has_position() ? std::optional(ReadPosition(s.position())) : std::nullopt, s.health(), {}, s.tick() };
			state.visibleEntityIds.assign(s.visible_entity_ids().begin(), s.visible_entity_ids().end()); value.body = std::move(state); break;
		}
		default: return false;
		}
		p = std::move(value); return true;
	}
}
