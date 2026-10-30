#pragma once
#include "FxData.h"
#include <vector>
#include <cstdint>

// Headless CPU simulation of one effects.fxp primitive (ParticleEmitter). Emission follows the decompiled FxEmitter_c::CreateParticle/CreateParticles
// kept as comments in gta-reversed (EmissionInfo_t fields filled from the FX_INFO_EM* blocks). DEVIATIONS (the exe's FxEmitterBP_c::UpdateParticle 0x4A21D0 was not
// read): particle motion is vel += FORCE*dt, pos += vel*dt only; FRICTION/WIND/NOISE/JITTER/GROUNDCOLLIDE/ATTRACTPT/TRAIL infos are ignored; the emission rate
// is RATE particles per second (the original multiplies by m_nRateMult*1000 in unknown units); colour, size, alpha are evaluated at normalised particle age (TIMEMODEPRT is not honoured).
struct FxMult { float r = 1, g = 1, b = 1, a = 1, size = 1, rot = 1, life = 1; };   // FxPrtMult_c

struct FxBillboard {
	float pos[3];
	float halfX, halfY;     // half extents (emitter Render 0x4A2C40 multiplies the camera right/up by the size directly)
	uint8_t r, g, b, a;
	float rot;              // radians
	int tex;                // 0..3 index into FxPrim TEXTURE..TEXTURE4
	bool flat;              // FX_INFO_FLAT: quad in the emitter plane instead of facing the camera
	float right[3], up[3];  // FX_INFO_FLAT RX..RZ / UX..UZ (valid when flat)
};

class FxEmitterSim {
public:
	FxEmitterSim(const FxPrim &prim, const FxSystem &sys) : prim_(prim), sys_(sys) {}
	void update(float dt, const float origin[3]);   // advances time, emits per EMRATE at the origin
	void addParticle(const float pos[3], const float *velOverride, const FxMult &m);   // FxEmitter_c::AddParticle
	void collect(std::vector<FxBillboard> &out) const;
	size_t alive() const { return parts_.size(); }
	float time = 0;
	unsigned seed = 12345;
private:
	struct P { float pos[3], vel[3], age, life, rot, spin, biasX, biasY; FxMult m; uint8_t rr, rg, rb; };
	float rnd(float a, float b);
	void spawn(const float origin[3], const float *velOverride, const FxMult &m);
	void step(P &p, float dt) const;
	const FxPrim &prim_; const FxSystem &sys_;
	std::vector<P> parts_;
	float emit_ = 0;
};
