namespace Helix.Protocol;

// Register before publishing; do not mutate a registry concurrently with Create.
public sealed class SerializerRegistry
{
	private readonly Dictionary<string, (byte Id, Func<IPacketSerializer> Factory)> _factories = new(StringComparer.Ordinal);
	public SerializerRegistry() => Register("protobuf", 1, () => new ProtobufSerializer());
	public void Register(string name, byte formatId, Func<IPacketSerializer> factory)
	{
		ArgumentNullException.ThrowIfNull(factory);
		if (string.IsNullOrWhiteSpace(name) || formatId == 0 || _factories.ContainsKey(name) || _factories.Values.Any(e => e.Id == formatId))
			throw new ArgumentException("Serializer name and nonzero format ID must be unique.");
		_factories.Add(name, (formatId, factory));
	}
	public IPacketSerializer Create(string name = "protobuf")
	{
		if (!_factories.TryGetValue(name, out var entry)) throw new ArgumentException($"Unsupported serializer: {name}");
		var codec = entry.Factory();
		if (codec is null || codec.FormatId != entry.Id) throw new InvalidOperationException("Serializer factory violated its registered format ID.");
		return new WireSerializer(codec);
	}
}
