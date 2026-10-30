#include "RadioStream.h"
#include <cstdio>
#include <cstring>
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

bool RadioStream::open(const std::string &path)
{
	static const uint8_t key[16] = {0xEA, 0x3A, 0xC4, 0xA1, 0x9A, 0xA8, 0x14, 0xF3, 0x48, 0xB0, 0xD7, 0x23, 0x9D, 0xE8, 0xFF, 0xF1};
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f) return false;
	std::fseek(f, 0, SEEK_END);
	long n = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> raw(n);
	if (std::fread(raw.data(), 1, n, f) != (size_t)n) { std::fclose(f); return false; }
	std::fclose(f);
	for (long i = 0; i < n; i++) raw[i] ^= key[i & 15];
	size_t at = 0;
	for (size_t i = 0; i + 4 <= raw.size() && i < 20000; i++)
		if (raw[i] == 'O' && raw[i + 1] == 'g' && raw[i + 2] == 'g' && raw[i + 3] == 'S') { at = i; break; }
	if (!at) return false;
	ogg.assign(raw.begin() + at, raw.end());
	int err = 0;
	stb_vorbis *v = stb_vorbis_open_memory(ogg.data(), (int)ogg.size(), &err, nullptr);
	if (!v) return false;
	stb_vorbis_info in = stb_vorbis_get_info(v);
	channels = in.channels; rate = (int)in.sample_rate;
	stb_vorbis_close(v);
	return true;
}

size_t RadioStream::decode(std::vector<int16_t> &out, size_t maxFrames) const
{
	int err = 0;
	stb_vorbis *v = stb_vorbis_open_memory(ogg.data(), (int)ogg.size(), &err, nullptr);
	if (!v) return 0;
	out.resize(maxFrames * channels);
	size_t got = 0;
	while (got < maxFrames) {
		int r = stb_vorbis_get_samples_short_interleaved(v, channels, out.data() + got * channels, (int)((maxFrames - got) * channels));
		if (r <= 0) break;
		got += r;
	}
	stb_vorbis_close(v);
	out.resize(got * channels);
	return got;
}

static const uint8_t kStreamKey[16] = {0xEA, 0x3A, 0xC4, 0xA1, 0x9A, 0xA8, 0x14, 0xF3, 0x48, 0xB0, 0xD7, 0x23, 0x9D, 0xE8, 0xFF, 0xF1};

static bool slurp(const std::string &p, std::vector<uint8_t> &o)
{
	FILE *f = std::fopen(p.c_str(), "rb");
	if (!f) return false;
	std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
	o.resize(n);
	bool ok = std::fread(o.data(), 1, n, f) == (size_t)n;
	std::fclose(f);
	return ok;
}

bool TrackTable::load(const std::string &audioDir)
{
	dir = audioDir;
	std::vector<uint8_t> a, b;
	if (!slurp(audioDir + "/CONFIG/StrmPaks.dat", a) || !slurp(audioDir + "/CONFIG/TrakLkup.dat", b)) return false;
	for (size_t i = 0; i + 16 <= a.size(); i += 16) packs.push_back(std::string((const char *)&a[i], strnlen((const char *)&a[i], 16)));
	for (size_t i = 0; i + 12 <= b.size(); i += 12) {
		Entry e; e.pack = b[i]; std::memcpy(&e.offset, &b[i + 4], 4); std::memcpy(&e.size, &b[i + 8], 4);
		tracks.push_back(e);
	}
	return !tracks.empty();
}

bool RadioStream::openTrack(const TrackTable &t, size_t id)
{
	if (id >= t.tracks.size()) return false;
	const auto &e = t.tracks[id];
	if (e.pack >= t.packs.size()) return false;
	FILE *f = std::fopen((t.dir + "/streams/" + t.packs[e.pack]).c_str(), "rb");
	if (!f) return false;
	uint32_t start = e.offset + 0x1F84;
	if (e.size <= 0x1F84) { std::fclose(f); return false; }
	ogg.resize(e.size - 0x1F84);
	std::fseek(f, (long)start, SEEK_SET);
	size_t got = std::fread(ogg.data(), 1, ogg.size(), f);
	std::fclose(f);
	if (got != ogg.size()) return false;
	for (size_t i = 0; i < ogg.size(); i++) ogg[i] ^= kStreamKey[(start + i) & 15];
	int err = 0;
	stb_vorbis *v = stb_vorbis_open_memory(ogg.data(), (int)ogg.size(), &err, nullptr);
	if (!v) return false;
	stb_vorbis_info in = stb_vorbis_get_info(v);
	channels = in.channels; rate = (int)in.sample_rate;
	stb_vorbis_close(v);
	return true;
}

uint64_t RadioStream::lengthFrames() const
{
	int err = 0;
	stb_vorbis *v = stb_vorbis_open_memory(ogg.data(), (int)ogg.size(), &err, nullptr);
	if (!v) return 0;
	uint64_t n = stb_vorbis_stream_length_in_samples(v);
	stb_vorbis_close(v);
	return n;
}
