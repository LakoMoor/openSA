#include "TextData.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace sa {

static std::string Trim(const std::string &s) {
	size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
	return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
static std::string Lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
	return s;
}
static std::string StripComment(const std::string &l) {
	size_t p = l.find('#');
	return Trim(p == std::string::npos ? l : l.substr(0, p));
}
static std::vector<std::string> Fields(const std::string &line) {
	std::vector<std::string> v;
	std::string cur;
	for (char c : line) {
		if (c == ',' || c == ' ' || c == '\t') { if (!cur.empty()) { v.push_back(cur); cur.clear(); } }
		else cur += c;
	}
	if (!cur.empty()) v.push_back(cur);
	return v;
}

bool LoadGtaDat(const std::string &path, std::vector<DatDirective> &out) {
	std::ifstream f(path);
	if (!f) return false;
	std::string line;
	while (std::getline(f, line)) {
		line = StripComment(line);
		if (line.empty()) continue;
		size_t sp = line.find_first_of(" \t");
		DatDirective d;
		d.kind = sp == std::string::npos ? line : line.substr(0, sp);
		d.arg = sp == std::string::npos ? "" : Trim(line.substr(sp));
		out.push_back(std::move(d));
	}
	return true;
}

// Same decision tree as CFileLoader::LoadObject: "id model txd drawDist flags" first,
// otherwise "id model txd meshCount dist{1..3} flags". Timed objects append "timeOn timeOff".
static bool ParseObj(const std::vector<std::string> &t, bool timed, ObjectDef &o) {
	if (t.size() < 5) return false;
	o.id = atoi(t[0].c_str());
	o.model = t[1];
	o.txd = t[2];
	o.timed = timed;
	size_t next;
	float d0 = (float)atof(t[3].c_str());
	bool flat = d0 >= 4.0f && t.size() == (timed ? 7u : 5u);
	if (flat) {
		o.meshCount = 1;
		o.drawDist[0] = d0;
		o.flags = (uint32_t)strtoul(t[4].c_str(), nullptr, 10);
		next = 5;
	} else {
		o.meshCount = atoi(t[3].c_str());
		if (o.meshCount < 1 || o.meshCount > 3 || t.size() < 5 + size_t(o.meshCount)) return false;
		for (int i = 0; i < o.meshCount; i++) o.drawDist[i] = (float)atof(t[4 + i].c_str());
		o.flags = (uint32_t)strtoul(t[4 + o.meshCount].c_str(), nullptr, 10);
		next = 5 + o.meshCount;
	}
	if (timed) {
		if (t.size() < next + 2) return false;
		o.timeOn = atoi(t[next].c_str());
		o.timeOff = atoi(t[next + 1].c_str());
	}
	return true;
}

