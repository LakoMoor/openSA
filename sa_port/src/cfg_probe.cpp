#include "Handling.h"
#include <cstdio>
int main(int argc, char **argv)
{
	HandlingCfg h;
	if (!h.load(argc > 1 ? argv[1] : "handling.cfg")) { std::printf("load failed\n"); return 1; }
	std::printf("car handling lines: %zu\n", h.cars.size());
	auto it = h.cars.find("LANDSTAL");
	if (it != h.cars.end()) {
		const HandlingData &d = it->second;
		std::printf("LANDSTAL mass %.1f turn %.1f gears %d vmax %.1f accel %.1f drive %c lock %.1f susp %.2f/%.2f flags %x %x\n", d.mass, d.turnMass, d.numGears, d.maxVelocity, d.engineAccel, d.driveType, d.steeringLock, d.suspForce, d.suspDamping, d.modelFlags, d.handlingFlags);
	}
}
