using MemoryPack;
using System.Buffers.Binary;
using M = Helix.Protocol.Messages;
using Serializer = MemoryPack.MemoryPackSerializer;

namespace Helix.Protocol.MemoryPack;

public sealed class MemoryPackSerializer : IPacketSerializer
{
	public MemoryPackSerializer()
	{
		if (!BitConverter.IsLittleEndian) throw new PlatformNotSupportedException("Helix MemoryPack wire requires little-endian peers.");
	}
	public byte FormatId => 3;
	private static void Position(MemoryPacket w, M.Position? p)
	{
		w.HasPosition = p is not null;
		if (p is not null) { w.X = p.X; w.Y = p.Y; w.Z = p.Z; }
	}
	private static M.Position? Position(MemoryPacket w) => w.HasPosition ? new(w.X, w.Y, w.Z) : null;
	public byte[] EncodeClient(M.ClientPacket p)
	{
		var w = new MemoryPacket { ProtocolVersion = p.ProtocolVersion, RequestId = p.RequestId };
		switch (p.Body)
		{
			case M.Move m: w.Kind = 1; Position(w, m.Destination); break;
			case M.Attack a: w.Kind = 2; w.TargetEntityId = a.TargetEntityId; break;
			case M.Look: w.Kind = 3; break;
			default: throw new ArgumentException("Missing/unsupported client body.");
		}
		return Serializer.Serialize(w);
	}
	public byte[] EncodeServer(M.ServerPacket p)
	{
		var w = new MemoryPacket { ProtocolVersion = p.ProtocolVersion, RequestId = p.RequestId };
		switch (p.Body)
		{
			case M.Welcome v: w.Kind = 4; w.EntityId = v.EntityId; w.WorldId = v.WorldId; break;
			case M.Reply v: w.Kind = 5; w.Status = (byte)v.Status; break;
			case M.State v: w.Kind = 6; w.EntityId = v.EntityId; Position(w, v.Position); w.Health = v.Health; w.VisibleEntityIds = v.VisibleEntityIds; w.Tick = v.Tick; break;
			default: throw new ArgumentException("Missing/unsupported server body.");
		}
		return Serializer.Serialize(w);
	}
	private static MemoryPacket? Decode(ReadOnlySpan<byte> bytes)
	{
		if (!VerifyLayout(bytes)) return null;
		MemoryPacket? packet = null;
		var consumed = Serializer.Deserialize(bytes, ref packet);
		return consumed == bytes.Length && packet?.VisibleEntityIds is not null ? packet : null;
	}
	private static bool VerifyLayout(ReadOnlySpan<byte> bytes)
	{
		using var state = MemoryPackReaderOptionalStatePool.Rent(null);
		var reader = new MemoryPackReader(bytes, state);
		try
		{
			if (!reader.TryReadObjectHeader(out var count)) return false;
			Span<int> lengths = stackalloc int[count];
			ReadOnlySpan<int> widths = [4, 8, 1, 1, 8, 8, 8, 8, 8, 8, 1, 4, -1, 8];
			long total = 0;
			for (var i = 0; i < count; i++)
			{
				var length = reader.ReadVarIntInt32();
				if (length < 0 || (i < widths.Length && widths[i] > 0 && length != 0 && length != widths[i])) return false;
				lengths[i] = length; total += length;
			}
			if (total != reader.Remaining) return false;
			var offset = reader.Consumed;
			for (var i = 0; i < count; i++)
			{
				var length = lengths[i];
				if (i == 3 && length != 0 && bytes[offset] > 1) return false;
				if (i == 12 && length != 0)
				{
					if (length < 4) return false;
					var ids = BinaryPrimitives.ReadInt32LittleEndian(bytes.Slice(offset, 4));
					if (ids < 0 || 4L + 8L * ids != length) return false;
				}
				offset += length;
			}
			return true;
		}
		finally { reader.Dispose(); }
	}
	public bool TryDecodeClient(ReadOnlySpan<byte> bytes, out M.ClientPacket? packet)
	{
		packet = null;
		try
		{
			var w = Decode(bytes); if (w is null) return false;
			M.ClientBody? body = w.Kind switch { 1 => new M.Move(Position(w)), 2 => new M.Attack(w.TargetEntityId), 3 => new M.Look(), _ => null };
			if (body is null) return false;
			packet = new(w.ProtocolVersion, w.RequestId, body); return true;
		}
		catch (Exception e) when (e is MemoryPackSerializationException or OverflowException or ArgumentException) { return false; }
	}
	public bool TryDecodeServer(ReadOnlySpan<byte> bytes, out M.ServerPacket? packet)
	{
		packet = null;
		try
		{
			var w = Decode(bytes); if (w is null) return false;
			M.ServerBody? body = w.Kind switch
			{
				4 => new M.Welcome(w.EntityId, w.WorldId), 5 when w.Status <= 4 => new M.Reply((M.ReplyStatus)w.Status),
				6 => new M.State(w.EntityId, Position(w), w.Health, w.VisibleEntityIds, w.Tick), _ => null
			};
			if (body is null) return false;
			packet = new(w.ProtocolVersion, w.RequestId, body); return true;
		}
		catch (Exception e) when (e is MemoryPackSerializationException or OverflowException or ArgumentException) { return false; }
	}
}

public static class MemoryPackRegistration
{
	public static void RegisterMemoryPack(this SerializerRegistry registry) => registry.Register("memorypack", 3, () => new MemoryPackSerializer());
}
