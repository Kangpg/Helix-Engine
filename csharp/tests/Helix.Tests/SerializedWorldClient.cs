using System.Buffers.Binary;
using System.Net.Sockets;
using Helix.Protocol;
using M = Helix.Protocol.Messages;

internal static class SerializedWorldClient
{
	private static void Check(bool ok, string message) { if (!ok) throw new Exception(message); }
	private sealed class Peer : IDisposable
	{
		private readonly TcpClient _client = new();
		private readonly IPacketSerializer _codec;
		private NetworkStream Stream => _client.GetStream();
		public ulong Id, World;
		public Peer(IPacketSerializer codec) { _codec = codec; }
		public async Task Connect(string host, int port)
		{
			await _client.ConnectAsync(host, port);
			var hello = await Receive(); Check(hello.Body is M.Welcome, "welcome");
			var welcome = (M.Welcome)hello.Body!; Id = welcome.EntityId; World = welcome.WorldId;
		}
		private async Task<M.ServerPacket> Receive()
		{
			var header = new byte[4]; await Stream.ReadExactlyAsync(header);
			var size = BinaryPrimitives.ReadUInt32BigEndian(header); Check(size <= 1048576, "bounded frame");
			var bytes = new byte[size]; await Stream.ReadExactlyAsync(bytes);
			Check(_codec.TryDecodeServer(bytes, out var packet) && packet is not null && M.MessageContract.IsValid(packet), "server codec/version/semantic agreement");
			return packet!;
		}
		public async Task<M.ServerPacket> Raw(byte[] bytes)
		{
			var header = new byte[4]; BinaryPrimitives.WriteUInt32BigEndian(header, (uint)bytes.Length);
			await Stream.WriteAsync(header.AsMemory(0, 1)); await Stream.WriteAsync(header.AsMemory(1)); await Stream.WriteAsync(bytes);
			return await Receive();
		}
		public async Task<M.ServerPacket> Request(M.ClientBody body, uint version = 1)
		{
			var packet = await Raw(_codec.EncodeClient(new(version, ulong.MaxValue, body)));
			Check(packet.RequestId == ulong.MaxValue, "request correlation"); return packet;
		}
		public async Task<M.State> Wait(Func<M.State, bool> predicate)
		{
			var stop = DateTime.UtcNow.AddSeconds(5);
			while (DateTime.UtcNow < stop)
			{
				if ((await Request(new M.Look())).Body is M.State state && predicate(state)) return state;
				await Task.Delay(10);
			}
			throw new Exception("world state timeout");
		}
		public void Dispose() => _client.Dispose();
	}
	internal static async Task Run(string host, int port, string format)
	{
		var codec = SerializerTests.Registry().Create(format);
		var peers = Enumerable.Range(0, 4).Select(_ => new Peer(codec)).ToArray();
		try
		{
			foreach (var peer in peers) { await peer.Connect(host, port); await peer.Wait(_ => true); }
			var field = peers.Where(p => p.World == 1).ToArray(); var arena = peers.Where(p => p.World == 2).ToArray();
			Check(field.Length == 2 && arena.Length == 2, "instance grouping");
			Check((await field[0].Request(new M.Move(new(3, 4, 5)))).Body is M.Reply { Status: M.ReplyStatus.Queued }, "move queue");
			await field[0].Wait(s => s.Position == new M.Position(3, 4, 5));
			Check((await arena[0].Request(new M.Attack(arena[1].Id))).Body is M.Reply { Status: M.ReplyStatus.Queued }, "attack queue");
			await arena[1].Wait(s => s.Health == 90);
			await field[0].Request(new M.Attack(field[1].Id)); await field[1].Request(new M.Move(new(1, 2, 3)));
			Check((await field[1].Wait(s => s.Position?.X == 1)).Health == 100, "field damage rejection");
			foreach (var group in new[] { field, arena }) foreach (var peer in group)
			{
				var state = await peer.Wait(_ => true);
				Check(state.VisibleEntityIds.SequenceEqual(group.Where(p => p != peer).Select(p => p.Id)), "AOI isolation");
			}
			foreach (var body in new M.ClientBody[] { new M.Move(null), new M.Move(new(double.NaN)), new M.Attack(0) })
				Check((await field[0].Request(body)).Body is M.Reply { Status: M.ReplyStatus.Invalid }, "invalid semantics");
			Check((await field[0].Request(new M.Look(), 2)).Body is M.Reply { Status: M.ReplyStatus.Invalid }, "unsupported version");
			var malformed = await field[0].Raw([0x52, 5, 8]);
			Check(malformed.RequestId == 0 && malformed.Body is M.Reply { Status: M.ReplyStatus.Invalid }, "malformed reply");
			var wrongCodec = SerializerTests.Registry().Create(format == "protobuf" ? "flatbuffers" : "protobuf");
			var mismatch = await field[0].Raw(wrongCodec.EncodeClient(new(1, 123, new M.Look())));
			Check(mismatch.RequestId == 0 && mismatch.Body is M.Reply { Status: M.ReplyStatus.Invalid }, "format mismatch rejection");
			Console.WriteLine($"PASS: {format} actual MMO wire; movement/combat/AOI, request uint64, invalid/version/format, fragmented frames");
		}
		finally { foreach (var peer in peers) peer.Dispose(); }
	}
}
