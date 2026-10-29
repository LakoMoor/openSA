#include "PopCycle.h"
#include <cstdio>
int main(int argc, char **argv)
{
	PopCycle pc;
	std::string d = argc > 1 ? argv[1] : "";
	if (!pc.load(d + "/popcycle.dat", d + "/cargrp.dat")) { std::printf("load failed\n"); return 1; }
	std::printf("popcycle types=%zu cargrp groups=%zu\n", pc.types.size(), pc.carGroups.size());
	for (size_t i = 0; i < pc.types.size(); i++)
		std::printf("  type %zu %s noon weekday: peds=%d cars=%d groups=%zu\n", i, pc.types[i].name.c_str(), pc.types[i].rows[0][6].maxPeds, pc.types[i].rows[0][6].maxCars, pc.types[i].rows[0][6].group.size());
	for (size_t i = 0; i < pc.carGroups.size(); i++)
		std::printf("  group %zu: %zu models, first %s\n", i, pc.carGroups[i].size(), pc.carGroups[i].empty() ? "-" : pc.carGroups[i][0].c_str());
}
