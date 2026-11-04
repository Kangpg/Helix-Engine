using Google.Protobuf;
using M = Helix.Protocol.Messages;
using W = Helix.Protocol.Wire;

namespace Helix.Protocol;

public sealed class ProtobufSerializer : IPacketSerializer
{
	public byte FormatId => 1;
	private static W.Position? Write(M.Position? p) => p is null ? null : new() { X = p.X, Y = p.Y, Z = p.Z };
	private static M.Position? Read(W.Position? p) => p is null ? null : new(p.X, p.Y, p.Z);
	public byte[] EncodeClient(M.ClientPacket p)
	{
		var w = new W.ClientPacket { ProtocolVersion = p.ProtocolVersion, RequestId = p.RequestId };
		switch (p.Body)
		{
			case M.Move m: w.Move = new W.MoveCommand { Destination = Write(m.Destination) }; break;
			case M.Attack a: w.Attack = new W.AttackCommand { TargetEntityId = a.TargetEntityId }; break;
			case M.Look: w.Look = new W.LookCommand(); break;
			default: throw new ArgumentException("Missing/unsupported client body.");
		}
		return w.ToByteArray();
	}
	public bool TryDecodeClient(ReadOnlySpan<byte> bytes, out M.ClientPacket? p)
	{
		p = null;
		try
		{
			var w = W.ClientPacket.Parser.ParseFrom(bytes);
			M.ClientBody? body = w.BodyCase switch
			{
				W.ClientPacket.BodyOneofCase.Move => new M.Move(Read(w.Move.Destination)),
				W.ClientPacket.BodyOneofCase.Attack => new M.Attack(w.Attack.TargetEntityId),
				W.ClientPacket.BodyOneofCase.Look => new M.Look(), _ => null
			};
			p = new(w.ProtocolVersion, w.RequestId, body); return true;
		}
		catch (InvalidProtocolBufferException) { return false; }
	}
	public byte[] EncodeServer(M.ServerPacket p)
	{
		var w = new W.ServerPacket { ProtocolVersion = p.ProtocolVersion, RequestId = p.RequestId };
		switch (p.Body)
		{
			case M.Welcome v: w.Welcome = new W.Welcome { EntityId = v.EntityId, WorldId = v.WorldId }; break;
			case M.Reply v: w.Reply = new W.CommandReply { Status = (W.ReplyStatus)v.Status }; break;
			case M.State v:
				w.State = new W.WorldState { EntityId = v.EntityId, Position = Write(v.Position), Health = v.Health, Tick = v.Tick };
				w.State.VisibleEntityIds.AddRange(v.VisibleEntityIds); break;
			default: throw new ArgumentException("Missing/unsupported server body.");
		}
		return w.ToByteArray();
	}
	public bool TryDecodeServer(ReadOnlySpan<byte> bytes, out M.ServerPacket? p)
	{
		p = null;
		try
		{
			var w = W.ServerPacket.Parser.ParseFrom(bytes);
			M.ServerBody? body = w.BodyCase switch
			{
				W.ServerPacket.BodyOneofCase.Welcome => new M.Welcome(w.Welcome.EntityId, w.Welcome.WorldId),
				W.ServerPacket.BodyOneofCase.Reply when (int)w.Reply.Status is >= 0 and <= 4 => new M.Reply((M.ReplyStatus)w.Reply.Status),
				W.ServerPacket.BodyOneofCase.State => new M.State(w.State.EntityId, Read(w.State.Position), w.State.Health, w.State.VisibleEntityIds.ToArray(), w.State.Tick),
				_ => null
			};
			if (body is null) return false;
			p = new(w.ProtocolVersion, w.RequestId, body); return true;
		}
		catch (InvalidProtocolBufferException) { return false; }
	}
}
