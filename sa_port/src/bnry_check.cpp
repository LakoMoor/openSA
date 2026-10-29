#include "BinaryIpl.h"
#include "ImgV2.h"
#include "TextData.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

#ifdef _MSC_VER
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

static std::string Join(const std::string &root, std::string rel) {
	std::replace(rel.begin(), rel.end(), '\\', '/');
	return root + "/" + rel;
}

// Loads every IDE listed in default.dat/gta.dat, then validates all binary IPLs in the given archives.
int main(int argc, char **argv) {
	if (argc < 2) { puts("usage: bnry_check <SA game dir>"); return 2; }
	std::string root = argv[1];
	std::vector<sa::DatDirective> dat;
	sa::LoadGtaDat(root + "/data/default.dat", dat);
	sa::LoadGtaDat(root + "/data/gta.dat", dat);
	sa::IdeDb ide;
	for (auto &d : dat)
		if (d.kind == "IDE") ide.LoadFile(Join(root, d.arg));

	size_t cgRandom = 0, cgFixed = 0, cgBad = 0, cgOut = 0;
	std::set<int> cgBadIds;
	size_t files = 0, parseFail = 0, insts = 0, noDef = 0, badQuat = 0, badPos = 0, badLod = 0, interior = 0, carGens = 0;
	std::set<int> noDefIds;
	for (const char *imgName : {"models/gta3.img", "models/gta_int.img"}) {
		sa::ImgV2 img;
		if (!img.Open(Join(root, imgName))) { printf("cannot open %s\n", imgName); continue; }
		for (auto &e : img.Entries()) {
			auto dot = e.name.rfind('.');
			if (dot == std::string::npos || strcasecmp(e.name.c_str() + dot, ".ipl") != 0) continue;
			std::vector<uint8_t> buf;
			if (!img.Read(e, buf)) { parseFail++; continue; }
			sa::BinaryIpl ipl;
			const char *err = nullptr;
			if (!sa::ParseBinaryIpl(buf.data(), buf.size(), ipl, &err)) {
				if (memcmp(buf.data(), "bnry", 4) == 0) { printf("FAIL %s/%s: %s\n", imgName, e.name.c_str(), err); parseFail++; }
				continue; // text IPLs inside archives are not expected
			}
			files++;
			carGens += ipl.numCarGens;
			for (auto &g : ipl.carGens) {
				if (g.modelId < 0) cgRandom++;
				else if (ide.FindVehicle(g.modelId)) cgFixed++;
				else { cgBad++; cgBadIds.insert(g.modelId); }
				if (std::fabs(g.pos[0]) > 4000 || std::fabs(g.pos[1]) > 4000 || std::fabs(g.angle) > 720) cgOut++;
			}
			for (auto &i : ipl.insts) {
				insts++;
				if (!ide.HasModel(i.modelId)) { noDef++; noDefIds.insert(i.modelId); }
				float q = i.rot[0] * i.rot[0] + i.rot[1] * i.rot[1] + i.rot[2] * i.rot[2] + i.rot[3] * i.rot[3];
				if (std::fabs(q - 1.0f) > 0.01f) badQuat++;
				if (std::fabs(i.pos[0]) > 4000 || std::fabs(i.pos[1]) > 4000 || std::fabs(i.pos[2]) > 2000) badPos++;
				// lodIndex points into the parent IPL, not this file; only reject values below -1 here.
				if (i.lodIndex < -1) badLod++;
				if (i.flags & 0xFF) interior++;
			}
		}
	}
	printf("binary IPL files parsed=%zu failed=%zu\n", files, parseFail);
	printf("instances=%zu, car generators=%zu, with interior byte set=%zu\n", insts, carGens, interior);
	printf("car generators: fixed vehicle model=%zu random(-1)=%zu unknown model=%zu (%zu ids) out of range=%zu\n", cgFixed, cgRandom, cgBad, cgBadIds.size(), cgOut);
	for (int id : cgBadIds) printf("  unknown car gen model id %d\n", id);
	printf("checks: no IDE object def=%zu (%zu ids), non-unit quaternion=%zu, position out of +-4000=%zu, bad lod index=%zu\n",
	       noDef, noDefIds.size(), badQuat, badPos, badLod);
	return 0;
}
