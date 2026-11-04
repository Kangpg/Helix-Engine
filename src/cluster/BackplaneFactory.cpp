#include "BackplaneFactory.h"
#include "RedisBackplane.h"

namespace helix::cluster
{
	BackplaneKind ParseBackplaneKind(std::string_view s) noexcept
	{
		if (s == "redis") { return BackplaneKind::Redis; }
		return BackplaneKind::None;
	}

	std::unique_ptr<IBackplane> CreateBackplane(BackplaneKind kind)
	{
		switch (kind)
		{
		case BackplaneKind::Redis:
			return std::make_unique<RedisBackplane>();
		case BackplaneKind::None:
		default:
			return nullptr;
		}
	}
}
