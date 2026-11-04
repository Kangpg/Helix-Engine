#pragma once
#include "helix/mmo/types.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace helix::mmo::detail
{
	inline bool Finite(Position p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
	inline bool Within(Position a, Position b, double radius)
	{
		// hypot avoids overflowing squares for finite, large coordinates.
		return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z) <= radius;
	}

	class SpatialIndex
	{
	public:
		explicit SpatialIndex(double cellSize) : _cellSize(cellSize) {}
		bool CanIndex(Position p) const
		{
			const auto x = std::floor(p.x / _cellSize), z = std::floor(p.z / _cellSize);
			return Finite(p) && x >= std::numeric_limits<std::int32_t>::min() && x <= std::numeric_limits<std::int32_t>::max()
				&& z >= std::numeric_limits<std::int32_t>::min() && z <= std::numeric_limits<std::int32_t>::max();
		}
		void Build(std::span<const EntityState> states)
		{
			_cells.clear();
			for (std::size_t i = 0; i < states.size(); ++i) { _cells[Cell(states[i].position)].push_back(i); }
		}
		std::vector<std::size_t> Query(std::span<const EntityState> states, Position center, double radius) const
		{
			std::vector<std::size_t> result;
			const double x0 = std::floor((center.x - radius) / _cellSize), x1 = std::floor((center.x + radius) / _cellSize);
			const double z0 = std::floor((center.z - radius) / _cellSize), z1 = std::floor((center.z + radius) / _cellSize);
			const auto accept = [&](std::size_t i) { if (Within(states[i].position, center, radius)) { result.push_back(i); } };
			// Huge queries scan occupied entities instead of iterating billions of cells.
			if (!CanIndex(center) || !std::isfinite(x0 + x1 + z0 + z1) ||
				x0 < std::numeric_limits<std::int32_t>::min() || x1 > std::numeric_limits<std::int32_t>::max() ||
				z0 < std::numeric_limits<std::int32_t>::min() || z1 > std::numeric_limits<std::int32_t>::max() ||
				(x1 - x0 + 1) * (z1 - z0 + 1) > static_cast<double>(states.size()))
			{
				for (std::size_t i = 0; i < states.size(); ++i) { accept(i); }
			}
			else
			{
				for (std::int64_t x = static_cast<std::int64_t>(x0); x <= static_cast<std::int64_t>(x1); ++x)
				{
					for (std::int64_t z = static_cast<std::int64_t>(z0); z <= static_cast<std::int64_t>(z1); ++z)
					{
						const auto it = _cells.find({ x, z });
						if (it != _cells.end()) { for (auto i : it->second) { accept(i); } }
					}
				}
			}
			std::sort(result.begin(), result.end());
			return result;
		}
	private:
		std::pair<std::int64_t, std::int64_t> Cell(Position p) const
		{
			return { static_cast<std::int64_t>(std::floor(p.x / _cellSize)), static_cast<std::int64_t>(std::floor(p.z / _cellSize)) };
		}
		double _cellSize;
		std::map<std::pair<std::int64_t, std::int64_t>, std::vector<std::size_t>> _cells;
	};
}
