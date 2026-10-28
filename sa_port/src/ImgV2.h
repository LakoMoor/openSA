#pragma once
#include <cstdint>
#include <string>
#include <vector>

// GTA SA IMG v2 archive (VER2). Layout verified against CStreaming::LoadCdDirectory (0x5B6170, 1.0 US):
// 4-byte magic, int32 count, then count * 32-byte entries; offsets/sizes are in 2048-byte sectors.
namespace sa {

constexpr uint32_t kImgSectorSize = 2048;

struct ImgEntry {
	uint32_t offsetSectors;
	uint16_t streamingSectors;
	uint16_t archiveSectors; // 0 in retail archives
	std::string name;        // up to 23 chars, with extension

	uint64_t Offset() const { return uint64_t(offsetSectors) * kImgSectorSize; }
	uint64_t Size() const { return uint64_t(streamingSectors) * kImgSectorSize; }
};

class ImgV2 {
public:
	bool Open(const std::string &path);
	const std::vector<ImgEntry> &Entries() const { return m_entries; }
	const ImgEntry *Find(const std::string &name) const; // case-insensitive
	bool Read(const ImgEntry &e, std::vector<uint8_t> &out) const;
	uint64_t FileSize() const { return m_fileSize; }

private:
	std::string m_path;
	uint64_t m_fileSize = 0;
	std::vector<ImgEntry> m_entries;
};

} // namespace sa
