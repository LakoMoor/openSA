#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Minimal RenderWare binary-stream walker (DFF = Clump 0x10, TXD = TexDictionary 0x16).
// It validates structure only: every container's children must exactly fill its payload.
namespace sa {

#pragma pack(push, 1)
struct RwChunkHeader {
	uint32_t type;
	uint32_t size;    // payload size, excluding this 12-byte header
	uint32_t version; // library stamp, SA models use 0x1803FFFF (RW 3.6.0.3)
};
#pragma pack(pop)
static_assert(sizeof(RwChunkHeader) == 12, "rw chunk header");

enum : uint32_t {
	kRwStruct = 0x01, kRwString = 0x02, kRwExtension = 0x03,
	kRwTexture = 0x06, kRwMaterial = 0x07, kRwMaterialList = 0x08,
	kRwFrameList = 0x0E, kRwGeometry = 0x0F, kRwClump = 0x10,
	kRwAtomic = 0x14, kRwTextureNative = 0x15, kRwTexDict = 0x16,
	kRwGeometryList = 0x1A, kRwUvAnimDict = 0x2B,
};

struct RwWalkStats {
	uint64_t chunks = 0;
	std::map<uint32_t, uint64_t> byType;
	std::map<uint32_t, uint64_t> versions;
	std::map<uint32_t, uint64_t> unknownPlugins; // chunk types seen under Extension
	uint32_t geometries = 0, atomics = 0, textures = 0;
	std::vector<uint32_t> texturePlatforms;
};

// Walks one top-level chunk starting at data[0]. Returns false (err set) if sizes are inconsistent.
bool WalkRwStream(const uint8_t *data, size_t size, RwWalkStats &stats, const char **err);

struct RwNativeTexInfo {
	uint32_t platform;     // 8 = D3D8, 9 = D3D9
	std::string name, mask;
	uint32_t rasterFormat;
	uint32_t d3dFormat;    // 0 = uncompressed, else FourCC (DXT1/DXT3/...)
	uint16_t width, height;
	uint8_t depth, numLevels, rasterType, flags;
};
// Collects metadata of every native texture in a TXD (only for platform 8/9 layouts).
bool ReadTxdTextures(const uint8_t *data, size_t size, std::vector<RwNativeTexInfo> &out, const char **err);

} // namespace sa
