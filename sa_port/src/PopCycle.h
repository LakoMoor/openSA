#pragma once
#include <string>
#include <vector>

// popcycle.dat (per zone population type, weekday/weekend, 12 two-hour slots) and cargrp.dat (car model names per ped group).
struct PopCycleRow {
	int maxPeds = 0, maxCars = 0;
	int dealers = 0, gang = 0, cops = 0, other = 0;
	std::vector<int> group;   // percentages per ped group (Workers, Business, ...)
};

struct PopCycleType {
	std::string name;
	PopCycleRow rows[2][12];   // [0] weekday, [1] weekend
};

struct PopCycle {
	std::vector<PopCycleType> types;
	std::vector<std::vector<std::string>> carGroups;   // cargrp.dat line n = group n (lower-case model names)
	bool load(const std::string &popcyclePath, const std::string &cargrpPath);
	// Pick a car group index by the "other" group percentages of the given type/time. rnd in [0,1).
	int pickGroup(int type, int hour, bool weekend, float rnd) const;
};
