#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace sa {

// Bounding sphere of one collision model, as stored in the COL file header (not the triangle data).
// Layout from gta-reversed ColHelpers.h: COL1 = {radius, center, min, max}; COL2/3/4 = {min, max, center, radius}.
struct ColBound {
	float radius = 0;
	float center[3] = {};
};

// Walks the concatenated COL records of one file ("COLL"/"COL2"/"COL3"/"COL4", u32 size, 22-byte name, u16 id, bounds, ...)
// and stores the bound under the lower-case model name. Returns the number of records read.
size_t ParseColBounds(const uint8_t *data, size_t size, std::unordered_map<std::string, ColBound> &out);

} // namespace sa
