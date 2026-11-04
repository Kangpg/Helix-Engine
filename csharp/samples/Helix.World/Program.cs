using System.Collections.Immutable;
using Helix.Engine;
using Helix.Mmo;

var config = EngineConfig.Load(args.Length > 0 ? args[0] : "config/helix.toml");
var behaviors = new WorldBehaviors(
	new DelegateMovementPolicy(context => context.Request.Destination),
	new DelegateCombatRules(_ => 0),
	new DelegateVisibilityPolicy((observer, entities) => entities.Where(entity => entity.Id != observer).Select(entity => entity.Id).ToImmutableArray()));
var limits = new WorldLimits(config.WorldMaxEntities, config.WorldMaxPendingCommands, config.WorldMaxCommandsPerTick);
var field = new WorldInstance(1, limits, behaviors);
var arena = new WorldInstance(2, limits, behaviors);
field.QueueAdd(new EntityState(1, default, 100));
field.QueueAdd(new EntityState(2, default, 100));
field.Tick();
field.ReplaceCombat(new DelegateCombatRules(_ => 10));
field.QueueAttack(new AttackRequest(1, 2));
field.Tick();
Console.WriteLine($"field target HP={field.Find(2)!.Value.Health}; independent arena entities={arena.Snapshot().Entities.Length}");
