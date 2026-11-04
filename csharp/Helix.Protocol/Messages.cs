namespace Helix.Protocol.Messages;

public sealed record Position(double X = 0, double Y = 0, double Z = 0);
public abstract record ClientBody;
public sealed record Move(Position? Destination) : ClientBody;
public sealed record Attack(ulong TargetEntityId) : ClientBody;
public sealed record Look : ClientBody;
public abstract record ServerBody;
public sealed record Welcome(ulong EntityId, ulong WorldId) : ServerBody;
public enum ReplyStatus : byte { Unspecified, Queued, Busy, Invalid, Pending }
public sealed record Reply(ReplyStatus Status) : ServerBody;
public sealed record State(ulong EntityId, Position? Position, int Health, ulong[] VisibleEntityIds, ulong Tick) : ServerBody;
public sealed record ClientPacket(uint ProtocolVersion, ulong RequestId, ClientBody? Body);
public sealed record ServerPacket(uint ProtocolVersion, ulong RequestId, ServerBody? Body);

public static class MessageContract
{
	public const uint Version = 1;
	private static bool Finite(Position? p) => p is not null && double.IsFinite(p.X) && double.IsFinite(p.Y) && double.IsFinite(p.Z);
	public static bool IsValid(ClientPacket p) => p.ProtocolVersion == Version && p.Body switch
	{
		Move m => Finite(m.Destination), Attack a => a.TargetEntityId != 0, Look => true, _ => false
	};
	public static bool IsValid(ServerPacket p) => p.ProtocolVersion == Version && p.Body switch
	{
		Welcome w => w.EntityId != 0 && w.WorldId != 0,
		Reply r => r.Status is >= ReplyStatus.Queued and <= ReplyStatus.Pending,
		State s => s.EntityId != 0 && Finite(s.Position) && s.VisibleEntityIds is not null,
		_ => false
	};
	public static ServerPacket Reply(ulong id, ReplyStatus status) => new(Version, id, new Reply(status));
}
