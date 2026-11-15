#include "headers.hpp"
#include <set>

static Mutex cpu_limits_lock;
static std::multiset<int> cpu_limits;
static u32 baseline_cpu_limit = 30;

void add_cpu_limit(int limit) {
	cpu_limits_lock.lock();
	const bool was_empty = cpu_limits.empty();
	if (was_empty) {
		APT_GetAppCpuTimeLimit(&baseline_cpu_limit);
	}
	const int previous_limit = was_empty ? (int)baseline_cpu_limit : *cpu_limits.begin();
	cpu_limits.insert(limit);
	if (*cpu_limits.begin() != previous_limit) {
		APT_SetAppCpuTimeLimit(*cpu_limits.begin());
	}
	cpu_limits_lock.unlock();
}
void remove_cpu_limit(int limit) {
	cpu_limits_lock.lock();
	auto entry = cpu_limits.find(limit);
	if (entry != cpu_limits.end()) {
		const int previous_limit = *cpu_limits.begin();
		cpu_limits.erase(entry);
		const int next_limit = cpu_limits.empty() ? (int)baseline_cpu_limit : *cpu_limits.begin();
		if (next_limit != previous_limit) {
			APT_SetAppCpuTimeLimit(next_limit);
		}
	}
	cpu_limits_lock.unlock();
}
int get_cpu_limit() {
	u32 res;
	APT_GetAppCpuTimeLimit(&res);
	return res;
}
