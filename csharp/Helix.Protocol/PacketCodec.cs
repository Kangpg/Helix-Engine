using Google.Protobuf;
using Helix.Protocol.Wire;

namespace Helix.Protocol;

public static class PacketCodec
{
	public const uint ProtocolVersion = Messages.MessageContract.Version;
	public static bool TryDecodeClient(ReadOnlySpan<byte> bytes, out ClientPacket? packet)
	{
		try { packet = ClientPacket.Parser.ParseFrom(bytes); return true; }
		catch (InvalidProtocolBufferException) { packet = null; return false; }
	}
	public static bool IsValid(ClientPacket packet)
	{
		if (packet.ProtocolVersion != ProtocolVersion) return false;
		return packet.BodyCase switch
		{
			ClientPacket.BodyOneofCase.Move => packet.Move.Destination is { } p && double.IsFinite(p.X) && double.IsFinite(p.Y) && double.IsFinite(p.Z),
			ClientPacket.BodyOneofCase.Attack => packet.Attack.TargetEntityId != 0,
			ClientPacket.BodyOneofCase.Look => true,
			_ => false
		};
	}
	public static byte[] Encode(ServerPacket packet) => packet.ToByteArray();
	public static ServerPacket Reply(ulong requestId, ReplyStatus status) => new()
	{
		ProtocolVersion = ProtocolVersion, RequestId = requestId, Reply = new CommandReply { Status = status }
	};
}
