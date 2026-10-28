#include "RwChunks.h"
#include <cstring>

namespace sa {

static bool IsContainer(uint32_t t) {
	switch (t) {
	case kRwExtension: case kRwTexture: case kRwMaterial: case kRwMaterialList:
	case kRwFrameList: case kRwGeometry: case kRwClump: case kRwAtomic:
	case kRwTextureNative: case kRwTexDict: case kRwGeometryList: case kRwUvAnimDict:
		return true;
	}
	return false;
}

// parentType is used so children of Extension are treated as opaque plugin chunks (their payload is plugin data).
static bool Walk(const uint8_t *p, size_t avail, uint32_t parentType, RwWalkStats &st, const char **err) {
	size_t off = 0;
	while (off < avail) {
		if (avail - off < sizeof(RwChunkHeader)) { *err = "truncated chunk header"; return false; }
		RwChunkHeader h;
		memcpy(&h, p + off, sizeof(h));
		off += sizeof(h);
		if (h.size > avail - off) { *err = "chunk size exceeds parent"; return false; }
		st.chunks++;
		st.byType[h.type]++;
		st.versions[h.version]++;
		if (parentType == kRwExtension) {
			st.unknownPlugins[h.type]++;
		} else {
			if (h.type == kRwGeometry) st.geometries++;
			if (h.type == kRwAtomic) st.atomics++;
			if (h.type == kRwTextureNative) st.textures++;
			if (IsContainer(h.type) && h.size > 0)
				if (!Walk(p + off, h.size, h.type, st, err)) return false;
		}
		off += h.size;
	}
	return true;
}

bool WalkRwStream(const uint8_t *data, size_t size, RwWalkStats &stats, const char **err) {
	const char *dummy;
	if (!err) err = &dummy;
	if (size < sizeof(RwChunkHeader)) { *err = "too small"; return false; }
	// A file holds one or more top-level chunks: [UV animation dictionary 0x2B] then Clump 0x10, or one TexDict 0x16.
	// Archive entries are padded to 2048-byte sectors; the walk stops at the first all-zero header.
	size_t off = 0;
	bool sawMain = false;
	while (off + sizeof(RwChunkHeader) <= size) {
		RwChunkHeader h;
		memcpy(&h, data + off, sizeof(h));
		if (h.type == 0 && h.size == 0 && h.version == 0) break;
		if (h.type != kRwClump && h.type != kRwTexDict && h.type != kRwUvAnimDict) {
			*err = "unexpected top-level chunk";
			return false;
		}
		if (uint64_t(h.size) + sizeof(h) > size - off) { *err = "top-level size exceeds data"; return false; }
		if (h.type != kRwUvAnimDict) sawMain = true;
		if (!Walk(data + off, h.size + sizeof(h), 0, stats, err)) return false;
		off += h.size + sizeof(h);
	}
	if (!sawMain) { *err = "no clump or texture dictionary"; return false; }
	return true;
}

static bool ReadNative(const uint8_t *p, size_t n, RwNativeTexInfo &t) {
	// struct payload: u32 platform, u32 filter/addr, char name[32], char mask[32], u32 rasterFormat,
	// u32 d3dFormat, u16 w, u16 h, u8 depth, u8 levels, u8 type, u8 flags
	if (n < 4 + 4 + 32 + 32 + 4 + 4 + 2 + 2 + 4) return false;
	memcpy(&t.platform, p, 4);
	if (t.platform != 8 && t.platform != 9) return false;
	char name[33] = {}, mask[33] = {};
	memcpy(name, p + 8, 32);
	memcpy(mask, p + 40, 32);
	t.name = name;
	t.mask = mask;
	memcpy(&t.rasterFormat, p + 72, 4);
	memcpy(&t.d3dFormat, p + 76, 4);
	memcpy(&t.width, p + 80, 2);
	memcpy(&t.height, p + 82, 2);
	t.depth = p[84];
	t.numLevels = p[85];
	t.rasterType = p[86];
	t.flags = p[87];
	return true;
}

bool ReadTxdTextures(const uint8_t *data, size_t size, std::vector<RwNativeTexInfo> &out, const char **err) {
	const char *dummy;
	if (!err) err = &dummy;
	if (size < 12) { *err = "too small"; return false; }
	RwChunkHeader top;
	memcpy(&top, data, 12);
	if (top.type != kRwTexDict || uint64_t(top.size) + 12 > size) { *err = "not a texture dictionary"; return false; }
	size_t off = 12, end = 12 + top.size;
	while (off + 12 <= end) {
		RwChunkHeader h;
		memcpy(&h, data + off, 12);
		size_t payload = off + 12;
		if (h.size > end - payload) { *err = "child exceeds dictionary"; return false; }
		if (h.type == kRwTextureNative) {
			// first child is a Struct chunk carrying the texture header
			if (h.size >= 12) {
				RwChunkHeader sh;
				memcpy(&sh, data + payload, 12);
				if (sh.type == kRwStruct && sh.size <= h.size - 12) {
					RwNativeTexInfo t = {};
					if (ReadNative(data + payload + 12, sh.size, t)) out.push_back(t);
				}
			}
		}
		off = payload + h.size;
	}
	return true;
}

} // namespace sa
