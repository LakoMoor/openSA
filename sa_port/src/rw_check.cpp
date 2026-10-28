#include "ImgV2.h"
#include "RwChunks.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

#ifdef _MSC_VER
#define strcasecmp _stricmp
#endif

static bool HasExt(const std::string &n, const char *ext) {
	auto d = n.rfind('.');
	return d != std::string::npos && strcasecmp(n.c_str() + d, ext) == 0;
}

static void PrintMap(const char *title, const std::map<uint32_t, uint64_t> &m, int limit = 12) {
	printf("  %s:", title);
	int i = 0;
	for (auto &kv : m) {
		if (i++ >= limit) { printf(" ..."); break; }
		printf(" 0x%X=%llu", kv.first, (unsigned long long)kv.second);
	}
	printf("\n");
}

int main(int argc, char **argv) {
	if (argc < 2) { puts("usage: rw_check <SA game dir>"); return 2; }
	std::string root = argv[1];

	for (const char *imgName : {"models/gta3.img", "models/gta_int.img", "models/player.img"}) {
		sa::ImgV2 img;
		if (!img.Open(root + "/" + imgName)) { printf("cannot open %s\n", imgName); continue; }
		size_t dffOk = 0, dffBad = 0, txdOk = 0, txdBad = 0;
		sa::RwWalkStats dffStats, txdStats;
		std::map<std::string, uint64_t> platforms, formats;
		size_t texCount = 0, npot = 0, noMips = 0;
		std::vector<std::string> failures;
		for (auto &e : img.Entries()) {
			bool dff = HasExt(e.name, ".dff"), txd = HasExt(e.name, ".txd");
			if (!dff && !txd) continue;
			std::vector<uint8_t> buf;
			if (!img.Read(e, buf)) { (dff ? dffBad : txdBad)++; continue; }
			const char *err = nullptr;
			bool ok = sa::WalkRwStream(buf.data(), buf.size(), dff ? dffStats : txdStats, &err);
			if (!ok) {
				(dff ? dffBad : txdBad)++;
				if (failures.size() < 5) failures.push_back(e.name + ": " + err);
				continue;
			}
			(dff ? dffOk : txdOk)++;
			if (txd) {
				std::vector<sa::RwNativeTexInfo> tex;
				if (sa::ReadTxdTextures(buf.data(), buf.size(), tex, &err)) {
					for (auto &t : tex) {
						texCount++;
						platforms[t.platform == 9 ? "D3D9" : "D3D8"]++;
						char fmt[16];
						if (t.d3dFormat >= 0x20202020) { memcpy(fmt, &t.d3dFormat, 4); fmt[4] = 0; }
						else snprintf(fmt, sizeof(fmt), "raw%u", t.d3dFormat);
						formats[fmt]++;
						auto pot = [](uint32_t v) { return v && !(v & (v - 1)); };
						if (!pot(t.width) || !pot(t.height)) npot++;
						if (t.numLevels <= 1) noMips++;
					}
				}
			}
		}
		printf("%s: DFF ok=%zu bad=%zu | TXD ok=%zu bad=%zu\n", imgName, dffOk, dffBad, txdOk, txdBad);
		for (auto &f : failures) printf("  FAIL %s\n", f.c_str());
		if (dffOk) {
			printf("  DFF geometries=%u atomics=%u chunks=%llu\n", dffStats.geometries, dffStats.atomics, (unsigned long long)dffStats.chunks);
			PrintMap("DFF versions", dffStats.versions);
			PrintMap("DFF extension plugin chunk types", dffStats.unknownPlugins, 16);
		}
		if (txdOk) {
			printf("  TXD textures (native)=%zu, non-power-of-two=%zu, single-level=%zu\n", texCount, npot, noMips);
			PrintMap("TXD versions", txdStats.versions);
			printf("  platforms:");
			for (auto &kv : platforms) printf(" %s=%llu", kv.first.c_str(), (unsigned long long)kv.second);
			printf("\n  formats:");
			for (auto &kv : formats) printf(" %s=%llu", kv.first.c_str(), (unsigned long long)kv.second);
			printf("\n");
		}
	}
	return 0;
}
