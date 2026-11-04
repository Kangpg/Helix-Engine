using System.Collections.Immutable;

namespace Helix.Mmo;

public sealed record WorldBehaviors(IMovementPolicy Movement, ICombatRules Combat, IVisibilityPolicy Visibility);

public sealed class WorldInstance
{
	private abstract record Command;
	private sealed record Add(EntityState Entity) : Command;
	private sealed record Remove(ulong Id) : Command;
	private sealed record Move(MoveRequest Request) : Command;
	private sealed record Attack(AttackRequest Request) : Command;
	private sealed record Published(WorldSnapshot Snapshot, WorldBehaviors Behaviors);
	private readonly object _queueLock = new();
	private readonly Queue<Command> _commands = new();
	private readonly SortedDictionary<ulong, EntityState> _entities = new();
	private readonly WorldLimits _limits;
	private WorldBehaviors _active;
	private IMovementPolicy? _pendingMovement;
	private ICombatRules? _pendingCombat;
	private IVisibilityPolicy? _pendingVisibility;
	private Published _published;
	private int _ticking;
	private ulong _tick;
	public ulong Id { get; }

	public WorldInstance(ulong id, WorldLimits limits, WorldBehaviors behaviors)
	{
		if (id == 0 || limits.MaxEntities <= 0 || limits.MaxPendingCommands <= 0 || limits.MaxCommandsPerTick <= 0)
			throw new ArgumentException("World id and limits must be positive.");
		ArgumentNullException.ThrowIfNull(behaviors);
		ArgumentNullException.ThrowIfNull(behaviors.Movement);
		ArgumentNullException.ThrowIfNull(behaviors.Combat);
		ArgumentNullException.ThrowIfNull(behaviors.Visibility);
		Id = id;
		_limits = limits;
		_active = behaviors;
		_published = new Published(new WorldSnapshot(0, []), behaviors);
	}

	public bool QueueAdd(EntityState entity) => Enqueue(new Add(entity));
	public bool QueueRemove(ulong entity) => Enqueue(new Remove(entity));
	public bool QueueMove(MoveRequest request) => Enqueue(new Move(request));
	public bool QueueAttack(AttackRequest request) => Enqueue(new Attack(request));
	private bool Enqueue(Command command)
	{
		lock (_queueLock)
		{
			if (_commands.Count >= _limits.MaxPendingCommands) return false;
			_commands.Enqueue(command);
			return true;
		}
	}

	public void ReplaceMovement(IMovementPolicy policy)
	{
		ArgumentNullException.ThrowIfNull(policy);
		lock (_queueLock) _pendingMovement = policy;
	}
	public void ReplaceCombat(ICombatRules rules)
	{
		ArgumentNullException.ThrowIfNull(rules);
		lock (_queueLock) _pendingCombat = rules;
	}
	public void ReplaceVisibility(IVisibilityPolicy policy)
	{
		ArgumentNullException.ThrowIfNull(policy);
		lock (_queueLock) _pendingVisibility = policy;
	}

	// 단일 writer. Tick 도중 들어온 명령/교체는 다음 Tick에서만 반영한다.
	public TickResult Tick()
	{
		if (Interlocked.CompareExchange(ref _ticking, 1, 0) != 0)
			throw new InvalidOperationException("Tick must have a single non-reentrant owner.");
		try
		{
			var batch = new List<Command>();
			lock (_queueLock)
			{
				_active = new WorldBehaviors(_pendingMovement ?? _active.Movement, _pendingCombat ?? _active.Combat, _pendingVisibility ?? _active.Visibility);
				_pendingMovement = null; _pendingCombat = null; _pendingVisibility = null;
				while (_commands.Count > 0 && batch.Count < _limits.MaxCommandsPerTick) batch.Add(_commands.Dequeue());
			}
			var applied = 0;
			foreach (var command in batch)
			{
				try { if (Apply(command)) applied++; }
				catch (Exception) { /* 사용자 규칙 실패는 해당 명령만 거부한다. */ }
			}
			Volatile.Write(ref _published, new Published(new WorldSnapshot(++_tick, States()), _active));
			return new TickResult(batch.Count, applied, batch.Count - applied);
		}
		finally { Volatile.Write(ref _ticking, 0); }
	}

	private ImmutableArray<EntityState> States() => _entities.Values.ToImmutableArray();
	private bool Apply(Command command)
	{
		switch (command)
		{
			case Add add:
				return add.Entity.Id != 0 && add.Entity.Health > 0 && add.Entity.Position.IsFinite &&
					_entities.Count < _limits.MaxEntities && _entities.TryAdd(add.Entity.Id, add.Entity);
			case Remove remove:
				return _entities.Remove(remove.Id);
			case Move move:
				if (!_entities.TryGetValue(move.Request.Entity, out var entity) || entity.Health <= 0 || !move.Request.Destination.IsFinite) return false;
				var position = _active.Movement.Resolve(new MovementContext(entity, move.Request, States()));
				if (position is not { IsFinite: true } next) return false;
				_entities[entity.Id] = entity with { Position = next };
				return true;
			case Attack attack:
				if (!_entities.TryGetValue(attack.Request.Attacker, out var attacker) || !_entities.TryGetValue(attack.Request.Target, out var target) ||
					attacker.Health <= 0 || target.Health <= 0) return false;
				var damage = _active.Combat.Resolve(new CombatContext(attacker, target, attack.Request, States()));
				if (damage is not >= 0) return false;
				_entities[target.Id] = target with { Health = target.Health - Math.Min(target.Health, damage.Value) };
				return true;
			default: return false;
		}
	}

	public WorldSnapshot Snapshot() => Volatile.Read(ref _published).Snapshot;
	public EntityState? Find(ulong id)
	{
		foreach (var entity in Snapshot().Entities) if (entity.Id == id) return entity;
		return null;
	}
	public ImmutableArray<ulong> VisibleTo(ulong observer)
	{
		var view = Volatile.Read(ref _published);
		if (!view.Snapshot.Entities.Any(entity => entity.Id == observer)) return [];
		var selected = view.Behaviors.Visibility.Select(observer, view.Snapshot.Entities);
		if (selected.IsDefault) return [];
		return selected.Where(id => id != observer && view.Snapshot.Entities.Any(entity => entity.Id == id)).Distinct().Order().ToImmutableArray();
	}
}
