#include "FxData.h"
#include <cmath>
#include <fstream>
#include <cstdlib>
#include <cstring>

float FxInterp::eval(float t) const
{
	if (keys.empty()) return 0.0f;
	if (looped && keys.back().first > 0.0f) { t = std::fmod(t, keys.back().first); }
	if (t <= keys.front().first) return keys.front().second;
	for (size_t i = 1; i < keys.size(); i++) {
		if (t <= keys[i].first) {
			float d = keys[i].first - keys[i - 1].first;
			float f = d > 0.0f ? (t - keys[i - 1].first) / d : 1.0f;
			return keys[i - 1].second + (keys[i].second - keys[i - 1].second) * f;
		}
	}
	return keys.back().second;
}

const FxInfo *FxPrim::info(const std::string &type) const
{
	for (const FxInfo &i : infos) if (i.type == type) return &i;
	return nullptr;
}

const FxSystem *FxProject::find(const std::string &name) const
{
	for (const FxSystem &s : systems) if (s.name == name) return &s;
	return nullptr;
}

static std::string trim(const std::string &s)
{
	size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
	return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool FxProject::load(const std::string &path)
{
	std::ifstream f(path);
	if (!f) return false;
	systems.clear();
	enum { NONE, SYS, BASE, INFO } where = NONE;
	FxSystem *sys = nullptr; FxPrim *prim = nullptr; FxInfo *info = nullptr;
	FxInterp *cur = nullptr;      // channel being filled
	std::string chan;             // pending channel name (a "KEY:" line with no value)
	std::string line;
	while (std::getline(f, line)) {
		line = trim(line);
		if (line.empty()) continue;
		size_t c = line.find(':');
		std::string key = c == std::string::npos ? line : line.substr(0, c);
		std::string val = c == std::string::npos ? std::string() : trim(line.substr(c + 1));
		if (key == "FX_SYSTEM_DATA") { systems.emplace_back(); sys = &systems.back(); prim = nullptr; info = nullptr; cur = nullptr; where = SYS; continue; }
		if (!sys) continue;
		if (key == "FX_PRIM_EMITTER_DATA") { sys->prims.emplace_back(); prim = &sys->prims.back(); info = nullptr; cur = nullptr; where = BASE; continue; }
		if (key == "FX_PRIM_BASE_DATA") { where = BASE; continue; }
		if (key == "NUM_PRIMS" || key == "NUM_INFOS" || key == "FX_PROJECT_DATA_END") continue;
		if (key.compare(0, 8, "FX_INFO_") == 0 && prim) {
			prim->infos.emplace_back(); info = &prim->infos.back();
			info->type = key.substr(8, key.size() - 8 - 5); // strip FX_INFO_ and _DATA
			cur = nullptr; chan.clear(); where = INFO; continue;
		}
		if (key == "FX_INTERP_DATA") {
			if (info && !chan.empty()) { cur = &info->chans[chan]; chan.clear(); }
			continue;
		}
		if (key == "FX_KEYFLOAT_DATA") { if (cur) cur->keys.push_back({0, 0}); continue; }
		if (cur && key == "LOOPED") { cur->looped = atoi(val.c_str()) != 0; continue; }
		if (cur && key == "NUM_KEYS") continue;
		if (cur && key == "TIME" && !cur->keys.empty()) { cur->keys.back().first = (float)atof(val.c_str()); continue; }
		if (cur && key == "VAL" && !cur->keys.empty()) { cur->keys.back().second = (float)atof(val.c_str()); continue; }
		if (c == std::string::npos) continue;   // stray count lines
		if (key == "LODSTART" || key == "LODEND" || key == "OMITTEXTURES" || key == "TXDNAME") {   // trail the last info but belong to the prim / system
			if (key == "TXDNAME") sys->txd = val; else if (prim) prim->base[key] = val;
			continue;
		}
		if (where == INFO && info) {
			if (val.empty()) { chan = key; cur = nullptr; }   // channel header, FX_INTERP_DATA follows
			else info->scalars[key] = (float)atof(val.c_str());
			continue;
		}
		if (where == BASE && prim) { prim->base[key] = val; continue; }
		if (where == SYS) {
			if (key == "NAME") sys->name = val; else if (key == "TXDNAME") sys->txd = val;
			else if (key != "FILENAME") sys->props[key] = val;
		}
	}
	return !systems.empty();
}
