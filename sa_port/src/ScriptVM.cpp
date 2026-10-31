#include "ScriptVM.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <unordered_map>

uint32_t gTraceLo = 0, gTraceHi = 0, gTraceLeft = 0;   // script_check aid: print up to gTraceLeft executed ops with ip in [lo, hi)
volatile uint32_t gDbgIp = 0, gDbgOp = 0, gDbgStage = 0;   // watchdog aid for script_check
namespace {
struct CmdInfo {
	uint16_t op;
	int8_t fixed;
	int8_t vararg;
	int8_t cond;
	const char *name;
};
const CmdInfo kCmds[] = {
#include "ScmTable.inc"
};

const std::unordered_map<uint16_t, const CmdInfo *> &table()
{
	static std::unordered_map<uint16_t, const CmdInfo *> t;
	if (t.empty())
		for (const CmdInfo &c : kCmds)
			t.emplace(c.op, &c);   // first definition wins
	return t;
}

inline int32_t rd32(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return v; }
inline uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
inline float asF(int32_t v) { float f; memcpy(&f, &v, 4); return f; }
inline int32_t asI(float f) { int32_t v; memcpy(&v, &f, 4); return v; }
bool has(const char *s, const char *sub) { return strstr(s, sub) != nullptr; }
}

const char *ScriptVM::opName(uint16_t op)
{
	auto it = table().find(op);
	return it == table().end() ? "?" : it->second->name;
}

void ScriptVM::setI(ScmThread &t, const ScmArg &a, int32_t v) { ref(t, a) = v; }
void ScriptVM::setF(ScmThread &t, const ScmArg &a, float v) { ref(t, a) = asI(v); }

ScriptVM::ScriptVM()
{
	// Minimal player state until the host binds the real game objects.
	setHandler(0x0053, [](ScriptVM &vm, ScmThread &t, std::vector<ScmArg> &a) { vm.setI(t, a[0], 0); return true; });   // CREATE_PLAYER
	setHandler(0x0256, [](ScriptVM &vm, ScmThread &t, std::vector<ScmArg> &) { vm.setCond(t, true); return true; });   // IS_PLAYER_PLAYING
	setHandler(0x01F5, [](ScriptVM &vm, ScmThread &t, std::vector<ScmArg> &a) { vm.setI(t, a[1], 1); return true; });   // GET_PLAYER_CHAR
}

bool ScriptVM::load(const std::string &path)
{
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return false;
	m_mem.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	m_mem.resize(m_mem.size() + 256, 0);
	ScmThread main;
	main.ip = 0;
	main.name = "main";
	m_threads.push_back(main);
	return true;
}

bool ScriptVM::readOne(ScmThread &t, ScmArg &a)
{
	if (t.ip + 24 > m_mem.size())   // up to 17 bytes of operand are read below
		return false;
	const uint8_t *p = &m_mem[t.ip];
	a = ScmArg();
	a.type = p[0];
	p++;
	t.ip++;
	auto locIdx = [&](uint16_t idx) { return idx; };
	switch (a.type) {
	case 1: a.value = rd32(p); t.ip += 4; break;
	case 2: a.value = rd16(p); a.isVar = true; t.ip += 2; break;
	case 3: a.value = locIdx(rd16(p)) * 4; a.isVar = true; t.ip += 2; break;
	case 4: a.value = (int8_t)p[0]; t.ip += 1; break;
	case 5: a.value = (int16_t)rd16(p); t.ip += 2; break;
	case 6: a.value = rd32(p); t.ip += 4; break;
	case 7: case 8: case 0xC: case 0xD: case 0x12: case 0x13: {
		uint16_t base = rd16(p), idxVar = rd16(p + 2);
		uint8_t flags = p[5];
		// a corrupt operand stream (misparsed earlier command) must not read outside the locals / memory: treat as index 0
		int32_t idx = (flags & 0x80) ? (idxVar < 34 ? t.locals[idxVar] : 0) : (idxVar + 4u <= m_mem.size() ? rd32(&m_mem[idxVar]) : 0);
		int elem = (a.type == 7 || a.type == 8) ? 4 : (a.type == 0xC || a.type == 0xD) ? 8 : 16;
		bool local = (a.type == 8 || a.type == 0xD || a.type == 0x13);
		a.isVar = true;
		a.value = local ? (int32_t)(base * 4 + idx * elem) : (int32_t)(base + idx * elem);
		a.type = local ? 3 : 2;
		t.ip += 6;
		break;
	}
	case 9: a.str.assign((const char *)p, strnlen((const char *)p, 8)); t.ip += 8; break;
	case 0xA: case 0x10: a.value = rd16(p); a.isVar = true; t.ip += 2; break;
	case 0xB: case 0x11: a.value = rd16(p) * 4; a.isVar = true; a.type = 0xB; t.ip += 2; break;
	case 0xE: { int n = p[0]; a.str.assign((const char *)p + 1, strnlen((const char *)p + 1, n)); t.ip += 1 + n; break; }
	case 0xF: a.str.assign((const char *)p, strnlen((const char *)p, 16)); t.ip += 16; break;
	default:
		return false;
	}
	return true;
}

