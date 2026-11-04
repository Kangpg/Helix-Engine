using System.Buffers.Binary;
using System.Collections.Immutable;
using System.Net.Sockets;
using Helix.Engine;
using Helix.Engine.Transport;
using Helix.Mmo;
using Google.Protobuf;
using Helix.Protocol;
using Helix.Protocol.Wire;
using Position = Helix.Mmo.Position;

if (args.Length == 3 && args[0] is "--write" or "--verify")
{
	SerializerTests.Fixtures(args[0], args[1], args[2]); return;
}
if (args.Length == 4 && args[0] == "--world-client")
{
	await SerializedWorldClient.Run(args[1], int.Parse(args[2]), args[3]).WaitAsync(TimeSpan.FromSeconds(30)); return;
}
SerializerTests.Run();

static void Check(bool value, string message) { if (!value) throw new Exception(message); }
static WorldBehaviors Behaviors() => new(
	new DelegateMovementPolicy(context => context.Request.Destination), new FixedDamage(),
	new DelegateVisibilityPolicy((_, _) => [1, 2, 2, 99]));

var packet = new ClientPacket { ProtocolVersion = 1, RequestId = 123,
	Move = new MoveCommand { Destination = new Helix.Protocol.Wire.Position { X = 3.5 } } };
var encoded = packet.ToByteArray().Concat(new byte[] { 0xA0, 0x06, 1 }).ToArray();
Check(PacketCodec.TryDecodeClient(encoded, out var decoded) && decoded is not null && PacketCodec.IsValid(decoded) && decoded.RequestId == 123 && decoded.Move.Destination.X == 3.5, "Protobuf move round-trip");
Check(decoded!.ToByteArray().SequenceEqual(encoded), "unknown fields survive round-trip");
decoded!.ProtocolVersion = 2; Check(!PacketCodec.IsValid(decoded), "unsupported version");
decoded.ProtocolVersion = 1; decoded.Move.Destination.X = double.NaN; Check(!PacketCodec.IsValid(decoded), "finite coordinates");
decoded.Attack = new AttackCommand(); Check(!PacketCodec.IsValid(decoded), "zero target");
decoded.Look = new LookCommand(); Check(PacketCodec.IsValid(decoded) && decoded.Attack is null, "oneof replacement");
decoded.ClearBody(); Check(!PacketCodec.IsValid(decoded), "missing body");
Check(!PacketCodec.TryDecodeClient(new byte[] { 0x52, 5, 8 }, out _), "truncated protobuf");
var replyPacket = ServerPacket.Parser.ParseFrom(PacketCodec.Encode(PacketCodec.Reply(123, ReplyStatus.Queued)));
Check(replyPacket.RequestId == 123 && replyPacket.Reply.Status == ReplyStatus.Queued, "reply correlation");

var limits = new WorldLimits(10, 20, 20);
var field = new WorldInstance(1, limits, Behaviors());
var arena = new WorldInstance(2, limits, Behaviors());
foreach (var world in new[] { field, arena })
{
	Check(world.QueueAdd(new EntityState(1, default, 100)) && world.QueueAdd(new EntityState(2, default, 100)), "initial queue");
	Check(world.Find(1) is null, "commands must wait for Tick");
	Check(world.Tick().Applied == 2, "initial Tick");
}
var oldSnapshot = field.Snapshot();
field.ReplaceMovement(new DelegateMovementPolicy(_ => new Position(3, 0, 0)));
field.ReplaceCombat(new DelegateCombatRules(_ => 20));
foreach (var world in new[] { field, arena })
{
	world.QueueMove(new MoveRequest(1, new Position(9, 0, 0))); world.QueueAttack(new AttackRequest(1, 2)); world.Tick();
}
Check(field.Find(1)!.Value.Position.X == 3 && arena.Find(1)!.Value.Position.X == 9, "instance movement isolation");
Check(field.Find(2)!.Value.Health == 80 && arena.Find(2)!.Value.Health == 93, "class/delegate combat parity");
Check(oldSnapshot.Entities[1].Health == 100, "immutable old snapshot");
Check(field.VisibleTo(1).SequenceEqual(new ulong[] { 2 }), "AOI filtering");
field.ReplaceVisibility(new DelegateVisibilityPolicy((_, _) => []));
Check(field.VisibleTo(1).Length == 1, "AOI replacement waits for Tick");
field.Tick();
Check(field.VisibleTo(1).Length == 0 && arena.VisibleTo(1).Length == 1, "AOI isolation");

