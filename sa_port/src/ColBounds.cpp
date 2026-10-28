#include "ColBounds.h"

#include <cctype>
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

} // namespace sa
