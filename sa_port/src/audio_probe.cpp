#include "AudioBank.h"
#include <cstdio>
int main(int argc, char **argv)
{
	SfxBanks b;
	if (!b.load(argc > 1 ? argv[1] : ".")) { std::printf("load failed\n"); return 1; }
	std::printf("%zu banks\n", b.bankCount());
	size_t ok = 0, bad = 0;
	for (int i = 0; i < (int)b.bankCount(); i++) {
		SfxBank bk;
		if (!b.loadBank(i, bk)) { bad++; if (bad < 6) std::printf("bank %d failed\n", i); continue; }
		ok++;
		if (i < 8) { std::printf("bank %d: %zu sounds", i, bk.sounds.size()); for (size_t s = 0; s < bk.sounds.size() && s < 6; s++) std::printf(" [%zu smp @%d]", bk.sounds[s].pcm.size(), bk.sounds[s].rate); std::printf("\n"); }
	}
	std::printf("loaded %zu, failed %zu\n", ok, bad);
}
