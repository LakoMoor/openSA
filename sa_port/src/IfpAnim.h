#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sa {

// ANP3 animation file (ped.ifp). Layout from CAnimManager::LoadAnimFile_ANP23 (gta-reversed, 0x4D47F0): header "ANP3", u32 size, name[24], u32 numAnims;
// per animation name[24], u32 numSequences, u32 dataSize, u32 flags(1 = compressed); per sequence name[24], u32 frameType, u32 numFrames, i32 boneTag, then
// the keys. frameType 3 = compressed rotation {i16 q[4] (/4096), i16 time (/60 s)}, 4 = same plus i16 t[3] (/1024). Types 1 and 2 hold float keys.
// Verified by walking all 294 animations of the Steam ped.ifp to the exact end of the file.
struct IfpKey {
	float time;
	float q[4]; // x, y, z, w
	float t[3];
};
struct IfpSeq {
	int boneTag = -1;
	bool hasTrans = false;
	std::vector<IfpKey> keys;
};
struct IfpAnim {
	std::vector<IfpSeq> seqs;
	float duration = 0.0f;
};

// Appends every animation of one file to `out`, keyed by lower-case name. Returns the count read, or 0 on a malformed file.
size_t ParseIfp(const std::vector<uint8_t> &data, std::unordered_map<std::string, IfpAnim> &out);

} // namespace sa
