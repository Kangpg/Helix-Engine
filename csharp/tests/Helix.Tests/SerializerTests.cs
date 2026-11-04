using Helix.Protocol;
using Helix.Protocol.FlatBuffers;
using Helix.Protocol.MemoryPack;
using Google.Protobuf;
using Google.FlatBuffers;
using MemoryPack;
using System.Buffers.Binary;
using M = Helix.Protocol.Messages;

internal static class SerializerTests
{
	internal static SerializerRegistry Registry()
	{
		var r = new SerializerRegistry(); r.RegisterFlatBuffers(); r.RegisterMemoryPack(); return r;
	}
	private static void Check(bool ok, string message) { if (!ok) throw new Exception(message); }
	internal static M.ClientPacket[] Clients() => [new(1, ulong.MaxValue, new M.Move(new(12.25, -34.5, 100.75))), new(1, 123, new M.Attack(ulong.MaxValue)), new(1, 999, new M.Look())];
	internal static M.ServerPacket[] Servers()
	{
		var values = new List<M.ServerPacket> { new(1, 0, new M.Welcome(ulong.MaxValue, 2)) };
		for (byte status = 1; status <= 4; status++) values.Add(M.MessageContract.Reply(ulong.MaxValue, (M.ReplyStatus)status));
		foreach (var count in new[] { 0, 16, 256, 4096 })
			values.Add(new(1, ulong.MaxValue, new M.State(ulong.MaxValue, new(12.25, -34.5, 100.75), int.MinValue, Enumerable.Range(0, count).Select(i => ulong.MaxValue - (ulong)i).ToArray(), ulong.MaxValue)));
		return values.ToArray();
	}
	internal static void Fixtures(string operation, string directory, string format)
	{
		var codec = Registry().Create(format);
		Directory.CreateDirectory(directory);
		var index = 0;
		foreach (var expected in Clients())
		{
			var file = Path.Combine(directory, $"client-{index++}.bin"); var bytes = codec.EncodeClient(expected);
			if (operation == "--write") File.WriteAllBytes(file, bytes);
			else
			{
				Check(codec.TryDecodeClient(File.ReadAllBytes(file), out var decoded) && decoded is not null && M.MessageContract.IsValid(decoded), "foreign client semantic decode");
				Check(codec.EncodeClient(decoded!).SequenceEqual(bytes), "foreign client all fields");
			}
		}
		index = 0;
		foreach (var expected in Servers())
		{
			var file = Path.Combine(directory, $"server-{index++}.bin"); var bytes = codec.EncodeServer(expected);
			if (operation == "--write") File.WriteAllBytes(file, bytes);
			else
			{
				Check(codec.TryDecodeServer(File.ReadAllBytes(file), out var decoded) && decoded is not null && M.MessageContract.IsValid(decoded), "foreign server semantic decode");
				Check(codec.EncodeServer(decoded!).SequenceEqual(bytes), "foreign server all fields");
			}
		}
		if (format == "flatbuffers")
		{
			var file = Path.Combine(directory, "future-client.bin");
			if (operation == "--write") File.WriteAllBytes(file, FutureFlatBuffersClient());
			else if (File.Exists(file)) Check(codec.TryDecodeClient(File.ReadAllBytes(file), out var future) && future == Clients()[2], "future FlatBuffers field");
		}
		Console.WriteLine($"PASS: cross-language {format} {operation}, all DTO fields/uint64/int32/AOI sizes");
	}
	internal static void Run()
	{
		var registry = Registry();
		foreach (var name in new[] { "protobuf", "flatbuffers", "memorypack" })
		{
			var codec = registry.Create(name);
			foreach (var p in Clients())
			{
				var bytes = codec.EncodeClient(p);
				Check(codec.TryDecodeClient(bytes, out var decoded) && decoded is not null && M.MessageContract.IsValid(decoded), "client round-trip");
				Check(codec.EncodeClient(decoded!).SequenceEqual(bytes), "all client fields");
				if (codec.FormatId != 1)
				{
					Check(!codec.TryDecodeServer(bytes, out _), "direction mismatch");
					foreach (var field in new[] { 4, 5, 6, 7 }) { var bad = bytes.ToArray(); bad[field] = 255; Check(!codec.TryDecodeClient(bad, out _), "envelope mismatch"); }
					foreach (var other in new[] { "protobuf", "flatbuffers", "memorypack" }.Where(n => n != name)) Check(!registry.Create(other).TryDecodeClient(bytes, out _), "wrong codec");
				}
				Check(!codec.TryDecodeClient(bytes.AsSpan(0, 3), out _), "truncation");
			}
			foreach (var p in Servers())
			{
				var bytes = codec.EncodeServer(p);
				Check(codec.TryDecodeServer(bytes, out var decoded) && decoded is not null && M.MessageContract.IsValid(decoded), "server round-trip");
				Check(codec.EncodeServer(decoded!).SequenceEqual(bytes), "all server fields");
			}
			foreach (var p in new M.ClientPacket[] { new(2, 45, new M.Look()), new(1, 45, new M.Move(null)), new(1, 45, new M.Move(new(double.NaN))), new(1, 45, new M.Attack(0)) })
			{
				Check(codec.TryDecodeClient(codec.EncodeClient(p), out var decoded) && decoded is not null && decoded.RequestId == 45 && !M.MessageContract.IsValid(decoded), "shared semantic validation");
			}
			if (codec.FormatId != 1)
			{
				var random = new Random(123);
				for (var i = 0; i < 1000; i++)
				{
					var bad = new byte[random.Next(8, 256)]; random.NextBytes(bad);
					"HXS1"u8.CopyTo(bad); bad[4] = codec.FormatId; bad[5] = 1; bad[6] = 1; bad[7] = 0;
					codec.TryDecodeClient(bad, out _); bad[6] = 2; codec.TryDecodeServer(bad, out _);
				}
			}
		}
		var legacy = new Helix.Protocol.Wire.ClientPacket { ProtocolVersion = 1, RequestId = 45 };
		Check(registry.Create().TryDecodeClient(legacy.ToByteArray(), out var missing) && missing?.RequestId == 45 && !M.MessageContract.IsValid(missing), "legacy missing-body correlation");
		var expectedLegacy = new Helix.Protocol.Wire.ClientPacket { ProtocolVersion = 1, RequestId = 999, Look = new() };
		Check(registry.Create().EncodeClient(Clients()[2]).SequenceEqual(expectedLegacy.ToByteArray()), "default legacy wire unchanged");
		var duplicate = false; try { registry.Register("another", 1, () => new ProtobufSerializer()); } catch (ArgumentException) { duplicate = true; }
		Check(duplicate, "duplicate ID");
		var unknown = false; try { registry.Create("unknown"); } catch (ArgumentException) { unknown = true; }
		Check(unknown, "unsupported serializer");
		registry.Register("custom", 42, () => new CustomSerializer());
		var custom = registry.Create("custom");
		Check(custom.TryDecodeClient(custom.EncodeClient(Clients()[2]), out var extension) && extension == Clients()[2], "new implementation without registry/core changes");
		Check(registry.Create("flatbuffers").TryDecodeClient(FutureFlatBuffersClient(), out var futureFlat) && futureFlat == Clients()[2], "FlatBuffers ignores appended field");
		var memoryCodec = registry.Create("memorypack");
		var normal = memoryCodec.EncodeClient(Clients()[2]);
		Check(memoryCodec.TryDecodeClient(FutureMemoryPackPacket(normal), out var futureMemory) && futureMemory == Clients()[2], "MemoryPack version-tolerant appended field");
		var stateBytes = memoryCodec.EncodeServer(Servers()[6]);
		foreach (var length in new[] { int.MaxValue, -1, -2, 4096 })
		{
			var bad = stateBytes.ToArray(); var idsOffset = MemoryMemberOffset(bad, 12);
			BinaryPrimitives.WriteInt32LittleEndian(bad.AsSpan(idsOffset, 4), length);
			Check(!memoryCodec.TryDecodeServer(bad, out _), "bounded MemoryPack array count");
		}
		Console.WriteLine("PASS: Protobuf/FlatBuffers/MemoryPack messages, shared validation, envelope/mismatch, malformed-input corpus, legacy wire");
	}
	private static byte[] FutureFlatBuffersClient()
	{
		var b = new FlatBufferBuilder(128); Helix.Protocol.FlatWire.Look.StartLook(b); var look = Helix.Protocol.FlatWire.Look.EndLook(b);
		b.StartTable(5); b.AddUlong(4, 42, 0); Helix.Protocol.FlatWire.Packet.AddBody(b, look.Value);
		Helix.Protocol.FlatWire.Packet.AddBodyType(b, Helix.Protocol.FlatWire.Body.Look);
		Helix.Protocol.FlatWire.Packet.AddRequestId(b, 999); Helix.Protocol.FlatWire.Packet.AddProtocolVersion(b, 1);
		var packet = new Offset<Helix.Protocol.FlatWire.Packet>(b.EndTable()); Helix.Protocol.FlatWire.Packet.FinishPacketBuffer(b, packet);
		var payload = b.SizedByteArray(); byte[] header = [72, 88, 83, 49, 2, 1, 1, 0]; return header.Concat(payload).ToArray();
	}
	private static int MemoryMemberOffset(byte[] bytes, int member)
	{
		using var state = MemoryPackReaderOptionalStatePool.Rent(null); var reader = new MemoryPackReader(bytes.AsSpan(8), state);
		try
		{
			Check(reader.TryReadObjectHeader(out var count), "memory header"); var offset = 0;
			for (var i = 0; i < count; i++) { var length = reader.ReadVarIntInt32(); if (i < member) offset += length; }
			return 8 + reader.Consumed + offset;
		}
		finally { reader.Dispose(); }
	}
	private static byte[] FutureMemoryPackPacket(byte[] bytes)
	{
		var bodyOffset = MemoryMemberOffset(bytes, 0); var result = new byte[bytes.Length + 9];
		bytes.AsSpan(0, bodyOffset).CopyTo(result); result[8]++; result[bodyOffset] = 8;
		bytes.AsSpan(bodyOffset).CopyTo(result.AsSpan(bodyOffset + 1));
		BinaryPrimitives.WriteUInt64LittleEndian(result.AsSpan(result.Length - 8), 42); return result;
	}
	private sealed class CustomSerializer : IPacketSerializer
	{
		private readonly ProtobufSerializer _inner = new();
		public byte FormatId => 42;
		public byte[] EncodeClient(M.ClientPacket p) => _inner.EncodeClient(p);
		public byte[] EncodeServer(M.ServerPacket p) => _inner.EncodeServer(p);
		public bool TryDecodeClient(ReadOnlySpan<byte> b, out M.ClientPacket? p) => _inner.TryDecodeClient(b, out p);
		public bool TryDecodeServer(ReadOnlySpan<byte> b, out M.ServerPacket? p) => _inner.TryDecodeServer(b, out p);
	}
}
