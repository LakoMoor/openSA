#include "ImgV2.h"
#include "TextData.h"
#include <algorithm>
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

int main(int argc, char **argv) {
	if (argc < 2) { puts("usage: load_all <SA game dir>"); return 2; }
	std::string root = argv[1];
	// The game reads data/default.dat first (default/vehicle/ped IDE), then data/gta.dat.
	std::vector<sa::DatDirective> dat;
	if (!sa::LoadGtaDat(root + "/data/default.dat", dat)) { puts("cannot read data/default.dat"); return 1; }
	if (!sa::LoadGtaDat(root + "/data/gta.dat", dat)) { puts("cannot read data/gta.dat"); return 1; }

	sa::IdeDb ide;
	std::vector<std::string> ipls;
	int ideOk = 0, ideFail = 0;
	for (auto &d : dat) {
		if (d.kind == "IDE") (ide.LoadFile(Join(root, d.arg)) ? ideOk : ideFail)++;
		else if (d.kind == "IPL") ipls.push_back(d.arg);
	}
	if (ide.Stats().badLines) printf("first bad IDE line: %s\n", ide.Stats().firstBadLine.c_str());
	printf("default.dat+gta.dat: %zu directives; IDE files ok=%d failed=%d; object defs=%zu, bad lines=%d\n",
	       dat.size(), ideOk, ideFail, ide.ObjectCount(), ide.Stats().badLines);

	printf("IDE: vehicles=%zu peds=%zu weapons=%zu anim-objects=%zu txd-parents=%zu\n", ide.VehicleCount(),
	       ide.PedCount(), ide.WeaponCount(), ide.AnimObjCount(), ide.TxdParentCount());
	for (auto &kv : ide.Stats().sections) printf("  [%s] %d\n", kv.first.c_str(), kv.second);

	// Streaming IPLs live inside gta3.img / gta_int.img as binary "bnry" files; text IPLs are on disk.
	size_t totalInst = 0;
	int textOk = 0, textFail = 0, missingDef = 0, nameMismatch = 0;
	std::set<int> missingIds;
	for (auto &p : ipls) {
		sa::IplText ipl;
		bool bin = false;
		if (!sa::LoadIplText(Join(root, p), ipl, bin)) { textFail++; continue; }
		textOk++;
		if (bin) continue;
		totalInst += ipl.insts.size();
		for (auto &i : ipl.insts) {
			if (!ide.HasModel(i.id)) { missingDef++; missingIds.insert(i.id); continue; }
			if (const sa::ObjectDef *o = ide.Find(i.id))
				if (strcasecmp(o->model.c_str(), i.model.c_str()) != 0) nameMismatch++;
		}
	}
	printf("text IPL files ok=%d failed=%d; inst=%zu; inst without IDE def=%d (%zu ids); model-name mismatches=%d\n",
	       textOk, textFail, totalInst, missingDef, missingIds.size(), nameMismatch);

	sa::ImgV2 img;
	if (img.Open(root + "/models/gta3.img")) {
		int bnry = 0, text = 0;
		for (auto &e : img.Entries()) {
			auto dot = e.name.rfind('.');
			if (dot == std::string::npos || strcasecmp(e.name.c_str() + dot, ".ipl") != 0) continue;
			std::vector<uint8_t> d;
			if (!img.Read(e, d) || d.size() < 4) continue;
			(memcmp(d.data(), "bnry", 4) == 0 ? bnry : text)++;
		}
		printf("gta3.img IPL: binary=%d text=%d\n", bnry, text);
	}
	return 0;
}
