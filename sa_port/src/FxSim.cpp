#include "FxSim.h"
#include <cmath>
#include <algorithm>

static float ch(const FxInfo *i, const char *name, float t, float def = 0.0f)
{
	if (!i) return def;
	auto it = i->chans.find(name);
	return it == i->chans.end() ? def : it->second.eval(t);
}

float FxEmitterSim::rnd(float a, float b)
{
	seed = seed * 1664525u + 1013904223u;
	return a + (b - a) * ((seed >> 8) & 0xFFFF) / 65535.0f;
}

void FxEmitterSim::step(P &p, float dt) const
{
	const FxInfo *f = prim_.info("FORCE");
	p.rot += p.spin * dt;
	float t = p.life > 0 ? p.age / p.life : 1.0f;
	if (f) { p.vel[0] += ch(f, "FORCEX", t) * dt; p.vel[1] += ch(f, "FORCEY", t) * dt; p.vel[2] += ch(f, "FORCEZ", t) * dt; }
	for (int k = 0; k < 3; k++) p.pos[k] += p.vel[k] * dt;
	p.age += dt;
}

void FxEmitterSim::spawn(const float origin[3], const float *velOverride, const FxMult &m)
{
	const float T = time;
	P p{};
	const FxInfo *life = prim_.info("EMLIFE"), *speed = prim_.info("EMSPEED"), *ang = prim_.info("EMANGLE"), *dir = prim_.info("EMDIR"),
		*size = prim_.info("EMSIZE"), *pos = prim_.info("EMPOS"), *rot = prim_.info("EMROTATION");
	float lf = ch(life, "LIFE", T, 1.0f), lb = ch(life, "BIAS", T);
	p.life = ((rnd(0, 1) * 2 - 1) * lb + lf) * m.life;
	if (p.life <= 0) return;
	p.m = m;
	p.biasX = rnd(-1, 1); p.biasY = rnd(-1, 1);
	if (const FxInfo *rs = prim_.info("ROTSPEED")) {   // degrees per second, either direction (MINCW..MAXCW or MINCCW..MAXCCW)
		bool cw = rnd(0, 1) < 0.5f;
		float lo = ch(rs, cw ? "MINCW" : "MINCCW", T), hi = ch(rs, cw ? "MAXCW" : "MAXCCW", T);
		p.spin = (lo + (hi - lo) * rnd(0, 1)) * (cw ? 1.0f : -1.0f);
	}
	p.rr = (uint8_t)rnd(0, 255); p.rg = (uint8_t)rnd(0, 255); p.rb = (uint8_t)rnd(0, 255);
	p.rot = rnd(0, 1) * (ch(rot, "ANGLEMAX", T) - ch(rot, "ANGLEMIN", T)) + ch(rot, "ANGLEMIN", T);
	float radius = ch(size, "RADIUS", T), v[3];
	if (std::fabs(radius) < 0.001f) {
		const char *mn[3] = {"SIZEMINX", "SIZEMINY", "SIZEMINZ"}, *mx[3] = {"SIZEMAXX", "SIZEMAXY", "SIZEMAXZ"};
		for (int k = 0; k < 3; k++) v[k] = rnd(0, 1) * (ch(size, mx[k], T) - ch(size, mn[k], T)) + ch(size, mn[k], T);
	} else {
		for (int k = 0; k < 3; k++) v[k] = rnd(0, 2) - 1;
		float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		float r = len > 0 ? (radius < 0 ? radius / len : rnd(0, 1) * radius / len) : 0;
		for (int k = 0; k < 3; k++) v[k] *= r;
	}
	v[0] += ch(pos, "X", T); v[1] += ch(pos, "Y", T); v[2] += ch(pos, "Z", T);
	for (int k = 0; k < 3; k++) p.pos[k] = origin[k] + v[k];   // emitter matrix is identity here
	if (velOverride) { for (int k = 0; k < 3; k++) p.vel[k] = velOverride[k]; }
	else {
		float ra = rnd(0, 6.2831853f), amin = ch(ang, "MIN", T) * 0.0174533f, amax = ch(ang, "MAX", T) * 0.0174533f, rb = amin + (amax - amin) * rnd(0, 1);
		float rv[3] = {std::cos(ra) * std::sin(rb), std::cos(rb), std::sin(ra) * std::sin(rb)};   // cone around +Y, rotated onto the direction below (RotateVecIntoVec)
		float d[3] = {ch(dir, "DIRX", T), ch(dir, "DIRY", T), ch(dir, "DIRZ", T, 1.0f)};
		if (d[0] > 10.0f) { for (int k = 0; k < 3; k++) d[k] = p.pos[k] - origin[k]; }   // DIRX > 10: radial (the original uses the particle position)
		float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (dl > 0) for (int k = 0; k < 3; k++) d[k] /= dl;
		float ax[3] = {std::fabs(d[1]) < 0.99f ? 0.0f : 1.0f, std::fabs(d[1]) < 0.99f ? 1.0f : 0.0f, 0.0f};
		float xv[3] = {ax[1] * d[2] - ax[2] * d[1], ax[2] * d[0] - ax[0] * d[2], ax[0] * d[1] - ax[1] * d[0]};
		float xl = std::sqrt(xv[0] * xv[0] + xv[1] * xv[1] + xv[2] * xv[2]);
		if (xl > 0) for (int k = 0; k < 3; k++) xv[k] /= xl;
		float zv[3] = {xv[1] * d[2] - xv[2] * d[1], xv[2] * d[0] - xv[0] * d[2], xv[0] * d[1] - xv[1] * d[0]};
		float sp = (rnd(0, 2) - 1) * ch(speed, "BIAS", T) + ch(speed, "SPEED", T);
		for (int k = 0; k < 3; k++) p.vel[k] = (xv[k] * rv[0] + d[k] * rv[1] + zv[k] * rv[2]) * sp;
	}
	parts_.push_back(p);
}

