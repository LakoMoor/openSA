#include "VehiclePhysics.h"
#include <algorithm>

namespace vp {

constexpr float kGravity = 0.008f;                 // exe 0x863984
constexpr float kDampingLimitInFrame = 0.25f;      // exe 0x8CD7A0
constexpr float kDampingLimitOfSpring = 0.998f;    // exe 0x8CD7A4 (0x3f7fbe77)
constexpr float kWsTracFracLimit = 0.3f, kWsAlreadySpinningLoss = 0.2f;   // exe 0x8D34C0 / 0x8D34C4
constexpr float kWheelFriction = 0.9f;             // cHandlingDataMgr::fWheelFriction (set in the constructor, gta-reversed cHandlingDataMgr.cpp:235)
constexpr float kTransmissionFreeAccel = 0.1f, kTransmissionSmoother = 0.85f;
static float sq(float v) { return v * v; }

// cHandlingDataMgr::ConvertDataToGameUnits + cTransmission::InitGearRatios (gta-reversed).
void CarPhysics::init(const HandlingData &h, const V3 wp[4], float wsize) {
	mass = h.mass; turnMass = h.turnMass; dragMult = h.dragMult;
	com = {h.com[0], h.com[1], h.com[2]};
	damageMult = h.damageMult * 2000.0f / h.mass; tractionMult = h.tractionMult; tractionLoss = h.tractionLoss; tractionBias = h.tractionBias;
	numGears = std::max(1, std::min(h.numGears, 6));
	driveType = h.driveType;
	engineInertia = h.engineInertia;
	brakeDecel = h.brakeDecel / (50.0f * 50.0f); brakeBias = h.brakeBias;
	steeringLock = h.steeringLock * 0.0174533f;
	suspForce = h.suspForce; suspDamping = h.suspDamping; suspUpper = h.suspUpper; suspLower = h.suspLower; suspBias = h.suspBias; suspAntiDive = h.suspAntiDive;
	engineAccel = h.engineAccel * 0.4f / (50.0f * 50.0f);   // tHandlingData::InitFromData scales the engine acceleration by 0.4 before ConvertDataToGameUnits
	maxVelocity = h.maxVelocity * 0.277778f / 50.0f;
	float accelLimit = engineAccel / 6.0f, v = maxVelocity;
	while (v > 0.0f) {
		v -= 0.01f;
		if (dragMult >= 0.01f) { if (sq(v) * (dragMult / 2.0f) / 1000.0f <= accelLimit) break; continue; }
		float recip = 1.0f / (sq(v) * dragMult + 1.0f);
		if ((recip - 1.0f) * -v <= accelLimit) break;
	}
	maxVelocity = v * 1.2f; maxFlatVelocity = v;
	maxReverse = std::min(-v * 0.3f, -0.2f);
	engineAccel /= driveType == '4' ? 4.0f : 2.0f;
	for (Gear &g : gears) g = Gear();
	float halfGear = 0.5f * maxVelocity / numGears, maxGear = maxVelocity - halfGear;
	for (int i = 1; i <= numGears; i++) {
		gears[i].maxVel = i * maxGear / numGears + halfGear;
		float diff = gears[i].maxVel - gears[i - 1].maxVel;
		if (i >= numGears) gears[i].up = maxVelocity;
		else { gears[i + 1].down = 0.42f * diff + gears[i - 1].maxVel; gears[i].up = 0.6667f * diff + gears[i - 1].maxVel; }
	}
	gears[0].maxVel = maxReverse; gears[0].up = -0.01f; gears[0].down = maxReverse; gears[1].down = -0.01f;
	airResistance = dragMult <= 0.01f ? dragMult : dragMult / 1000.0f / 2.0f;
	for (int i = 0; i < 4; i++) wheelLocal[i] = wp[i];
	wheelSize = wsize;
	springLength = suspUpper - suspLower;
	lineLength = springLength + wheelSize / 2.0f;
}

void CarPhysics::place(const V3 &p, float heading) {
	pos = p;
	fwd = {-std::sin(heading), std::cos(heading), 0};
	right = {std::cos(heading), std::sin(heading), 0};
	up = {0, 0, 1};
	moveSpeed = turnSpeed = moveFriction = turnFriction = {0, 0, 0};
	gear = 1; steerAngle = 0;
	for (int i = 0; i < 4; i++) { wheelTimer[i] = 0; wheelState[i] = WS_NORMAL; compression[i] = 1; }
}

// CPhysical::ApplyForce(force, point, true)
void CarPhysics::applyForce(const V3 &f, const V3 &point) {
	moveSpeed += f / mass;
	turnSpeed += cross(point - mul3(com), f) / turnMass;
}

// cTransmission::CalculateDriveAcceleration (gta-reversed 0x6D05E0). gas in -1..1; velocity per tick.
float CarPhysics::driveAcceleration(float gas, float velocity, bool allWheelsOnGround) {
	if (velocity < maxReverse) return 0.0f;
	bool useInertia = true;
	while (velocity <= maxVelocity) {
		const Gear &g = gears[gear];
		bool accelerate = false, shiftLower = false;
		if (velocity > g.up) {
			if (gear == 0 && gas <= 0.0f) accelerate = true; else gear++;
		} else {
			if (velocity >= g.down || gear == 0 || (gear == 1 && gas >= 0.0f)) accelerate = true;
			shiftLower = true;
		}
		if (accelerate) {
			float speedMul;
			if (numGears == 1) speedMul = 1.0f;
			else if (gear >= 1) { float gn = 1.0f - ((float)gear - 1.0f) / ((float)numGears - 1.0f); gn *= gn; speedMul = gn * 3.0f + 1.0f; }   // no 1G/2G boost flags handled
			else speedMul = 4.5f;
			float drive = speedMul * engineAccel * 0.4f * gas;   // * timestep applied by the caller
			if (useInertia) {
				if (allWheelsOnGround) {
					float maxChange = maxVelocity / numGears * (1.0f / 3.0f), cur, span;
					if (gear) {
						if (gear == 1) { cur = maxChange + velocity; span = maxChange + gears[1].up; }
						else { cur = velocity - g.down; span = g.up - g.down; }
					} else { cur = maxChange - velocity; span = maxChange - gears[0].down; }
					float ratio = cur / span, inertiaMul = ratio - inertia1, accel = 1.0f - inertiaMul * engineInertia;
					accel = std::min(1.0f, std::max(0.1f, accel));
					inertia1 = ratio;
					inertia2 = accel * (1.0f - kTransmissionSmoother) + kTransmissionSmoother * inertia2;
					drive *= inertia2;
				} else {
					inertia1 = std::min(inertia1 + std::fabs(gas) / engineInertia * kTransmissionFreeAccel, 1.0f);
					inertia2 = 0.1f;
				}
			}
			float gearMax = g.maxVel, change;
			if (gearMax >= 0.0f || velocity >= gearMax) {
				if (gearMax <= 0.0f) return drive;
				if (velocity <= gearMax) return drive;
				change = velocity - gearMax;
			} else change = gearMax - velocity;
			return drive * (1.0f - std::min(change / 0.05f, 1.0f));
		}
		if (shiftLower) gear--;
		useInertia = false; allWheelsOnGround = false;
		if (velocity < maxReverse) return 0.0f;
	}
	return 0.0f;
}

// CVehicle::ProcessWheel (SA). NOTE: the original keeps bAlreadySkidding in a static that is never reset (0xC1CDAC); kept here.
void CarPhysics::processWheel(const V3 &wf, const V3 &wr, const V3 &contactSpeed, const V3 &contactPoint, int onGround, float thrust, float brake, float adhesion, int wheel, float gas) {
	static bool alreadySkidding = false;
	float right = 0, fwdF = 0, csFwd = dot(wf, contactSpeed), csRight = dot(wr, contactSpeed);
	bool braking = brake != 0.0f, driving = !braking && thrust != 0.0f;
	adhesion *= 1.0f;   // times timestep, applied by the caller
	if (wheelState[wheel] != WS_NORMAL) {
		alreadySkidding = true;
		adhesion *= tractionLoss;
		if (wheelState[wheel] == WS_SPINNING) adhesion *= (1.0f - std::fabs(gas) * kWsAlreadySpinningLoss);   // player vehicle
	}
	wheelState[wheel] = WS_NORMAL;
	if (csRight != 0.0f) right = -(csRight / onGround);
	if (driving) {
		fwdF = thrust;
		right = std::min(adhesion, std::max(-adhesion, right));
	} else if (csFwd != 0.0f) {
		fwdF = -csFwd / onGround;
		if (!braking && std::fabs(gas) < 0.01f) {
			brake = kWheelFriction / mass;
			if (mass < 500.0f) brake *= 0.1f;
		}
		if (brake > adhesion) { if (std::fabs(csFwd) > 0.005f) wheelState[wheel] = WS_FIXED; }
		else fwdF = std::min(brake, std::max(-brake, fwdF));
	}
	float speedSq = right * right + fwdF * fwdF;
	if (speedSq > adhesion * adhesion) {
		if (wheelState[wheel] != WS_FIXED) {
			float limit = kWsTracFracLimit;
			if (csFwd > 0.15f && (wheel == W_FL || wheel == W_FR)) limit += limit;
			wheelState[wheel] = (driving && limit * adhesion < std::fabs(fwdF)) ? WS_SPINNING : WS_SKIDDING;
		}
		float loss = tractionLoss;
		if (alreadySkidding) loss = 1.0f;
		else if (wheelState[wheel] == WS_SPINNING) loss *= (1.0f - std::fabs(gas) * kWsAlreadySpinningLoss);
		float l = std::sqrt(speedSq);
		fwdF *= adhesion * loss / l; right *= adhesion * loss / l;
	}
	if (fwdF != 0.0f || right != 0.0f) {
		bool separate = false;
		V3 total = wf * fwdF + wr * right, turnDir = total;
		if (suspAntiDive > 0.0f) {
			if (braking) { separate = true; turnDir -= wf * (suspAntiDive * fwdF); }
			else if (driving) { separate = true; turnDir -= wf * (0.5f * suspAntiDive * fwdF); }
		}
		V3 dir = total;
		float speed = len(total), turnSp = separate ? len(turnDir) : speed;
		dir = norm(dir);
		turnDir = separate ? norm(turnDir) : dir;
		applyMoveForce(dir * (speed * mass));
		float m = 1.0f / (dot(cross(contactPoint, turnDir), cross(contactPoint, turnDir)) / turnMass + 1.0f / mass);   // CPhysical::GetMass(pos, dir)
		applyTurnForce(turnDir * (turnSp * m), contactPoint);
	}
}

void CarPhysics::orthonormalise() {
	fwd = norm(fwd);
	right = norm(cross(fwd, up));
	up = norm(cross(right, fwd));
	fwd = cross(up, right);
}

void CarPhysics::step(float ticks, const Input &in, const GroundFn &ground) {
	int n = std::max(1, (int)std::ceil(ticks));
	float ts = ticks / n;
	for (int s = 0; s < n; s++) {
		float tc = std::min(ts, 3.0f);
		// steering: ASSUMED smoothing rate 0.1 / tick (the original's pad scaling was not ported); speed limit as in the SA steer-angle clamp
		float fs = forwardSpeed();
		float target = in.steer * steeringLock;
		if (std::fabs(fs) > 0.01f) {
			float t = 4.5f * (tractionMult / 250.0f * 0.25f) * 4.0f * 4.0f / (fs * fs);
			float f = std::min(t, 1.0f);
			f = std::asin(f) / steeringLock;
			if (f < 1.0f) target *= f;
		}
		steerAngle += (target - steerAngle) * std::min(1.0f, 0.1f * ts);

		// suspension lines (vertical ground probes at the line start, spring ratio measured along the car's up vector)
		V3 contactPoint[4], normal[4], contactSpeed[4], dirs[4];
		float adh[4], ratio[4], dampLimit[4] = {};
		float wheelRadius = 1.0f - springLength / lineLength;
		for (int i = 0; i < 4; i++) {
			V3 sp = pos + mul3({wheelLocal[i].x, wheelLocal[i].y, wheelLocal[i].z + suspUpper});
			GroundHit gh;
			ratio[i] = 1.0f; adh[i] = 1.0f; normal[i] = {0, 0, 1};
			float zEnd = sp.z - lineLength / std::max(up.z, 0.3f);
			if (ground(sp.x, sp.y, sp.z + 0.05f, zEnd - 0.2f, gh)) {
				float d = (sp.z - gh.z) * up.z, r = d / lineLength;
				if (r < 1.0f) {
					r = std::max(r, 0.0f);
					ratio[i] = std::max(0.0f, (r - wheelRadius) / (1.0f - wheelRadius));
					contactPoint[i] = V3{sp.x, sp.y, gh.z} - pos;
					normal[i] = gh.normal; adh[i] = gh.adhesion;
				}
			}
			compression[i] = ratio[i];
			if (ratio[i] >= 1.0f) contactPoint[i] = mul3(wheelLocal[i]);
		}
		for (int i = 0; i < 4; i++) {   // springs push up (ApplySpringCollisionAlt)
			if (ratio[i] >= 1.0f) continue;
			float bias = (i == W_RL || i == W_RR) ? 1.0f - suspBias : suspBias, stress = 1.0f - ratio[i];
			V3 nrm = normal[i], down = up * -1.0f;
			if (dot(down, nrm) > 0.0f) nrm = nrm * -1.0f;
			dampLimit[i] = stress * (tc * mass) * suspForce * bias * 0.016f;
			applyForce(nrm * dampLimit[i], contactPoint[i]);
		}
		for (int i = 0; i < 4; i++) {
			contactSpeed[i] = getSpeed(contactPoint[i] - mul3(com));
			dirs[i] = (ratio[i] < 1.0f && normal[i].z > 0.35f) ? normal[i] * -1.0f : up * -1.0f;
		}
		for (int i = 0; i < 4; i++) {   // ApplySpringDampening
			if (ratio[i] >= 1.0f) continue;
			float posDot = dot(contactSpeed[i], dirs[i]);
			float pointSpeed = dot(getSpeed(contactPoint[i] - mul3(com)), dirs[i]);
			float step = std::min(std::max(tc * suspDamping, -kDampingLimitInFrame), kDampingLimitInFrame);
			float dSpeed = -(step * posDot);
			if (dSpeed > 0.0f && dSpeed + pointSpeed > 0.0f) dSpeed = pointSpeed >= 0.0f ? 0.0f : -pointSpeed;
			else if (dSpeed < 0.0f && dSpeed + pointSpeed < 0.0f) dSpeed = pointSpeed <= 0.0f ? 0.0f : -pointSpeed;
			V3 dist = contactPoint[i] - mul3(com);
			float m = 1.0f / (dot(cross(dist, dirs[i]), cross(dist, dirs[i])) / turnMass + 1.0f / mass);
			float f = m * dSpeed, lim = std::fabs(dampLimit[i]) * kDampingLimitOfSpring;
			f = std::min(f, lim);
			applyForce(dirs[i] * f, contactPoint[i]);
		}
		for (int i = 0; i < 4; i++) contactSpeed[i] = getSpeed(contactPoint[i] - mul3(com));   // speeds again after damping

		// engine, brakes
		fs = forwardSpeed();
		bool allOn = false;
		int driveOn = 0;
		wheelsOnGround = 0;
		for (int i = 0; i < 4; i++) {
			wheelTimer[i] = ratio[i] < 1.0f ? 4.0f : std::max(wheelTimer[i] - ts, 0.0f);
			if (wheelTimer[i] > 0.0f) {
				wheelsOnGround++;
				if (driveType == '4' || (driveType == 'F' && (i == W_FL || i == W_FR)) || (driveType == 'R' && (i == W_RL || i == W_RR))) driveOn++;
			}
		}
		allOn = driveOn > 0;
		float gas = in.gas;
		float accel = driveAcceleration(gas, fs, allOn) * ts;
		float brake = in.brake * brakeDecel * ts;
		float traction = tractionMult / 250.0f * 0.25f;
		bool frontDrive = driveType == 'F' || driveType == '4', rearDrive = driveType == 'R' || driveType == '4';
		if (wheelsOnGround > 0) {
			for (int i = 0; i < 4; i++) {
				if (wheelTimer[i] <= 0.0f) continue;
				bool front = i == W_FL || i == W_FR;
				V3 n = normal[i], wf = norm(fwd - n * dot(fwd, n)), wr = norm(cross(wf, n));
				if (front) { float sn = std::sin(steerAngle), cs = std::cos(steerAngle); V3 tmp = wf * cs - wr * sn; wr = wf * sn + wr * cs; wf = tmp; }
				float thrust = ((front && frontDrive) || (!front && rearDrive)) ? accel : 0.0f;
				float bBias = front ? 2.0f * brakeBias : 2.0f - 2.0f * brakeBias, tBias = front ? 2.0f * tractionBias : 2.0f - 2.0f * tractionBias;
				float wheelBrake = brake * bBias;
				if (!front && in.handbrake) wheelBrake = 20000.0f * ts;
				// player vehicle: adhesion scaled by the wheel load (CAutomobile wheel loop, gta-reversed Automobile.cpp:4645)
				float sBias = front ? 2.0f * suspBias : 2.0f - 2.0f * suspBias;
				float load = std::min(sBias * suspForce * 4.0f * (1.0f - compression[i]), 2.0f);
				processWheel(wf, wr, contactSpeed[i], contactPoint[i], wheelsOnGround, thrust, wheelBrake, adh[i] * traction * tBias * load * ts, i, gas);
			}
		}

		// CPhysical::ProcessControl: gravity, friction accumulators, air resistance; then movement
		moveSpeed.z -= kGravity * ts;
		moveSpeed += moveFriction; turnSpeed += turnFriction;
		moveFriction = turnFriction = {0, 0, 0};
		float mag = len(moveSpeed) * airResistance;
		moveSpeed *= std::pow(1.0f - mag, ts);
		turnSpeed *= std::pow(0.99f, ts);
		pos += moveSpeed * ts;
		V3 tv = turnSpeed * ts;
		right += cross(tv, right); fwd += cross(tv, fwd); up += cross(tv, up);
		orthonormalise();
	}
}

float CarPhysics::hitStatic(const V3 &pt, const V3 &n) {
	float sp = dot(getSpeed(pt), n);
	if (sp >= 0.0f) return 0.0f;
	float imp = -(1.0f + 0.05f) * sp * massAt(pt, n);   // elasticity 0.05 ASSUMED
	V3 f = n * imp;
	applyMoveForce(f); applyTurnForce(f, pt);
	// VehicleDamage (0x6A8052): (impulse - mass/1500*25) * damageMult * 0.6 * collForceMult, /2 for the player vehicle. ASSUMED: impulse == m_fDamageIntensity, collForceMult 1/3
	float minDmg = mass / 1500.0f * 25.0f;
	if (imp > minDmg) health -= (imp - minDmg) * damageMult * 0.6f * (1.0f / 3.0f) / 2.0f;
	return imp;
}

}
