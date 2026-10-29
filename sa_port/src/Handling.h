#pragma once
#include <map>
#include <string>

// handling.cfg, car lines (tHandlingData column order, gta-reversed HandlingMgr). Bike/boat/plane/heli extension lines (! % $ prefixes) are not read yet.
struct HandlingData {
	std::string name;
	float mass = 0, turnMass = 0, dragMult = 0;
	float com[3] = {};
	float percentSubmerged = 0, tractionMult = 0, tractionLoss = 0, tractionBias = 0;
	int numGears = 0;
	float maxVelocity = 0, engineAccel = 0, engineInertia = 0;
	char driveType = 'F', engineType = 'P';
	float brakeDecel = 0, brakeBias = 0;
	bool abs = false;
	float steeringLock = 0;
	float suspForce = 0, suspDamping = 0, suspHighSpeedDamp = 0, suspUpper = 0, suspLower = 0, suspBias = 0, suspAntiDive = 0;
	float seatOffset = 0, damageMult = 0;
	int monetary = 0;
	unsigned modelFlags = 0, handlingFlags = 0;
	int frontLights = 0, rearLights = 0, animGroup = 0;
};

struct HandlingCfg {
	std::map<std::string, HandlingData> cars;   // upper-case handling id from vehicles.ide
	bool load(const std::string &path);
};