using var entered = new ManualResetEventSlim();
using var resume = new ManualResetEventSlim();
var busy = new WorldInstance(3, limits, Behaviors());
busy.QueueAdd(new EntityState(1, default, 100)); busy.Tick();
busy.ReplaceMovement(new DelegateMovementPolicy(context => { entered.Set(); resume.Wait(); return context.Request.Destination; }));
busy.QueueMove(new MoveRequest(1, new Position(1, 0, 0)));
var ticker = Task.Run(() => busy.Tick());
Check(entered.Wait(TimeSpan.FromSeconds(5)), "Tick entered rule");
busy.ReplaceMovement(new DelegateMovementPolicy(_ => new Position(8, 0, 0)));
busy.QueueMove(new MoveRequest(1, new Position(2, 0, 0)));
var overlapRejected = false;
try { busy.Tick(); } catch (InvalidOperationException) { overlapRejected = true; }
resume.Set(); await ticker;
Check(overlapRejected && busy.Find(1)!.Value.Position.X == 1, "running Tick retains original batch/policy");
busy.Tick(); Check(busy.Find(1)!.Value.Position.X == 8, "next Tick policy");

var bounded = new WorldInstance(4, new WorldLimits(1, 2, 1), Behaviors());
Check(bounded.QueueAdd(new EntityState(1, default, 100)) && bounded.QueueAdd(new EntityState(2, default, 100)), "queue capacity");
Check(!bounded.QueueRemove(1), "queue overflow");
Check(bounded.Tick().Processed == 1 && bounded.Tick().Rejected == 1, "tick budget and entity limit");
bounded.ReplaceMovement(new DelegateMovementPolicy(_ => new Position(double.NaN, 0, 0)));
bounded.QueueMove(new MoveRequest(1, default));
Check(bounded.Tick().Rejected == 1 && bounded.Find(1)!.Value.Position.X == 0, "invalid rule result");
bounded.ReplaceMovement(new DelegateMovementPolicy(_ => throw new Exception("user rule")));
bounded.QueueMove(new MoveRequest(1, default)); Check(bounded.Tick().Rejected == 1, "rule failure containment");

var sendQueue = new SendQueue(1);
Check(sendQueue.TryEnqueue([1]) && !sendQueue.TryEnqueue([2]), "Channels must reject overflow without silent drop");
sendQueue.Complete(); Check(!sendQueue.TryEnqueue([3]), "completed queue");

var injected = new StubTransport();
await using (var host = new HelixServer(new EngineConfig { Transport = "custom" }, injected))
{
	await host.RunAsync(new DelegateServerApp((session, data) => session.Send(data)));
	Check(injected.Started && injected.Disposed && injected.Sent, "custom transport must work through abstraction without editing core");
}

foreach (var (headerSize, bigEndian) in new[] { (4, true), (2, false) })
{
	await using var server = new HelixServer(new EngineConfig { Ip = "127.0.0.1", Port = 0, HeaderSize = headerSize, BigEndian = bigEndian, MaxMessageSize = 32 });
	var ready = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
	var app = new EchoApp(ready);
	var running = server.RunAsync(app);
	await ready.Task.WaitAsync(TimeSpan.FromSeconds(5));
	using var client = new TcpClient(); await client.ConnectAsync("127.0.0.1", server.Port);
	var stream = client.GetStream();
	byte[] Frame(byte[] payload)
	{
		var bytes = new byte[headerSize + payload.Length];
		if (headerSize == 2) BinaryPrimitives.WriteUInt16LittleEndian(bytes, (ushort)payload.Length);
		else BinaryPrimitives.WriteUInt32BigEndian(bytes, (uint)payload.Length);
		payload.CopyTo(bytes, headerSize); return bytes;
	}
	var first = Frame([1, 2, 3]); var second = Frame([4, 5]);
	await stream.WriteAsync(first.AsMemory(0, 1));
	await stream.WriteAsync(first.AsMemory(1));
	await stream.WriteAsync(second);
	var echoed = new byte[first.Length + second.Length];
	await stream.ReadExactlyAsync(echoed).AsTask().WaitAsync(TimeSpan.FromSeconds(5));
	Check(echoed.SequenceEqual(first.Concat(second)), "fragmented/combined frames must echo in order");
	using var bad = new TcpClient(); await bad.ConnectAsync("127.0.0.1", server.Port);
	await bad.GetStream().WriteAsync(Frame(new byte[33]));
	var oversizedClosed = false;
	try { oversizedClosed = await bad.GetStream().ReadAsync(new byte[1]).AsTask().WaitAsync(TimeSpan.FromSeconds(5)) == 0; }
	catch (IOException error) when (error.InnerException is SocketException { SocketErrorCode: SocketError.ConnectionReset }) { oversizedClosed = true; }
	Check(oversizedClosed, "oversized frame must close before body allocation");
	server.Stop(); await running.WaitAsync(TimeSpan.FromSeconds(5));
	Check(server.SessionCount == 0 && app.Disconnects == app.Connects, "shutdown must finish all sessions and notify exactly once");
}

