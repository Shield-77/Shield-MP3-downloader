#pragma once

#define OLD_3DS_CPU_LIMIT 65
#define CPU_LIMIT OLD_3DS_CPU_LIMIT

#define OLD_3DS_ADDITIONAL_CPU_LIMIT 50
#define ADDITIONAL_CPU_LIMIT OLD_3DS_ADDITIONAL_CPU_LIMIT

void add_cpu_limit(int limit);

// there must have been corresponding adding
void remove_cpu_limit(int limit);

int get_cpu_limit();