void FxEmitterSim::addParticle(const float pos[3], const float *velOverride, const FxMult &m)
{
	spawn(pos, velOverride, m);
}

void FxEmitterSim::update(float dt, const float origin[3])
{
	if (emitting) emit_ += ch(prim_.info("EMRATE"), "RATE", time) * dt;
	while (emit_ >= 1.0f) { spawn(origin, nullptr, defMult); emit_ -= 1.0f; }
	for (P &p : parts_) step(p, dt);
	parts_.erase(std::remove_if(parts_.begin(), parts_.end(), [](const P &p) { return p.age >= p.life; }), parts_.end());
	time += dt;
}

void FxEmitterSim::collect(std::vector<FxBillboard> &out) const
{
	const FxInfo *col = prim_.info("COLOUR"), *sz = prim_.info("SIZE"), *at = prim_.info("ANIMTEX");
	bool flat = prim_.info("FLAT") != nullptr;
	for (const P &p : parts_) {
		float t = p.life > 0 ? p.age / p.life : 1.0f;
		FxBillboard b{};
		for (int k = 0; k < 3; k++) b.pos[k] = p.pos[k];
		b.halfX = (ch(sz, "SIZEX", t, 1.0f) + ch(sz, "SIZEXBIAS", t) * p.biasX) * p.m.size; b.halfY = (ch(sz, "SIZEY", t, 1.0f) + ch(sz, "SIZEYBIAS", t) * p.biasY) * p.m.size;
		auto cl = [](float v) { return (uint8_t)std::max(0.0f, std::min(255.0f, v)); };
		b.r = cl(ch(col, "RED", t, 255) * p.m.r); b.g = cl(ch(col, "GREEN", t, 255) * p.m.g); b.b = cl(ch(col, "BLUE", t, 255) * p.m.b); b.a = cl(ch(col, "ALPHA", t, 255) * p.m.a);
		b.rot = p.rot * 0.0174533f * p.m.rot;
		b.tex = at ? std::max(0, std::min(3, (int)ch(at, "TEXID", t, 1.0f) - 1)) : 0;
		b.flat = flat;
		if (flat) { const FxInfo *fl = prim_.info("FLAT"); const char *rn[3] = {"RX", "RY", "RZ"}, *un[3] = {"UX", "UY", "UZ"}; for (int k = 0; k < 3; k++) { b.right[k] = ch(fl, rn[k], t); b.up[k] = ch(fl, un[k], t); } }
		out.push_back(b);
	}
}
