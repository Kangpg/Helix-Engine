#include "helix/helix.h"
#include "helix/engine_config.h"
#include "helix/mmo/delegate_policies.h"
#include "helix/mmo/world_instance.h"
#ifdef HELIX_HAS_PARALLEL_MMO
#include "helix/mmo/parallel_world.h"
#endif
#include "helix/protocol/serializer.h"
#ifdef HELIX_HAS_FLATBUFFERS
#include "helix/protocol/flatbuffers_serializer.h"
#endif
#include <algorithm>
#include <condition_variable>
#include <csignal>
#include <iostream>
#include <thread>

using namespace helix::mmo;
namespace msg = helix::protocol::messages;
namespace
{
	helix::Server* g_server = nullptr;
	void Stop(int) { if (g_server) { g_server->Stop(); } }
	WorldBehaviors Behaviors(bool arena)
	{
		return {
			std::make_shared<DelegateMovementPolicy>([](const MovementContext& c) { return std::optional(c.request.destination); }),
			std::make_shared<DelegateCombatRules>([arena](const CombatContext&) -> std::optional<std::int32_t> { return arena ? std::optional<std::int32_t>(10) : std::nullopt; }),
			std::make_shared<DelegateVisibilityPolicy>([](EntityId observer, std::span<const EntityState> entities)
			{
				std::vector<EntityId> ids;
				for (const auto& entity : entities) { if (entity.id != observer) { ids.push_back(entity.id); } }
				return ids;
			})
		};
	}
	void SendReply(helix::Session& session, const msg::ServerPacket& packet, const helix::protocol::IServerPacketSerializer& serializer)
	{
		std::string value;
		if (!serializer.EncodeServer(packet, value)) { session.Close(helix::SessionCloseReason::IoError); return; }
		if (session.Send(helix::ByteSpan(reinterpret_cast<const std::byte*>(value.data()), value.size())) != helix::Status::Ok)
			{ session.Close(helix::SessionCloseReason::IoError); }
	}
}

// 포맷 독립 메시지를 월드 요청으로 변환한다. 직렬화 구현은 조립 지점에서 주입한다.
template<class TWorld>
class WorldApp final : public helix::IServerApp
{
public:
	WorldApp(const helix::config::EngineConfig& config, std::shared_ptr<const helix::protocol::IServerPacketSerializer> serializer,
		std::unique_ptr<TWorld> field, std::unique_ptr<TWorld> arena)
		: _serializer(std::move(serializer)), _field(std::move(field)), _arena(std::move(arena)),
		  _tickMs(config.worldTickMs) {}
	~WorldApp() override { OnStop(); }
	void OnStart(helix::Server&) override
	{
		_ticker = std::jthread([this](std::stop_token stop)
		{
			std::mutex mutex;
			std::condition_variable_any wake;
			std::unique_lock lock(mutex);
			while (!stop.stop_requested())
			{
				_field->Tick(); _arena->Tick();
				wake.wait_for(lock, stop, std::chrono::milliseconds(_tickMs), [] { return false; });
			}
		});
	}
	void OnStop() override { if (_ticker.joinable()) { _ticker.request_stop(); _ticker.join(); } }
	void OnConnect(helix::Session& session) override
	{
		auto& world = World(session.Id());
		if (!world.QueueAdd({ session.Id(), {}, 100 })) { session.Close(helix::SessionCloseReason::Kicked); return; }
		Reply(session, { msg::Version, 0, msg::Welcome{ session.Id(), world.Id() } });
	}
	void OnDisconnect(helix::Session& session, helix::SessionCloseReason) override
	{
		// 큐 초과는 무시하지 않는다. 샘플은 운영자가 관찰할 수 있게 보고한다.
		if (!World(session.Id()).QueueRemove(session.Id())) { std::cerr << "world removal queue full: " << session.Id() << '\n'; }
	}
	void OnMessage(helix::Session& session, helix::ByteSpan bytes) override
	{
		msg::ClientPacket request;
		if (!_serializer->DecodeClient(bytes, request))
			{ Reply(session, msg::MakeReply(0, msg::ReplyStatus::Invalid)); return; }
		if (!msg::IsValid(request))
			{ Reply(session, msg::MakeReply(request.requestId, msg::ReplyStatus::Invalid)); return; }
		auto& world = World(session.Id());
		if (const auto* move = std::get_if<msg::Move>(&request.body))
		{
			const auto& p = *move->destination;
			Reply(session, msg::MakeReply(request.requestId, world.QueueMove({ session.Id(), { p.x, p.y, p.z } })
				? msg::ReplyStatus::Queued : msg::ReplyStatus::Busy));
		}
		else if (const auto* attack = std::get_if<msg::Attack>(&request.body))
		{
			Reply(session, msg::MakeReply(request.requestId, world.QueueAttack({ session.Id(), attack->targetEntityId })
				? msg::ReplyStatus::Queued : msg::ReplyStatus::Busy));
		}
		else
		{
			const auto snapshot = world.Snapshot();
			const auto entity = std::find_if(snapshot->entities.begin(), snapshot->entities.end(), [&](const auto& e) { return e.id == session.Id(); });
			if (entity == snapshot->entities.end()) { Reply(session, msg::MakeReply(request.requestId, msg::ReplyStatus::Pending)); return; }
			msg::State state{ entity->id, msg::Position{ entity->position.x, entity->position.y, entity->position.z },
				entity->health, world.VisibleTo(session.Id()), snapshot->tick };
			Reply(session, { msg::Version, request.requestId, std::move(state) });
		}
	}
private:
	void Reply(helix::Session& session, const msg::ServerPacket& packet) { SendReply(session, packet, *_serializer); }
	std::shared_ptr<const helix::protocol::IServerPacketSerializer> _serializer;
	TWorld& World(EntityId id) { return (id & 1) ? *_field : *_arena; }
	std::unique_ptr<TWorld> _field;
	std::unique_ptr<TWorld> _arena;
	std::uint32_t _tickMs;
	std::jthread _ticker;
};

