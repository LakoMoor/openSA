#pragma once
#include <string>
#include <vector>

namespace sa {

// data/water.dat: one polygon per line, 3 or 4 vertices of 7 floats (x y z flowX flowY bigWaves smallWaves), then an optional flags integer.
// Layout from CWaterLevel::LoadDataFile (gta-reversed WaterLevel.cpp, AddWaterLevelQuad 0x6E7EF0 / AddWaterLevelTriangle 0x6E7D40).
struct WaterPoly {
	int n = 0; // 3 or 4
	float x[4] = {}, y[4] = {}, z[4] = {};
	float bigWaves[4] = {}, smallWaves[4] = {};
	float flowX[4] = {}, flowY[4] = {}; // stored by the game as int8(flow * 64)
	int flags = 0;
};

bool LoadWaterDat(const std::string &path, std::vector<WaterPoly> &out);

} // namespace sa
