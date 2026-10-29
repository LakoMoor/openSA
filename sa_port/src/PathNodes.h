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

struct NodeAddr { uint16_t area, node; bool valid() const { return area != 0xFFFF && node != 0xFFFF; } };

struct CarPathLink {   // CCarPathLink (14 bytes, "navi node")
	float x, y;
	NodeAddr attachedTo;
	float dirX, dirY;
	float width;
	uint8_t oppositeLanes, sameLanes;
	bool trafficLightDir;
	uint8_t trafficLightState;
};

struct PathArea {
	int numNodes = 0, numVehicleNodes = 0, numPedNodes = 0, numCarLinks = 0, numAddresses = 0;
	std::vector<PathNode> nodes;
	std::vector<CarPathLink> carLinks;
	std::vector<NodeAddr> links;          // node link addresses (numAddresses real entries; node.baseLinkId + i indexes this)
	std::vector<uint16_t> naviLinks;      // per link: car path link id (10 bits) | area (6 bits)
	std::vector<uint8_t> linkLengths;     // per link, metres
	std::vector<uint8_t> intersections;   // per link flags
};

struct PathData {
	PathArea areas[64];
	bool load(const std::string &pathsDir);   // reads nodes0..63.dat
	size_t totalNodes() const;
};
