#include "VehiclePhysics.h"
#include <cstdio>
#include <cstdlib>
int main(int argc, char **argv)
{
	HandlingCfg cfg;
	if (!cfg.load(argc > 1 ? argv[1] : "handling.cfg")) { std::printf("handling.cfg failed\n"); return 1; }
	const char *id = argc > 2 ? argv[2] : "ELEGANT";
	auto it = cfg.cars.find(id);
	if (it == cfg.cars.end()) { std::printf("no %s\n", id); return 1; }
	const HandlingData &h = it->second;
	vp::V3 wp[4] = {{-0.85f, 1.4f, -0.45f}, {-0.85f, -1.4f, -0.45f}, {0.85f, 1.4f, -0.45f}, {0.85f, -1.4f, -0.45f}};
	vp::CarPhysics car;
	car.init(h, wp, 0.7f);
	car.place({0, 0, 0.5f}, 0);
	vp::GroundFn flat = [](float, float, float zFrom, float zTo, vp::GroundHit &o) { if (zFrom < 0.0f || zTo > 0.0f) return false; o.z = 0.0f; o.adhesion = 4.5f; return true; };
	vp::Input in;
	in.gas = 1.0f;
	std::printf("%s: mass %.0f cfg top %.0f km/h\n", id, h.mass, h.maxVelocity);
	for (int sec = 1; sec <= 40; sec++) {
		for (int t = 0; t < 50; t++) car.step(1.0f, in, flat);
		if (sec <= 12 || sec % 5 == 0) std::printf("t=%2ds speed %6.1f km/h gear %d wheels %d z %.2f\n", sec, car.forwardSpeed() * 50.0f * 3.6f, (int)car.gear, car.wheelsOnGround, car.pos.z);
	}
	in.gas = 0; in.brake = 1.0f;
	for (int sec = 1; sec <= 6; sec++) { for (int t = 0; t < 50; t++) car.step(1.0f, in, flat); std::printf("brake t=%ds speed %.1f km/h\n", sec, car.forwardSpeed() * 50.0f * 3.6f); }
	// crash test: 60 km/h head-on into a wall plane at the front bumper, then 20 km/h
	for (float kmh : {20.0f, 60.0f, 100.0f}) {
		vp::CarPhysics c; c.init(h, wp, 0.7f); c.place({0, 0, 0.5f}, 0);
		for (int t = 0; t < 30; t++) c.step(1.0f, vp::Input(), flat);
		c.moveSpeed = c.fwd * (kmh / 3.6f / 50.0f);
		float imp = c.hitStatic({0, 2.3f, 0}, {0, -1, 0});
		std::printf("crash %.0f km/h: impulse %.1f health %.1f speed after %.1f km/h\n", kmh, imp, c.health, c.forwardSpeed() * 50.0f * 3.6f);
	}
}
