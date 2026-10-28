// Loads GTA SA clumps from IMG archives through librw (NULL platform, no GPU) and prints geometry stats.
#include "ImgV2.h"
#include <rw.h>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#ifdef _MSC_VER
#define strcasecmp _stricmp
#endif

static bool HasExt(const std::string &n, const char *ext) {
	auto d = n.rfind('.');
	return d != std::string::npos && strcasecmp(n.c_str() + d, ext) == 0;
}

struct ClumpStats {
	int atomics = 0, geometries = 0, frames = 0;
	long long vertices = 0, triangles = 0, materials = 0;
	std::set<std::string> textures;
};

static bool LoadClump(std::vector<uint8_t> &buf, ClumpStats &st) {
	rw::StreamMemory stream;
	stream.open(buf.data(), (rw::uint32)buf.size());
	rw::uint32 length = 0, version = 0;
	// Files may start with a UV animation dictionary (0x2B); skip to the clump chunk.
	if (!rw::findChunk(&stream, rw::ID_CLUMP, &length, &version)) {
		stream.close();
		return false;
	}
	rw::Clump *clump = rw::Clump::streamRead(&stream);
	stream.close();
	if (!clump) return false;
	FORLIST(lnk, clump->atomics) {
		rw::Atomic *a = rw::Atomic::fromClump(lnk);
		st.atomics++;
		if (a->geometry) {
			rw::Geometry *g = a->geometry;
			st.geometries++;
			st.vertices += g->numVertices;
			st.triangles += g->numTriangles;
			st.materials += g->matList.numMaterials;
			for (int i = 0; i < g->matList.numMaterials; i++) {
				rw::Material *m = g->matList.materials[i];
				if (m && m->texture) st.textures.insert(m->texture->name);
			}
		}
	}
	clump->destroy();
	return true;
}

int main(int argc, char **argv) {
	if (argc < 2) { puts("usage: rw_load <SA game dir> [model.dff]"); return 2; }
	std::string root = argv[1];

	rw::Engine::init();
	rw::registerHAnimPlugin();
	rw::registerMatFXPlugin();
	rw::registerSkinPlugin();
	rw::Engine::open(nil);
	rw::Engine::start();

	if (argc > 2) {
		sa::ImgV2 img;
		for (const char *n : {"models/gta3.img", "models/gta_int.img", "models/player.img"}) {
			if (!img.Open(root + "/" + n)) continue;
			if (auto *e = img.Find(argv[2])) {
				std::vector<uint8_t> buf;
				ClumpStats st;
				if (img.Read(*e, buf) && LoadClump(buf, st)) {
					printf("%s (%s): atomics=%d geometries=%d vertices=%lld triangles=%lld materials=%lld\n", e->name.c_str(), n,
					       st.atomics, st.geometries, st.vertices, st.triangles, st.materials);
					printf("  textures:");
					for (auto &t : st.textures) printf(" %s", t.c_str());
					printf("\n");
				} else printf("%s: librw failed to load clump\n", e->name.c_str());
				return 0;
			}
		}
		puts("not found");
		return 1;
	}

	for (const char *imgName : {"models/gta3.img", "models/gta_int.img", "models/player.img"}) {
		sa::ImgV2 img;
		if (!img.Open(root + "/" + imgName)) continue;
		int ok = 0, bad = 0;
		long long verts = 0, tris = 0, mats = 0;
		std::vector<std::string> fails;
		for (auto &e : img.Entries()) {
			if (!HasExt(e.name, ".dff")) continue;
			std::vector<uint8_t> buf;
			ClumpStats st;
			if (img.Read(e, buf) && LoadClump(buf, st)) {
				ok++; verts += st.vertices; tris += st.triangles; mats += st.materials;
			} else {
				bad++;
				if (fails.size() < 5) fails.push_back(e.name);
			}
		}
		printf("%s: loaded=%d failed=%d, vertices=%lld triangles=%lld materials=%lld\n", imgName, ok, bad, verts, tris, mats);
		for (auto &f : fails) printf("  FAIL %s\n", f.c_str());
	}
	return 0;
}
