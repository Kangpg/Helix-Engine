using MemoryPack;

namespace Helix.Protocol.MemoryPack;

// The order IDs are permanent: append fields, never reuse IDs or change member types.
// Explicit primitive fields avoid CLR struct padding/endian assumptions in the message contract.
[MemoryPackable(GenerateType.VersionTolerant)]
internal sealed partial class MemoryPacket
{
	[MemoryPackOrder(0)] public uint ProtocolVersion { get; set; }
	[MemoryPackOrder(1)] public ulong RequestId { get; set; }
	[MemoryPackOrder(2)] public byte Kind { get; set; }
	[MemoryPackOrder(3)] public bool HasPosition { get; set; }
	[MemoryPackOrder(4)] public double X { get; set; }
	[MemoryPackOrder(5)] public double Y { get; set; }
	[MemoryPackOrder(6)] public double Z { get; set; }
	[MemoryPackOrder(7)] public ulong TargetEntityId { get; set; }
	[MemoryPackOrder(8)] public ulong EntityId { get; set; }
	[MemoryPackOrder(9)] public ulong WorldId { get; set; }
	[MemoryPackOrder(10)] public byte Status { get; set; }
	[MemoryPackOrder(11)] public int Health { get; set; }
	[MemoryPackOrder(12), BoundedIds] public ulong[] VisibleEntityIds { get; set; } = [];
	[MemoryPackOrder(13)] public ulong Tick { get; set; }
}
