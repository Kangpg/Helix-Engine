using System.Collections.Immutable;

namespace Helix.Mmo;

// 불변 스냅샷에서 조회. 동시에 호출될 수 있으므로 구현/캡처는 thread-safe해야 한다.
public interface IVisibilityPolicy
{
	ImmutableArray<ulong> Select(ulong observer, ImmutableArray<EntityState> entities);
}
