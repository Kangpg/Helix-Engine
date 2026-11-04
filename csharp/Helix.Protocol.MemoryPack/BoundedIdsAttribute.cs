using System.Buffers;
using MemoryPack;

namespace Helix.Protocol.MemoryPack;

// Bound the count by actual payload bytes before allocating or multiplying the count.
// Retains MemoryPack's unmanaged ulong[] layout while rejecting corrupt/overflowing counts.
internal sealed class BoundedIdsAttribute : MemoryPackCustomFormatterAttribute<ulong[]>
{
	public override IMemoryPackFormatter<ulong[]> GetFormatter() => new BoundedIdsFormatter();
}
internal sealed class BoundedIdsFormatter : MemoryPackFormatter<ulong[]>
{
	public override void Serialize<TBufferWriter>(ref MemoryPackWriter<TBufferWriter> writer, scoped ref ulong[]? value)
		=> writer.WriteUnmanagedArray(value);
	public override void Deserialize(ref MemoryPackReader reader, scoped ref ulong[]? value)
	{
		if (!reader.TryReadCollectionHeader(out var length)) { value = null; return; }
		if (length < 0 || length > reader.Remaining / sizeof(ulong)) throw new MemoryPackSerializationException("ID count exceeds remaining packet bytes.");
		value = new ulong[length];
		for (var i = 0; i < length; i++) reader.ReadUnmanaged(out value[i]);
	}
}
