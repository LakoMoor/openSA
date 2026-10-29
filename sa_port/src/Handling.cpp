#include "Handling.h"
#include <fstream>
#include <sstream>

bool HandlingCfg::load(const std::string &path)
{
	std::ifstream f(path);
	if (!f)
		return false;
	std::string line;
	while (std::getline(f, line)) {
		if (line.empty() || line[0] == ';' || line[0] == '!' || line[0] == '%' || line[0] == '$' || line[0] == '^' || line[0] == '&' || line[0] == '#')
			continue;
		size_t c = line.find(';');
		if (c != std::string::npos) line.resize(c);
		std::istringstream is(line);
		HandlingData h;
		std::string dt, et, mf, hf;
		int absI = 0;
		is >> h.name >> h.mass >> h.turnMass >> h.dragMult >> h.com[0] >> h.com[1] >> h.com[2] >> h.percentSubmerged
		   >> h.tractionMult >> h.tractionLoss >> h.tractionBias >> h.numGears >> h.maxVelocity >> h.engineAccel >> h.engineInertia
		   >> dt >> et >> h.brakeDecel >> h.brakeBias >> absI >> h.steeringLock
		   >> h.suspForce >> h.suspDamping >> h.suspHighSpeedDamp >> h.suspUpper >> h.suspLower >> h.suspBias >> h.suspAntiDive
		   >> h.seatOffset >> h.damageMult >> h.monetary >> mf >> hf >> h.frontLights >> h.rearLights >> h.animGroup;
		if (is.fail() || dt.empty() || et.empty())
			continue;
		h.driveType = dt[0]; h.engineType = et[0]; h.abs = absI != 0;
		try {
			h.modelFlags = (unsigned)std::stoul(mf, nullptr, 16);
			h.handlingFlags = (unsigned)std::stoul(hf, nullptr, 16);
		} catch (...) { continue; }
		cars[h.name] = h;
	}
	return !cars.empty();
}
