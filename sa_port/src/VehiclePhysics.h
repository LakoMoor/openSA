#pragma once
#include "Handling.h"
#include <cmath>
#include <functional>

// Rigid-body car model: CPhysical force application + CAutomobile suspension / wheel traction + cTransmission drive acceleration.
// Structure follows reVC (CPhysical, CAutomobile::ProcessControl, CVehicle::ProcessWheel); SA-specific formulas (CVehicle::ProcessWheel with the spinning loss,
// ApplySpringCollisionAlt / ApplySpringDampening with the damping limits, cTransmission, ConvertDataToGameUnits) follow gta-reversed. Constants checked in the exe:
// gravity 0.008 (0x863984), damping limits 0.25 / 0.998 (0x8CD7A0), wheel spin limit 0.3 / spinning loss 0.2 (0x8D34C0). Units: metres, 1 tick = 1/50 s.
namespace vp {

struct V3 {
	float x = 0, y = 0, z = 0;
	V3 operator+(const V3 &o) const { return {x + o.x, y + o.y, z + o.z}; }
	V3 operator-(const V3 &o) const { return {x - o.x, y - o.y, z - o.z}; }
	V3 operator*(float s) const { return {x * s, y * s, z * s}; }
	V3 operator/(float s) const { return {x / s, y / s, z / s}; }
	V3 &operator+=(const V3 &o) { x += o.x; y += o.y; z += o.z; return *this; }
	V3 &operator-=(const V3 &o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
	V3 &operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
};
inline float dot(const V3 &a, const V3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(const V3 &a, const V3 &b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float len(const V3 &a) { return std::sqrt(dot(a, a)); }
inline V3 norm(const V3 &a) { float l = len(a); return l > 1e-9f ? a / l : V3{0, 0, 0}; }

struct GroundHit { float z = 0; V3 normal{0, 0, 1}; float adhesion = 1.0f; };
// Highest ground at or below zFrom, searching down to zTo; false when there is none.
using GroundFn = std::function<bool(float x, float y, float zFrom, float zTo, GroundHit &out)>;

enum WheelState { WS_NORMAL, WS_SPINNING, WS_SKIDDING, WS_FIXED };
enum { W_FL = 0, W_RL = 1, W_FR = 2, W_RR = 3 };   // SA order of the wheel dummies: lf, lb, rf, rb

struct Input { float gas = 0, brake = 0, steer = 0; bool handbrake = false; };   // gas -1..1 (negative = reverse), brake 0..1, steer -1..1 (positive = left)

class CarPhysics {
public:
	void init(const HandlingData &h, const V3 wheelPos[4], float wheelSize);
	void place(const V3 &pos, float headingRad);   // heading as the generator angle: forward = (-sin, cos)
	void step(float ticks, const Input &in, const GroundFn &ground);   // ticks = dt * 50
	V3 pos, right{1, 0, 0}, fwd{0, 1, 0}, up{0, 0, 1};
	V3 moveSpeed, turnSpeed;   // per tick
	float steerAngle = 0, compression[4] = {1, 1, 1, 1};
	int wheelsOnGround = 0;
	float forwardSpeed() const { return dot(moveSpeed, fwd); }   // m per tick
	uint8_t gear = 1;
public:
	// handling in game units
	float mass = 1500, turnMass = 3000, tractionMult = 1, tractionLoss = 0.8f, tractionBias = 0.5f, brakeDecel = 0, brakeBias = 0.5f, steeringLock = 0.5f;
	float suspForce = 1, suspDamping = 0.1f, suspUpper = 0.2f, suspLower = -0.15f, suspBias = 0.5f, suspAntiDive = 0, dragMult = 1, airResistance = 0.001f;
	V3 com;
	char driveType = 'R';
	int numGears = 4;
	float engineAccel = 0, engineInertia = 5, maxVelocity = 0, maxFlatVelocity = 0, maxReverse = -0.2f;
	struct Gear { float maxVel = 0, up = 0, down = 0; } gears[7];
	V3 wheelLocal[4];
	float wheelSize = 0.7f, springLength = 0, lineLength = 0;
	V3 moveFriction, turnFriction;
	WheelState wheelState[4] = {WS_NORMAL, WS_NORMAL, WS_NORMAL, WS_NORMAL};
	float wheelTimer[4] = {};
	float inertia1 = 0, inertia2 = 1;
	float gearChange = 0;
	float lastGas = 0;

	V3 mul3(const V3 &l) const { return right * l.x + fwd * l.y + up * l.z; }
	V3 getSpeed(const V3 &r) const { return moveSpeed + moveFriction + cross(turnSpeed + turnFriction, r); }
	float health = 1000.0f, damageMult = 1.0f;
	// Static-wall hit (CPhysical::ApplyCollision static-B branch, reVC) + CAutomobile::VehicleDamage health loss; returns the impulse, 0 if moving away. `point` relative to the car centre.
	float hitStatic(const V3 &point, const V3 &n);
	void applyForce(const V3 &f, const V3 &point);   // CPhysical::ApplyForce(.., true)
	void applyMoveForce(const V3 &j) { moveSpeed += j / mass; }
	void applyTurnForce(const V3 &j, const V3 &point) { turnSpeed += cross(point - mul3(com), j) / turnMass; }
	float massAt(const V3 &pos, const V3 &dir) const { V3 c = cross(pos - mul3(com), dir); return 1.0f / (dot(c, c) / turnMass + 1.0f / mass); }
	float driveAcceleration(float gas, float velocity, bool allWheelsOnGround);
	void processWheel(const V3 &wf, const V3 &wr, const V3 &contactSpeed, const V3 &contactPoint, int onGround, float thrust, float brake, float adhesion, int wheel, float gas);
	void orthonormalise();
};

}
