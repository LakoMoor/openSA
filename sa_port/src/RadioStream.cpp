#include "RadioStream.h"
#include <cstdio>
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
