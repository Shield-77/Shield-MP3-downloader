#pragma once
#include <cstdint>

namespace fourthtube {
// The downloader fills [current, window_end); block zero always stays cached.
// max_blocks includes that pinned header block, and forward_blocks includes current.
struct CacheEvictionPolicy {
	std::uint64_t current;
	std::uint64_t window_start;
	std::uint64_t window_end;

	constexpr CacheEvictionPolicy(std::uint64_t current, std::uint64_t max_blocks, std::uint64_t forward_blocks)
	    : current(current),
	      window_start(current > max_blocks - 1 - forward_blocks ? current - (max_blocks - 1 - forward_blocks) : 0),
	      window_end(current + forward_blocks) {}

	constexpr bool protected_block(std::uint64_t block) const { return block == 0 || block == current; }
	constexpr bool outside_window(std::uint64_t block) const {
		return block < window_start || block >= window_end;
	}
	constexpr std::uint64_t distance(std::uint64_t block) const {
		return block > current ? block - current : current - block;
	}
	constexpr bool prefer_eviction(std::uint64_t candidate, std::uint64_t victim, bool has_victim) const {
		return !protected_block(candidate) &&
		       (!has_victim || (outside_window(candidate) && !outside_window(victim)) ||
		        (outside_window(candidate) == outside_window(victim) && distance(candidate) > distance(victim)));
	}
};
} // namespace fourthtube
