using System.Buffers.Binary;

namespace Helix.Engine.Protocol;

internal sealed class FrameCodec(EngineConfig config)
{
	public byte[] Encode(ReadOnlySpan<byte> message)
	{
		if (message.Length > config.MaxMessageSize) throw new InvalidDataException("Message exceeds frame limit.");
		var bytes = new byte[config.HeaderSize + message.Length];
		if (config.HeaderSize == 2)
		{
			if (config.BigEndian) BinaryPrimitives.WriteUInt16BigEndian(bytes, (ushort)message.Length);
			else BinaryPrimitives.WriteUInt16LittleEndian(bytes, (ushort)message.Length);
		}
		else if (config.BigEndian) BinaryPrimitives.WriteUInt32BigEndian(bytes, (uint)message.Length);
		else BinaryPrimitives.WriteUInt32LittleEndian(bytes, (uint)message.Length);
		message.CopyTo(bytes.AsSpan(config.HeaderSize));
		return bytes;
	}

	public async ValueTask<byte[]?> ReadAsync(Stream stream, CancellationToken cancellation)
	{
		var header = new byte[config.HeaderSize];
		var first = await stream.ReadAsync(header.AsMemory(0, 1), cancellation).ConfigureAwait(false);
		if (first == 0) return null;
		await stream.ReadExactlyAsync(header.AsMemory(1), cancellation).ConfigureAwait(false);
		var length = DecodeLength(header);
		if (length > config.MaxMessageSize) throw new InvalidDataException("Incoming frame exceeds limit.");
		var payload = new byte[(int)length];
		await stream.ReadExactlyAsync(payload, cancellation).ConfigureAwait(false);
		return payload;
	}

	private uint DecodeLength(byte[] header) => config.HeaderSize == 2
		? (config.BigEndian ? BinaryPrimitives.ReadUInt16BigEndian(header) : BinaryPrimitives.ReadUInt16LittleEndian(header))
		: (config.BigEndian ? BinaryPrimitives.ReadUInt32BigEndian(header) : BinaryPrimitives.ReadUInt32LittleEndian(header));
}