bool IdeDb::LoadFile(const std::string &path) {
	std::ifstream f(path);
	if (!f) return false;
	std::string line, section;
	while (std::getline(f, line)) {
		line = StripComment(line);
		if (line.empty()) continue;
		if (section.empty()) { section = Lower(line); continue; }
		if (Lower(line) == "end") { section.clear(); continue; }
		m_stats.sections[section]++;
		bool ok = true;
		auto t = Fields(line);
		if (section == "objs" || section == "tobj") {
			ObjectDef o;
			if ((ok = ParseObj(t, section == "tobj", o))) { m_modelIds[o.id] = true; NoteTxd(o.txd); m_objs[o.id] = std::move(o); }
		} else if (section == "cars") {
			// id model txd type handling gameName anims class freq flags comprules(hex) wheelId scaleF scaleR upgradeClass
			// Boats/trains/planes omit the wheel fields; sscanf in the game accepts the partial match.
			if ((ok = t.size() >= 11)) {
				VehicleDef v;
				v.id = atoi(t[0].c_str());
				v.model = t[1]; v.txd = t[2]; v.type = t[3]; v.handling = t[4];
				v.gameName = t[5]; v.anims = t[6]; v.vclass = t[7];
				v.frequency = atoi(t[8].c_str());
				v.flags = atoi(t[9].c_str());
				v.compRules = (uint32_t)strtoul(t[10].c_str(), nullptr, 16);
				if (t.size() > 11) v.wheelId = atoi(t[11].c_str());
				if (t.size() > 12) v.wheelScaleFront = (float)atof(t[12].c_str());
				if (t.size() > 13) v.wheelScaleRear = (float)atof(t[13].c_str());
				if (t.size() > 14) v.upgradeClass = atoi(t[14].c_str());
				m_modelIds[v.id] = true;
				NoteTxd(v.txd);
				m_vehicles[v.id] = std::move(v);
			}
		} else if (section == "peds") {
			// id model txd pedType stats animGroup carsCanDrive(hex) flags(hex) animFile radio1 radio2 voice x3
			if ((ok = t.size() >= 14)) {
				PedDef p;
				p.id = atoi(t[0].c_str());
				p.model = t[1]; p.txd = t[2]; p.pedType = t[3]; p.stats = t[4]; p.animGroup = t[5];
				p.carsCanDrive = (uint32_t)strtoul(t[6].c_str(), nullptr, 16);
				p.flags = (uint32_t)strtoul(t[7].c_str(), nullptr, 16);
				p.animFile = t[8];
				p.radio1 = atoi(t[9].c_str());
				p.radio2 = atoi(t[10].c_str());
				for (int k = 0; k < 3; k++) p.voice[k] = t[11 + k];
				m_modelIds[p.id] = true;
				NoteTxd(p.txd);
				m_peds[p.id] = std::move(p);
			}
		} else if (section == "weap") {
			// id model txd animGroup subDraw drawDist
			if ((ok = t.size() >= 6)) {
				WeaponDef w;
				w.id = atoi(t[0].c_str());
				w.model = t[1]; w.txd = t[2]; w.animGroup = t[3];
				w.subDraw = atoi(t[4].c_str());
				w.drawDist = (float)atof(t[5].c_str());
				m_modelIds[w.id] = true;
				NoteTxd(w.txd);
				m_weapons[w.id] = std::move(w);
			}
		} else if (section == "anim") {
			// id model txd animName drawDist flags (the game rejects the line unless all 6 fields scan)
			if ((ok = t.size() >= 6)) {
				AnimObjDef a;
				a.id = atoi(t[0].c_str());
				a.model = t[1]; a.txd = t[2]; a.animName = t[3];
				a.drawDist = (float)atof(t[4].c_str());
				a.flags = atoi(t[5].c_str());
				m_modelIds[a.id] = true;
				NoteTxd(a.txd);
				m_animObjs[a.id] = std::move(a);
			}
		} else if (section == "hier") {
			// id model txd (layout not verified against the exe; only the id is recorded)
			if ((ok = t.size() >= 3)) m_modelIds[atoi(t[0].c_str())] = true;
		} else if (section == "txdp") {
			if ((ok = t.size() >= 2)) m_txdParents[Lower(t[0])] = Lower(t[1]);
		}
		if (!ok && m_stats.badLines++ == 0) m_stats.firstBadLine = path + " [" + section + "]: " + line;
	}
	return true;
}

const VehicleDef *IdeDb::FindVehicle(int id) const {
	auto it = m_vehicles.find(id);
	return it == m_vehicles.end() ? nullptr : &it->second;
}

const PedDef *IdeDb::FindPed(int id) const {
	auto it = m_peds.find(id);
	return it == m_peds.end() ? nullptr : &it->second;
}

const ObjectDef *IdeDb::Find(int id) const {
	auto it = m_objs.find(id);
	return it == m_objs.end() ? nullptr : &it->second;
}

bool LoadIplText(const std::string &path, IplText &out, bool &isBinary) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	char magic[4] = {};
	f.read(magic, 4);
	isBinary = f.gcount() == 4 && memcmp(magic, "bnry", 4) == 0;
	if (isBinary) return true;
	f.clear();
	f.seekg(0);
	std::string line, section;
	while (std::getline(f, line)) {
		line = StripComment(line);
		if (line.empty()) continue;
		if (section.empty()) { section = Lower(line); continue; }
		if (Lower(line) == "end") { section.clear(); continue; }
		out.sections[section]++;
		if (section == "inst") {
			auto t = Fields(line);
			if (t.size() != 11 && t.size() != 10) { out.badLines++; continue; }
			Inst i;
			i.id = atoi(t[0].c_str());
			i.model = t[1];
			i.interior = atoi(t[2].c_str());
			for (int k = 0; k < 3; k++) i.pos[k] = (float)atof(t[3 + k].c_str());
			for (int k = 0; k < 4; k++) i.rot[k] = (float)atof(t[6 + k].c_str());
			if (t.size() == 11) i.lod = atoi(t[10].c_str());
			out.insts.push_back(std::move(i));
		}
	}
	return true;
}

} // namespace sa

namespace sa {
void IdeDb::NoteTxd(const std::string &txd) {
	std::string k = Lower(txd);
	if (m_txdSeen.insert(k).second) m_txdOrder.push_back(k);
}
std::vector<std::pair<int, std::string>> IdeDb::AllModels() const {
	std::vector<std::pair<int, std::string>> r;
	for (auto &kv : m_objs) r.emplace_back(kv.first, kv.second.model);
	for (auto &kv : m_vehicles) r.emplace_back(kv.first, kv.second.model);
	for (auto &kv : m_peds) r.emplace_back(kv.first, kv.second.model);
	for (auto &kv : m_weapons) r.emplace_back(kv.first, kv.second.model);
	for (auto &kv : m_animObjs) r.emplace_back(kv.first, kv.second.model);
	return r;
}

} // namespace sa