int main(int argc, char** argv)
{
	const std::string path = argc > 1 ? argv[1] : "config/helix.toml";
	helix::config::ConfigLoader loader;
	const auto config = loader.Load(path);
	if (!config) { std::cerr << loader.GetError(); return 1; }
	helix::protocol::SerializerRegistry serializers;
#ifdef HELIX_HAS_FLATBUFFERS
	helix::protocol::RegisterFlatBuffers(serializers);
#endif
	std::shared_ptr<const helix::protocol::IPacketSerializer> serializer;
	try { serializer = serializers.Create(config->protocolSerializer); }
	catch (const std::exception& e) { std::cerr << e.what(); return 1; }
	std::cout << "serializer=" << config->protocolSerializer << std::endl;
	std::unique_ptr<helix::IServerApp> app;
	const WorldLimits limits{ config->worldMaxEntities, config->worldMaxPendingCommands, config->worldMaxCommandsPerTick };
	if (config->worldExecution == "parallel")
	{
#ifdef HELIX_HAS_PARALLEL_MMO
		auto executor = CreateTaskflowWorldExecutor(config->worldWorkerThreads);
		const ParallelWorldOptions options{ config->worldBatchSize, config->worldCellSize, config->worldVisibilityRadius, config->worldMaxHitsPerTick };
		app = std::make_unique<WorldApp<ParallelWorldInstance>>(*config, serializer,
			std::make_unique<ParallelWorldInstance>(1, limits, Behaviors(false), options, executor),
			std::make_unique<ParallelWorldInstance>(2, limits, Behaviors(true), options, executor));
#else
		std::cerr << "parallel world requires HELIX_BUILD_PARALLEL_MMO=ON\n";
		return 1;
#endif
	}
	else
	{
		app = std::make_unique<WorldApp<WorldInstance>>(*config, serializer,
			std::make_unique<WorldInstance>(1, limits, Behaviors(false)), std::make_unique<WorldInstance>(2, limits, Behaviors(true)));
	}
	std::cout << "world_execution=" << config->worldExecution << std::endl;
	auto server = helix::Server::LoadConfig(path);
	g_server = &server;
	std::signal(SIGINT, Stop); std::signal(SIGTERM, Stop);
	return server.Run(std::move(app));
}
