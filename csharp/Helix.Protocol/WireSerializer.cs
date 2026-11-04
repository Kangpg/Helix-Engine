using Helix.Protocol.Messages;

namespace Helix.Protocol;

public sealed class WireSerializer : IPacketSerializer
{
	private readonly IPacketSerializer _serializer;
	public byte FormatId => _serializer.FormatId;
	public WireSerializer(IPacketSerializer serializer)
	{
		ArgumentNullException.ThrowIfNull(serializer);
		if (serializer.FormatId == 0) throw new ArgumentException("Format ID zero is reserved.");
		_serializer = serializer;
	}
	private byte[] Wrap(byte[] payload, byte direction)
	{
		if (FormatId == 1) return payload;
		var bytes = new byte[checked(payload.Length + 8)];
		"HXS1"u8.CopyTo(bytes); bytes[4] = FormatId; bytes[5] = 1; bytes[6] = direction;
		payload.CopyTo(bytes, 8); return bytes;
	}
	private bool Unwrap(ref ReadOnlySpan<byte> bytes, byte direction)
	{
		if (FormatId == 1) return !bytes.StartsWith("HXS1"u8);
		if (bytes.Length < 8 || !bytes[..4].SequenceEqual("HXS1"u8) || bytes[4] != FormatId || bytes[5] != 1 || bytes[6] != direction || bytes[7] != 0) return false;
		bytes = bytes[8..]; return true;
	}
	public byte[] EncodeClient(ClientPacket packet) => Wrap(_serializer.EncodeClient(packet), 1);
	public byte[] EncodeServer(ServerPacket packet) => Wrap(_serializer.EncodeServer(packet), 2);
	public bool TryDecodeClient(ReadOnlySpan<byte> bytes, out ClientPacket? packet)
	{
		packet = null; return Unwrap(ref bytes, 1) && _serializer.TryDecodeClient(bytes, out packet);
	}
	public bool TryDecodeServer(ReadOnlySpan<byte> bytes, out ServerPacket? packet)
	{
		packet = null; return Unwrap(ref bytes, 2) && _serializer.TryDecodeServer(bytes, out packet);
	}
}
