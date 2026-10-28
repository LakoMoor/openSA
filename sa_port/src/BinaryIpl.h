#pragma once
#include <cstdint>
#include <vector>

// Binary ("bnry") streaming IPL. Layout verified against CIplStore::LoadIpl (0x406080)
// and CFileLoader::LoadObjectInstance (0x538090), GTA SA 1.0 US.
namespace sa {

#pragma pack(push, 1)
struct BnryHeader {            // 0x4C bytes
	char magic[4];             // "bnry"
	uint32_t numInst;          // game reads it as int16
	uint32_t numUnk1;
	uint32_t numUnk2;
	uint32_t numUnk3;
	uint32_t numCarGens;       // +0x14, game reads as int16
	uint32_t numUnk4;
	uint32_t offInst;          // +0x1C, relative to file start
	uint32_t offUnk1, offUnk2, offUnk3, offUnk4, offUnk5, offUnk6, offUnk7;
	uint32_t offCarGens;       // +0x3C
	uint32_t offUnk8, offUnk9;
	uint32_t offUnk10;         // header is 0x4C bytes long
};

struct BnryInst {              // 40 bytes
	float pos[3];
	float rot[4];              // quaternion x,y,z,w
	int32_t modelId;
	uint32_t flags;            // low byte = interior/area; bits 0x100,0x200,0x400,0x800,0x1000 = entity flags
	int32_t lodIndex;          // -1 = none; otherwise index into the PARENT IPL (X_streamN -> X), see CIplStore::LoadIpl
	                           // (verified: 4648/4648 refs in gta3.img are < parent text IPL inst count)
};
#pragma pack(pop)

static_assert(sizeof(BnryHeader) == 0x4C, "bnry header");
static_assert(sizeof(BnryInst) == 40, "bnry inst");

struct BinaryIpl {
	uint32_t numCarGens = 0;
	std::vector<BnryInst> insts;
};

// Parses and bounds-checks a "bnry" buffer. Returns false (with err set) on malformed data.
bool ParseBinaryIpl(const uint8_t *data, size_t size, BinaryIpl &out, const char **err);

} // namespace sa
