#pragma once
#include <cstdint>
#include <string>
#include <vector>

// data/paths/nodesN.dat (CPathFind::LoadPathFindData 0x4529F0, layout from gta-reversed): 5 ints header, numNodes x 28-byte CPathNode, then car path links,
// node link addresses, link lengths, intersection flags. Only the node block is read here (links are the next step).
struct PathNode {
	float x, y, z;
	int16_t baseLinkId;
	uint16_t areaId, nodeId;
	uint8_t width;         // /16 = metres
	uint8_t numLinks;      // flags byte 0 bits 0..3
	bool deadEnd, switchedOff, roadBlock, water;
	bool notHighway, highway;
	uint8_t spawnProbability, behaviourType;   // byte 2 low / high nibble (behaviour 2 = parking node)
	bool vehicle;          // index < numVehicleNodes (vehicle nodes come first, ped nodes after)
};

struct PathArea {
	int numNodes = 0, numVehicleNodes = 0, numPedNodes = 0, numCarLinks = 0, numAddresses = 0;
	std::vector<PathNode> nodes;
};

struct PathData {
	PathArea areas[64];
	bool load(const std::string &pathsDir);   // reads nodes0..63.dat
	size_t totalNodes() const;
};
