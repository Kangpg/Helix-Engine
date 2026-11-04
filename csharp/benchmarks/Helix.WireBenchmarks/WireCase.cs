using System.Buffers.Binary;
using Helix.Protocol;
using Helix.Protocol.FlatBuffers;
using Helix.Protocol.MemoryPack;
using M = Helix.Protocol.Messages;

namespace Helix.WireBenchmarks;

public sealed class WireCase
{
	public static readonly string[] Names = ["Move", "Attack", "Look", "Welcome", "Reply", "State0", "State16", "State256", "State4096"];
	public static readonly string[] Formats = ["protobuf", "flatbuffers", "memorypack"];
	private readonly IPacketSerializer _codec;
	private readonly M.ClientPacket? _client;
	private readonly M.ServerPacket? _server;
	public byte[] Input { get; }
	public int EnvelopeBytes => _codec.FormatId == 1 ? 0 : 8;
	public WireCase(string name, string format)
	{
		var r = new SerializerRegistry(); r.RegisterFlatBuffers(); r.RegisterMemoryPack(); _codec = r.Create(format);
		const ulong request = 123;
		var position = new M.Position(12.25, -34.5, 100.75);
		switch (name)
		{
			case "Move": _client = new(1, request, new M.Move(position)); break;
			case "Attack": _client = new(1, request, new M.Attack(1234567890123)); break;
			case "Look": _client = new(1, request, new M.Look()); break;
			case "Welcome": _server = new(1, 0, new M.Welcome(1234567890123, 2)); break;
			case "Reply": _server = M.MessageContract.Reply(request, M.ReplyStatus.Queued); break;
			default:
				if (!name.StartsWith("State", StringComparison.Ordinal)) throw new ArgumentException("Unknown case");
				var count = int.Parse(name.AsSpan(5));
				_server = new(1, request, new M.State(1234567890123, position, 87,
					Enumerable.Range(0, count).Select(i => 1000000000000UL + (ulong)i).ToArray(), 987654)); break;
		}
		Input = EncodeFramed();
		var actual = DecodeValidated();
		if (_client is not null && actual is M.ClientPacket client && client == _client) return;
		if (_server is not null && actual is M.ServerPacket server && server.ProtocolVersion == _server.ProtocolVersion && server.RequestId == _server.RequestId)
		{
			if (_server.Body is M.State expected && server.Body is M.State state &&
				state.EntityId == expected.EntityId && state.Position == expected.Position && state.Health == expected.Health &&
				state.Tick == expected.Tick && state.VisibleEntityIds.SequenceEqual(expected.VisibleEntityIds)) return;
			if (server.Body == _server.Body) return;
		}
		throw new InvalidDataException("Round-trip changed message fields.");
	}
	public byte[] EncodeFramed()
	{
		// Mirrors Session.Send framing: fresh owned frame, big-endian length and payload copy.
		// The actual registered adapter includes DTO mapping and optional HXS1 envelope.
		var payload = _client is not null ? _codec.EncodeClient(_client) : _codec.EncodeServer(_server!);
		if (payload.Length > 1048576) throw new InvalidDataException("Frame limit");
		var frame = new byte[checked(4 + payload.Length)]; BinaryPrimitives.WriteUInt32BigEndian(frame, (uint)payload.Length);
		payload.CopyTo(frame, 4); return frame;
	}
	public object DecodeValidated()
	{
		if (Input.Length < 4) throw new InvalidDataException("Truncated header");
		var size = BinaryPrimitives.ReadUInt32BigEndian(Input);
		if (size > 1048576 || size != Input.Length - 4) throw new InvalidDataException("Frame size");
		var payload = Input.AsSpan(4);
		if (_client is not null)
		{
			if (!_codec.TryDecodeClient(payload, out var p) || p is null || !M.MessageContract.IsValid(p)) throw new InvalidDataException("Client validation");
			return p;
		}
		if (!_codec.TryDecodeServer(payload, out var s) || s is null || !M.MessageContract.IsValid(s)) throw new InvalidDataException("Server validation");
		return s;
	}
}
