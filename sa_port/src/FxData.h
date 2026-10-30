#pragma once
#include <string>
#include <vector>
#include <map>

// models/effects.fxp: text dump of the Fx project (FX_PROJECT_DATA / FX_SYSTEM_DATA / FX_PRIM_EMITTER_DATA / FX_INFO_*_DATA),
// parsed generically: the grammar is "KEY: value..." lines, channels are "KEY:" followed by FX_INTERP_DATA with FX_KEYFLOAT_DATA keys.
struct FxInterp {
	bool looped = false;
	std::vector<std::pair<float, float>> keys; // time, value
	float eval(float t) const; // linear between keys, clamped (looped: wraps over the last key time)
};

struct FxInfo {
	std::string type;                       // e.g. "EMLIFE", "SIZE", "COLOUR" (FX_INFO_<type>_DATA)
	std::map<std::string, float> scalars;   // TIMEMODEPRT, MIN, MAX, ANGLEMIN, ...
	std::map<std::string, FxInterp> chans;  // LIFE, BIAS, SIZEX, RED, ...
};

struct FxPrim {
	std::map<std::string, std::string> base; // NAME, TEXTURE..TEXTURE4, ALPHAON, SRCBLENDID, DSTBLENDID, LODSTART, LODEND, MATRIX
	std::vector<FxInfo> infos;
	const FxInfo *info(const std::string &type) const;
};

struct FxSystem {
	std::string name, txd;
	std::map<std::string, std::string> props; // LENGTH, PLAYMODE, CULLDIST, BOUNDINGSPHERE, LOOPINTERVALMIN, OMITTEXTURES
	std::vector<FxPrim> prims;
};

struct FxProject {
	std::vector<FxSystem> systems;
	bool load(const std::string &path);
	const FxSystem *find(const std::string &name) const;
};
