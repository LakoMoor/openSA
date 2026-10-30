#pragma once
#include <string>
#include <vector>
#include <cstdint>

// audio/streams/* : XOR-encrypted with a 16-byte table (CAEStreamTransformer, 0x4f1750), 8000+68 byte header, then Ogg Vorbis.
struct TrackTable {
	struct Entry { uint8_t pack; uint32_t offset, size; };
	std::vector<std::string> packs;
	std::vector<Entry> tracks;
	std::string dir; // audio/
	bool load(const std::string &audioDir); // CONFIG/StrmPaks.dat (16-byte names), CONFIG/TrakLkup.dat (12-byte entries)
};

struct RadioStream {
	std::vector<uint8_t> ogg;
	int channels = 0, rate = 0;
	bool open(const std::string &path);
	// one track of a pack: Ogg starts at entry.offset + sizeof(tTrackInfo)=0x1F84 (CAETrackLoader::GetDataStream, 0x4E0D20)
	bool openTrack(const TrackTable &t, size_t id);
	uint64_t lengthFrames() const;
	// incremental decode: begin(), then read() until it returns 0, end()
	bool begin();
	size_t read(int16_t *out, size_t frames); // interleaved frames
	void end();
	RadioStream() = default;
	RadioStream(const RadioStream &) = delete;
	~RadioStream() { end(); }
	void *dec = nullptr;
	// decode up to maxFrames interleaved 16-bit frames starting at the stream start; returns frames decoded
	size_t decode(std::vector<int16_t> &out, size_t maxFrames) const;
};
