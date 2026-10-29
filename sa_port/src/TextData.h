#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Text data loaders for GTA SA 1.0 US: gta.dat, IDE, text IPL.
// objs parsing mirrors CFileLoader::LoadObject (0x5B3C60).
namespace sa {

struct DatDirective {
	std::string kind; // IMG, IDE, IPL, SPLASH, COLFILE, MAPZONE, ...
	std::string arg;  // path (backslashes kept) or the rest of the line
};
bool LoadGtaDat(const std::string &path, std::vector<DatDirective> &out);

struct ObjectDef {
	int id = 0;
	std::string model, txd;
	int meshCount = 1;
	float drawDist[3] = {};
	uint32_t flags = 0;
	bool timed = false;
	int timeOn = 0, timeOff = 0;
};

// Layouts verified against CFileLoader::LoadVehicleObject (0x5B6F30), LoadPedObject (0x5B7420),
// LoadWeaponObject (0x5B3FB0), LoadAnimatedClumpObject (0x5B40C0), LoadTXDParent (0x5B75E0).
struct VehicleDef {
	int id = 0;
	std::string model, txd, type, handling, gameName, anims, vclass;
	int frequency = 0, flags = 0;
	uint32_t compRules = 0;
	int wheelId = -1;
	float wheelScaleFront = 0, wheelScaleRear = 0;
	int upgradeClass = 0;
};

struct PedDef {
	int id = 0;
	std::string model, txd, pedType, stats, animGroup;
	uint32_t carsCanDrive = 0, flags = 0;
	std::string animFile;
	int radio1 = 0, radio2 = 0;
	std::string voice[3];
};

struct WeaponDef {
	int id = 0;
	std::string model, txd, animGroup;
	int subDraw = 0;
	float drawDist = 0;
};

struct AnimObjDef {
	int id = 0;
	std::string model, txd, animName;
	float drawDist = 0;
	int flags = 0;
};

struct IdeStats {
	std::unordered_map<std::string, int> sections; // section name -> data line count
	int badLines = 0;
	std::string firstBadLine; // for diagnostics
};

class IdeDb {
public:
	bool LoadFile(const std::string &path);
	const ObjectDef *Find(int id) const;
	const VehicleDef *FindVehicle(int id) const;
	const VehicleDef *FindVehicleByModelName(const std::string &lowerName) const;
	const PedDef *FindPed(int id) const;
	// True if any section (objs/tobj/cars/peds/weap/anim/hier) defines this model id.
	bool HasModel(int id) const { return m_modelIds.count(id) != 0; }
	// (id, model name) of every model defined by any IDE section.
	std::vector<std::pair<int, std::string>> AllModels() const;
	// Lower-case TXD names in order of first reference (the order CTxdStore::AddTxdSlot would see them).
	const std::vector<std::string> &TxdOrder() const { return m_txdOrder; }
	size_t ObjectCount() const { return m_objs.size(); }
	size_t VehicleCount() const { return m_vehicles.size(); }
	size_t PedCount() const { return m_peds.size(); }
	size_t WeaponCount() const { return m_weapons.size(); }
	size_t AnimObjCount() const { return m_animObjs.size(); }
	size_t TxdParentCount() const { return m_txdParents.size(); }
	const std::unordered_map<std::string, std::string> &TxdParents() const { return m_txdParents; }
	const IdeStats &Stats() const { return m_stats; }
private:
	std::unordered_map<int, ObjectDef> m_objs;
	std::unordered_map<int, VehicleDef> m_vehicles;
	std::unordered_map<int, PedDef> m_peds;
	std::unordered_map<int, WeaponDef> m_weapons;
	std::unordered_map<int, AnimObjDef> m_animObjs;
	std::unordered_map<std::string, std::string> m_txdParents; // lower-case child -> parent
	std::unordered_map<int, bool> m_modelIds;
	IdeStats m_stats;
	std::vector<std::string> m_txdOrder;
	std::unordered_set<std::string> m_txdSeen;
	void NoteTxd(const std::string &txd);
};

struct Inst {
	int id = 0;
	std::string model;
	int interior = 0;
	float pos[3] = {}, rot[4] = {};
	int lod = -1;
};

struct IplText {
	std::vector<Inst> insts;
	std::unordered_map<std::string, int> sections;
	int badLines = 0;
};
// Returns false if the file cannot be read; sets isBinary for "bnry" streaming IPLs.
bool LoadIplText(const std::string &path, IplText &out, bool &isBinary);

} // namespace sa
