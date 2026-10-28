#include "ColBounds.h"
#include "ImgV2.h"
#include <cstdio>
#include <fstream>
int main(int, char **argv) {
	sa::ImgV2 img;
	if (!img.Open(std::string(argv[1]) + "/models/gta3.img")) { puts("open failed"); return 1; }
	std::unordered_map<std::string, sa::ColModel> m;
	size_t recs = 0, tris = 0, boxes = 0, nonempty = 0;
	for (auto &e : img.Entries()) {
		auto d = e.name.rfind('.');
		if (d == std::string::npos || e.name.substr(d) != ".col") continue;
		std::vector<uint8_t> b;
		if (img.Read(e, b)) recs += sa::ParseColModels(b.data(), b.size(), m);
	}
	for (auto &kv : m) { tris += kv.second.tris.size(); boxes += kv.second.boxes.size(); nonempty += !kv.second.tris.empty() || !kv.second.boxes.empty(); }
	printf("records %zu models %zu with geometry %zu tris %zu boxes %zu\n", recs, m.size(), nonempty, tris, boxes);
	{
		std::ifstream f(std::string(argv[1]) + "/models/coll/peds.col", std::ios::binary);
		std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
		std::unordered_map<std::string, sa::ColModel> pm;
		sa::ParseColModels(b.data(), b.size(), pm);
		for (auto &kv : pm) { printf("peds.col %s r=%.2f spheres:", kv.first.c_str(), kv.second.bound.radius); for (auto &sp : kv.second.spheres) printf(" (%.2f,%.2f,%.2f r%.2f)", sp.center[0], sp.center[1], sp.center[2], sp.radius); puts(""); }
	}
	auto it = m.find("lae2_roads89"); if (it != m.end()) printf("lae2_roads89 tris %zu verts %zu\n", it->second.tris.size(), it->second.verts.size() / 3);
}
