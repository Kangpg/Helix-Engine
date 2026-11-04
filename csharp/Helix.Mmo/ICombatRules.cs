namespace Helix.Mmo;

// null = 거부. 결과는 비음수 피해량이며 HP는 월드가 변경한다.
public interface ICombatRules
{
	int? Resolve(CombatContext context);
}
