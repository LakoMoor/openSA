#pragma once
#include <cstdint>
#include <string>
#include <vector>

// SFX banks (audio/CONFIG/BankLkup.dat + audio/sfx/<PAK>): each lookup entry is {u8 pak, 3 pad, u32 file offset (bytes), u32 data bytes}; at the offset
// sits a 0x12C4 byte header (i16 numSounds, i16 pad, 400 x {u32 offset in data, u32 loop start, u16 sample rate, i16 headroom}) followed by the 16-bit mono PCM.
// Layout from gta-reversed CAEBankLoader, checked here against the file sizes.
struct SfxSound { std::vector<int16_t> pcm; int rate = 22050; int32_t loopStart = -1; };
struct SfxBank { std::vector<SfxSound> sounds; };

struct SfxBanks {
	bool load(const std::string &audioDir);                 // reads BankLkup.dat and PakFiles.dat
	bool loadBank(int bank, SfxBank &out) const;            // reads one bank's sounds from its pak
	size_t bankCount() const { return m_lk.size(); }
private:
	struct Lk { uint8_t pak; uint32_t offset, bytes; };
	std::vector<Lk> m_lk;
	std::vector<std::string> m_paks;
	std::string m_dir;
};
