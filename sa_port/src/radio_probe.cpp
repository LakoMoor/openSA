#include "RadioStream.h"
#include <cstdio>
#include <cmath>
int main(int argc, char **argv)
{
	std::string dir = argc > 1 ? argv[1] : ".";
	if (argc > 2) { // radio_probe <audio dir> tracks
		TrackTable t;
		if (!t.load(dir)) { std::printf("track table failed\n"); return 1; }
		std::printf("%zu packs, %zu tracks\n", t.packs.size(), t.tracks.size());
		size_t ok = 0, bad = 0; double total = 0; std::vector<double> perPack(t.packs.size());
		for (size_t i = 0; i < t.tracks.size(); i++) {
			RadioStream s;
			if (!s.openTrack(t, i)) { if (bad++ < 5) std::printf("track %zu (pack %d off %u size %u) failed\n", i, t.tracks[i].pack, t.tracks[i].offset, t.tracks[i].size); continue; }
			double sec = (double)s.lengthFrames() / s.rate; ok++; total += sec; perPack[t.tracks[i].pack] += sec;
			if (i < 6) std::printf("track %zu pack %s: %d ch @%d Hz %.1f s\n", i, t.packs[t.tracks[i].pack].c_str(), s.channels, s.rate, sec);
		}
		std::printf("opened %zu, failed %zu, total %.0f s\n", ok, bad, total);
		for (size_t p = 0; p < perPack.size(); p++) std::printf("  %-9s %.0f s\n", t.packs[p].c_str(), perPack[p]);
		return 0;
	}
	for (const char *n : {"AA", "ADVERTS", "AMBIENCE", "BEATS", "CH", "CO", "CR", "CUTSCENE", "DS", "HC", "MH", "MR", "NJ", "RE", "RG", "TK"}) {
		RadioStream s;
		if (!s.open(dir + "/" + n)) { std::printf("%-9s open failed\n", n); continue; }
		std::vector<int16_t> pcm;
		size_t fr = s.decode(pcm, (size_t)s.rate * 10);
		double e = 0; for (int16_t x : pcm) e += (double)x * x;
		std::printf("%-9s %d ch @%d Hz, decoded %.1f s, rms %.0f\n", n, s.channels, s.rate, (double)fr / s.rate, pcm.empty() ? 0.0 : std::sqrt(e / pcm.size()));
	}
}
