using Helix.Protocol.Messages;

namespace Helix.Protocol;

// Thread-safe codecs create owned bytes/DTOs. Decode parses; MessageContract validates semantics.
public interface IClientPacketSerializer
{
	byte[] EncodeClient(ClientPacket packet);
	bool TryDecodeServer(ReadOnlySpan<byte> bytes, out ServerPacket? packet);
}
public interface IServerPacketSerializer
{
	bool TryDecodeClient(ReadOnlySpan<byte> bytes, out ClientPacket? packet);
	byte[] EncodeServer(ServerPacket packet);
}
public interface IPacketSerializer : IClientPacketSerializer, IServerPacketSerializer
{
	byte FormatId { get; }
}
