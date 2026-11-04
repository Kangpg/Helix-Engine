using System.Collections.Immutable;
using Helix.Engine;
using Helix.Mmo;
using Helix.Protocol;
using Helix.Protocol.Messages;
using Helix.Protocol.FlatBuffers;
using Helix.Protocol.MemoryPack;
using Position = Helix.Mmo.Position;

var config = EngineConfig.Load(args.Length > 0 ? args[0] : "config/helix.toml");
await using var server = new HelixServer(config);
Console.CancelKeyPress += (_, e) => { e.Cancel = true; server.Stop(); };
var serializers = new SerializerRegistry();
serializers.RegisterFlatBuffers(); serializers.RegisterMemoryPack();
var serializer = serializers.Create(config.ProtocolSerializer);
Console.WriteLine($"serializer={config.ProtocolSerializer}");
await server.RunAsync(new WorldApp(config, serializer));

sealed class WorldApp : IHelixApp
{
	private readonly IServerPacketSerializer _serializer;
	private readonly WorldInstance _field;
	private readonly WorldInstance _arena;
	private readonly int _tickMs;
	private readonly CancellationTokenSource _stop = new();
	private Task? _ticker;
	public WorldApp(EngineConfig config, IServerPacketSerializer serializer)
	{
		_serializer = serializer;
		var limits = new WorldLimits(config.WorldMaxEntities, config.WorldMaxPendingCommands, config.WorldMaxCommandsPerTick);
		_field = new WorldInstance(1, limits, Behaviors(false));
		_arena = new WorldInstance(2, limits, Behaviors(true));
		_tickMs = config.WorldTickMs;
	}
	private static WorldBehaviors Behaviors(bool arena) => new(
		new DelegateMovementPolicy(context => context.Request.Destination),
		new DelegateCombatRules(_ => arena ? 10 : null),
		new DelegateVisibilityPolicy((observer, entities) => entities.Where(entity => entity.Id != observer).Select(entity => entity.Id).ToImmutableArray()));
	private WorldInstance World(ulong id) => (id & 1) != 0 ? _field : _arena;
	private void Reply(Session session, ServerPacket packet)
	{
		if (!session.Send(_serializer.EncodeServer(packet))) session.Close(CloseReason.IoError);
	}
	public void OnStart(HelixServer server)
	{
		_ticker = Task.Run(async () =>
		{
			using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(_tickMs));
			try
			{
				while (!_stop.IsCancellationRequested)
				{
					_field.Tick(); _arena.Tick();
					if (!await timer.WaitForNextTickAsync(_stop.Token)) break;
				}
			}
			catch (OperationCanceledException) when (_stop.IsCancellationRequested) { }
		});
	}
	public void OnStop() { _stop.Cancel(); _ticker?.GetAwaiter().GetResult(); _stop.Dispose(); }
	public void OnConnect(Session session)
	{
		var world = World(session.Id);
		if (!world.QueueAdd(new EntityState(session.Id, default, 100))) { session.Close(); return; }
		Reply(session, new ServerPacket(MessageContract.Version, 0, new Welcome(session.Id, world.Id)));
	}
	public void OnDisconnect(Session session, CloseReason reason)
	{
		if (!World(session.Id).QueueRemove(session.Id)) Console.Error.WriteLine($"world removal queue full: {session.Id}");
	}
	public void OnMessage(Session session, ReadOnlySpan<byte> message)
	{
		if (!_serializer.TryDecodeClient(message, out var request) || request is null)
			{ Reply(session, MessageContract.Reply(0, ReplyStatus.Invalid)); return; }
		if (!MessageContract.IsValid(request))
			{ Reply(session, MessageContract.Reply(request.RequestId, ReplyStatus.Invalid)); return; }
		var world = World(session.Id);
		if (request.Body is Move move)
		{
			var p = move.Destination!;
			Reply(session, MessageContract.Reply(request.RequestId, world.QueueMove(new MoveRequest(session.Id, new Position(p.X, p.Y, p.Z))) ? ReplyStatus.Queued : ReplyStatus.Busy));
		}
		else if (request.Body is Attack attack)
			Reply(session, MessageContract.Reply(request.RequestId, world.QueueAttack(new AttackRequest(session.Id, attack.TargetEntityId)) ? ReplyStatus.Queued : ReplyStatus.Busy));
		else
		{
			var snapshot = world.Snapshot();
			var entity = snapshot.Entities.FirstOrDefault(e => e.Id == session.Id);
			if (entity.Id == 0) { Reply(session, MessageContract.Reply(request.RequestId, ReplyStatus.Pending)); return; }
			var state = new State(entity.Id, new Helix.Protocol.Messages.Position(entity.Position.X, entity.Position.Y, entity.Position.Z),
				entity.Health, world.VisibleTo(session.Id).ToArray(), snapshot.Tick);
			Reply(session, new ServerPacket(MessageContract.Version, request.RequestId, state));
		}
	}
}
