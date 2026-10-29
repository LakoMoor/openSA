#include "AudioBank.h"
#include <cstdio>
#include <cstring>
#include <fstream>

bool SfxBanks::load(const std::string &audioDir) {
	m_dir = audioDir;
	std::ifstream f(audioDir + "/CONFIG/BankLkup.dat", std::ios::binary);
	if (!f) return false;
	std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	for (size_t i = 0; i + 12 <= b.size(); i += 12) {
		Lk l;
		l.pak = b[i];
		memcpy(&l.offset, &b[i + 4], 4);
		memcpy(&l.bytes, &b[i + 8], 4);
		m_lk.push_back(l);
	}
	std::ifstream p(audioDir + "/CONFIG/PakFiles.dat", std::ios::binary);
	std::vector<uint8_t> pb((std::istreambuf_iterator<char>(p)), std::istreambuf_iterator<char>());
	for (size_t i = 0; i + 52 <= pb.size(); i += 52) m_paks.push_back(std::string((const char *)&pb[i], strnlen((const char *)&pb[i], 12)));
	return !m_lk.empty() && !m_paks.empty();
}

bool SfxBanks::loadBank(int bank, SfxBank &out) const {
	if (bank < 0 || bank >= (int)m_lk.size() || m_lk[bank].pak >= m_paks.size()) return false;
	const Lk &l = m_lk[bank];
	std::ifstream f(m_dir + "/sfx/" + m_paks[l.pak], std::ios::binary);
	if (!f) return false;
	std::vector<uint8_t> hdr(0x12C4);
	f.seekg(l.offset);
	if (!f.read((char *)hdr.data(), hdr.size())) return false;
	int16_t n;
	memcpy(&n, &hdr[0], 2);
	std::vector<uint8_t> data(l.bytes);
	if (!f.read((char *)data.data(), data.size())) return false;
	struct It { uint32_t off, loop; uint16_t rate; int16_t head; };
	int count = n < 0 ? 1 : n;
	if (count > 400) return false;
	It items[400];
	memcpy(items, &hdr[4], sizeof(items));
	out.sounds.clear();
	for (int i = 0; i < count; i++) {
		uint32_t s = items[i].off, e = (n < 0 || i + 1 >= count) ? l.bytes : items[i + 1].off;
		if (s > e || e > l.bytes) return false;
		SfxSound snd;
		snd.rate = items[i].rate;
		snd.loopStart = (int32_t)items[i].loop;
		snd.pcm.resize((e - s) / 2);
		memcpy(snd.pcm.data(), &data[s], snd.pcm.size() * 2);
		out.sounds.push_back(std::move(snd));
	}
	return true;
}
