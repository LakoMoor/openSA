#include "StreamingWorld.h"
#include <algorithm>
#include <cstdlib>

namespace sa {

static std::string Lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
	return s;
}

static std::string Join(const std::string &root, std::string rel) {
	std::replace(rel.begin(), rel.end(), '\\', '/');
	return root + "/" + rel;
}

int SlotTable::FindOrAdd(const std::string &name) {
	auto it = slots.find(name);
	if (it != slots.end()) return it->second;
	int s = first + (int)slots.size();
	slots[name] = s;
	return s;
}

int SlotTable::Find(const std::string &name) const {
	auto it = slots.find(name);
	return it == slots.end() ? -1 : it->second;
}

int StreamingWorld::ModelTxdId(int modelId) const {
	std::string name;
	if (const ObjectDef *o = ide.Find(modelId)) name = o->txd;
	else if (const VehicleDef *v = ide.FindVehicle(modelId)) name = v->txd;
	else if (const PedDef *p = ide.FindPed(modelId)) name = p->txd;
	else return -1;
	int slot = txd.Find(Lower(name));
	return slot < 0 ? -1 : kTxdBase + slot;
}

int StreamingWorld::TxdParentId(int txdStreamId) const {
	auto n = txdNameBySlot.find(txdStreamId - kTxdBase);
	if (n == txdNameBySlot.end()) return -1;
	auto p = ide.TxdParents().find(n->second);
	if (p == ide.TxdParents().end()) return -1;
	int slot = txd.Find(p->second);
	return slot < 0 ? -1 : kTxdBase + slot;
}

bool StreamingWorld::Build(const std::string &root, std::string *error) {
	auto fail = [&](const std::string &m) { if (error) *error = m; return false; };
	std::vector<DatDirective> d0, d1;
	if (!LoadGtaDat(root + "/data/default.dat", d0) || !LoadGtaDat(root + "/data/gta.dat", d1)) return fail("cannot read default.dat/gta.dat");

	txd.first = kReservedTxdSlots;
	// default.dat IDEs come first (their TXD names get the first named slots)
	for (auto &d : d0) if (d.kind == "IDE") ide.LoadFile(Join(root, d.arg));
	for (auto &n : ide.TxdOrder()) txd.FindOrAdd(n);
	// the rest of the IDEs are loaded after the directories in the game, but model names are needed to resolve DFFs
	for (auto &d : d1) if (d.kind == "IDE") ide.LoadFile(Join(root, d.arg));
	for (auto &m : ide.AllModels()) modelByName[Lower(m.second)] = m.first;

	std::vector<std::string> paths;
	for (const char *p : {"models/gta3.img", "models/gta_int.img", "models/player.img"}) paths.push_back(root + "/" + p);
	for (auto &d : d1)
		if (d.kind == "IMG" && Lower(d.arg).find("cutscene") == std::string::npos) paths.push_back(Join(root, d.arg));

	archives.resize(paths.size());
	for (size_t a = 0; a < paths.size(); a++) {
		if (!archives[a].Open(paths[a])) return fail("cannot open " + paths[a]);
		for (const ImgEntry &e : archives[a].Entries()) {
			size_t dot = e.name.find('.');
			if (dot == std::string::npos) continue;
			std::string base = Lower(e.name.substr(0, dot)), ext = Lower(e.name.substr(dot + 1, 3));
			int id;
			if (ext == "dff") {
				auto it = modelByName.find(base);
				if (it == modelByName.end()) continue;
				id = it->second;
			} else if (ext == "txd") id = kTxdBase + txd.FindOrAdd(base);
			else if (ext == "col") id = kColBase + col.FindOrAdd(base);
			else if (ext == "ipl") id = kIplBase + ipl.FindOrAdd(base);
			else if (ext == "dat") id = kDatBase + atoi(base.c_str() + (base.size() > 5 ? 5 : base.size()));
			else if (ext == "ifp") id = kIfpBase + ifp.FindOrAdd(base);
			else if (ext == "rrr") id = kRrrBase + rrr.FindOrAdd(base);
			else if (ext == "scm") id = kScmBase + scm.FindOrAdd(base);
			else continue;
			if (StreamingInfo *si = store.At(id)) {
				si->imgId = (uint8_t)a;
				si->cdOffset = e.offsetSectors;
				si->cdSize = e.streamingSectors;
			}
		}
	}
	for (auto &n : ide.TxdOrder()) txd.FindOrAdd(n);
	for (auto &kv : txd.slots) txdNameBySlot[kv.second] = kv.first;
	return true;
}

} // namespace sa
