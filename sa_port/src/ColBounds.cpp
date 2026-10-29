#include "ColBounds.h"

#include <cctype>
#include <algorithm>
#include <cstring>

namespace sa {

size_t ParseColBounds(const uint8_t *data, size_t size, std::unordered_map<std::string, ColBound> &out) {
	size_t pos = 0, count = 0;
	while (pos + 8 + 24 + 40 <= size) {
		const uint8_t *h = data + pos;
		bool v1 = !memcmp(h, "COLL", 4);
		if (!v1 && memcmp(h, "COL2", 4) && memcmp(h, "COL3", 4) && memcmp(h, "COL4", 4)) break;
		uint32_t len;
		memcpy(&len, h + 4, 4);
		if (len < 24 + 40 || pos + 8 + len > size) break;
		char name[23] = {};
		memcpy(name, h + 8, 22);
		std::string key(name);
		for (auto &c : key) c = (char)tolower((unsigned char)c);
		float b[10];
		memcpy(b, h + 8 + 24, sizeof(b));
		ColBound cb;
		if (v1) {
			cb.radius = b[0];
			memcpy(cb.center, b + 1, 12);
		} else {
			memcpy(cb.center, b + 6, 12);
			cb.radius = b[9];
		}
		out[key] = cb;
		count++;
		pos += 8 + len;
	}
	return count;
}

size_t ParseColModels(const uint8_t *data, size_t size, std::unordered_map<std::string, ColModel> &out) {
	size_t pos = 0, count = 0;
	while (pos + 8 + 24 + 40 <= size) {
		const uint8_t *h = data + pos;
		int ver = !memcmp(h, "COLL", 4) ? 1 : !memcmp(h, "COL2", 4) ? 2 : !memcmp(h, "COL3", 4) ? 3 : !memcmp(h, "COL4", 4) ? 4 : 0;
		if (!ver) break;
		uint32_t len;
		memcpy(&len, h + 4, 4);
		if (len < 24 + 40 || pos + 8 + len > size) break;
		char name[23] = {};
		memcpy(name, h + 8, 22);
		std::string key(name);
		for (auto &ch : key) ch = (char)tolower((unsigned char)ch);
		ColModel &m = out[key];
		m = ColModel{};
		float b[10];
		memcpy(b, h + 32, sizeof b);
		if (ver == 1) { m.bound.radius = b[0]; memcpy(m.bound.center, b + 1, 12); }
		else { memcpy(m.bound.center, b + 6, 12); m.bound.radius = b[9]; }
		const size_t recEnd = pos + 8 + len;
		if (ver >= 2 && pos + 32 + 40 + 36 <= recEnd) {
			const uint8_t *p = h + 32 + 40;
			uint16_t nSph, nBox, nFace;
			uint32_t offSph, offBox, offVert, offFace;
			memcpy(&nSph, p, 2); memcpy(&nBox, p + 2, 2); memcpy(&nFace, p + 4, 2);
			memcpy(&offSph, p + 12, 4); memcpy(&offBox, p + 16, 4); memcpy(&offVert, p + 24, 4); memcpy(&offFace, p + 28, 4);
			auto inRange = [&](uint32_t off, size_t bytes) { return (size_t)off + 4 + bytes <= 8 + (size_t)len; };
			if (nSph && offSph && inRange(offSph, (size_t)nSph * 20)) {
				for (int i = 0; i < nSph; i++) {
					float f[4];
					memcpy(f, h + 4 + offSph + i * 20, 16);
					m.spheres.push_back({{f[0], f[1], f[2]}, f[3]}); // COL sphere: center xyz, radius, surface (20 bytes)
				}
			}
			if (nBox && offBox && inRange(offBox, (size_t)nBox * 28)) {
				for (int i = 0; i < nBox; i++) {
					float f[6];
					memcpy(f, h + 4 + offBox + i * 28, 24);
					m.boxes.push_back({{f[0], f[1], f[2]}, {f[3], f[4], f[5]}});
				}
			}
			if (nFace && offFace && offVert && offVert < offFace && inRange(offFace, (size_t)nFace * 8)) {
				// the vertex array runs from offVert to offFace; its length is implied by the highest index used
				int maxIdx = 0;
				m.tris.resize(nFace);
				for (int i = 0; i < nFace; i++) {
					const uint8_t *f = h + 4 + offFace + i * 8;
					ColTri t;
					memcpy(&t.a, f, 2); memcpy(&t.b, f + 2, 2); memcpy(&t.c, f + 4, 2);
					t.material = f[6];
					m.tris[i] = t;
					maxIdx = std::max<int>(maxIdx, std::max<int>(t.a, std::max<int>(t.b, t.c)));
				}
				size_t nVert = (size_t)maxIdx + 1;
				if ((size_t)offVert + 4 + nVert * 6 <= 8 + (size_t)len) {
					m.verts.resize(nVert * 3);
					for (size_t v = 0; v < nVert; v++) {
						int16_t s[3];
						memcpy(s, h + 4 + offVert + v * 6, 6);
						for (int k = 0; k < 3; k++) m.verts[v * 3 + k] = s[k] / 128.0f;
					}
				} else
					m.tris.clear();
			}
		}
		count++;
		pos = recEnd;
	}
	return count;
}

} // namespace sa
