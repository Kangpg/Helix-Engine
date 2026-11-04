using System.Collections.Immutable;

namespace Helix.Mmo;

public sealed class DelegateMovementPolicy(Func<MovementContext, Position?> function) : IMovementPolicy
{
	private readonly Func<MovementContext, Position?> _function = function ?? throw new ArgumentNullException(nameof(function));
	public Position? Resolve(MovementContext context) => _function(context);
}

public sealed class DelegateCombatRules(Func<CombatContext, int?> function) : ICombatRules
{
	private readonly Func<CombatContext, int?> _function = function ?? throw new ArgumentNullException(nameof(function));
	public int? Resolve(CombatContext context) => _function(context);
}

public sealed class DelegateVisibilityPolicy(Func<ulong, ImmutableArray<EntityState>, ImmutableArray<ulong>> function) : IVisibilityPolicy
{
	private readonly Func<ulong, ImmutableArray<EntityState>, ImmutableArray<ulong>> _function = function ?? throw new ArgumentNullException(nameof(function));
	public ImmutableArray<ulong> Select(ulong observer, ImmutableArray<EntityState> entities) => _function(observer, entities);
}
