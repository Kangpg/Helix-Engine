namespace Helix.Mmo;

// null = 요청 거부. 실제 상태 변경은 WorldInstance가 수행한다.
public interface IMovementPolicy
{
	Position? Resolve(MovementContext context);
}
