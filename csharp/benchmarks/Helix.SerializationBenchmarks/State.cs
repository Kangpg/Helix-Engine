using System.Text.Json.Serialization;
using MemoryPack;
using MessagePack;

namespace Helix.SerializationBenchmarks;

// All codecs materialize this same domain object; no view-only read is counted as a full decode.
[MemoryPackable, MessagePackObject]
public partial class State
{
	[Key(0)] public ulong EntityId { get; set; }
	[Key(1)] public double X { get; set; }
	[Key(2)] public double Y { get; set; }
	[Key(3)] public double Z { get; set; }
	[Key(4)] public int Health { get; set; }
	[Key(5)] public ulong Tick { get; set; }
	[Key(6)] public ulong[] VisibleEntityIds { get; set; } = [];
}
[JsonSerializable(typeof(State))]
internal partial class StateJsonContext : JsonSerializerContext;
