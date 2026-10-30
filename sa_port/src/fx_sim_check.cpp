#include "FxSim.h"
#include <cstdio>
#include <cstdlib>
#include <cfloat>
// usage: fx_sim_check <effects.fxp> <system> [seconds]   runs the first primitive at 50 Hz and prints particle statistics
int main(int argc, char **argv)
{
	if (argc < 3) { std::printf("usage: fx_sim_check <effects.fxp> <system> [seconds]\n"); return 1; }
	FxProject p; if (!p.load(argv[1])) return 1;
	const FxSystem *s = p.find(argv[2]); if (!s || s->prims.empty()) { std::printf("not found\n"); return 1; }
	float secs = argc > 3 ? (float)atof(argv[3]) : 1.0f;
	FxEmitterSim sim(s->prims[0], *s);
	float o[3] = {0, 0, 0};
	size_t peak = 0;
	for (int f = 0; f < (int)(secs * 50); f++) { sim.update(0.02f, o); if (sim.alive() > peak) peak = sim.alive(); }
	std::vector<FxBillboard> b; sim.collect(b);
	float mn[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, mx[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
	for (auto &q : b) for (int k = 0; k < 3; k++) { if (q.pos[k] < mn[k]) mn[k] = q.pos[k]; if (q.pos[k] > mx[k]) mx[k] = q.pos[k]; }
	std::printf("%s after %.1f s: alive %zu peak %zu", argv[2], secs, sim.alive(), peak);
	if (!b.empty()) std::printf(" bbox (%.2f %.2f %.2f)-(%.2f %.2f %.2f) first half %.2fx%.2f rgba %d %d %d %d", mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], b[0].halfX, b[0].halfY, b[0].r, b[0].g, b[0].b, b[0].a);
	std::printf("\n");
}
