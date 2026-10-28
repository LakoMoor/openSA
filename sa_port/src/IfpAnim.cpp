#include "IfpAnim.h"

#include <cctype>
#include <cstring>

namespace sa {

size_t ParseIfp(const std::vector<uint8_t> &data, std::unordered_map<std::string, IfpAnim> &out) {
	if (data.size() < 36 || memcmp(data.data(), "ANP3", 4)) return 0;
	const uint8_t *d = data.data();
	const size_t size = data.size();
	auto u32 = [&](size_t p) { uint32_t v; memcpy(&v, d + p, 4); return v; };
	auto name = [&](size_t p) {
		char b[25] = {};
		memcpy(b, d + p, 24);
		std::string s(b);
		for (auto &c : s) c = (char)tolower((unsigned char)c);
		return s;
	};
	uint32_t numAnims = u32(32);
	size_t p = 36, count = 0;
	for (uint32_t a = 0; a < numAnims; a++) {
		if (p + 36 > size) return 0;
		std::string an = name(p);
		uint32_t numSeq = u32(p + 24);
		p += 36;
		IfpAnim anim;
		for (uint32_t s = 0; s < numSeq; s++) {
			if (p + 36 > size) return 0;
			uint32_t type = u32(p + 24), nf = u32(p + 28);
			IfpSeq seq;
			seq.boneTag = (int)u32(p + 32);
			p += 36;
			if (type != 3 && type != 4) return 0; // uncompressed key types do not occur in ped.ifp
			seq.hasTrans = type == 4;
			size_t ks = type == 4 ? 16 : 10;
			if (p + nf * ks > size) return 0;
			seq.keys.resize(nf);
			for (uint32_t k = 0; k < nf; k++) {
				const uint8_t *q = d + p + k * ks;
				int16_t v[8];
				memcpy(v, q, ks);
				IfpKey &key = seq.keys[k];
				for (int i = 0; i < 4; i++) key.q[i] = v[i] / 4096.0f;
				key.time = v[4] / 60.0f;
				if (type == 4) for (int i = 0; i < 3; i++) key.t[i] = v[5 + i] / 1024.0f;
				if (key.time > anim.duration) anim.duration = key.time;
			}
			p += nf * ks;
			anim.seqs.push_back(std::move(seq));
		}
		out[an] = std::move(anim);
		count++;
	}
	return p == size ? count : 0;
}

} // namespace sa
