#include "RadioStream.h"
#include <cstdio>
#include <cmath>
int main(int argc, char **argv)
{
	std::string dir = argc > 1 ? argv[1] : ".";
	for (const char *n : {"AA", "ADVERTS", "AMBIENCE", "BEATS", "CH", "CO", "CR", "CUTSCENE", "DS", "HC", "MH", "MR", "NJ", "RE", "RG", "TK"}) {
		RadioStream s;
		if (!s.open(dir + "/" + n)) { std::printf("%-9s open failed\n", n); continue; }
		std::vector<int16_t> pcm;
		size_t fr = s.decode(pcm, (size_t)s.rate * 10);
		double e = 0; for (int16_t x : pcm) e += (double)x * x;
		std::printf("%-9s %d ch @%d Hz, decoded %.1f s, rms %.0f\n", n, s.channels, s.rate, (double)fr / s.rate, pcm.empty() ? 0.0 : std::sqrt(e / pcm.size()));
	}
}