bool ScriptVM::readArgs(ScmThread &t, int fixed, bool vararg, std::vector<ScmArg> &out)
{
	out.clear();
	for (int i = 0; i < fixed; i++) {
		ScmArg a;
		if (!readOne(t, a))
			return false;
		out.push_back(a);
	}
	if (vararg) {
		while (t.ip < m_mem.size() && m_mem[t.ip] != 0) {
			ScmArg a;
			if (!readOne(t, a))
				return false;
			out.push_back(a);
		}
		t.ip++;
	}
	return true;
}

int32_t &ScriptVM::ref(ScmThread &t, const ScmArg &a)
{
	static int32_t dummy;
	if (!a.isVar) { dummy = a.value; return dummy; }
	if (a.type == 3 || a.type == 0xB) {
		size_t off = (size_t)a.value < sizeof(t.locals) - 3 ? (size_t)a.value : 0;
		return *reinterpret_cast<int32_t *>(reinterpret_cast<uint8_t *>(t.locals) + off);
	}
	return *reinterpret_cast<int32_t *>(&m_mem[(size_t)a.value % (m_mem.size() - 4)]);
}

int32_t ScriptVM::getI(ScmThread &t, const ScmArg &a)
{
	if (a.type == 6) return (int32_t)asF(a.value);
	return a.isVar ? ref(t, a) : a.value;
}

float ScriptVM::getF(ScmThread &t, const ScmArg &a)
{
	if (a.type == 6) return asF(a.value);
	if (a.isVar) return asF(ref(t, a));
	return asF(a.value);
}

uint32_t ScriptVM::label(const ScmThread &t, int32_t v) const
{
	return v >= 0 ? (uint32_t)v : t.base + (uint32_t)(-v);
}

void ScriptVM::condResult(ScmThread &t, bool v, bool neg)
{
	if (neg)
		v = !v;
	if (t.andorCount > 0) {
		if (t.andorMode == 1) t.condFlag = t.condFlag && v;
		else t.condFlag = t.condFlag || v;
		t.andorCount--;
	} else {
		t.condFlag = v;
	}
}

