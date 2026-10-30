#pragma once
#include <string>
#include <vector>
#include <cstdint>

// audio/streams/* : XOR-encrypted with a 16-byte table (CAEStreamTransformer, 0x4f1750), 8000+68 byte header, then Ogg Vorbis.
struct RadioStream {
	std::vector<uint8_t> ogg;
	int channels = 0, rate = 0;
	bool open(const std::string &path);
	// decode up to maxFrames interleaved 16-bit frames starting at the stream start; returns frames decoded
	size_t decode(std::vector<int16_t> &out, size_t maxFrames) const;
};
