#include "WaterDat.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sa {

bool LoadWaterDat(const std::string &path, std::vector<WaterPoly> &out) {
	FILE *f = fopen(path.c_str(), "r");
	if (!f) return false;
	char line[1024];
	while (fgets(line, sizeof line, f)) {
		const char *p = line;
		WaterPoly w;
		while (w.n < 4) {
			float v[7];
			int used = 0, k = 0;
			const char *q = p;
			for (; k < 7; k++) {
				char *end;
				v[k] = strtof(q, &end);
				if (end == q) break;
				q = end;
			}
			(void)used;
			if (k < 7) break;
			w.x[w.n] = v[0]; w.y[w.n] = v[1]; w.z[w.n] = v[2];
			w.bigWaves[w.n] = v[5]; w.smallWaves[w.n] = v[6];
			w.n++;
			p = q;
		}
		if (w.n < 3) continue; // the "processed" header and malformed lines
		w.flags = atoi(p);
		out.push_back(w);
	}
	fclose(f);
	return !out.empty();
}

} // namespace sa