bool ScriptVM::execute(ScmThread &t, uint16_t op, bool neg, std::vector<ScmArg> &args)
{
	const CmdInfo *ci = table().at(op);
	const char *n = ci->name;
	auto isFloat = [&]() { return has(n, "FLOAT"); };

	auto hit = m_handlers.find(op);
	if (hit != m_handlers.end() && hit->second(*this, t, args))
		return true;

	switch (op) {
	case 0x0000: return true;
	case 0x0001: t.wakeMs = m_now + (uint32_t)getI(t, args[0]); return true;
	// SWITCH_START (0871): value, total case count, flag, default label, then 7 (case value, label) slots; SWITCH_CONTINUED (0872): 9 more slots each. Unused slots are padding
	// (-1, default label). Layout read from the arguments main.scm passes; the jump happens once all `total` cases have been collected. Labels resolve like GOTO's.
	case 0x0871: case 0x0872: {
		size_t first = op == 0x0871 ? 4 : 0;
		if (op == 0x0871) { t.swValue = getI(t, args[0]); t.swTotal = getI(t, args[1]); t.swDefault = getI(t, args[3]); t.swCases.clear(); }
		for (size_t k = first; k + 1 < args.size() && (int32_t)t.swCases.size() < t.swTotal; k += 2)
			t.swCases.push_back({getI(t, args[k]), getI(t, args[k + 1])});
		if ((int32_t)t.swCases.size() >= t.swTotal) {
			int32_t target = t.swDefault;
			for (auto &c : t.swCases) if (c.first == t.swValue) { target = c.second; break; }
			t.ip = label(t, target);
		}
		return true;
	}
	case 0x090F: return true;   // MARK_STREAMED_SCRIPT_AS_NO_LONGER_NEEDED: streamed scripts are not unloaded here
	// Environment queries answered with fixed values. DEVIATIONS: no real clock/day/language/city tracking, no garages, no weapon inventory, no player position in the VM,
	// so the locate/area/garage/weapon conditions are always false.
	case 0x01BD: ref(t, args[0]) = (int32_t)m_now; return true;      // GET_GAME_TIMER (ms)
	case 0x07D0: ref(t, args[0]) = 0; return true;                   // GET_CURRENT_DAY_OF_WEEK (fixed)
	case 0x077E: ref(t, args[0]) = 0; return true;                   // GET_AREA_VISIBLE (outside)
	case 0x09FB: ref(t, args[0]) = 0; return true;                   // GET_CURRENT_LANGUAGE (0 = American)
	case 0x0842: ref(t, args[1]) = 1; return true;                   // GET_CITY_PLAYER_IS_IN (1 = Los Santos, game start)
	case 0x0652: ref(t, args[1]) = 0; return true;                   // GET_INT_STAT (all stats 0)
	case 0x04AE: ref(t, args[0]) = getI(t, args[1]); return true;    // SET_VAR_INT_TO_CONSTANT
	case 0x03B0: case 0x0491: case 0x00FE: case 0x00EC: case 0x00A3:
		condResult(t, false, neg); return true;                      // IS_GARAGE_OPEN / HAS_CHAR_GOT_WEAPON / LOCATE_CHAR_ANY_MEANS_3D/2D / IS_CHAR_IN_AREA_2D
	// IS/SET/CLEAR_{GLOBAL,LOCAL}_VAR_BIT_{CONST,VAR,LVAR} (08B4..08C5): (variable, bit index); the const/var/lvar forms differ only in how the bit index is encoded, which getI resolves.
	case 0x08B4: case 0x08B5: case 0x08B6: case 0x08B7: case 0x08B8: case 0x08B9:
		condResult(t, ((uint32_t)ref(t, args[0]) >> (getI(t, args[1]) & 31)) & 1u, neg); return true;
	case 0x08BA: case 0x08BB: case 0x08BC: case 0x08BD: case 0x08BE: case 0x08BF:
		ref(t, args[0]) = (int32_t)((uint32_t)ref(t, args[0]) | (1u << (getI(t, args[1]) & 31))); return true;
	case 0x08C0: case 0x08C1: case 0x08C2: case 0x08C3: case 0x08C4: case 0x08C5:
		ref(t, args[0]) = (int32_t)((uint32_t)ref(t, args[0]) & ~(1u << (getI(t, args[1]) & 31))); return true;
	// Pickups / set pieces / streamed scripts: bookkeeping only. DEVIATIONS: pickups are not created in the world (CREATE_PICKUP only hands out a unique handle), nothing is ever collected,
	// set pieces are ignored, no streamed script is ever running, missions may always be started, scripted objects are never reported as existing.
	case 0x0213: { static int32_t nextPickup = 1; ref(t, args[5]) = nextPickup++; return true; }       // CREATE_PICKUP (model, type, x, y, z, out)
	case 0x032B: { static int32_t nextPickupA = 5000; ref(t, args[6]) = nextPickupA++; return true; } // CREATE_PICKUP_WITH_AMMO (model, type, ammo, x, y, z, out)
	case 0x0214: case 0x03CA: condResult(t, false, neg); return true;                                  // HAS_PICKUP_BEEN_COLLECTED / DOES_OBJECT_EXIST
	case 0x03EE: condResult(t, true, neg); return true;                                               // CAN_PLAYER_START_MISSION
	case 0x0926: ref(t, args[1]) = 0; return true;                                                     // GET_NUMBER_OF_INSTANCES_OF_STREAMED_SCRIPT
	// More pure bookkeeping (no effect here): zone gang/dealer strength + race, stunt jumps, object heading / keep-alive, pickup removal, blips, entry/exit flags, script names,
	// relationships, brain registration, car-generator ownership. Scripted objects get a unique handle but are never created; oyster/horseshoe/snapshot pickups likewise.
	case 0x076C: case 0x076A: case 0x0874: case 0x0814: case 0x01C7: case 0x0177: case 0x0215: case 0x0929: case 0x0570: case 0x018B: case 0x09B4:
	case 0x03A4: case 0x0746: case 0x0884: case 0x0A17: return true;
	case 0x095A: case 0x0959: case 0x0958: { static int32_t nextSpecial = 9000; ref(t, args[3]) = nextSpecial++; return true; }
	case 0x0107: case 0x029B: { static int32_t nextObject = 20000; ref(t, args[4]) = nextObject++; return true; }
	case 0x0517: { static int32_t nextProp = 7000; ref(t, args[4]) = nextProp++; return true; }
	// Side-effect-only commands with no visible counterpart in this port (garage / restart / object physics + proofs / LOD / fade / on-screen text / weather+time forcing / stats /
	// camera / IPL + collision requests / mission bookkeeping). DEVIATION: accepted and ignored, so none of those effects exist; text-label stores do not write the label.
	case 0x07F7: case 0x02B9: case 0x09CA: case 0x07FB: case 0x0392: case 0x04CE: case 0x016C: case 0x0827: case 0x02FA: case 0x016D: case 0x0550: case 0x08E8:
	case 0x062A: case 0x0453: case 0x0363: case 0x0111: case 0x016A: case 0x09BA: case 0x0629: case 0x01F0: case 0x09B7: case 0x0997: case 0x08F8: case 0x08CA:
	case 0x07D3: case 0x0793: case 0x0776: case 0x070D: case 0x06D1: case 0x06CF: case 0x06C8: case 0x060D: case 0x05AA: case 0x0566: case 0x054C: case 0x04E4:
	case 0x04BB: case 0x042C: case 0x03F0: case 0x03E0: case 0x03CB: case 0x0381: case 0x0373: case 0x0349: case 0x0343: case 0x0342: case 0x0340: case 0x033F:
	case 0x033E: case 0x030D: case 0x01E8: case 0x01B7: case 0x01B6: case 0x01B4: case 0x0180: case 0x0173: case 0x0169: case 0x00C0: case 0x0928:
		return true;
	case 0x0518: ref(t, args[5]) = 7500; return true;               // CREATE_FORSALE_PROPERTY_PICKUP (handle only)
	case 0x0A40: ref(t, args[4]) = 8500; return true;               // CREATE_USER_3D_MARKER (handle only)
	case 0x0176: case 0x08E1: case 0x07AF: case 0x058C: { size_t o = op == 0x058C || op == 0x08E1 ? 0 : 1; ref(t, args[o]) = 0; return true; }   // GET_OBJECT_HEADING / FIND_NUMBER_TAGS_TAGGED / GET_PLAYER_GROUP / GET_PROGRESS_PERCENTAGE
	case 0x01BB: ref(t, args[1]) = 0; ref(t, args[2]) = 0; ref(t, args[3]) = 0; return true;   // GET_OBJECT_COORDINATES (origin: scripted objects are not created)
	case 0x0400: ref(t, args[4]) = 0; ref(t, args[5]) = 0; ref(t, args[6]) = 0; return true;   // GET_OFFSET_FROM_OBJECT_IN_WORLD_COORDS
	case 0x034D: condResult(t, true, neg); return true;             // ROTATE_OBJECT: reports finished so scripts do not wait on an object that does not exist
	case 0x08AB: case 0x00FF: case 0x0118: condResult(t, false, neg); return true;   // HAS_STREAMED_SCRIPT_LOADED / LOCATE_CHAR_ON_FOOT_3D / IS_CHAR_DEAD (never loaded / false / alive)
	case 0x04F8: case 0x014C: case 0x0914: return true;                                               // ADD_SET_PIECE / SWITCH_CAR_GENERATOR / REGISTER_STREAMED_SCRIPT_INTERNAL
	case 0x09E8: ref(t, args[1]) = 0; return true;   // GET_CHAR_AREA_VISIBLE: always outside any interior (area 0); DEVIATION: interiors are not tracked
	case 0x0002: t.ip = label(t, getI(t, args[0])); return true;
	case 0x004D: if (!t.condFlag) t.ip = label(t, getI(t, args[0])); return true;
	case 0x004E: t.active = false; return true;
	case 0x004F: {
		ScmThread nt;
		nt.ip = label(t, getI(t, args[0]));
		nt.base = t.base;
		nt.isMission = t.isMission;
		nt.name = "script";
		for (size_t i = 1; i < args.size() && i - 1 < 32; i++) {
			if (args[i].type == 6) nt.locals[i - 1] = args[i].value;
			else nt.locals[i - 1] = getI(t, args[i]);
		}
		spawned.push_back(nt.ip);
		m_threads.push_back(nt);
		return true;
	}
	case 0x0417: {   // LOAD_AND_LAUNCH_MISSION_INTERNAL: index into the mission offset table at 53180
		int32_t idx = getI(t, args[0]);
		ScmThread nt;
		nt.ip = nt.base = (uint32_t)rd32(&m_mem[53180 + 4 * (size_t)idx]);
		nt.isMission = true;
		nt.name = "mission";
		spawned.push_back(nt.ip);
		m_threads.push_back(nt);
		return true;
	}
	case 0x00D7: {   // LAUNCH_MISSION: the label is the mission's offset inside main.scm
		ScmThread nt;
		nt.ip = nt.base = (uint32_t)getI(t, args[0]);
		nt.isMission = true;
		nt.name = "mission";
		spawned.push_back(nt.ip);
		m_threads.push_back(nt);
		return true;
	}
	case 0x0050: t.stack.push_back(t.ip); t.ip = label(t, getI(t, args[0])); return true;
	case 0x0051: if (!t.stack.empty()) { t.ip = t.stack.back(); t.stack.pop_back(); } return true;
	case 0x00D6: {
		int c = getI(t, args[0]);
		if (c < 8) { t.andorMode = 1; t.andorCount = c + 1; t.condFlag = true; }
		else { t.andorMode = 2; t.andorCount = c - 20 + 1; t.condFlag = false; }
		if (c == 0) { t.andorMode = 0; t.andorCount = 0; }
		return true;
	}
	case 0x0099: { // GENERATE_RANDOM_INT
		m_rng = m_rng * 1664525u + 1013904223u;
		ref(t, args[0]) = (int32_t)((m_rng >> 8) & 0x7fff);
		return true;
	}
	case 0x0098: {
		m_rng = m_rng * 1664525u + 1013904223u;
		ref(t, args[0]) = asI(((m_rng >> 8) & 0xffff) / 65536.0f);
		return true;
	}
	case 0x0917: // SWITCH_AUDIO_ZONE
		audioZoneSwitches.emplace_back(args[0].str, getI(t, args[1]) != 0);
		return true;
	case 0x0767: // SET_ZONE_POPULATION_TYPE
		zonePopType[args[0].str] = getI(t, args[1]);
		return true;
	// Player-state conditions: the viewer's player state is not visible to the VM, so on-foot answers are returned (not in a car / boat / train / flying vehicle / model, no car
	// cheats; controls on). DEVIATION: driving in the viewer is not visible to scripts.
	case 0x00DF: case 0x00DD: case 0x04A7: case 0x04C8: case 0x09AE: case 0x0445:
		condResult(t, false, neg); return true;
	case 0x09E7:
		condResult(t, true, neg); return true;
	default:
		break;
	}

	// Generic families, selected by name.
	bool cmpName = !strncmp(n, "IS_INT_", 7) || !strncmp(n, "IS_FLOAT_", 9) || !strncmp(n, "IS_NUMBER_", 10);
	if (ci->cond && args.size() == 2 && cmpName && (has(n, "_GREATER_") || has(n, "_EQUAL_TO"))) {
		bool res;
		if (isFloat() || args[0].type == 6 || args[1].type == 6) {
			float a = getF(t, args[0]), b = getF(t, args[1]);
			res = has(n, "GREATER_THAN") ? a > b : has(n, "GREATER_OR_EQUAL") ? a >= b : a == b;
		} else {
			int32_t a = getI(t, args[0]), b = getI(t, args[1]);
			res = has(n, "GREATER_THAN") ? a > b : has(n, "GREATER_OR_EQUAL") ? a >= b : a == b;
		}
		condResult(t, res, neg);
		return true;
	}
	bool arith = (op >= 0x0004 && op <= 0x0017) || (op >= 0x0058 && op <= 0x0093);
	if (arith && args.size() == 2) {
		bool timed = has(n, "TIMED");
		if (op >= 0x0004 && op <= 0x0007 || op >= 0x0084) {
			if (has(n, "CSET_")) {
				if (has(n, "INT_TO") ) { // CSET_<int>_TO_<float>: dest int from float source
					ref(t, args[0]) = (int32_t)getF(t, args[1]);
				} else {
					ref(t, args[0]) = asI((float)getI(t, args[1]));
				}
			} else {
				ref(t, args[0]) = args[1].isVar ? ref(t, args[1]) : args[1].value;
			}
			return true;
		}
		if (has(n, "ADD_") || has(n, "SUB_") || has(n, "MULT_") || has(n, "DIV_")) {
			bool f = isFloat();
			float k = timed ? m_timeStep : 1.0f;
			if (f) {
				float d = getF(t, args[0]), s = getF(t, args[1]) * k;
				d = has(n, "ADD_") ? d + s : has(n, "SUB_") ? d - s : has(n, "MULT_") ? d * s : (s != 0 ? d / s : d);
				ref(t, args[0]) = asI(d);
			} else {
				int32_t d = getI(t, args[0]), s = getI(t, args[1]);
				d = has(n, "ADD_") ? d + s : has(n, "SUB_") ? d - s : has(n, "MULT_") ? d * s : (s != 0 ? d / s : d);
				ref(t, args[0]) = d;
			}
			return true;
		}
	}
	return false;
}

