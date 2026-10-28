#include "Timecyc.h"

#include <cstdio>
#include <cstring>

namespace sa {

const int Timecyc::kHourOfKey[Timecyc::kHours] = {0, 5, 6, 7, 12, 19, 20, 22};

bool Timecyc::Load(const std::string &path) {
	FILE *f = fopen(path.c_str(), "r");
	if (!f) return false;
	char line[1024];
	m_keys.clear();
	while (fgets(line, sizeof line, f)) {
		const char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '/' || *p == '\r' || *p == '\n' || !*p) continue;
		int v[21];
		int n = sscanf(p, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8],
		               &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15], &v[16], &v[17], &v[18], &v[19], &v[20]);
		TimecycKey k{};
		// the float columns follow the 21 colour ints: sizes (3 floats), 3 shadow ints, farclip, fogstart
		if (n == 21) {
			for (int i = 0; i < 3; i++) {
				k.ambient[i] = (float)v[i];
				k.ambientObj[i] = (float)v[3 + i];
				k.directional[i] = (float)v[6 + i];
				k.skyTop[i] = (float)v[9 + i];
				k.skyBottom[i] = (float)v[12 + i];
				k.sunCore[i] = (float)v[15 + i];
				k.sunCorona[i] = (float)v[18 + i];
			}
			float a, b, c, farClip, fogStart, rest[11] = {};
			int s0, s1, s2;
			// skip the 21 ints, then read the remaining columns
			const char *q = p;
			for (int i = 0; i < 21; i++) {
				while (*q == ' ' || *q == '\t') q++;
				while (*q && *q != ' ' && *q != '\t') q++;
			}
			if (sscanf(q, "%f %f %f %d %d %d %f %f %f %f %f %f %f %f %f %f %f %f %f", &a, &b, &c, &s0, &s1, &s2, &farClip, &fogStart, rest, rest + 1, rest + 2, rest + 3,
			           rest + 4, rest + 5, rest + 6, rest + 7, rest + 8, rest + 9, rest + 10) >= 8) {
				// after fogStart: lightOnGround, low cloud rgb, bottom cloud rgb, water rgb + alpha (column order from CTimeCycle::Initialise)
				for (int i = 0; i < 4; i++) k.water[i] = rest[7 + i];
				k.sunSize = (float)(int)(a * 10.0f + 0.5f); // stored as int8(sunSize*10+0.5) by CTimeCycle::Initialise
				k.farClip = farClip;
				k.fogStart = fogStart;
				k.ok = true;
			}
		}
		m_keys.push_back(k);
	}
	fclose(f);
	return !m_keys.empty();
}

TimecycKey Timecyc::Sample(int weather, float hour) const {
	TimecycKey out{};
	if (weather < 0 || weather >= NumWeathers()) return out;
	const TimecycKey *w = &m_keys[weather * kHours];
	int a = kHours - 1;
	for (int i = 0; i < kHours; i++)
		if (hour >= kHourOfKey[i]) a = i;
	int b = (a + 1) % kHours;
	float ha = (float)kHourOfKey[a], hb = (float)kHourOfKey[b];
	if (hb <= ha) hb += 24.0f;
	float h = hour < ha ? hour + 24.0f : hour;
	float t = (h - ha) / (hb - ha);
	auto mix = [&](const float *x, const float *y, float *o) { for (int i = 0; i < 3; i++) o[i] = x[i] + (y[i] - x[i]) * t; };
	const TimecycKey &A = w[a], &B = w[b];
	mix(A.ambient, B.ambient, out.ambient);
	mix(A.ambientObj, B.ambientObj, out.ambientObj);
	mix(A.directional, B.directional, out.directional);
	mix(A.skyTop, B.skyTop, out.skyTop);
	mix(A.skyBottom, B.skyBottom, out.skyBottom);
	mix(A.sunCore, B.sunCore, out.sunCore);
	mix(A.sunCorona, B.sunCorona, out.sunCorona);
	for (int i = 0; i < 4; i++) out.water[i] = A.water[i] + (B.water[i] - A.water[i]) * t;
	out.sunSize = A.sunSize + (B.sunSize - A.sunSize) * t;
	out.farClip = A.farClip + (B.farClip - A.farClip) * t;
	out.fogStart = A.fogStart + (B.fogStart - A.fogStart) * t;
	out.ok = A.ok && B.ok;
	return out;
}

TimecycKey Timecyc::Lerp(const TimecycKey &a, const TimecycKey &b, float t) {
	TimecycKey o{};
	auto mix = [&](const float *x, const float *y, float *r) { for (int i = 0; i < 3; i++) r[i] = x[i] + (y[i] - x[i]) * t; };
	mix(a.ambient, b.ambient, o.ambient);
	mix(a.ambientObj, b.ambientObj, o.ambientObj);
	mix(a.directional, b.directional, o.directional);
	mix(a.skyTop, b.skyTop, o.skyTop);
	mix(a.skyBottom, b.skyBottom, o.skyBottom);
	mix(a.sunCore, b.sunCore, o.sunCore);
	mix(a.sunCorona, b.sunCorona, o.sunCorona);
	for (int i = 0; i < 4; i++) o.water[i] = a.water[i] + (b.water[i] - a.water[i]) * t;
	o.sunSize = a.sunSize + (b.sunSize - a.sunSize) * t;
	o.farClip = a.farClip + (b.farClip - a.farClip) * t;
	o.fogStart = a.fogStart + (b.fogStart - a.fogStart) * t;
	o.ok = a.ok && b.ok;
	return o;
}

} // namespace sa
