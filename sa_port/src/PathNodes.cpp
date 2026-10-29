#include "PathNodes.h"
#include <cstdio>
#include <cstring>

bool PathData::load(const std::string &dir)
{
	bool any = false;
	for (int a = 0; a < 64; a++) {
		FILE *f = std::fopen((dir + "/nodes" + std::to_string(a) + ".dat").c_str(), "rb");
		if (!f)
			f = std::fopen((dir + "/NODES" + std::to_string(a) + ".DAT").c_str(), "rb");
		if (!f)
			continue;
		PathArea &ar = areas[a];
		int hdr[5];
		if (std::fread(hdr, 4, 5, f) != 5) { std::fclose(f); continue; }
		ar.numNodes = hdr[0]; ar.numVehicleNodes = hdr[1]; ar.numPedNodes = hdr[2]; ar.numCarLinks = hdr[3]; ar.numAddresses = hdr[4];
		if (ar.numNodes < 0 || ar.numNodes > 100000 || ar.numNodes != ar.numVehicleNodes + ar.numPedNodes) { std::fclose(f); continue; }
		for (int i = 0; i < ar.numNodes; i++) {
			uint8_t r[28];
			if (std::fread(r, 1, 28, f) != 28) break;
			int16_t p[3]; std::memcpy(p, r + 8, 6);
			PathNode n;
			n.x = p[0] / 8.0f; n.y = p[1] / 8.0f; n.z = p[2] / 8.0f;
			std::memcpy(&n.baseLinkId, r + 16, 2);
			std::memcpy(&n.areaId, r + 18, 2);
			std::memcpy(&n.nodeId, r + 20, 2);
			n.width = r[22];
			uint32_t fl; std::memcpy(&fl, r + 24, 4);
			n.numLinks = fl & 0xF; n.deadEnd = fl >> 4 & 1; n.switchedOff = fl >> 5 & 1; n.roadBlock = fl >> 6 & 1; n.water = fl >> 7 & 1;
			n.notHighway = fl >> 12 & 1; n.highway = fl >> 13 & 1;
			n.spawnProbability = fl >> 16 & 0xF; n.behaviourType = fl >> 20 & 0xF;
			n.vehicle = i < ar.numVehicleNodes;
			ar.nodes.push_back(n);
		}
		std::fclose(f);
		any = true;
	}
	return any;
}

size_t PathData::totalNodes() const
{
	size_t t = 0;
	for (const PathArea &a : areas) t += a.nodes.size();
	return t;
}