void ScriptVM::runThread(ScmThread &t)
{
	std::vector<ScmArg> args;
	for (int guard = 0; guard < 3000 && t.active && t.wakeMs <= m_now; guard++) {
		if (t.ip + 2 > m_mem.size()) { t.active = false; return; }
		uint16_t raw = rd16(&m_mem[t.ip]);
		uint16_t op = raw & 0x7fff;
		bool neg = (raw & 0x8000) != 0;
		t.ip += 2;
		auto it = table().find(op);
		if (it == table().end()) {
			std::fprintf(stderr, "script: unknown opcode %04X at %u\n", op, t.ip - 2);
			t.active = false;
			return;
		}
		const CmdInfo *ci = it->second;
		int fixed = ci->fixed;
		if (op == 0x05B6) { t.ip += 128; continue; }
		if (op == 0x004C) fixed = 1;
		gDbgIp = t.ip - 2; gDbgOp = op; gDbgStage = 1;
		if (gTraceLeft && gDbgIp >= gTraceLo && gDbgIp < gTraceHi) { gTraceLeft--; std::printf("  trace ip %u op %04X %s\n", gDbgIp, op, opName(op)); }
		static uint16_t lastOps[16]; static uint32_t lastIps[16]; static int lastN = 0;
		lastOps[lastN % 16] = op; lastIps[lastN % 16] = t.ip - 2; lastN++;
		if (!readArgs(t, fixed, ci->vararg != 0, args)) {
			std::fprintf(stderr, "script: bad argument stream for %04X (%s) at %u\n", op, ci->name, t.ip);
			for (int k = 15; k >= 1; k--) if (lastN - 1 - k >= 0) std::fprintf(stderr, "   prev %04X at %u (%s)\n", lastOps[(lastN - 1 - k) % 16], lastIps[(lastN - 1 - k) % 16], t.name.c_str());
			t.active = false;
			return;
		}
		commandsRun++;
		static const bool trace = getenv("SCM_TRACE") != nullptr;
		if (trace && commandsRun < 100000) std::fprintf(stderr, "%u: %04X %s (%zu args)\n", t.ip, op, ci->name, args.size());
		t.notFlag = neg;
		gDbgStage = 2;
		if (execute(t, op, neg, args)) {
			handled[op]++;
		} else {
			unhandled[op]++;
			if (ci->cond)
				condResult(t, false, neg);
		}
	}
}

void ScriptVM::tick(uint32_t deltaMs)
{
	m_now += deltaMs;
	m_timeStep = deltaMs / 20.0f;
	for (size_t i = 0; i < m_threads.size(); i++) {
		ScmThread &t = m_threads[i];
		if (!t.active)
			continue;
		t.locals[32] += (int32_t)deltaMs;   // timerA
		t.locals[33] += (int32_t)deltaMs;   // timerB
		runThread(m_threads[i]);
	}
}
