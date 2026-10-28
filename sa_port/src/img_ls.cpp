#include "ImgV2.h"
#include <cstdio>
#include <map>

int main(int argc, char **argv) {
	if (argc < 2) { puts("usage: img_ls <file.img> [name-to-check]"); return 2; }
	sa::ImgV2 img;
	if (!img.Open(argv[1])) { puts("not a valid IMG v2"); return 1; }
	std::map<std::string, int> byExt;
	int outOfRange = 0;
	uint64_t maxEnd = 0;
	for (auto &e : img.Entries()) {
		auto dot = e.name.rfind('.');
		byExt[dot == std::string::npos ? "(none)" : e.name.substr(dot + 1)]++;
		if (e.Offset() + e.Size() > img.FileSize()) outOfRange++;
		if (e.Offset() + e.Size() > maxEnd) maxEnd = e.Offset() + e.Size();
	}
	printf("entries: %zu, file bytes: %llu, last entry ends at: %llu, out-of-range: %d\n",
	       img.Entries().size(), (unsigned long long)img.FileSize(), (unsigned long long)maxEnd, outOfRange);
	for (auto &kv : byExt) printf("  .%s: %d\n", kv.first.c_str(), kv.second);
	if (argc > 2) {
		if (auto *e = img.Find(argv[2])) {
			std::vector<uint8_t> data;
			bool ok = img.Read(*e, data);
			printf("%s: offset %llu size %llu read=%d first4=%02X %02X %02X %02X\n", e->name.c_str(),
			       (unsigned long long)e->Offset(), (unsigned long long)e->Size(), ok,
			       data.size() > 0 ? data[0] : 0, data.size() > 1 ? data[1] : 0,
			       data.size() > 2 ? data[2] : 0, data.size() > 3 ? data[3] : 0);
		} else puts("name not found");
	}
	return 0;
}
