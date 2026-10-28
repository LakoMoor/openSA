#include "ImgV2.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace sa {

static bool iequals(const std::string &a, const std::string &b) {
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
		return std::tolower((unsigned char)x) == std::tolower((unsigned char)y);
	});
}

bool ImgV2::Open(const std::string &path) {
	m_entries.clear();
	m_path = path;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return false;
	_fseeki64(f, 0, SEEK_END);
	m_fileSize = (uint64_t)_ftelli64(f);
	_fseeki64(f, 0, SEEK_SET);

	char magic[4];
	int32_t count = 0;
	if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "VER2", 4) != 0 ||
	    fread(&count, 4, 1, f) != 1 || count < 0 || uint64_t(count) * 32 + 8 > m_fileSize) {
		fclose(f);
		return false;
	}
	m_entries.reserve(count);
	for (int32_t i = 0; i < count; i++) {
		uint8_t raw[32];
		if (fread(raw, 1, 32, f) != 32) { fclose(f); m_entries.clear(); return false; }
		ImgEntry e;
		memcpy(&e.offsetSectors, raw, 4);
		memcpy(&e.streamingSectors, raw + 4, 2);
		memcpy(&e.archiveSectors, raw + 6, 2);
		char name[25] = {};
		memcpy(name, raw + 8, 24);
		e.name = name;
		m_entries.push_back(std::move(e));
	}
	fclose(f);
	return true;
}

const ImgEntry *ImgV2::Find(const std::string &name) const {
	for (auto &e : m_entries)
		if (iequals(e.name, name)) return &e;
	return nullptr;
}

bool ImgV2::Read(const ImgEntry &e, std::vector<uint8_t> &out) const {
	if (e.Offset() + e.Size() > m_fileSize) return false;
	FILE *f = fopen(m_path.c_str(), "rb");
	if (!f) return false;
	_fseeki64(f, (long long)e.Offset(), SEEK_SET);
	out.resize((size_t)e.Size());
	bool ok = fread(out.data(), 1, out.size(), f) == out.size();
	fclose(f);
	return ok;
}

} // namespace sa
