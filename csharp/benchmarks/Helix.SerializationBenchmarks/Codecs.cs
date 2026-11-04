using System.Buffers.Binary;
using System.Text.Json;
using System.Runtime.InteropServices;
using Google.Protobuf;
using Google.FlatBuffers;
using MemoryPack;
using MessagePack;
using Proto = Helix.Protocol.Wire;
using FlatState = Helix.SerializationBenchmarks.Flat.WorldState;

namespace Helix.SerializationBenchmarks;

public interface IStateCodec
{
	string Name { get; }
	byte[] Encode(State state);
	State Decode(byte[] bytes);
}
public enum CodecKind { Protobuf, MessagePack, MemoryPack, Json, FlatBuffers, Binary }
public static class CodecFactory
{
	public static IStateCodec Create(CodecKind kind) => kind switch
	{
		CodecKind.Protobuf => new ProtobufCodec(), CodecKind.MessagePack => new MessagePackCodec(),
		CodecKind.MemoryPack => new MemoryPackCodec(), CodecKind.Json => new JsonCodec(),
		CodecKind.FlatBuffers => new FlatBuffersCodec(), CodecKind.Binary => new BinaryCodec(),
		_ => throw new ArgumentOutOfRangeException(nameof(kind))
	};
}
sealed class ProtobufCodec : IStateCodec
{
	public string Name => "Protobuf";
	public byte[] Encode(State state)
	{
		var packet = new Proto.WorldState { EntityId = state.EntityId, Health = state.Health, Tick = state.Tick,
			Position = new Proto.Position { X = state.X, Y = state.Y, Z = state.Z } };
		packet.VisibleEntityIds.AddRange(state.VisibleEntityIds);
		return packet.ToByteArray();
	}
	public State Decode(byte[] bytes)
	{
		var packet = Proto.WorldState.Parser.ParseFrom(bytes);
		return new State { EntityId = packet.EntityId, X = packet.Position.X, Y = packet.Position.Y, Z = packet.Position.Z,
			Health = packet.Health, Tick = packet.Tick, VisibleEntityIds = packet.VisibleEntityIds.ToArray() };
	}
}
sealed class MessagePackCodec : IStateCodec
{
	public string Name => "MessagePack";
	private static readonly MessagePackSerializerOptions Options = MessagePackSerializerOptions.Standard.WithSecurity(MessagePackSecurity.UntrustedData);
	public byte[] Encode(State state) => MessagePackSerializer.Serialize(state, Options);
	public State Decode(byte[] bytes) => MessagePackSerializer.Deserialize<State>(bytes, Options);
}
sealed class MemoryPackCodec : IStateCodec
{
	public string Name => "MemoryPack";
	public byte[] Encode(State state) => MemoryPackSerializer.Serialize(state);
	public State Decode(byte[] bytes) => MemoryPackSerializer.Deserialize<State>(bytes)!;
}
sealed class JsonCodec : IStateCodec
{
	public string Name => "JSON";
	public byte[] Encode(State state) => JsonSerializer.SerializeToUtf8Bytes(state, StateJsonContext.Default.State);
	public State Decode(byte[] bytes) => JsonSerializer.Deserialize(bytes, StateJsonContext.Default.State)!;
}
sealed class FlatBuffersCodec : IStateCodec
{
	public string Name => "FlatBuffers";
	public byte[] Encode(State state)
	{
		var builder = new FlatBufferBuilder(128 + state.VisibleEntityIds.Length * 8);
		var ids = FlatState.CreateVisibleEntityIdsVectorBlock(builder, state.VisibleEntityIds);
		var packet = FlatState.CreateWorldState(builder, state.EntityId, state.X, state.Y, state.Z, state.Health, state.Tick, ids);
		FlatState.FinishWorldStateBuffer(builder, packet);
		return builder.SizedByteArray();
	}
	public State Decode(byte[] bytes)
	{
		var packet = FlatState.GetRootAsWorldState(new ByteBuffer(bytes));
		var ids = packet.GetVisibleEntityIdsArray();
		return new State { EntityId = packet.EntityId, X = packet.X, Y = packet.Y, Z = packet.Z,
			Health = packet.Health, Tick = packet.Tick, VisibleEntityIds = ids };
	}
}
// Fixed, versionless LE layout for a lower-bound comparison; not a production protocol replacement.
sealed class BinaryCodec : IStateCodec
{
	public string Name => "Binary";
	public byte[] Encode(State state)
	{
		var bytes = new byte[48 + state.VisibleEntityIds.Length * 8];
		var span = bytes.AsSpan();
		BinaryPrimitives.WriteUInt64LittleEndian(span, state.EntityId);
		BinaryPrimitives.WriteDoubleLittleEndian(span[8..], state.X);
		BinaryPrimitives.WriteDoubleLittleEndian(span[16..], state.Y);
		BinaryPrimitives.WriteDoubleLittleEndian(span[24..], state.Z);
		BinaryPrimitives.WriteInt32LittleEndian(span[32..], state.Health);
		BinaryPrimitives.WriteUInt64LittleEndian(span[36..], state.Tick);
		BinaryPrimitives.WriteInt32LittleEndian(span[44..], state.VisibleEntityIds.Length);
		if (BitConverter.IsLittleEndian) MemoryMarshal.AsBytes(state.VisibleEntityIds.AsSpan()).CopyTo(span[48..]);
		else for (var i = 0; i < state.VisibleEntityIds.Length; i++) BinaryPrimitives.WriteUInt64LittleEndian(span[(48 + i * 8)..], state.VisibleEntityIds[i]);
		return bytes;
	}
	public State Decode(byte[] bytes)
	{
		var span = bytes.AsSpan();
		if (span.Length < 48) throw new FormatException("Truncated state");
		var count = BinaryPrimitives.ReadInt32LittleEndian(span[44..]);
		if (count < 0 || count > (span.Length - 48) / 8 || span.Length != 48L + count * 8L) throw new FormatException("Invalid array size");
		var ids = BitConverter.IsLittleEndian ? MemoryMarshal.Cast<byte, ulong>(span[48..]).ToArray() : new ulong[count];
		if (!BitConverter.IsLittleEndian) for (var i = 0; i < count; i++) ids[i] = BinaryPrimitives.ReadUInt64LittleEndian(span[(48 + i * 8)..]);
		return new State { EntityId = BinaryPrimitives.ReadUInt64LittleEndian(span),
			X = BinaryPrimitives.ReadDoubleLittleEndian(span[8..]), Y = BinaryPrimitives.ReadDoubleLittleEndian(span[16..]),
			Z = BinaryPrimitives.ReadDoubleLittleEndian(span[24..]), Health = BinaryPrimitives.ReadInt32LittleEndian(span[32..]),
			Tick = BinaryPrimitives.ReadUInt64LittleEndian(span[36..]), VisibleEntityIds = ids };
	}
}