var path = Path.GetTempFileName();
var previous = Environment.GetEnvironmentVariable("HELIX_MMO__MAX_ENTITIES");
var previousSerializer = Environment.GetEnvironmentVariable("HELIX_PROTOCOL__SERIALIZER");
try
{
	Environment.SetEnvironmentVariable("HELIX_PROTOCOL__SERIALIZER", null);
	File.WriteAllText(path, "[mmo]\nmax_entities=12\n[protocol]\nserializer=flatbuffers\n");
	Check(EngineConfig.Load(path).WorldMaxEntities == 12, "file configuration");
	Check(EngineConfig.Load(path).ProtocolSerializer == "flatbuffers", "serializer file selection");
	Environment.SetEnvironmentVariable("HELIX_PROTOCOL__SERIALIZER", "memorypack");
	Check(EngineConfig.Load(path).ProtocolSerializer == "memorypack", "serializer env precedence");
	Environment.SetEnvironmentVariable("HELIX_MMO__MAX_ENTITIES", "13");
	Check(EngineConfig.Load(path).WorldMaxEntities == 13, "environment precedence");
	Environment.SetEnvironmentVariable("HELIX_MMO__MAX_ENTITIES", "invalid");
	var rejected = false; try { EngineConfig.Load(path); } catch (FormatException) { rejected = true; }
	Check(rejected, "invalid config must fail");
}
finally
{
	Environment.SetEnvironmentVariable("HELIX_MMO__MAX_ENTITIES", previous);
	Environment.SetEnvironmentVariable("HELIX_PROTOCOL__SERIALIZER", previousSerializer); File.Delete(path);
}

Console.WriteLine("PASS: Protobuf validation/unknown fields/oneof/request IDs, MMO replacements/isolation/tick boundaries/snapshots/limits, Channels backpressure, managed TCP framing/oversize/shutdown and config precedence");

sealed class FixedDamage : ICombatRules { public int? Resolve(CombatContext context) => 7; }
sealed class StubTransport : IServerTransport, ISessionTransport
{
	public bool Started; public bool Disposed; public bool Sent;
	public int Port => 0;
	public int SessionCount => 0;
	public Exception? LastError => null;
	public void Start() => Started = true;
	public Task RunAsync(IHelixApp app, CancellationToken cancellation)
	{
		var session = new Session(1, this);
		app.OnConnect(session); app.OnMessage(session, [1]); app.OnDisconnect(session, CloseReason.Normal);
		return Task.CompletedTask;
	}
	public Session? FindSession(ulong id) => null;
	public bool Send(ReadOnlySpan<byte> message) { Sent = message.SequenceEqual(new byte[] { 1 }); return true; }
	public void Close(CloseReason reason) { }
	public ValueTask DisposeAsync() { Disposed = true; return ValueTask.CompletedTask; }
}
sealed class EchoApp(TaskCompletionSource ready) : IHelixApp
{
	public int Connects; public int Disconnects;
	public void OnStart(HelixServer server) => ready.TrySetResult();
	public void OnConnect(Session session) => Interlocked.Increment(ref Connects);
	public void OnMessage(Session session, ReadOnlySpan<byte> message) => session.Send(message);
	public void OnDisconnect(Session session, CloseReason reason) => Interlocked.Increment(ref Disconnects);
}
