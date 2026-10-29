#include "PopCycle.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

static std::string trim(const std::string &s)
{
	size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
	return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool PopCycle::load(const std::string &popcyclePath, const std::string &cargrpPath)
{
	std::ifstream f(popcyclePath);
	if (!f)
		return false;
	std::string line, prev;
	bool afterBar = false;
	int day = 0, rowIdx = 0;
	while (std::getline(f, line)) {
		std::string t = trim(line);
		if (t.rfind("////", 0) == 0) { afterBar = true; prev = t; continue; }
		if (t.rfind("//", 0) == 0) {
			std::string body = trim(t.substr(2));
			if (afterBar && !body.empty() && body.find_first_of("abcdefghijklmnopqrstuvwxyz#") == std::string::npos &&
			    body != "Weekday" && body != "Weekend") {
				// type header such as "BUSINESS" or "ENTERTAINMENT   - this is ..."
				size_t sp = body.find_first_of(" \t");
				types.push_back({});
				types.back().name = sp == std::string::npos ? body : body.substr(0, sp);
				day = 0; rowIdx = 0;
			} else if (body == "Weekday") { day = 0; rowIdx = 0; }
			else if (body == "Weekend") { day = 1; rowIdx = 0; }
			afterBar = false;
			continue;
		}
		afterBar = false;
		size_t c = t.find("//");
		if (c != std::string::npos) t = trim(t.substr(0, c));
		if (t.empty() || types.empty() || rowIdx >= 12 || !std::isdigit((unsigned char)t[0]))
			continue;
		std::istringstream is(t);
		PopCycleRow r;
		is >> r.maxPeds >> r.maxCars >> r.dealers >> r.gang >> r.cops >> r.other;
		int v;
		while (is >> v) r.group.push_back(v);
		types.back().rows[day][rowIdx++] = r;
	}
	std::ifstream g(cargrpPath);
	if (!g)
		return false;
	std::string acc;
	while (std::getline(g, line)) {
		size_t c = line.find('#');
		if (c != std::string::npos) line = line.substr(0, c);
		line = trim(line);
		if (line.empty()) continue;
		acc += (acc.empty() ? "" : " ") + line;
		if (acc.back() == ',') continue;   // list continues on the next line
		std::vector<std::string> names;
		std::istringstream is(acc);
		std::string n;
		while (std::getline(is, n, ',')) {
			n = trim(n);
			std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return std::tolower(ch); });
			if (!n.empty()) names.push_back(n);
		}
		carGroups.push_back(names);
		acc.clear();
	}
	return !types.empty() && !carGroups.empty();
}

int PopCycle::pickGroup(int type, int hour, bool weekend, float rnd) const
{
	if (type < 0 || type >= (int)types.size())
		return -1;
	const PopCycleRow &r = types[type].rows[weekend ? 1 : 0][(hour / 2) % 12];
	int sum = 0;
	for (int p : r.group) sum += p;
	if (sum <= 0)
		return -1;
	float x = rnd * sum;
	for (size_t i = 0; i < r.group.size(); i++) {
		x -= r.group[i];
		if (x < 0) return (int)i;
	}
	return (int)r.group.size() - 1;
}
