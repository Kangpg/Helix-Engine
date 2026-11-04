using Google.FlatBuffers;
using M = Helix.Protocol.Messages;
using F = Helix.Protocol.FlatWire;

namespace Helix.Protocol.FlatBuffers;

public sealed class FlatBuffersSerializer : IPacketSerializer
{
	public byte FormatId => 2;
	private static F.PositionT? Write(M.Position? p) => p is null ? null : new() { X = p.X, Y = p.Y, Z = p.Z };
	private static M.Position? Read(F.PositionT? p) => p is null ? null : new(p.X, p.Y, p.Z);
	private static byte[] Encode(uint version, ulong id, F.BodyUnion body)
	{
		var builder = new FlatBufferBuilder(128);
		F.Packet.FinishPacketBuffer(builder, F.Packet.Pack(builder, new F.PacketT { ProtocolVersion = version, RequestId = id, Body = body }));
		return builder.SizedByteArray();
	}
	private static F.PacketT? Decode(ReadOnlySpan<byte> bytes)
	{
		if (bytes.Length < 8) return null;
		var buffer = new ByteBuffer(bytes.ToArray());
		// Unlike the trusted-input benchmark, network decoding always verifies offsets first.
		if (!F.Packet.VerifyPacket(buffer)) return null;
		var p = F.Packet.GetRootAsPacket(buffer).UnPack();
		return p.Body?.Value is null ? null : p;
	}
	public byte[] EncodeClient(M.ClientPacket p) => Encode(p.ProtocolVersion, p.RequestId, p.Body switch
	{
		M.Move v => F.BodyUnion.FromMove(new() { Destination = Write(v.Destination) }),
		M.Attack v => F.BodyUnion.FromAttack(new() { TargetEntityId = v.TargetEntityId }),
		M.Look => F.BodyUnion.FromLook(new()), _ => throw new ArgumentException("Missing/unsupported client body.")
	});
	public byte[] EncodeServer(M.ServerPacket p) => Encode(p.ProtocolVersion, p.RequestId, p.Body switch
	{
		M.Welcome v => F.BodyUnion.FromWelcome(new() { EntityId = v.EntityId, WorldId = v.WorldId }),
		M.Reply v => F.BodyUnion.FromReply(new() { Status = (F.ReplyStatus)v.Status }),
		M.State v => F.BodyUnion.FromState(new() { EntityId = v.EntityId, Position = Write(v.Position), Health = v.Health, VisibleEntityIds = v.VisibleEntityIds.ToList(), Tick = v.Tick }),
		_ => throw new ArgumentException("Missing/unsupported server body.")
	});
	private static bool Malformed(Exception e) => e is ArgumentException or IndexOutOfRangeException or InvalidOperationException or OverflowException;
	public bool TryDecodeClient(ReadOnlySpan<byte> bytes, out M.ClientPacket? packet)
	{
		packet = null;
		try
		{
			var p = Decode(bytes); if (p is null) return false;
			M.ClientBody? body = p.Body.Type switch
			{
				F.Body.Move => new M.Move(Read(p.Body.AsMove().Destination)),
				F.Body.Attack => new M.Attack(p.Body.AsAttack().TargetEntityId), F.Body.Look => new M.Look(), _ => null
			};
			if (body is null) return false;
			packet = new(p.ProtocolVersion, p.RequestId, body); return true;
		}
		catch (Exception e) when (Malformed(e)) { return false; }
	}
	public bool TryDecodeServer(ReadOnlySpan<byte> bytes, out M.ServerPacket? packet)
	{
		packet = null;
		try
		{
			var p = Decode(bytes); if (p is null) return false;
			M.ServerBody? body = p.Body.Type switch
			{
				F.Body.Welcome => new M.Welcome(p.Body.AsWelcome().EntityId, p.Body.AsWelcome().WorldId),
				F.Body.Reply when p.Body.AsReply().Status <= F.ReplyStatus.Pending => new M.Reply((M.ReplyStatus)p.Body.AsReply().Status),
				F.Body.State => ReadState(p.Body.AsState()), _ => null
			};
			if (body is null) return false;
			packet = new(p.ProtocolVersion, p.RequestId, body); return true;
		}
		catch (Exception e) when (Malformed(e)) { return false; }
	}
	private static M.State ReadState(F.StateT s) => new(s.EntityId, Read(s.Position), s.Health, s.VisibleEntityIds?.ToArray() ?? [], s.Tick);
}

public static class FlatBuffersRegistration
{
	public static void RegisterFlatBuffers(this SerializerRegistry registry) => registry.Register("flatbuffers", 2, () => new FlatBuffersSerializer());
}
