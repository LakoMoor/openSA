#include "BinaryIpl.h"
#include <cstring>

namespace sa {

bool ParseBinaryIpl(const uint8_t *data, size_t size, BinaryIpl &out, const char **err) {
	const char *dummy;
	if (!err) err = &dummy;
	out = {};
	if (size < sizeof(BnryHeader)) { *err = "too small for header"; return false; }
	BnryHeader h;
	memcpy(&h, data, sizeof(h));
	if (memcmp(h.magic, "bnry", 4) != 0) { *err = "bad magic"; return false; }

	// The game reads both counts as signed 16-bit values.
	uint32_t numInst = (uint16_t)h.numInst;
	uint32_t numCars = (uint16_t)h.numCarGens;
	if (numInst > 0) {
		uint64_t end = uint64_t(h.offInst) + uint64_t(numInst) * sizeof(BnryInst);
		if (h.offInst < sizeof(BnryHeader) || end > size) { *err = "instance table out of range"; return false; }
		out.insts.resize(numInst);
		memcpy(out.insts.data(), data + h.offInst, numInst * sizeof(BnryInst));
	}
	out.numCarGens = numCars;
	if (numCars > 0 && (h.offCarGens < sizeof(BnryHeader) || h.offCarGens > size)) {
		*err = "car generator table out of range";
		return false;
	}
	if (numCars > 0) {
		if (uint64_t(h.offCarGens) + uint64_t(numCars) * sizeof(BnryCarGen) > size) { *err = "car generator table out of range"; return false; }
		out.carGens.resize(numCars);
		memcpy(out.carGens.data(), data + h.offCarGens, numCars * sizeof(BnryCarGen));
	}
	return true;
}

} // namespace sa
