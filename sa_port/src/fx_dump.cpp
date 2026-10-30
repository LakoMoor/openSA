#include "FxData.h"
#include <cstdio>
// usage: fx_dump <effects.fxp> [system]   lists systems (or one in detail)
int main(int argc, char **argv)
{
	if (argc < 2) { std::printf("usage: fx_dump <effects.fxp> [system]\n"); return 1; }
	FxProject p;
	if (!p.load(argv[1])) { std::printf("cannot load\n"); return 1; }
	if (argc < 3) {
		for (const FxSystem &s : p.systems) {
			std::printf("%-28s prims %zu txd %s\n", s.name.c_str(), s.prims.size(), s.txd.c_str());
		}
		std::printf("%zu systems\n", p.systems.size());
		return 0;
	}
	const FxSystem *s = p.find(argv[2]);
	if (!s) { std::printf("not found\n"); return 1; }
	std::printf("%s txd %s\n", s->name.c_str(), s->txd.c_str());
	for (auto &kv : s->props) std::printf("  %s = %s\n", kv.first.c_str(), kv.second.c_str());
	for (const FxPrim &pr : s->prims) {
		std::printf(" prim\n");
		for (auto &kv : pr.base) std::printf("   %s = %s\n", kv.first.c_str(), kv.second.c_str());
		for (const FxInfo &i : pr.infos) {
			std::printf("   info %s\n", i.type.c_str());
			for (auto &kv : i.scalars) std::printf("     %s = %g\n", kv.first.c_str(), kv.second);
			for (auto &kv : i.chans) {
				std::printf("     %s%s:", kv.first.c_str(), kv.second.looped ? " (loop)" : "");
				for (auto &k : kv.second.keys) std::printf(" [%g]=%g", k.first, k.second);
				std::printf("\n");
			}
		}
	}
	return 0;
}
