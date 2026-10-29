#include "PathNodes.h"
#include <cmath>
#include <cstdio>
int main(int argc, char **argv)
{
	PathData pd;
	if (!pd.load(argc > 1 ? argv[1] : ".")) { std::printf("load failed\n"); return 1; }
	size_t veh = 0, ped = 0, areas = 0, mism = 0, outOfAreaId = 0;
	for (int a = 0; a < 64; a++) {
		const PathArea &ar = pd.areas[a];
		if (ar.nodes.empty()) continue;
		areas++;
		for (const PathNode &n : ar.nodes) { (n.vehicle ? veh : ped)++; if (n.areaId != a) outOfAreaId++; if (std::fabs(n.x) > 3100 || std::fabs(n.y) > 3100) mism++; }
	}
	std::printf("areas %zu nodes %zu (vehicle %zu, ped %zu), nodes whose areaId != file %zu, outside +-3100 m %zu\n", areas, pd.totalNodes(), veh, ped, outOfAreaId, mism);
	float bx = 2495, by = -1665, bd = 1e9; const PathNode *best = nullptr;
	for (const PathArea &ar : pd.areas) for (const PathNode &n : ar.nodes) if (!n.vehicle) { float d = (n.x - bx) * (n.x - bx) + (n.y - by) * (n.y - by); if (d < bd) { bd = d; best = &n; } }
	if (best) std::printf("nearest ped node to Grove St: (%.1f, %.1f, %.1f) %.1f m away\n", best->x, best->y, best->z, std::sqrt(bd));
}
