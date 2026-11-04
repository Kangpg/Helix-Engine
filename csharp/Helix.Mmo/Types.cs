using System.Collections.Immutable;

namespace Helix.Mmo;

public readonly record struct Position(double X, double Y, double Z)
{
	public bool IsFinite => double.IsFinite(X) && double.IsFinite(Y) && double.IsFinite(Z);
}
public readonly record struct EntityState(ulong Id, Position Position, int Health);
public readonly record struct MoveRequest(ulong Entity, Position Destination);
public readonly record struct AttackRequest(ulong Attacker, ulong Target);
public readonly record struct MovementContext(EntityState Entity, MoveRequest Request, ImmutableArray<EntityState> Entities);
public readonly record struct CombatContext(EntityState Attacker, EntityState Target, AttackRequest Request, ImmutableArray<EntityState> Entities);
public readonly record struct WorldLimits(int MaxEntities, int MaxPendingCommands, int MaxCommandsPerTick);
public readonly record struct TickResult(int Processed, int Applied, int Rejected);
public sealed record WorldSnapshot(ulong Tick, ImmutableArray<EntityState> Entities);
