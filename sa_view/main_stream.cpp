// sa_stream: the GTA SA map streamed around a free camera. Models and TXDs are requested through sa::Streamer
// when instances come within the load radius and are dropped again beyond the unload radius.
// usage: sa_stream.exe <SA game dir> [x y [loadRadius]]     default: Grove Street (2495 -1690), radius 300
// Controls: W/A/S/D move, Q/E down/up, Shift = fast, left mouse drag = look, Esc = quit. Stats go to sa_stream.log.
#include <rw.h>
#include <skeleton.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "BinaryIpl.h"
#include "ColBounds.h"
#include "Timecyc.h"
#include "IfpAnim.h"
#include "WaterDat.h"
#include "Streamer.h"
#include "StreamingWorld.h"

rw::EngineOpenParams engineOpenParams;

namespace {

struct Inst {
	const sa::ColModel *col = nil; // collision geometry (COL), nil when the model has none
	int id;
	rw::V3d pos;
	rw::Quat rot;
	rw::Clump *clump = nil;
	float colRadius = 0;     // bound radius from the model's COL record (CColModel +0x24 radius)
	float drawDist = 0;      // ObjectDef draw distance (first of the IDE distances)
	int lodParent = -1;      // instance that stands in for this one at a distance
	int numLodChildren = 0;  // instances that reference this one as their LOD (CEntity +0x34)
	int activeChildren = 0;  // of those, currently placed (CEntity +0x35 "rendered")
};

struct ModelState {
	rw::Clump *proto = nil;
	int users = 0;
	bool requested = false, failed = false;
};

rw::World *gWorld;
rw::Camera *gCamera;
rw::RGBA Background = {120, 160, 210, 255};
float gFarClip = 800.0f; // CTimeCycle far clip of the current hour/weather (clamped to 1000 like CTimeCycle::Update)
sa::TimecycKey gTc;

std::string gameDir;
float centerX = 2495.0f, centerY = -1690.0f, loadRadius = 1000.0f; // cap on any model's draw distance
rw::V3d camPos;
float camYaw = 0.0f, camPitch = -12.0f; // degrees; yaw 0 looks along +Y
FILE *gLog;

sa::StreamingWorld gW;
sa::Streamer *gStreamer;
std::vector<Inst> gInsts;
std::unordered_map<long long, std::vector<int>> gGrid; // cell -> instance indices
std::vector<int> gActive;                              // instances that currently have a clump
std::unordered_map<int, ModelState> gModels;
std::unordered_set<int> gPending;                      // requested models whose proto is not built yet
std::unordered_map<int, rw::TexDictionary *> gRawTxd; // streaming id -> parsed dictionary
const float kCell = 128.0f;

std::string Lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
	return s;
}

std::string Join(const std::string &rel) {
	std::string r = rel;
	std::replace(r.begin(), r.end(), '\\', '/');
	return gameDir + "/" + r;
}

long long CellKey(int cx, int cy) { return ((long long)cx << 32) ^ (unsigned int)cy; }
int CellOf(float v) { return (int)std::floor(v / kCell); }

bool IsLodName(const std::string &n) { return Lower(n).compare(0, 3, "lod") == 0; }
bool TimedVisibleAtNoon(int on, int off) { return on <= off ? (on <= 12 && 12 < off) : (12 >= on || 12 < off); }

rw::TexDictionary *ReadTxd(const std::vector<uint8_t> &bytes) {
	rw::StreamMemory s;
	s.open(const_cast<uint8_t *>(bytes.data()), (rw::uint32)bytes.size());
	rw::TexDictionary *txd = nil;
	if (rw::findChunk(&s, rw::ID_TEXDICTIONARY, nil, nil)) txd = rw::TexDictionary::streamRead(&s);
	s.close();
	return txd;
}

// Parsed TXDs, one dictionary per streaming id. Textures are looked up along the parent chain by Texture::findCB,
// so no texture is shared between dictionaries and each dictionary can be destroyed on its own.
std::vector<rw::TexDictionary *> gChain;
std::unordered_map<int, int> gTxdUsers; // streaming id -> number of prototypes whose chain contains it

rw::Texture *FindInChain(const char *name) {
	for (rw::TexDictionary *d : gChain)
		if (rw::Texture *t = d->find(name)) return t;
	return nil;
}

rw::TexDictionary *RawTxd(int id) {
	auto it = gRawTxd.find(id);
	if (it != gRawTxd.end()) return it->second;
	const std::vector<uint8_t> *b = gStreamer->Data(id);
	rw::TexDictionary *d = b ? ReadTxd(*b) : nil;
	if (d) // many SA TXDs store NEAREST; the game does not show them blocky
		FORLIST(lnk, d->textures) rw::Texture::fromDict(lnk)->setFilter(rw::Texture::LINEARMIPLINEAR);
	return gRawTxd[id] = d;
}

std::vector<int> TxdChain(int id) {
	std::vector<int> c;
	for (int t = id; t >= 0 && c.size() < 5; t = gW.TxdParentId(t)) c.push_back(t);
	return c;
}

bool TxdChainLoaded(int id) {
	for (int t : TxdChain(id))
		if (!gStreamer->IsLoaded(t)) return false;
	return true;
}

void ReleaseTxdChain(int id) {
	for (int t : TxdChain(id)) {
		if (--gTxdUsers[t] > 0) continue;
		gTxdUsers.erase(t);
		auto it = gRawTxd.find(t);
		if (it != gRawTxd.end()) {
			if (it->second) it->second->destroy();
			gRawTxd.erase(it);
		}
		gStreamer->Remove(t);
	}
}

void DestroyProto(int id, ModelState &ms) {
	ms.proto->destroy();
	ms.proto = nil;
	ms.requested = false;
	gStreamer->Remove(id);
	int txd = gW.ModelTxdId(id);
	if (txd >= 0) ReleaseTxdChain(txd);
}

// "Extra Vert Colour" geometry plugin (id 0x253F2F9, CCustomBuildingDNPipeline 0x5D6DE0): u32 flag, then numVerts RGBA night colours.
// The day colours are the geometry's own pre-lit colours.
struct ExtraVertColour { rw::RGBA *night, *day; };
int gExtraVertOffset = -1;
constexpr rw::uint32 ID_EXTRAVERTCOLOUR = 0x253F2F9;
float gDayNightBalance = -1.0f; // 0 = day colours, 1 = night colours (CCustomBuildingDNPipeline::m_fDNBalanceParam)

void *CreateExtraVert(void *obj, rw::int32 off, rw::int32) { PLUGINOFFSET(ExtraVertColour, obj, off)->night = nil; PLUGINOFFSET(ExtraVertColour, obj, off)->day = nil; return obj; }
void *DestroyExtraVert(void *obj, rw::int32 off, rw::int32) {
	ExtraVertColour *e = PLUGINOFFSET(ExtraVertColour, obj, off);
	free(e->night);
	free(e->day);
	e->night = e->day = nil;
	return obj;
}
rw::Stream *ReadExtraVert(rw::Stream *st, rw::int32 len, void *obj, rw::int32 off, rw::int32) {
	rw::Geometry *g = (rw::Geometry *)obj;
	rw::uint32 flag = 0;
	st->read32(&flag, 4);
	size_t bytes = (size_t)g->numVertices * 4;
	if (flag && len >= (rw::int32)(4 + bytes)) {
		ExtraVertColour *e = PLUGINOFFSET(ExtraVertColour, obj, off);
		e->night = (rw::RGBA *)malloc(bytes);
		st->read8(e->night, (rw::uint32)bytes);
		if (g->colors) { e->day = (rw::RGBA *)malloc(bytes); memcpy(e->day, g->colors, bytes); }
		len -= (rw::int32)(4 + bytes);
	} else
		len -= 4;
	if (len > 0) st->seek(len);
	return st;
}
rw::Stream *WriteExtraVert(rw::Stream *st, rw::int32, void *, rw::int32, rw::int32) { return st; }
rw::int32 SizeExtraVert(void *, rw::int32, rw::int32) { return 0; }

// lerp(day, night, balance) written into the geometry's colours (re-instanced on the next render)
void ApplyDayNight(rw::Clump *c) {
	FORLIST(lnk, c->atomics) {
		rw::Geometry *g = rw::Atomic::fromClump(lnk)->geometry;
		ExtraVertColour *e = PLUGINOFFSET(ExtraVertColour, g, gExtraVertOffset);
		if (!e->night || !e->day || !g->colors) continue;
		g->lock(rw::Geometry::LOCKPRELIGHT);
		for (int i = 0; i < g->numVertices; i++) {
			rw::uint8 *o = (rw::uint8 *)&g->colors[i], *d = (rw::uint8 *)&e->day[i], *n = (rw::uint8 *)&e->night[i];
			for (int k = 0; k < 3; k++) o[k] = (rw::uint8)(d[k] + (n[k] - d[k]) * gDayNightBalance);
		}
		g->unlock();
	}
}

// Game clock. CClock::Update (0x52CF10): one game minute passes per ms_nMillisecondsPerGameMinute of game time.
// The value is runtime-set (0 in the exe image); 1000 is taken from gta-reversed. SA_CLOCK=0 freezes the clock.
constexpr float kMsPerGameMinute = 1000.0f;
float gGameMinutes = 12.0f * 60.0f; // minutes since midnight
// Weather: CWeather::Update (0x72B850) keeps old and new weather and blends them by the fraction of the hour elapsed; on each hour
// rollover old = new and new = forecast table[region][index++ & 63] (tables at 0x8D5EB0, 64 bytes per region; region by position,
// 0x72A640). Starting state from CWeather::Init (0x72A480): old = new = 0, region 0, index 0. SA_WEATHER forces one weather (ForceWeather).
// DEVIATION: only the timecyc blend follows the weather; rain, fog banks, sandstorm, wet roads and lightning are not implemented.
const uint8_t kForecast[5][64] = {
	{13,13,13,13,13,13,13,13,14,15,9,9,15,14,13,14,14,13,13,13,13,14,14,13,13,13,13,13,13,13,13,13,13,13,14,14,14,14,14,14,15,9,9,15,16,16,16,16,15,14,14,14,14,13,13,13,13,13,13,13,13,13,13,13},
	{2,2,0,0,0,2,2,2,3,0,3,2,0,0,3,3,3,4,4,0,0,1,1,0,2,2,2,2,2,2,2,2,2,2,3,3,3,3,3,3,2,2,3,3,0,0,16,0,4,1,1,1,1,0,0,2,2,2,2,2,2,2,2,2},
	{5,5,5,5,5,7,7,9,9,7,5,5,5,5,5,7,7,7,8,8,7,7,5,5,5,5,6,6,5,7,9,9,7,5,5,7,5,5,6,6,5,5,5,7,7,8,8,7,7,9,9,5,5,6,6,6,5,5,6,7,7,6,6,5},
	{11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,10,10,10,10,10,11,11,11,11,11,11,11,11,11,10,11,11,11,11,10,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,10,10,12,12,10,12,10,10},
	{17,17,17,19,19,17,17,17,17,17,17,17,17,17,17,17,17,17,17,18,18,18,18,18,17,17,17,17,17,17,17,17,17,17,17,17,17,19,19,19,17,17,17,17,17,17,17,17,17,17,18,18,18,17,17,17,17,17,17,17,19,19,19,17},
};
int gWeather = 0, gOldWeather = 0, gForecastIdx = 0, gWeatherRegion = 0;
bool gWeatherForced = false;
float gWeatherPrevFrac = 0.0f;

// CWeather::UpdateWeatherRegion (0x72A640): world rectangles -> forecast table (0 countryside, 1 LA, 2 SF, 3 LV, 4 desert)
void UpdateWeatherRegion(float x, float y) {
	if (x > 1000.0f && y > 910.0f) gWeatherRegion = 3;
	else if (x > -850.0f && x < 1000.0f && y > 1280.0f) gWeatherRegion = 4;
	else if (x < -1430.0f && y > -580.0f && y < 1430.0f) gWeatherRegion = 2;
	else if (x <= 250.0f || x >= 3000.0f || y <= -3000.0f || y >= -850.0f) gWeatherRegion = 0;
	else gWeatherRegion = 1;
}
sa::Timecyc gTimecyc;
rw::Light *gAmbient = nil;
rw::Light *gSunLight = nil;
rw::V3d gToSun = {0.5f, -0.5f, 0.7f}; // unit vector from the world to the sun

// timecyc sample + day/night balance for the current game time
void ApplyTimeOfDay() {
	float hour = gGameMinutes / 60.0f;
	float frac = std::fmod(gGameMinutes, 60.0f) / 60.0f;
	if (!gWeatherForced && frac < gWeatherPrevFrac) {
		UpdateWeatherRegion(camPos.x, camPos.y);
		gOldWeather = gWeather;
		gForecastIdx = (gForecastIdx + 1) & 63;
		gWeather = kForecast[gWeatherRegion][gForecastIdx];
		fprintf(gLog, "weather: %d -> %d (region %d, forecast slot %d)\n", gOldWeather, gWeather, gWeatherRegion, gForecastIdx);
	}
	gWeatherPrevFrac = frac;
	gTc = gWeatherForced ? gTimecyc.Sample(gWeather, hour)
	                     : sa::Timecyc::Lerp(gTimecyc.Sample(gOldWeather, hour), gTimecyc.Sample(gWeather, hour), frac);
	// Lightning (CWeather::Update 0x72B96E, rolled once per frame like the original): only while both old and new weather are rainy.
	// A flash forces sky top/bottom, fog and ambient to white (app_game.cpp / app_light.cpp). DEVIATION: no thunder sound, pad shake, or lightning bolt/cloud flash.
	{
		static bool burst = false, flash = false;
		static unsigned frame = 0, start = 0, lastChange = 0;
		static auto t0l = std::chrono::steady_clock::now();
		unsigned nowMs = (unsigned)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0l).count();
		frame++;
		auto pct = [](float p) { return (float)rand() / (float)RAND_MAX * 100.0f < p; };
		bool rainy = (gOldWeather == 8 || gOldWeather == 16) && (gWeather == 8 || gWeather == 16);
		if (rainy) {
			if (burst) {
				if (pct(9.4f)) { burst = false; flash = false; }
				else if (nowMs - lastChange > 50) { bool was = flash; flash = rand() & 1; if (flash != was) lastChange = nowMs; }
			} else if (pct(0.6f)) { start = frame; lastChange = nowMs; burst = true; flash = true; }
			else flash = false;
		} else { burst = flash = false; }
		if (getenv("SA_LIGHTNING")) flash = true; // test aid
		if (flash && gTc.ok) {
			for (int i = 0; i < 3; i++) gTc.skyTop[i] = gTc.skyBottom[i] = gTc.ambient[i] = gTc.ambientObj[i] = 255.0f;
		}
	}
	if (gTc.ok) {
		gFarClip = std::min(gTc.farClip, 1000.0f);
		Background = {(uint8_t)gTc.skyBottom[0], (uint8_t)gTc.skyBottom[1], (uint8_t)gTc.skyBottom[2], 255};
		gCamera->setFarPlane(gFarClip);
		gCamera->fogPlane = std::min(gTc.fogStart, gFarClip - 1.0f);
		if (gAmbient) gAmbient->setColor(gTc.ambientObj[0] / 255.0f, gTc.ambientObj[1] / 255.0f, gTc.ambientObj[2] / 255.0f);
	}
	// CTimeCycle::Update (0x560A59, constants read from the exe): vector to the sun = normalise(0.7 + sin a, -0.7, 0.2 - cos a), a = minutes * pi / 720
	{
		float a = gGameMinutes * 3.14159265f / 720.0f;
		rw::V3d v = {0.7f + std::sin(a), -0.7f, 0.2f - std::cos(a)};
		float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
		gToSun = {v.x / l, v.y / l, v.z / l};
		if (gSunLight) {
			rw::V3d dir = {-gToSun.x, -gToSun.y, -gToSun.z}, up = {0, 0, 1};
			rw::Frame *lf = gSunLight->getFrame();
			lf->matrix.lookAt(dir, up);
			lf->matrix.update();
			lf->updateObjects();
			if (gTc.ok) gSunLight->setColor(gTc.directional[0] / 255.0f, gTc.directional[1] / 255.0f, gTc.directional[2] / 255.0f);
		}
	}
	// CCustomBuildingRenderer::UpdateDayNightBalanceParam (0x5D7F80); decompiled in the exe, curve identical
	float m = gGameMinutes;
	float bal = m < 360.0f ? 1.0f : m < 420.0f ? (420.0f - m) / 60.0f : m < 1200.0f ? 0.0f : m >= 1260.0f ? 1.0f : 1.0f - (1260.0f - m) / 60.0f;
	if (std::fabs(bal - gDayNightBalance) > 0.004f || gDayNightBalance < 0.0f) {
		gDayNightBalance = bal;
		for (auto &kv : gModels)
			if (kv.second.proto) ApplyDayNight(kv.second.proto);
	}
}

// Builds the prototype once the model bytes and its whole TXD chain are in memory.
void TryBuild(int id, ModelState &ms) {
	const std::vector<uint8_t> *b = gStreamer->Data(id);
	if (!b) return;
	int txd = gW.ModelTxdId(id);
	if (txd >= 0 && !TxdChainLoaded(txd)) return;
	gChain.clear();
	if (txd >= 0)
		for (int t : TxdChain(txd))
			if (rw::TexDictionary *d = RawTxd(t)) gChain.push_back(d);
	rw::TexDictionary::setCurrent(nil);
	rw::Texture::findCB = FindInChain;
	rw::StreamMemory s;
	s.open(const_cast<uint8_t *>(b->data()), (rw::uint32)b->size());
	if (rw::findChunk(&s, rw::ID_CLUMP, nil, nil)) ms.proto = rw::Clump::streamRead(&s);
	s.close();
	if (ms.proto) ApplyDayNight(ms.proto);
	if (!ms.proto) ms.failed = true;
	else if (txd >= 0)
		for (int t : TxdChain(txd)) gTxdUsers[t]++;
	gPending.erase(id);
}

const float kMaxRange = 1000.0f;

// Text IPLs come first in a "name -> local index -> global index" table because the binary streaming IPLs
// (X_streamN) reference their LOD parent by index into the text IPL X (CIplStore::LoadIpl, see BinaryIpl.h).
std::unordered_map<std::string, sa::ColModel> gColBounds; // lower-case model name -> COL record (bounds and geometry)

void LoadColBounds() {
	const sa::ImgV2 &gta3 = gW.archives[0];
	size_t files = 0;
	for (auto &e : gta3.Entries()) {
		auto dot = e.name.rfind('.');
		if (dot == std::string::npos || Lower(e.name.substr(dot)) != ".col") continue;
		std::vector<uint8_t> buf;
		if (gta3.Read(e, buf)) { sa::ParseColModels(buf.data(), buf.size(), gColBounds); files++; }
	}
	fprintf(gLog, "COL bounds: %zu models from %zu files\n", gColBounds.size(), files);
}

void CollectPlacements() {
	LoadColBounds();
	std::unordered_map<std::string, std::vector<int>> localToGlobal; // lower-case IPL name -> global idx or -1
	auto add = [&](int id, float x, float y, float z, const float q[4], uint32_t interiorByte) -> int {
		if (interiorByte) return -1;
		const sa::ObjectDef *d = gW.ide.Find(id);
		if (!d) return -1;
		if (d->timed && !TimedVisibleAtNoon(d->timeOn, d->timeOff)) return -1;
		Inst i;
		i.id = id;
		i.pos = {x, y, z};
		i.rot = {-q[0], -q[1], -q[2], q[3]}; // IPL quaternions are stored conjugated
		i.drawDist = d->drawDist[0];
		auto cb = gColBounds.find(Lower(d->model));
		if (cb != gColBounds.end()) { i.colRadius = cb->second.bound.radius; i.col = &cb->second; }
		int idx = (int)gInsts.size();
		gGrid[CellKey(CellOf(x), CellOf(y))].push_back(idx);
		gInsts.push_back(i);
		return idx;
	};
	std::vector<std::pair<int, int>> textLinks; // (global idx, local lod index) within the same text IPL
	std::vector<sa::DatDirective> dat;
	sa::LoadGtaDat(gameDir + "/data/gta.dat", dat);
	for (auto &d : dat) {
		if (d.kind != "IPL") continue;
		sa::IplText ipl;
		bool bin = false;
		if (!sa::LoadIplText(Join(d.arg), ipl, bin) || bin) continue;
		std::string path = Lower(d.arg), name = path.substr(path.find_last_of("\\/") + 1);
		name = name.substr(0, name.rfind('.'));
		std::vector<int> &map = localToGlobal[name];
		for (auto &i : ipl.insts) {
			int g = add(i.id, i.pos[0], i.pos[1], i.pos[2], i.rot, (uint32_t)i.interior);
			map.push_back(g);
			if (g >= 0 && i.lod >= 0) textLinks.emplace_back(g, i.lod);
		}
		for (auto &l : textLinks) if (l.second < (int)map.size() && map[l.second] >= 0) gInsts[l.first].lodParent = map[l.second];
		textLinks.clear();
	}
	const sa::ImgV2 &gta3 = gW.archives[0];
	for (auto &e : gta3.Entries()) {
		auto dot = e.name.rfind('.');
		if (dot == std::string::npos || Lower(e.name.substr(dot)) != ".ipl") continue;
		std::vector<uint8_t> buf;
		sa::BinaryIpl b;
		if (!gta3.Read(e, buf) || !sa::ParseBinaryIpl(buf.data(), buf.size(), b, nullptr)) continue;
		std::string base = Lower(e.name.substr(0, dot));
		size_t us = base.find("_stream");
		const std::vector<int> *parent = nullptr;
		if (us != std::string::npos) {
			auto it = localToGlobal.find(base.substr(0, us));
			if (it != localToGlobal.end()) parent = &it->second;
		}
		for (auto &i : b.insts) {
			int g = add(i.modelId, i.pos[0], i.pos[1], i.pos[2], i.rot, i.flags & 0xFF);
			if (g >= 0 && parent && i.lodIndex >= 0 && i.lodIndex < (int)parent->size() && (*parent)[i.lodIndex] >= 0)
				gInsts[g].lodParent = (*parent)[i.lodIndex];
		}
	}
	int withParent = 0;
	for (auto &i : gInsts)
		if (i.lodParent >= 0) { withParent++; gInsts[i.lodParent].numLodChildren++; }
	fprintf(gLog, "LOD links: %d of %zu instances have a LOD parent\n", withParent, gInsts.size());
}

void Place(int idx, ModelState &ms) {
	Inst &in = gInsts[idx];
	rw::Clump *c = ms.proto->clone(); // the prototype is not in the world, so the clone is not added automatically
	rw::Frame *f = c->getFrame();
	f->rotate(&in.rot, rw::COMBINEREPLACE);
	rw::V3d pos = in.pos;
	f->translate(&pos, rw::COMBINEPOSTCONCAT);
	gWorld->addClump(c);
	in.clump = c;
	ms.users++;
	gActive.push_back(idx);
	if (in.lodParent >= 0) gInsts[in.lodParent].activeChildren++;
}

void Unplace(size_t activePos) {
	Inst &in = gInsts[gActive[activePos]];
	gWorld->removeClump(in.clump);
	in.clump->destroy();
	in.clump = nil;
	if (in.lodParent >= 0) gInsts[in.lodParent].activeChildren--;
	ModelState &ms = gModels[in.id];
	if (--ms.users == 0 && ms.proto) DestroyProto(in.id, ms);
	gActive[activePos] = gActive.back();
	gActive.pop_back();
}

double gNextLog = 0;
int gLoadedModels = 0;

// CRenderer::SetupMapEntityVisibility (0x553F60), distance part. Differences from the original:
// colModel radius comes from the COL headers in gta3.img (models without a record get 0), lodDistMultiplier follows CCamera::Process (FOV assumed 70), the "big building" flag
// follows CFileLoader but uses the final multiplier (the original uses the one at load time), no occlusion/tunnel/on-screen test, no alpha fade.
constexpr float kFading = 20.0f, kStreamMargin = 50.0f, kLowLodDistance = 150.0f, kLowLodDistScale = 1.0f;
// CCamera::Process (0x52B730): m_fLODDistMultiplier = ms_lodDistScale (0x8CD800 = 1.2) * 70 / CDraw::ms_fFOV (0x8D5038),
// or 1.0 during a cutscene (CCutsceneMgr::ms_running 0xB5F851 set and ms_useLodMultiplier 0xB5F853 clear; names from gta-reversed).
// This viewer has no cutscenes, so it always takes the FOV branch.
// ms_fFOV is set from the active camera mode every frame (gta-reversed Cam.cpp: 70 for ped/vehicle follow cams, 60/80 for some others),
// so the multiplier tracks the FOV this viewer renders with. SA_FOV overrides the default 70.
constexpr float kLodDistScale = 1.2f;
float gFovDeg = 70.0f;
inline float LodDistMult() { return kLodDistScale * 70.0f / gFovDeg; }

// CFileLoader (0x5B51E0, decompiled): SetupBigBuilding is called for an instance with LOD children or lodMult * drawDist > 300.
bool IsBigBuilding(const Inst &in) { return in.numLodChildren > 0 || LodDistMult() * in.drawDist > 300.0f; }

float DrawLimit(const Inst &in, float &fading) {
	float farClip = gFarClip;
	float d = std::min(LodDistMult() * in.drawDist, farClip + in.colRadius);
	fading = kFading;
	if (in.lodParent < 0) {
		float f = std::min(in.drawDist, d);
		if (f > kLowLodDistance) fading = f / 15.0f + 10.0f;
		if (IsBigBuilding(in)) d *= kLowLodDistScale;
	}
	return d;
}

float Dist3(const Inst &in) {
	float dx = in.pos.x - camPos.x, dy = in.pos.y - camPos.y, dz = in.pos.z - camPos.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// a LOD parent with several children is replaced by them once all of them are rendered (CRenderer::ProcessLodRenderLists)
bool CoveredByChildren(const Inst &in) { return in.numLodChildren > 1 && in.activeChildren >= in.numLodChildren; }

// ---- collision: ground height under a point (vertical ray against COL triangles and boxes of nearby instances) ----
rw::V3d QuatRotate(const rw::Quat &q, const rw::V3d &v, bool inverse) {
	float ux = inverse ? -q.x : q.x, uy = inverse ? -q.y : q.y, uz = inverse ? -q.z : q.z, w = q.w;
	rw::V3d c1 = {uy * v.z - uz * v.y, uz * v.x - ux * v.z, ux * v.y - uy * v.x}; // u x v
	rw::V3d c2 = {uy * c1.z - uz * c1.y, uz * c1.x - ux * c1.z, ux * c1.y - uy * c1.x}; // u x (u x v)
	return {v.x + 2.0f * (w * c1.x + c2.x), v.y + 2.0f * (w * c1.y + c2.y), v.z + 2.0f * (w * c1.z + c2.z)};
}

// distance along a ray to a triangle (Moller-Trumbore), -1 on miss
float RayTri(const rw::V3d &o, const rw::V3d &d, const float *a, const float *b, const float *c) {
	float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
	float p[3] = {d.y * e2[2] - d.z * e2[1], d.z * e2[0] - d.x * e2[2], d.x * e2[1] - d.y * e2[0]};
	float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
	if (std::fabs(det) < 1e-9f) return -1;
	float inv = 1.0f / det, t[3] = {o.x - a[0], o.y - a[1], o.z - a[2]};
	float u = (t[0] * p[0] + t[1] * p[1] + t[2] * p[2]) * inv;
	if (u < 0 || u > 1) return -1;
	float q[3] = {t[1] * e1[2] - t[2] * e1[1], t[2] * e1[0] - t[0] * e1[2], t[0] * e1[1] - t[1] * e1[0]};
	float v = (d.x * q[0] + d.y * q[1] + d.z * q[2]) * inv;
	if (v < 0 || u + v > 1) return -1;
	return (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
}

float RayBox(const rw::V3d &o, const rw::V3d &d, const sa::ColBox &b) {
	float tmin = 0, tmax = 1e9f;
	const float oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z};
	for (int k = 0; k < 3; k++) {
		if (std::fabs(dd[k]) < 1e-9f) { if (oo[k] < b.min[k] || oo[k] > b.max[k]) return -1; continue; }
		float t1 = (b.min[k] - oo[k]) / dd[k], t2 = (b.max[k] - oo[k]) / dd[k];
		if (t1 > t2) std::swap(t1, t2);
		tmin = std::max(tmin, t1); tmax = std::min(tmax, t2);
		if (tmin > tmax) return -1;
	}
	return tmin;
}

// Highest collision surface at or below zFrom (searching down to zFrom - maxDrop); returns false when there is none.
// Big buildings (LOD models) have collision disabled in the original (CEntity::SetupBigBuilding), so they are skipped.
bool GroundHeight(float x, float y, float zFrom, float maxDrop, float &outZ) {
	bool found = false;
	float best = zFrom - maxDrop;
	for (int cx = CellOf(x) - 1; cx <= CellOf(x) + 1; cx++)
		for (int cy = CellOf(y) - 1; cy <= CellOf(y) + 1; cy++) {
			auto g = gGrid.find(CellKey(cx, cy));
			if (g == gGrid.end()) continue;
			for (int idx : g->second) {
				const Inst &in = gInsts[idx];
				if (!in.col || IsBigBuilding(in)) continue;
				float reach = in.col->bound.radius + std::sqrt(in.col->bound.center[0] * in.col->bound.center[0] + in.col->bound.center[1] * in.col->bound.center[1] +
				                                              in.col->bound.center[2] * in.col->bound.center[2]) + 1.0f;
				if (std::fabs(in.pos.x - x) > reach || std::fabs(in.pos.y - y) > reach) continue;
				rw::V3d o = QuatRotate(in.rot, {x - in.pos.x, y - in.pos.y, zFrom - in.pos.z}, true);
				rw::V3d d = QuatRotate(in.rot, {0, 0, -1}, true);
				float tBest = zFrom - best; // only hits nearer than the current best are interesting
				const std::vector<float> &vs = in.col->verts;
				for (const sa::ColTri &t : in.col->tris) {
					float h = RayTri(o, d, &vs[t.a * 3], &vs[t.b * 3], &vs[t.c * 3]);
					if (h >= 0 && h < tBest) tBest = h;
				}
				for (const sa::ColBox &b : in.col->boxes) {
					float h = RayBox(o, d, b);
					if (h >= 0 && h < tBest) tBest = h;
				}
				if (tBest < zFrom - best) { best = zFrom - tBest; found = true; }
			}
		}
	if (found) outZ = best;
	return found;
}

// nearest hit of the ray o + t*d (d unit length) against the COL geometry of nearby instances within maxT; -1 when nothing is hit
float RayCastWorld(const rw::V3d &o, const rw::V3d &d, float maxT) {
	float best = maxT;
	bool found = false;
	for (int cx = CellOf(o.x) - 1; cx <= CellOf(o.x) + 1; cx++)
		for (int cy = CellOf(o.y) - 1; cy <= CellOf(o.y) + 1; cy++) {
			auto g = gGrid.find(CellKey(cx, cy));
			if (g == gGrid.end()) continue;
			for (int idx : g->second) {
				const Inst &in = gInsts[idx];
				if (!in.col || IsBigBuilding(in)) continue;
				const float *bc = in.col->bound.center;
				float reach = in.col->bound.radius + std::sqrt(bc[0] * bc[0] + bc[1] * bc[1] + bc[2] * bc[2]) + maxT + 1.0f;
				float dx = in.pos.x - o.x, dy = in.pos.y - o.y, dz = in.pos.z - o.z;
				if (dx * dx + dy * dy + dz * dz > reach * reach) continue;
				rw::V3d lo = QuatRotate(in.rot, {o.x - in.pos.x, o.y - in.pos.y, o.z - in.pos.z}, true);
				rw::V3d ld = QuatRotate(in.rot, d, true);
				const std::vector<float> &vs = in.col->verts;
				for (const sa::ColTri &t : in.col->tris) {
					float h = RayTri(lo, ld, &vs[t.a * 3], &vs[t.b * 3], &vs[t.c * 3]);
					if (h >= 0 && h < best) { best = h; found = true; }
				}
				for (const sa::ColBox &b : in.col->boxes) {
					float h = RayBox(lo, ld, b);
					if (h >= 0 && h < best) { best = h; found = true; }
				}
			}
		}
	return found ? best : -1.0f;
}

// closest point on triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5)
rw::V3d ClosestOnTri(const rw::V3d &p, const float *A, const float *B, const float *C) {
	auto sub = [](const float *x, const float *y) { return rw::V3d{x[0] - y[0], x[1] - y[1], x[2] - y[2]}; };
	auto dot = [](const rw::V3d &x, const rw::V3d &y) { return x.x * y.x + x.y * y.y + x.z * y.z; };
	rw::V3d a = {A[0], A[1], A[2]}, ab = sub(B, A), ac = sub(C, A), ap = {p.x - a.x, p.y - a.y, p.z - a.z};
	float d1 = dot(ab, ap), d2 = dot(ac, ap);
	if (d1 <= 0 && d2 <= 0) return a;
	rw::V3d bp = {p.x - B[0], p.y - B[1], p.z - B[2]};
	float d3 = dot(ab, bp), d4 = dot(ac, bp);
	if (d3 >= 0 && d4 <= d3) return {B[0], B[1], B[2]};
	float vc = d1 * d4 - d3 * d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0) { float v = d1 / (d1 - d3); return {a.x + ab.x * v, a.y + ab.y * v, a.z + ab.z * v}; }
	rw::V3d cp = {p.x - C[0], p.y - C[1], p.z - C[2]};
	float d5 = dot(ab, cp), d6 = dot(ac, cp);
	if (d6 >= 0 && d5 <= d6) return {C[0], C[1], C[2]};
	float vb = d5 * d2 - d1 * d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0) { float w = d2 / (d2 - d6); return {a.x + ac.x * w, a.y + ac.y * w, a.z + ac.z * w}; }
	float va = d3 * d6 - d5 * d4;
	if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
		float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		return {B[0] + (C[0] - B[0]) * w, B[1] + (C[1] - B[1]) * w, B[2] + (C[2] - B[2]) * w};
	}
	float denom = 1.0f / (va + vb + vc), v = vb * denom, w = vc * denom;
	return {a.x + ab.x * v + ac.x * w, a.y + ab.y * v + ac.y * w, a.z + ab.z * v + ac.z * w};
}

// Pushes a sphere out of the collision geometry of nearby instances; only the horizontal part of the push is applied and
// near-horizontal (walkable) surfaces are ignored. Returns true if anything pushed.
bool gWalk = false, gGrounded = true, gSwimming = false;
struct Contact { rw::V3d n; float depth; };
bool PushOutSphere(rw::V3d &c, float r, std::vector<Contact> *out = nullptr) {
	bool pushed = false;
	for (int cx = CellOf(c.x - 3); cx <= CellOf(c.x + 3); cx++)
		for (int cy = CellOf(c.y - 3); cy <= CellOf(c.y + 3); cy++) {
			auto g = gGrid.find(CellKey(cx, cy));
			if (g == gGrid.end()) continue;
			for (int idx : g->second) {
				const Inst &in = gInsts[idx];
				if (!in.col || IsBigBuilding(in)) continue;
				float reach = in.col->bound.radius + std::sqrt(in.col->bound.center[0] * in.col->bound.center[0] + in.col->bound.center[1] * in.col->bound.center[1] +
				                                              in.col->bound.center[2] * in.col->bound.center[2]) + r + 0.5f;
				float dx = in.pos.x - c.x, dy = in.pos.y - c.y, dz = in.pos.z - c.z;
				if (dx * dx + dy * dy + dz * dz > reach * reach) continue;
				rw::V3d lc = QuatRotate(in.rot, {c.x - in.pos.x, c.y - in.pos.y, c.z - in.pos.z}, true);
				const std::vector<float> &vs = in.col->verts;
				for (const sa::ColTri &t : in.col->tris) {
					rw::V3d q = ClosestOnTri(lc, &vs[t.a * 3], &vs[t.b * 3], &vs[t.c * 3]);
					rw::V3d d = {lc.x - q.x, lc.y - q.y, lc.z - q.z};
					float dist2 = d.x * d.x + d.y * d.y + d.z * d.z;
					if (dist2 >= r * r || dist2 < 1e-12f) continue;
					rw::V3d wd = QuatRotate(in.rot, d, false); // world-space push direction
					float dist = std::sqrt(dist2), nz = std::fabs(wd.z) / dist;
					if (out) { out->push_back({{wd.x / dist, wd.y / dist, wd.z / dist}, r - dist}); pushed = true; continue; }
					if (nz > 0.7f) continue; // floor or ceiling, not a wall
					float k = (r - dist) / dist;
					c.x += wd.x * k; c.y += wd.y * k;
					lc = QuatRotate(in.rot, {c.x - in.pos.x, c.y - in.pos.y, c.z - in.pos.z}, true);
					pushed = true;
				}
				for (const sa::ColBox &b : in.col->boxes) {
					rw::V3d q = {std::min(std::max(lc.x, b.min[0]), b.max[0]), std::min(std::max(lc.y, b.min[1]), b.max[1]), std::min(std::max(lc.z, b.min[2]), b.max[2])};
					rw::V3d d = {lc.x - q.x, lc.y - q.y, lc.z - q.z};
					float dist2 = d.x * d.x + d.y * d.y + d.z * d.z;
					if (dist2 >= r * r || dist2 < 1e-12f) continue;
					rw::V3d wd = QuatRotate(in.rot, d, false);
					float dist = std::sqrt(dist2);
					if (out) { out->push_back({{wd.x / dist, wd.y / dist, wd.z / dist}, r - dist}); pushed = true; continue; }
					if (std::fabs(wd.z) / dist > 0.7f) continue;
					float k = (r - dist) / dist;
					c.x += wd.x * k; c.y += wd.y * k;
					lc = QuatRotate(in.rot, {c.x - in.pos.x, c.y - in.pos.y, c.z - in.pos.z}, true);
					pushed = true;
				}
			}
		}
	return pushed;
}

// ped collision model, verified at 0x5BAB5C (CTempColModels): 3 spheres r=0.35 at z -0.2/0.2/0.6 about the ped origin, which is 1.0 above
// the feet (box min z = -1.0). In walk mode camPos IS the ped origin (the camera itself is the third-person follow camera).
void CollideWalker() {
	// CPhysical::ProcessShiftSectorList (0x546670, as reversed in gta-reversed): sum the contact normals of all three spheres, track the deepest contact, clamp the
	// sum to unit length, then move the ped by shift * (1.5 * depth) (+ the 0.3 / 0.005 terms of the reference); one pass per frame. For a ped on the ground the
	// contact normal first has its xy part normalised and is renormalised (CPed::ProcessEntityCollision 0x5E2530, second contact loop).
	// DEVIATIONS: only the horizontal part of the shift is applied (height comes from my ground line), the 0x5E2530 special case for near-vertical-down floor
	// normals is skipped, vehicles/peds/objects are not contact sources, and the original's follow-up ProcessCollisionSectorList check is not done.
	constexpr float kRadius = 0.35f;
	const float heights[3] = {-0.2f, 0.2f, 0.6f}; // relative to the ped origin
	std::vector<Contact> contacts;
	for (float h : heights) {
		rw::V3d c = {camPos.x, camPos.y, camPos.z + h};
		PushOutSphere(c, kRadius, &contacts);
	}
	rw::V3d shift = {0, 0, 0};
	float maxDepth = 0.0f;
	int n = 0;
	for (Contact &ct : contacts) {
		if (ct.depth <= 0.0f) continue;
		rw::V3d v = ct.n;
		if (gGrounded) {
			float hz = std::sqrt(v.x * v.x + v.y * v.y);
			if (hz != 0.0f) { v.x /= hz; v.y /= hz; }
			float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
			if (l > 0.0f) { v.x /= l; v.y /= l; v.z /= l; }
		}
		shift.x += v.x; shift.y += v.y; shift.z += v.z;
		maxDepth = std::max(maxDepth, ct.depth);
		n++;
	}
	static int dbgN = 0;
	if (getenv("SA_COLDEBUG") && (dbgN++ % 30 == 0)) fprintf(gLog, "collide: contacts=%zu used=%d at (%.1f,%.1f,%.1f)\n", contacts.size(), n, camPos.x, camPos.y, camPos.z);
	if (!n) return;
	float mag = std::sqrt(shift.x * shift.x + shift.y * shift.y + shift.z * shift.z);
	if (mag > 1.0f) { shift.x /= mag; shift.y /= mag; shift.z /= mag; }
	float dx, dy;
	if (shift.z >= -0.5f) {
		float dm = 1.5f * maxDepth;
		dx = shift.x * dm; dy = shift.y * dm;
		if (dm >= 0.005f) { if (dm > 0.3f) { dx += shift.x * maxDepth * 0.3f; dy += shift.y * maxDepth * 0.3f; } }
		else { dx += shift.x * maxDepth * 0.005f; dy += shift.y * maxDepth * 0.005f; }
	} else {
		dx = shift.x * maxDepth * 0.75f; dy = shift.y * maxDepth * 0.75f;
	}
	camPos.x += dx; camPos.y += dy;
}

// Walk mode (G): the camera is a point 1.7 m above the ground with gravity. DEVIATION: the original uses a ped with a collision
// capsule, animations and physics; horizontal collision uses the exe's three ped spheres (CollideWalker); only the eye height is assumed.
// Water height at (x, y) from the water.dat polygons (no waves), false when no polygon covers the point.
std::vector<sa::WaterPoly> gWater;
bool WaterLevelAt(float x, float y, float &z) {
	for (const sa::WaterPoly &w : gWater) {
		float minx = w.x[0], maxx = w.x[0], miny = w.y[0], maxy = w.y[0];
		for (int i = 1; i < w.n; i++) { minx = std::min(minx, w.x[i]); maxx = std::max(maxx, w.x[i]); miny = std::min(miny, w.y[i]); maxy = std::max(maxy, w.y[i]); }
		if (x < minx || x > maxx || y < miny || y > maxy) continue;
		bool inside = true;
		if (w.n == 3) { // the bounding box is not enough for a triangle: barycentric test
			float d = (w.y[1] - w.y[2]) * (w.x[0] - w.x[2]) + (w.x[2] - w.x[1]) * (w.y[0] - w.y[2]);
			if (std::fabs(d) > 1e-6f) {
				float a = ((w.y[1] - w.y[2]) * (x - w.x[2]) + (w.x[2] - w.x[1]) * (y - w.y[2])) / d;
				float b = ((w.y[2] - w.y[0]) * (x - w.x[2]) + (w.x[0] - w.x[2]) * (y - w.y[2])) / d;
				inside = a >= 0 && b >= 0 && a + b <= 1;
			}
		}
		if (!inside) continue;
		z = 0.0f;
		for (int i = 0; i < w.n; i++) z += w.z[i];
		z /= w.n;
		return true;
	}
	return false;
}

float gVelZ = 0.0f, gAirVx = 0.0f, gAirVy = 0.0f;

void UpdateWalk(float dt) {
	// Ground contact as in CPed::ProcessEntityCollision (0x5E2530, read in the exe): a standing ped's collision is one line from the ped origin straight down
	// to -1.0, lengthened by 0.15 * timeStep while the ped is on the ground (flag 0x46c bit 1); a hit snaps the origin to hitZ + 1.0. So kStep = 1.0 (line starts at the
	// origin) and the snap-down reach 0.15 * timeStep below are exe-derived; kMaxDrop is only the search range of the ray. 20 = 0.008 @0x863984, origin height 1.0 (ped box min z).
	// DEVIATION: wall pushing uses my sphere test, not the full CPhysical contact resolution; slopes steeper than -0.867 normal z (exe check) are not handled.
	constexpr float kOriginAboveFeet = 1.0f, kGravity = 20.0f, kStep = 1.0f, kMaxDrop = 60.0f;
	float gz;
	bool ground = GroundHeight(camPos.x, camPos.y, camPos.z - kOriginAboveFeet + kStep, kMaxDrop, gz);
	// Swimming. Entry rule from CPed::ProcessBuoyancy (0x5E1FA0) + cBuoyancy::ProcessBuoyancy: immersion = clamp((waterLevel - z + 1) / 1.9),
	// buoyancy = immersion * 1.1 * mass/125 per tick; CEventInWater (-> swim task) fires once buoyancy/mass > 1/125 per tick or z + 0.6 < waterLevel,
	// i.e. origin z < waterLevel - 0.6 (the 1.1 factor alone gives -0.727, so the second test decides). Standing on ground that means water
	// deeper than 1.6 m over the feet. Leaving: the ped stands again once the tread height (waterLevel - 0.55) is at or below ground + 1.0, so a
	// swimming ped keeps swimming while depth > 1.55 (hysteresis follows from the equilibrium, not from CTaskSimpleSwim's m_fSwimStopTime,
	// whose exit threshold is NOT reversed). Height follows CTaskSimpleSwim::ProcessSwimmingResistance (0x68A1D0, constants read in the exe:
	// 0.55 tread, 0.4 breast, 0.2 crawl, 0.65 surface test, 0.1 per tick clamp). DEVIATION: the 0.4/0.2 blends and the smoothing are not
	// implemented (no clip blending), so moving also sits at the tread height.
	float wl;
	// without ground collision below (open sea has none), a ped over water falls under gravity until the same rule above applies
	bool inWater = WaterLevelAt(camPos.x, camPos.y, wl);
	gSwimming = inWater && (ground ? (camPos.z < wl - 0.6f || (gSwimming && wl - gz > 1.55f)) : camPos.z < wl - 0.6f);
	if (gSwimming) {
		float target = wl - 0.55f, dz = target - camPos.z, maxStep = 5.0f * dt;
		camPos.z += std::max(-maxStep, std::min(maxStep, dz));
		gVelZ = 0.0f;
		gGrounded = false;
		return;
	}
	if (!ground) {
		if (inWater) { gVelZ -= kGravity * dt; camPos.z += gVelZ * dt; gGrounded = false; } // open sea: nothing to stand on
		else gVelZ = 0.0f; // no collision loaded here (yet): hold the height
		return;
	}
	float floorZ = gz + kOriginAboveFeet;
	bool wasGrounded = gGrounded;
	gGrounded = (camPos.z <= floorZ + 0.02f || (wasGrounded && camPos.z - floorZ <= 0.15f * dt * 50.0f)) && gVelZ <= 0.0f;
	if (!gGrounded) {
		gVelZ -= kGravity * dt;
		camPos.z += gVelZ * dt;
		if (camPos.z < floorZ && gVelZ < 0.0f) { camPos.z = floorZ; gVelZ = 0.0f; gGrounded = true; }
	} else {
		camPos.z = floorZ;
		gVelZ = 0.0f;
	}
}

void UpdateStreaming(double now) {
	for (size_t i = 0; i < gActive.size();) {
		const Inst &in = gInsts[gActive[i]];
		float fading, d = DrawLimit(in, fading);
		if (Dist3(in) - kFading >= d || CoveredByChildren(in)) Unplace(i); else i++;
	}
	// request / place what came close
	int budget = 200;
	float reach = gFarClip + 400.0f + kStreamMargin; // 400: room for the colModel radius
	int c0x = CellOf(camPos.x - reach), c1x = CellOf(camPos.x + reach);
	int c0y = CellOf(camPos.y - reach), c1y = CellOf(camPos.y + reach);
	for (int cx = c0x; cx <= c1x; cx++)
		for (int cy = c0y; cy <= c1y; cy++) {
			auto g = gGrid.find(CellKey(cx, cy));
			if (g == gGrid.end()) continue;
			for (int idx : g->second) {
				Inst &in = gInsts[idx];
				if (in.clump) continue;
				float fading, d = DrawLimit(in, fading), dist = Dist3(in);
				if (dist - kStreamMargin >= d) continue; // outside the request margin
				bool drawable = dist - kFading < d && !CoveredByChildren(in);
				ModelState &ms = gModels[in.id];
				if (ms.failed) continue;
				if (!ms.proto) {
					if (!ms.requested) {
						if (!gStreamer->Request(in.id)) { ms.failed = true; continue; }
						ms.requested = true;
						gPending.insert(in.id);
					}
					continue;
				}
				if (drawable && budget-- > 0) Place(idx, ms);
			}
		}
	gLoadedModels += gStreamer->Process(3);
	for (auto it = gPending.begin(); it != gPending.end();) {
		int id = *it++;
		TryBuild(id, gModels[id]); // erases from gPending when built or failed
	}
	if (now >= gNextLog) {
		gNextLog = now + 2.0;
		// models built for instances that moved out of range before they could be placed
		for (auto &kv : gModels) {
			ModelState &ms = kv.second;
			if (ms.proto && ms.users == 0) DestroyProto(kv.first, ms);
		}
		fprintf(gLog, "z=%.2f walk=%d grounded=%d ", camPos.z, (int)gWalk, (int)gGrounded);
			fprintf(gLog, "clock %02d:%02d balance %.2f  ", (int)(gGameMinutes / 60), (int)gGameMinutes % 60, gDayNightBalance);
			fprintf(gLog, "t=%.0fs cam=(%.0f,%.0f) active=%zu protos=%d pending=%zu streamer: loaded=%zu %.1f MB queued=%zu\n", now, camPos.x,
		        camPos.y, gActive.size(), (int)std::count_if(gModels.begin(), gModels.end(), [](auto &kv) { return kv.second.proto != nil; }),
		        gPending.size(), gStreamer->LoadedCount(), gStreamer->LoadedBytes() / 1048576.0, gStreamer->PendingCount());
		fflush(gLog);
	}
}

// Visible ped (walk mode). DEVIATIONS: CJ is assembled from the base player.img parts without clothes (see LoadCj);
// the follow camera uses an assumed distance/height instead of CCam::Process_FollowPed_SA, with a simple ray test against COL geometry.
// The ped is one or more skinned clumps that share the same animation (CJ: one per body part, each with its own copy of the skeleton).
struct PedPart { rw::Clump *clump = nil; rw::HAnimHierarchy *hier = nil; };
std::vector<PedPart> gPedParts;
constexpr float kPedFollowDist = 4.0f, kPedFollowUp = 0.6f; // up = table f0 at 0x8CC548 (exe-verified look-at height above the ped origin); dist = ASSUMED: the exe gives min(2.0 + zoom, 4.0) with the default zoom unknown (0xB6F0FC), 4.0 is only the cap

// ped.ifp animation on the ped's hierarchy. Rotation keys replace the bone's local rotation, translation keys (root only) its position.
// Key quaternions are used as stored (checked visually: conjugating them lays the ped on its side). ASSUMED: bones without a translation
// key keep their bind position.
std::unordered_map<std::string, sa::IfpAnim> gAnims;
const sa::IfpAnim *gPedAnim = nil;
std::vector<rw::V3d> gBindPos;
float gAnimTime = 0.0f;
bool gAnimLoop = true;
float gLandTimer = 0.0f, gLaunchTimer = 0.0f; // launch timer: > 0 while jump_launch plays // > 0 while jump_land plays after touching down
bool gWasAirborne = false;

void LoadPedAnims() {
	std::ifstream f(gameDir + "/anim/ped.ifp", std::ios::binary);
	std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	size_t n = sa::ParseIfp(b, gAnims);
	fprintf(gLog, "ped.ifp: %zu animations\n", n);
	// the swim clips live in anim.img (swim.ifp)
	sa::ImgV2 animImg;
	if (animImg.Open(gameDir + "/anim/anim.img")) {
		const sa::ImgEntry *e = animImg.Find("swim.ifp");
		std::vector<uint8_t> sb;
		if (e && animImg.Read(*e, sb)) fprintf(gLog, "swim.ifp: %zu animations\n", sa::ParseIfp(sb, gAnims));
	}
	auto it = gAnims.find("idle_stance");
	gPedAnim = it != gAnims.end() ? &it->second : nil;
}

rw::Quat SampleQuat(const std::vector<sa::IfpKey> &k, float t) {
	size_t i = 0;
	while (i + 1 < k.size() && k[i + 1].time <= t) i++;
	const sa::IfpKey &a = k[i], &b = k[std::min(i + 1, k.size() - 1)];
	float u = b.time > a.time ? std::min(1.0f, std::max(0.0f, (t - a.time) / (b.time - a.time))) : 0.0f;
	float dot = a.q[0] * b.q[0] + a.q[1] * b.q[1] + a.q[2] * b.q[2] + a.q[3] * b.q[3], sgn = dot < 0 ? -1.0f : 1.0f;
	rw::Quat q;
	q.x = a.q[0] + (sgn * b.q[0] - a.q[0]) * u; q.y = a.q[1] + (sgn * b.q[1] - a.q[1]) * u;
	q.z = a.q[2] + (sgn * b.q[2] - a.q[2]) * u; q.w = a.q[3] + (sgn * b.q[3] - a.q[3]) * u;
	float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	if (l > 0) { q.x /= l; q.y /= l; q.z /= l; q.w /= l; }
	return q;
}

// PC controls: moving runs by default, Left Alt walks, Space sprints, Left Shift jumps. ASSUMED: the sprint clip is sprint_civi.
const char *PedClip(bool moving) {
	if (!moving) return "idle_stance";
	if (sk::keys[sk::KEY_LALT]) return "walk_player";
	if (sk::keys[' ']) return "sprint_civi";
	return "run_player";
}

// Forward speed of a clip from its root translation over one loop (VERIFIED from ped.ifp: walk_player 1.56, run_player 5.71, sprint_civi 9.56 m/s).
// DEVIATION: the original moves the ped by the clip's root motion every frame; this just applies the average speed.
float ClipSpeed(const char *name) {
	auto it = gAnims.find(name);
	if (it == gAnims.end()) return 0.0f;
	for (const sa::IfpSeq &sq : it->second.seqs)
		if (sq.boneTag == 0 && sq.hasTrans && sq.keys.size() > 1) {
			const sa::IfpKey &a = sq.keys.front(), &b = sq.keys.back();
			return b.time > a.time ? (b.t[1] - a.t[1]) / (b.time - a.time) : 0.0f;
		}
	return 0.0f;
}

void SelectPedAnim(const char *name, bool loop = true) {
	auto it = gAnims.find(name);
	if (it != gAnims.end() && gPedAnim != &it->second) { gPedAnim = &it->second; gAnimTime = 0.0f; }
	gAnimLoop = loop;
}

void AnimatePed(float dt) {
	// DEVIATION: the original blends clips through the ped task system; this just switches between idle / walk / run
	bool moving = sk::keys['W'] || sk::keys['A'] || sk::keys['S'] || sk::keys['D'];
	// jump: jump_launch (once) -> jump_glide while airborne -> jump_land (once) after touching down. DEVIATION: no blending between clips.
	bool air = gWalk && !gGrounded && !gSwimming;
	if (gWasAirborne && !air && gWalk && !gSwimming) { auto it = gAnims.find("jump_land"); gLandTimer = it != gAnims.end() ? it->second.duration : 0.0f; }
	gWasAirborne = air;
	if (gLandTimer > 0.0f) gLandTimer -= dt;
	if (gLaunchTimer > 0.0f) gLaunchTimer -= dt;
	if (gWalk && gSwimming) SelectPedAnim(!moving ? "swim_tread" : sk::keys[' '] ? "swim_crawl" : "swim_breast"); // Space = sprint -> crawl
	else if (air) SelectPedAnim(gLaunchTimer > 0.0f ? "jump_launch" : "jump_glide", gLaunchTimer <= 0.0f);
	else if (gLandTimer > 0.0f) SelectPedAnim("jump_land", false);
	else SelectPedAnim(PedClip(moving));
	if (gPedParts.empty() || !gPedAnim || gPedAnim->duration <= 0) return;
	gAnimTime = gAnimLoop ? std::fmod(gAnimTime + dt, gPedAnim->duration) : std::min(gAnimTime + dt, gPedAnim->duration);
	for (PedPart &part : gPedParts) {
		if (!part.hier) continue;
		for (const sa::IfpSeq &sq : gPedAnim->seqs) {
			int ni = part.hier->getIndex(sq.boneTag);
			if (ni < 0 || sq.keys.empty()) continue;
			rw::Frame *fr = part.hier->nodeInfo[ni].frame;
			if (!fr) continue;
			rw::Quat q = SampleQuat(sq.keys, gAnimTime);
			rw::Matrix m;
			rw::Matrix::makeRotation(&m, q);
			fr->matrix.right = m.right; fr->matrix.up = m.up; fr->matrix.at = m.at;
			if (sq.hasTrans) {
				const sa::IfpKey &k = sq.keys[0];
				fr->matrix.pos = {k.t[0], k.t[1], k.t[2]};
			}
			fr->matrix.update();
		}
		part.clump->getFrame()->updateObjects();
		// the hierarchy has no interpolator (updateMatrices would crash), so take the bone matrices straight from the frames' LTMs
		for (int i = 0; i < part.hier->numNodes; i++)
			if (part.hier->nodeInfo[i].frame) part.hier->matrices[i] = *part.hier->nodeInfo[i].frame->getLTM();
	}
}

// Streams one clump (dff bytes) with textures taken from the given dictionaries; adds it to the world and binds its skeleton.
bool AddPedPart(const std::vector<uint8_t> &dff, const std::vector<rw::TexDictionary *> &txds, const char *what, const char *skinTexture = nullptr) {
	gChain = txds;
	rw::TexDictionary::setCurrent(nil);
	rw::Texture::findCB = FindInChain;
	rw::StreamMemory sm;
	sm.open(const_cast<uint8_t *>(dff.data()), (rw::uint32)dff.size());
	rw::Clump *c = rw::findChunk(&sm, rw::ID_CLUMP, nil, nil) ? rw::Clump::streamRead(&sm) : nil;
	sm.close();
	if (!c) { fprintf(gLog, "ped: %s failed to load\n", what); return false; }
	if (skinTexture) { // CJ's body parts carry no textures; the clothes builder assigns them
		rw::Texture *tex = FindInChain(skinTexture);
		FORLIST(lnk, c->atomics) {
			rw::Geometry *g = rw::Atomic::fromClump(lnk)->geometry;
			for (int i = 0; i < g->matList.numMaterials; i++) g->matList.materials[i]->setTexture(tex);
		}
		if (!tex) fprintf(gLog, "ped: texture %s not found\n", skinTexture);
	}
	gWorld->addClump(c);
	PedPart part;
	part.clump = c;
	part.hier = rw::HAnimHierarchy::find(c->getFrame());
	if (part.hier) part.hier->attach(); // binds each node to the frame with the same hanim id
	FORLIST(lnk, c->atomics) {
		rw::Atomic *at = rw::Atomic::fromClump(lnk);
		if (part.hier && rw::Skin::get(at->geometry)) rw::Skin::setHierarchy(at, part.hier);
	}
	fprintf(gLog, "ped: %s loaded (hierarchy %s, %d nodes)\n", what, part.hier ? "found" : "missing", part.hier ? part.hier->numNodes : 0);
	gPedParts.push_back(part);
	return true;
}

// CJ without clothes: the base body parts of player.img (an all-zero CPedClothesDesc selects model key 0 / texture key 0) with the
// plain player_face / player_torso / player_legs / player_feet textures. DEVIATION: the outfit the original game starts with comes from
// the script/save (GIVE_PLAYER_CLOTHES) and is merged into one geometry by CClothesBuilder; here every part is its own skinned clump.
bool LoadCj() {
	sa::ImgV2 img;
	if (!img.Open(gameDir + "/models/player.img")) return false;
	auto read = [&](const char *name, std::vector<uint8_t> &out) {
		const sa::ImgEntry *e = img.Find(name);
		return e && img.Read(*e, out);
	};
	std::vector<rw::TexDictionary *> txds;
	for (const char *t : {"player_torso.txd", "player_face.txd", "player_legs.txd", "player_feet.txd"}) {
		std::vector<uint8_t> b;
		if (!read(t, b)) return false;
		if (rw::TexDictionary *d = ReadTxd(b)) {
			FORLIST(lnk, d->textures) rw::Texture::fromDict(lnk)->setFilter(rw::Texture::LINEARMIPLINEAR);
			txds.push_back(d);
		}
	}
	bool ok = true;
	// ASSUMED part -> texture mapping (head: face, hands share the torso skin texture)
	struct { const char *dff, *tex; } parts[] = {{"head.dff", "face"}, {"hands.dff", "torso"}, {"torso.dff", "torso"}, {"legs.dff", "legs"}, {"feet.dff", "foot"}};
	for (auto &pt : parts) {
		std::vector<uint8_t> b;
		if (!read(pt.dff, b) || !AddPedPart(b, txds, pt.dff, pt.tex)) ok = false;
	}
	return ok;
}

void DestroyPed() {
	for (PedPart &p : gPedParts) { gWorld->removeClump(p.clump); p.clump->destroy(); }
	gPedParts.clear();
}

void EnsurePed() {
	if (!gPedParts.empty()) return;
	if (gAnims.empty()) LoadPedAnims();
	if (!getenv("SA_MALE01") && LoadCj()) return;
	DestroyPed();
	const sa::ImgV2 &gta3 = gW.archives[0];
	const sa::ImgEntry *ed = gta3.Find("male01.dff"), *et = gta3.Find("male01.txd");
	std::vector<uint8_t> dff, txd;
	if (!ed || !et || !gta3.Read(*ed, dff) || !gta3.Read(*et, txd)) { fprintf(gLog, "ped: male01 not found\n"); return; }
	std::vector<rw::TexDictionary *> t;
	if (rw::TexDictionary *d = ReadTxd(txd)) t.push_back(d);
	AddPedPart(dff, t, "male01");
}

void UpdatePed() {
	const bool show = getenv("SA_PEDSHOW") != nullptr; // test aid: draw the ped 6 m ahead of the free camera
	if (!gWalk && !show) { DestroyPed(); return; }
	EnsurePed();
	rw::V3d zaxis = {0, 0, 1};
	rw::V3d origin = {camPos.x, camPos.y, camPos.z}; // camPos is the ped origin (1.0 above the feet, verified)
	if (show && !gWalk) origin = {camPos.x + std::sin(camYaw * 0.0174533f) * 6.0f, camPos.y + std::cos(camYaw * 0.0174533f) * 6.0f, camPos.z - 2.0f};
	for (PedPart &p : gPedParts) {
		rw::Frame *f = p.clump->getFrame();
		f->rotate(&zaxis, -camYaw, rw::COMBINEREPLACE);
		f->translate(&origin, rw::COMBINEPOSTCONCAT);
	}
}

void UpdateCamera() {
	float yr = camYaw * 3.14159265f / 180.0f, pr = camPitch * 3.14159265f / 180.0f;
	rw::V3d dir = {std::sin(yr) * std::cos(pr), std::cos(yr) * std::cos(pr), std::sin(pr)};
	rw::V3d up = {0.0f, 0.0f, 1.0f};
	rw::Frame *f = gCamera->getFrame();
	f->matrix.lookAt(dir, up);
	f->matrix.pos = camPos;
	if (gWalk && !gPedParts.empty()) { // third-person: back off along the view direction, stopping short of geometry in between
		rw::V3d back = {-dir.x, -dir.y, -dir.z};
		rw::V3d pivot = {camPos.x, camPos.y, camPos.z + kPedFollowUp};
		float dist = kPedFollowDist, hit = RayCastWorld(pivot, back, kPedFollowDist + 0.3f);
		if (hit >= 0.0f) dist = std::max(0.3f, hit - 0.3f);
		static int logged = 0;
		if (logged++ % 120 == 0) fprintf(gLog, "follow camera: ray hit %.2f -> distance %.2f, down %.2f, forward %.2f\n", hit, dist, RayCastWorld(pivot, {0, 0, -1}, 20.0f), RayCastWorld(pivot, dir, 20.0f));
		f->matrix.pos = {pivot.x + back.x * dist, pivot.y + back.y * dist, pivot.z + back.z * dist};
	}
	f->matrix.update();
	f->updateObjects();
}

bool Initialize3D() {
	if (!sk::InitRW()) return false;
	rw::BBox bb;
	bb.inf.x = bb.inf.y = -4000.0f; bb.inf.z = -500.0f;
	bb.sup.x = bb.sup.y = 4000.0f; bb.sup.z = 1000.0f;
	gWorld = rw::World::create(&bb);

	gCamera = sk::CameraCreate(sk::globals.width, sk::globals.height, 1);
	gCamera->setNearPlane(0.3f);
	if (getenv("SA_WALK")) gWalk = atoi(getenv("SA_WALK")) != 0;
	if (getenv("SA_FOV")) gFovDeg = (float)atof(getenv("SA_FOV"));
	if (getenv("SA_HOUR")) gGameMinutes = (float)atof(getenv("SA_HOUR")) * 60.0f;
	if (getenv("SA_WEATHER")) { gWeather = gOldWeather = atoi(getenv("SA_WEATHER")); gWeatherForced = true; }
	gTimecyc.Load(gameDir + "/data/timecyc.dat");
	gCamera->setFOV(gFovDeg, (float)sk::globals.width / sk::globals.height);
	gWorld->addCamera(gCamera);

	rw::Light *amb = rw::Light::create(rw::Light::AMBIENT);
	amb->setColor(0.65f, 0.65f, 0.7f);
	gAmbient = amb;
	ApplyTimeOfDay();
	fprintf(gLog, "timecyc water rgba %.0f %.0f %.0f %.0f\n", gTc.water[0], gTc.water[1], gTc.water[2], gTc.water[3]);
	fprintf(gLog, "timecyc: minute-of-day %.0f weather %d ok=%d farClip=%.0f fogStart=%.0f\n", gGameMinutes, gWeather, (int)gTc.ok, gFarClip, gTc.fogStart);
	gWorld->addLight(amb);
	rw::Light *sun = rw::Light::create(rw::Light::DIRECTIONAL);
	sun->setColor(0.6f, 0.6f, 0.55f);
	rw::Frame *sf = rw::Frame::create();
	sun->setFrame(sf);
	gWorld->addLight(sun);
	gSunLight = sun;

	auto t0 = std::chrono::steady_clock::now();
	std::string err;
	if (!gW.Build(gameDir, &err)) { fprintf(gLog, "streaming world: %s\n", err.c_str()); return false; }
	gStreamer = new sa::Streamer(gW);
	CollectPlacements();
	double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	fprintf(gLog, "startup %.0f ms: %zu instances in %zu cells, %d slots with archive entry\n", ms, gInsts.size(), gGrid.size(),
	        gW.store.CountWithArchiveEntry());
	fflush(gLog);

	camPos = {centerX - 30.0f, centerY - 30.0f, getenv("SA_SPAWNZ") ? (float)atof(getenv("SA_SPAWNZ")) : 40.0f};
	camYaw = getenv("SA_YAW") ? (float)atof(getenv("SA_YAW")) : 45.0f;
	if (getenv("SA_PITCH")) camPitch = (float)atof(getenv("SA_PITCH"));
	UpdatePed();
	UpdateCamera();
	return true;
}

void Terminate3D() {
	if (gCamera) { gWorld->removeCamera(gCamera); sk::CameraDestroy(gCamera); gCamera = nil; }
	sk::TerminateRW();
}

bool AttachPlugins() {
	rw::registerMeshPlugin();
	rw::registerNativeDataPlugin();
	rw::registerAtomicRightsPlugin();
	rw::registerMaterialRightsPlugin();
	rw::registerSkinPlugin();
	rw::registerUserDataPlugin();
	rw::registerHAnimPlugin();
	rw::registerMatFXPlugin();
	rw::registerUVAnimPlugin();
	gExtraVertOffset = rw::Geometry::registerPlugin(sizeof(ExtraVertColour), ID_EXTRAVERTCOLOUR, CreateExtraVert, DestroyExtraVert, nil);
	rw::Geometry::registerPluginStream(ID_EXTRAVERTCOLOUR, ReadExtraVert, WriteExtraVert, SizeExtraVert);
	return true;
}

// Sun: projected along the vector to the sun (verified formula), filled square in the timecyc core/corona colours.
// DEVIATION: the real sun/corona are textured sprites from particle.txd with sizes from CClouds/CCoronas; sizes here are guesses.
rw::Texture *ParticleTex(const char *name);

void DrawSunDisc(float W, float H, float tanHalf) {
	rw::Matrix *m = &gCamera->getFrame()->matrix;
	rw::V3d d = gToSun;
	float cx = d.x * m->right.x + d.y * m->right.y + d.z * m->right.z, cy = d.x * m->up.x + d.y * m->up.y + d.z * m->up.z,
	      cz = d.x * m->at.x + d.y * m->at.y + d.z * m->at.z;
	if (cz <= 0.05f) return;
	float aspect = W / H;
	float sx = W * 0.5f + cx / cz / (tanHalf * aspect) * W * 0.5f, sy = H * 0.5f - cy / cz / tanHalf * H * 0.5f;
	auto quad = [&](float half, const float *col, uint8_t alpha) {
		rw::RWDEVICE::Im2DVertex v[4];
		float xs[4] = {sx - half, sx + half, sx - half, sx + half}, ys[4] = {sy - half, sy - half, sy + half, sy + half}, us[4] = {0, 1, 0, 1}, vs[4] = {0, 0, 1, 1};
		float recipZ = 1.0f / gCamera->nearPlane;
		for (int i = 0; i < 4; i++) {
			v[i].setScreenX(xs[i]); v[i].setScreenY(ys[i]); v[i].setScreenZ(rw::im2d::GetNearZ());
			v[i].setCameraZ(gCamera->nearPlane); v[i].setRecipCameraZ(recipZ);
			v[i].setColor((uint8_t)col[0], (uint8_t)col[1], (uint8_t)col[2], alpha);
			v[i].setU(us[i], recipZ); v[i].setV(vs[i], recipZ);
		}
		rw::uint16 idx[6] = {0, 1, 2, 1, 3, 2};
		rw::im2d::RenderIndexedPrimitive(rw::PRIMTYPETRILIST, v, 4, idx, 6);
	};
	// coronas are drawn additively (CCoronas::Render: SRCBLEND ONE / DESTBLEND ONE) with the "coronastar" sprite (CORONATYPE_SHINYSTAR)
	static rw::Texture *star = ParticleTex("coronastar");
	rw::SetRenderStatePtr(rw::TEXTURERASTER, star ? star->raster : nil);
	rw::SetRenderState(rw::VERTEXALPHA, 1);
	rw::SetRenderState(rw::SRCBLEND, rw::BLENDONE);
	rw::SetRenderState(rw::DESTBLEND, rw::BLENDONE);
	// CCoronas::DoSunAndMoon (0x6FC5A0): corona 1 (sun core colour) has size sunSize*2.7335, corona 2 (corona colour) sunSize*6, both placed at
	// farClip*0.95 along the vector to the sun. DEVIATION: the sizes are taken as half-widths and drawn with the coronastar sprite (the original also fades
	// by line of sight in CCoronas::Render and draws the SUN flare sprites), alpha values are guesses.
	float dist = std::max(gFarClip, 1.0f) * 0.95f, pxPerUnit = H * 0.5f / (tanHalf * dist);
	// CRegisteredCorona::Update (0x6FADF0 path): corona 2 (LOS check on) fades down by timeStep*15 per tick while the line camera->sun is blocked (or the
	// sun is off screen), up otherwise. DEVIATION: the LOS test uses RayCastWorld, which only reaches the 3x3 cells around the camera (not the full 760 m
	// to the corona); SunBlockedByClouds is not modelled; fade runs on wall-clock dt.
	static float sunFade = 0.0f;
	static auto lastT = std::chrono::steady_clock::now();
	auto nowT = std::chrono::steady_clock::now();
	float fdt = std::min(std::chrono::duration<float>(nowT - lastT).count(), 0.1f);
	lastT = nowT;
	bool blocked = RayCastWorld(camPos, gToSun, 400.0f) < 400.0f;
	bool offscreen = sx < 0 || sx > W || sy < 0 || sy > H;
	float step = fdt * 50.0f * 15.0f;
	if (blocked || offscreen) sunFade = std::max(0.0f, sunFade - step); else sunFade = std::min(90.0f, sunFade + step);
	if (sunFade > 0.5f) quad(gTc.sunSize * 6.0f * pxPerUnit, gTc.sunCorona, (uint8_t)sunFade);
	quad(gTc.sunSize * 2.7335f * pxPerUnit, gTc.sunCore, 255);
	rw::SetRenderStatePtr(rw::TEXTURERASTER, nil);
}

// Low clouds: CClouds::Render_RenderLowClouds (0x7138xx..): 12 additive "cloud1" sprites at camera + offset*(800,800,60) + (0,0,40) with z not
// offset by the camera height, sized by CSprite::CalcScreenCoors * (320, 40), tinted by the timecyc low-cloud colour scaled by (1 - balance),
// balance = max(foggyness, cloudCoverage, extraSunnyness) from the weather pair (CWeather::Update 0x72B850 flags, blended by the hour fraction).
// DEVIATION: sprite extents are read as half-sizes in the original 640x448 screen units and scaled to the window; the roll is 0; the
// rainbow, moon and Rockstar-logo parts of CClouds::Render are not drawn.
rw::TexDictionary *gParticleTxd = nullptr;
bool gParticleTried = false;

rw::Texture *ParticleTex(const char *name) {
	if (!gParticleTried) {
		gParticleTried = true;
		std::ifstream f(gameDir + "/models/particle.txd", std::ios::binary);
		std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		if (!b.empty()) gParticleTxd = ReadTxd(b);
		if (gParticleTxd) FORLIST(lnk, gParticleTxd->textures) rw::Texture::fromDict(lnk)->setFilter(rw::Texture::LINEAR);
		fprintf(gLog, "particle.txd: %s\n", gParticleTxd ? "loaded" : "missing");
	}
	if (!gParticleTxd) return nullptr;
	FORLIST(lnk, gParticleTxd->textures) {
		rw::Texture *t = rw::Texture::fromDict(lnk);
		if (Lower(t->name) == name) return t;
	}
	return nullptr;
}

rw::Texture *gCloudTex = nullptr;
bool gCloudTried = false;

float WeatherCloudBalance() {
	auto extraSunny = [](int w) { return w == 0 || w == 2 || w == 6 || w == 11 || w == 13 || w == 17; };
	auto sunny = [](int w) { return w == 1 || w == 3 || w == 5 || w == 10 || w == 14 || w == 18; };
	auto foggy = [](int w) { return w == 9 || w == 19; };
	float f = std::fmod(gGameMinutes, 60.0f) / 60.0f;
	if (gWeatherForced) f = 1.0f;
	auto blend = [&](auto pred) {
		float a = pred(gOldWeather) ? 1.0f : 0.0f, b = pred(gWeather) ? 1.0f : 0.0f;
		return a == b ? b : a + (b - a) * f;
	};
	float cover = blend([&](int w) { return !sunny(w) && !extraSunny(w); });
	return std::max({blend(foggy), cover, blend(extraSunny)});
}

void DrawLowClouds(float W, float H, float tanHalf) {
	if (!gCloudTried) {
		gCloudTried = true;
		gCloudTex = ParticleTex("cloud1");
	}
	if (!gCloudTex) return;
	float keep = 1.0f - WeatherCloudBalance();
	if (keep <= 0.0f) return;
	static const float kOff[12][3] = {{1.0f, 0.0f, 0.0f}, {0.7f, -0.7f, 1.0f}, {0.0f, -1.0f, 0.5f}, {-0.7f, -0.7f, 0.0f}, {-1.0f, 0.0f, 1.0f}, {-0.7f, 0.7f, 0.3f},
	                                  {0.0f, 1.0f, 0.9f}, {0.7f, 0.7f, 0.4f}, {0.8f, 0.4f, 1.3f}, {-0.8f, 0.4f, 1.4f}, {0.4f, -0.8f, 1.2f}, {-0.4f, -0.8f, 1.7f}};
	rw::Matrix *m = &gCamera->getFrame()->matrix;
	float aspect = W / H, fovDeg = 2.0f * std::atan(tanHalf * aspect) * 180.0f / 3.14159265f;
	uint8_t cr = (uint8_t)(gTc.lowCloud[0] * keep), cg = (uint8_t)(gTc.lowCloud[1] * keep), cb = (uint8_t)(gTc.lowCloud[2] * keep);
	rw::SetRenderStatePtr(rw::TEXTURERASTER, gCloudTex->raster);
	rw::SetRenderState(rw::VERTEXALPHA, 1);
	rw::SetRenderState(rw::SRCBLEND, rw::BLENDONE);
	rw::SetRenderState(rw::DESTBLEND, rw::BLENDONE);
	float recipZ = 1.0f / gCamera->nearPlane;
	for (const auto &o : kOff) {
		float dx = o[0] * 800.0f + camPos.x - camPos.x, dy = o[1] * 800.0f, dz = o[2] * 60.0f + 40.0f - camPos.z;
		float cx = dx * m->right.x + dy * m->right.y + dz * m->right.z, cy = dx * m->up.x + dy * m->up.y + dz * m->up.z,
		      cz = dx * m->at.x + dy * m->at.y + dz * m->at.z;
		if (cz <= gCamera->nearPlane + 1.0f) continue;
		float sx = W * 0.5f + cx / cz / (tanHalf * aspect) * W * 0.5f, sy = H * 0.5f - cy / cz / tanHalf * H * 0.5f;
		float rd = 1.0f / cz, scale = rd / fovDeg * 70.0f;
		float pw = 640.0f * scale * 320.0f * (W / 640.0f), ph = 448.0f * scale * 40.0f * (H / 448.0f);
		rw::RWDEVICE::Im2DVertex v[4];
		float xs[4] = {sx - pw, sx + pw, sx - pw, sx + pw}, ys[4] = {sy - ph, sy - ph, sy + ph, sy + ph}, us[4] = {0, 1, 0, 1}, vs[4] = {0, 0, 1, 1};
		for (int i = 0; i < 4; i++) {
			v[i].setScreenX(xs[i]); v[i].setScreenY(ys[i]); v[i].setScreenZ(rw::im2d::GetNearZ());
			v[i].setCameraZ(gCamera->nearPlane); v[i].setRecipCameraZ(recipZ);
			v[i].setColor(cr, cg, cb, 255);
			v[i].setU(us[i], recipZ); v[i].setV(vs[i], recipZ);
		}
		rw::uint16 idx[6] = {0, 1, 2, 1, 3, 2};
		rw::im2d::RenderIndexedPrimitive(rw::PRIMTYPETRILIST, v, 4, idx, 6);
	}
	rw::SetRenderStatePtr(rw::TEXTURERASTER, nil);
}

// Moon: CClouds::Render_MaybeRenderMoon (0x7139B2). Visible while |minutesToday - 220| < 220 (exe-read: 00:00..07:20), brightness colorB = 220 - that,
// scaled by (1 - max(foggyness, cloudCoverage)); sprite "coronamoon" at camera + (0,-100,15) (world offset, not camera-relative), half-size
// (MoonSize*2+4) * CalcScreenCoors size with MoonSize = 3 (0x8D4B60), colour (b, b, 0.85*b). DEVIATION: the black pre-pass and "lunar" mask that carve the
// crescent phase (destination-alpha blend) are not drawn, so the moon is a full disc; additive blend instead of DESTALPHA/ONE.
void DrawMoon(float W, float H, float tanHalf) {
	float mins = std::fmod(gGameMinutes, 1440.0f), vis = std::fabs(mins - 220.0f);
	if ((int)vis >= 220) return;
	auto foggy = [](int w) { return w == 9 || w == 19; };
	auto sunny = [](int w) { return w == 1 || w == 3 || w == 5 || w == 10 || w == 14 || w == 18; };
	auto extra = [](int w) { return w == 0 || w == 2 || w == 6 || w == 11 || w == 13 || w == 17; };
	float f = gWeatherForced ? 1.0f : std::fmod(gGameMinutes, 60.0f) / 60.0f;
	auto blend = [&](auto pred) { float a = pred(gOldWeather) ? 1.0f : 0.0f, b = pred(gWeather) ? 1.0f : 0.0f; return a == b ? b : a + (b - a) * f; };
	float balance = std::max(blend(foggy), blend([&](int w) { return !sunny(w) && !extra(w); }));
	float colorB = 220.0f - (float)(int)vis, rg = colorB * (1.0f - balance);
	if ((int)rg <= 0) return;
	static rw::Texture *tex = ParticleTex("coronamoon");
	if (!tex) return;
	rw::Matrix *mt = &gCamera->getFrame()->matrix;
	float dx = 0.0f, dy = -100.0f, dz = 15.0f;
	float cx = dx * mt->right.x + dy * mt->right.y + dz * mt->right.z, cy = dx * mt->up.x + dy * mt->up.y + dz * mt->up.z,
	      cz = dx * mt->at.x + dy * mt->at.y + dz * mt->at.z;
	if (cz <= gCamera->nearPlane + 1.0f) return;
	float aspect = W / H, fovDeg = 2.0f * std::atan(tanHalf * aspect) * 180.0f / 3.14159265f;
	float sx = W * 0.5f + cx / cz / (tanHalf * aspect) * W * 0.5f, sy = H * 0.5f - cy / cz / tanHalf * H * 0.5f;
	float half = (3.0f * 2.0f + 4.0f) * 640.0f / cz * 70.0f / fovDeg * (W / 640.0f);
	rw::SetRenderStatePtr(rw::TEXTURERASTER, tex->raster);
	rw::SetRenderState(rw::VERTEXALPHA, 1);
	rw::SetRenderState(rw::SRCBLEND, rw::BLENDONE);
	rw::SetRenderState(rw::DESTBLEND, rw::BLENDONE);
	rw::RWDEVICE::Im2DVertex v[4];
	float xs[4] = {sx - half, sx + half, sx - half, sx + half}, ys[4] = {sy - half, sy - half, sy + half, sy + half}, us[4] = {0, 1, 0, 1}, vs[4] = {0, 0, 1, 1};
	float recipZ = 1.0f / gCamera->nearPlane;
	for (int i = 0; i < 4; i++) {
		v[i].setScreenX(xs[i]); v[i].setScreenY(ys[i]); v[i].setScreenZ(rw::im2d::GetNearZ());
		v[i].setCameraZ(gCamera->nearPlane); v[i].setRecipCameraZ(recipZ);
		v[i].setColor((uint8_t)rg, (uint8_t)rg, (uint8_t)(colorB * 0.85f * (1.0f - balance)), 255);
		v[i].setU(us[i], recipZ); v[i].setV(vs[i], recipZ);
	}
	rw::uint16 idx[6] = {0, 1, 2, 1, 3, 2};
	rw::im2d::RenderIndexedPrimitive(rw::PRIMTYPETRILIST, v, 4, idx, 6);
	rw::SetRenderStatePtr(rw::TEXTURERASTER, nil);
}

// Rockstar logo: CClouds::Render_MaybeRenderRockstarLogo (0x713D2A): 11 "coronastar" sprites forming an R (offsets from the camera in world axes, base (100,0,10),
// y -= STARS_Y*90, z += STARS_Z*80, the last two mirrored in x) plus a big star at (100,-90,10); visible 22:00..06:00 with a fade in the first and last hour, brightness
// (1 - weather balance) * fade with per-frame random twinkle. DEVIATION: sprite half-size taken as CalcScreenCoors size * factor in 640x448 units scaled to the window;
// the weather balance here includes only fog and cloud cover like the moon.
void DrawRockstarLogo(float W, float H, float tanHalf) {
	float mins = std::fmod(gGameMinutes, 1440.0f);
	int hr = (int)(mins / 60.0f), mn = (int)std::fmod(mins, 60.0f);
	if (!(hr >= 22 || hr < 6)) return;
	unsigned t = hr == 22 ? (unsigned)mn : hr == 5 ? 60u - (unsigned)mn : 60u;
	if (t == 0) return;
	float colorB = 255.0f * (float)t / 60.0f;
	static rw::Texture *tex = ParticleTex("coronastar");
	if (!tex) return;
	static const float kY[9] = {0.00f, 0.05f, 0.13f, 0.40f, 0.70f, 0.60f, 0.27f, 0.55f, 0.75f}, kZ[9] = {0.00f, 0.45f, 0.90f, 1.00f, 0.85f, 0.52f, 0.48f, 0.35f, 0.20f},
	                         kS[9] = {1.00f, 1.40f, 0.90f, 1.00f, 0.60f, 1.50f, 1.30f, 1.00f, 0.80f};
	rw::Matrix *mt = &gCamera->getFrame()->matrix;
	float aspect = W / H, fovDeg = 2.0f * std::atan(tanHalf * aspect) * 180.0f / 3.14159265f, recipZ = 1.0f / gCamera->nearPlane;
	rw::SetRenderStatePtr(rw::TEXTURERASTER, tex->raster);
	rw::SetRenderState(rw::VERTEXALPHA, 1);
	rw::SetRenderState(rw::SRCBLEND, rw::BLENDONE);
	rw::SetRenderState(rw::DESTBLEND, rw::BLENDONE);
	auto sprite = [&](float dx, float dy, float dz, float sizeMul, float bright) {
		float cx = dx * mt->right.x + dy * mt->right.y + dz * mt->right.z, cy = dx * mt->up.x + dy * mt->up.y + dz * mt->up.z,
		      cz = dx * mt->at.x + dy * mt->at.y + dz * mt->at.z;
		if (cz <= gCamera->nearPlane + 1.0f) return;
		float sx = W * 0.5f + cx / cz / (tanHalf * aspect) * W * 0.5f, sy = H * 0.5f - cy / cz / tanHalf * H * 0.5f;
		float half = 640.0f / cz * 70.0f / fovDeg * sizeMul * (W / 640.0f);
		uint8_t c = (uint8_t)std::max(0.0f, std::min(255.0f, bright));
		rw::RWDEVICE::Im2DVertex v[4];
		float xs[4] = {sx - half, sx + half, sx - half, sx + half}, ys[4] = {sy - half, sy - half, sy + half, sy + half}, us[4] = {0, 1, 0, 1}, vs[4] = {0, 0, 1, 1};
		for (int i = 0; i < 4; i++) {
			v[i].setScreenX(xs[i]); v[i].setScreenY(ys[i]); v[i].setScreenZ(rw::im2d::GetNearZ());
			v[i].setCameraZ(gCamera->nearPlane); v[i].setRecipCameraZ(recipZ);
			v[i].setColor(c, c, c, 255);
			v[i].setU(us[i], recipZ); v[i].setV(vs[i], recipZ);
		}
		rw::uint16 idx[6] = {0, 1, 2, 1, 3, 2};
		rw::im2d::RenderIndexedPrimitive(rw::PRIMTYPETRILIST, v, 4, idx, 6);
	};
	for (int i = 0; i < 11; i++) {
		int p = i % 9;
		float dx = i >= 9 ? -100.0f : 100.0f, dy = 0.0f - kY[p] * 90.0f, dz = 10.0f + kZ[p] * 80.0f;
		float cc = colorB * (1.0f - (float)(rand() % 32) * 0.015f);
		sprite(dx, dy, dz, kS[p] * 0.8f, cc);
	}
	sprite(100.0f, -90.0f, 10.0f, 5.0f, colorB * (1.0f - ((float)(rand() % 128) * 0.0015625f + 0.5f)));
	rw::SetRenderStatePtr(rw::TEXTURERASTER, nil);
}

// Sky gradient: timecyc sky-top colour at the top of the screen blending to sky-bottom at the horizon (the clear colour fills below it).
// DEVIATION: the original (CClouds::Render) also draws the sun, moon, cloud layers and low-level clouds; none of that is implemented here.
void DrawSky() {
	if (!gTc.ok) return;
	rw::Raster *r = gCamera->frameBuffer;
	float W = (float)r->width, H = (float)r->height;
	float tanHalf = gCamera->viewWindow.y;
	float pitch = camPitch * 3.14159265f / 180.0f;
	// CClouds::RenderSkyPolys (0x714650): a camera-centred wall 30 m away; skyTop above 0.5*30 m, skyTop->skyBottom down to the horizon,
	// skyBottom to -0.1, then skyBottom -> m_BelowHorizonGrey at -0.3 (heights as fractions of the 30 m distance = tan(elevation)).
	// m_BelowHorizonGrey = GreyValuesDuringDay (0x8CDED8) blended by the hour, mixed to skyBottom with UnderWaterness (0 here).
	static const float kGrey[8] = {30, 30, 30, 50, 60, 60, 50, 35};
	float hour = std::fmod(gGameMinutes / 60.0f, 24.0f), grey = kGrey[7], t = 0;
	{
		int a = 7;
		for (int i = 0; i < 8; i++) if (hour >= sa::Timecyc::kHourOfKey[i]) a = i;
		int b2 = (a + 1) % 8;
		float ha = (float)sa::Timecyc::kHourOfKey[a], hb = (float)sa::Timecyc::kHourOfKey[b2];
		if (hb <= ha) hb += 24.0f;
		float h = hour < ha ? hour + 24.0f : hour;
		t = (h - ha) / (hb - ha);
		grey = kGrey[a] + (kGrey[b2] - kGrey[a]) * t;
	}
	float greyCol[3] = {grey, grey, grey};
	struct Stop { float h; const float *c; };
	const Stop stops[] = {{0.5f, gTc.skyTop}, {0.0f, gTc.skyBottom}, {-0.1f, gTc.skyBottom}, {-0.3f, greyCol}};
	auto screenY = [&](float h) { return H * 0.5f - std::tan(std::atan(h) - pitch) / tanHalf * H * 0.5f; };
	float recipZ = 1.0f / gCamera->nearPlane;
	rw::SetRenderStatePtr(rw::TEXTURERASTER, nil);
	rw::SetRenderState(rw::ZTESTENABLE, 0);
	rw::SetRenderState(rw::ZWRITEENABLE, 0);
	rw::SetRenderState(rw::CULLMODE, rw::CULLNONE);
	rw::SetRenderState(rw::FOGENABLE, 0);
	rw::SetRenderState(rw::VERTEXALPHA, 0);
	// strip from y0 (colour c0) to y1 (colour c1), clipped to the screen; colours interpolate linearly in screen space like the original quads
	auto strip = [&](float y0, const float *c0, float y1, const float *c1) {
		if (y1 <= 0.0f || y0 >= H || y1 <= y0) return;
		auto at = [&](float y, float *o) {
			float f = (y - y0) / (y1 - y0);
			for (int k = 0; k < 3; k++) o[k] = c0[k] + (c1[k] - c0[k]) * f;
		};
		float cy0 = std::max(y0, 0.0f), cy1 = std::min(y1, H), ca[3], cb[3];
		at(cy0, ca); at(cy1, cb);
		rw::RWDEVICE::Im2DVertex v[4];
		float xs[4] = {0, W, 0, W}, ys[4] = {cy0, cy0, cy1, cy1};
		const float *col[4] = {ca, ca, cb, cb};
		for (int i = 0; i < 4; i++) {
			v[i].setScreenX(xs[i]); v[i].setScreenY(ys[i]); v[i].setScreenZ(rw::im2d::GetNearZ());
			v[i].setCameraZ(gCamera->nearPlane); v[i].setRecipCameraZ(recipZ);
			v[i].setColor((uint8_t)col[i][0], (uint8_t)col[i][1], (uint8_t)col[i][2], 255);
			v[i].setU(0, recipZ); v[i].setV(0, recipZ);
		}
		rw::uint16 idx[6] = {0, 1, 2, 1, 3, 2};
		rw::im2d::RenderIndexedPrimitive(rw::PRIMTYPETRILIST, v, 4, idx, 6);
	};
	float yPrev = -1e9f;
	const float *cPrev = gTc.skyTop;
	strip(-1e9f, gTc.skyTop, screenY(stops[0].h), gTc.skyTop); // everything above the wall is skyTop
	yPrev = screenY(stops[0].h); cPrev = stops[0].c;
	for (int i = 1; i < 4; i++) {
		float y = screenY(stops[i].h);
		strip(yPrev, cPrev, y, stops[i].c);
		yPrev = y; cPrev = stops[i].c;
	}
	strip(yPrev, greyCol, H + 1.0f, greyCol); // DEVIATION: below -0.3 the original shows the grey back wall / clear colour; filled with the grey here
	DrawMoon(W, H, tanHalf);
	DrawRockstarLogo(W, H, tanHalf);
	DrawLowClouds(W, H, tanHalf);
	DrawSunDisc(W, H, tanHalf);
	rw::SetRenderState(rw::SRCBLEND, rw::BLENDSRCALPHA);
	rw::SetRenderState(rw::DESTBLEND, rw::BLENDINVSRCALPHA);
	rw::SetRenderState(rw::ZTESTENABLE, 1);
	rw::SetRenderState(rw::ZWRITEENABLE, 1);
	rw::SetRenderState(rw::CULLMODE, rw::CULLBACK);
}

// Water polygons from data/water.dat, drawn as one flat translucent colour at their stored height.
// DEVIATION: the original (CWaterLevel::RenderWater) animates waves, flow and foam with the water textures and timecyc water colour;
// none of that is implemented, and the polygon colour here is a guess.
bool gWaterTried = false;

void DrawWater() {
	if (!gWaterTried) {
		gWaterTried = true;
		bool ok = sa::LoadWaterDat(gameDir + "/data/water.dat", gWater);
		fprintf(gLog, "water.dat: %zu polygons (%s)\n", gWater.size(), ok ? "ok" : "failed");
	}
	// CWaterLevel::RenderWater: two flat layers textured with "waterclear256" (particle.txd), UV = world xy / 25 and / 12.5 m (GetWaterLayerTexInfo),
	// colour = raw timecyc water rgb (0xC2116C is filled from 0xB7C508..14 at 0x6EFAFF; gta-reversed's 0.577 factor is not applied), layer alphas derived from the timecyc water alpha as the exe does each frame (the static 160/90 at 0x8D3808 are overwritten). DEVIATION: the UV scroll (TextureShift*, animated by
	// CWaterLevel::Update), the wave/vertex animation on the water grid, flow, foam and the timecyc water alpha are not implemented; the water
	// polygons are drawn as stored in water.dat instead of the original's 2x2 m renderer grid.
	rw::Texture *tex = ParticleTex("waterclear256");
	if (tex) { tex->setAddressU(rw::Texture::WRAP); tex->setAddressV(rw::Texture::WRAP); }
	auto c8 = [](float v) { return (rw::uint8)std::min(255.0f, v); };
	for (int layer = 0; layer < 2; layer++) {
		const float scale = layer == 0 ? 25.0f : 12.5f;
		// layer alphas are rewritten every frame at 0x6EFB0A..: a = int(timecyc water alpha * 0.5); layer 1 (0x8D380C) = a, layer 0 (0x8D3808) = min(255, (a << 8) / (256 - a))
		const int wa = (int)(gTc.water[3] * 0.5f);
		const rw::uint8 alpha = (rw::uint8)(layer == 0 ? std::min(255, (wa << 8) / std::max(1, 256 - wa)) : wa);
		// UV shift (RenderWater 0x6EF673): layer 0 U = acc0U, V = cos((t & 0x1FFF) * 0.00076699) * 0.024 + acc0V; layer 1 U = sin((t & 0xFFF) * 0.0015339808) * K * 0.08
		// + acc1U, V = cos(..) * K * 0.08 + acc1V. The accumulators advance per tick by wind * 0.04 * (0.04 / 0.08 for layer 0 / 1) and wrap at 1; wind chases the
		// flow of the water vertex nearest the camera (0x6E9D75, int8 flow / 64) at 0.001 per tick. DEVIATION: K (0xC812E8, runtime-set, 0 in the file) is taken as 0,
		// the nearest-flow search is simplified to the nearest vertex, t is wall-clock ms instead of the game timer.
		static const auto t0 = std::chrono::steady_clock::now();
		static float windX = 0, windY = 0, acc0U = 0, acc0V = 0, acc1U = 0, acc1V = 0;
		static std::chrono::steady_clock::time_point last = t0;
		if (layer == 0) {
			auto now = std::chrono::steady_clock::now();
			float ts = std::min(std::chrono::duration<float>(now - last).count(), 0.1f) * 50.0f; // game ticks (1 tick = 20 ms)
			last = now;
			float tx = 0, ty = 0, best = 1e30f;
			for (const sa::WaterPoly &w : gWater)
				for (int i = 0; i < w.n; i++) {
					float d2 = (w.x[i] - camPos.x) * (w.x[i] - camPos.x) + (w.y[i] - camPos.y) * (w.y[i] - camPos.y);
					if (d2 < best) { best = d2; tx = std::trunc(w.flowX[i] * 64.0f) / 64.0f; ty = std::trunc(w.flowY[i] * 64.0f) / 64.0f; }
				}
			auto chase = [&](float &cur, float target) {
				float step = ts * 0.001f, d = target - cur;
				cur = step <= std::fabs(d) ? cur + (d >= 0 ? step : -step) : target;
			};
			chase(windX, tx); chase(windY, ty);
			float fx = ts * windX * 0.04f, fy = ts * windY * 0.04f;
			acc1U += fx * 0.08f; acc1V += fy * 0.08f; acc0U += fx * 0.04f; acc0V += fy * 0.04f;
			for (float *a : {&acc0U, &acc0V, &acc1U, &acc1V}) if (*a > 1.0f) *a -= 1.0f;
		}
		unsigned tms = (unsigned)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
		// K = CWeather::Wavyness (0xC812E8, set at 0x72C659): min(min(Wind,1) + 0.3, 1), Wind = lerp of the exe table at 0x8D5E50 (read) between old and new weather
		static const float kWindForWeather[23] = {0, 0.25f, 0, 0.2f, 0.7f, 0.25f, 0, 0.7f, 1.0f, 0, 0.2f, 0, 0.4f, 0, 0.3f, 0.7f, 1.0f, 0, 0.3f, 1.5f, 0, 0, 0};
		float wf = gWeatherForced ? 1.0f : std::fmod(gGameMinutes, 60.0f) / 60.0f;
		float wind = kWindForWeather[gOldWeather % 23] + (kWindForWeather[gWeather % 23] - kWindForWeather[gOldWeather % 23]) * wf;
		const float kWaterK = std::min(std::min(wind, 1.0f) + 0.3f, 1.0f);
		float shiftU, shiftV;
		if (layer == 0) {
			shiftU = acc0U; shiftV = std::cos((float)(tms & 0x1FFF) * 0.0007669904f) * 0.024f + acc0V;
		} else {
			float a = (float)(tms & 0xFFF) * 0.0015339808f;
			shiftU = std::sin(a) * kWaterK * 0.08f + acc1U; shiftV = std::cos(a) * kWaterK * 0.08f + acc1V;
		}
		std::vector<rw::RWDEVICE::Im3DVertex> verts;
		std::vector<rw::uint16> idx;
		for (const sa::WaterPoly &w : gWater) {
			float cx = 0, cy = 0;
			for (int i = 0; i < w.n; i++) { cx += w.x[i]; cy += w.y[i]; }
			cx /= w.n; cy /= w.n;
			float dx = cx - camPos.x, dy = cy - camPos.y;
			if (dx * dx + dy * dy > (gFarClip + 3000.0f) * (gFarClip + 3000.0f)) continue; // polygons are large; keep generous
			if (verts.size() + 4 > 65000) break;
			rw::uint16 base = (rw::uint16)verts.size();
			for (int i = 0; i < w.n; i++) {
				rw::RWDEVICE::Im3DVertex v;
				v.setX(w.x[i]); v.setY(w.y[i]); v.setZ(w.z[i]);
				v.setColor(c8(gTc.water[0]), c8(gTc.water[1]), c8(gTc.water[2]), alpha); // tried x0.577 (gta-reversed GetWaterColorForRendering): makes the sea much darker than the original, so not applied
				v.setU(w.x[i] / scale + shiftU); v.setV(w.y[i] / scale + shiftV);
				verts.push_back(v);
			}
			idx.push_back(base); idx.push_back(base + 1); idx.push_back(base + 2);
			if (w.n == 4) { idx.push_back(base + 1); idx.push_back(base + 3); idx.push_back(base + 2); }
		}
		if (verts.empty()) return;
		rw::SetRenderStatePtr(rw::TEXTURERASTER, tex ? tex->raster : nil);
		rw::SetRenderState(rw::VERTEXALPHA, 1);
		rw::SetRenderState(rw::SRCBLEND, rw::BLENDSRCALPHA);
		rw::SetRenderState(rw::DESTBLEND, rw::BLENDINVSRCALPHA);
		rw::SetRenderState(rw::ZWRITEENABLE, 0);
		rw::SetRenderState(rw::FOGENABLE, 1); // RenderWater (0x6EF673) sets render state 14 (fog) to 1
		rw::SetRenderState(rw::CULLMODE, rw::CULLNONE);
		rw::im3d::Transform(verts.data(), (rw::int32)verts.size(), nil, rw::im3d::EVERYTHING);
		rw::im3d::RenderIndexedPrimitive(rw::PRIMTYPETRILIST, idx.data(), (rw::int32)idx.size());
		rw::im3d::End();
	}
	rw::SetRenderStatePtr(rw::TEXTURERASTER, nil);
	rw::SetRenderState(rw::ZWRITEENABLE, 1);
	rw::SetRenderState(rw::CULLMODE, rw::CULLBACK);
}

// CWeather::Update wind (0x72C398..0x72C63C); tables at 0x8D5E50 / 0x8D5FF8 / 0x8D6038 read from the exe.
struct WindState { float wind; rw::V3d dir; };
WindState WeatherWind(unsigned timeMs) {
	static const float kWindForWeather[23] = {0, 0.25f, 0, 0.2f, 0.7f, 0.25f, 0, 0.7f, 1.0f, 0, 0.2f, 0, 0.4f, 0, 0.3f, 0.7f, 1.0f, 0, 0.3f, 1.5f, 0, 0, 0};
	static const float kScale[16] = {1.0f, 0.5f, 1.0f, 0.2f, 0.4f, 1.0f, 1.0f, 1.0f, 1.0f, 0.8f, 0.0f, 1.0f, 1.0f, 0.7f, 1.0f, 1.0f};
	static const float kOff[16] = {0.5f, -0.3f, 0.8f, 0.0f, -0.4f, -0.8f, 0.3f, -0.1f, -0.9f, -0.5f, 0.7f, 0.7f, 0.3f, 0.7f, 0.0f, -0.5f};
	float wf = gWeatherForced ? 1.0f : std::fmod(gGameMinutes, 60.0f) / 60.0f;
	float a = kWindForWeather[gOldWeather % 23], b = kWindForWeather[gWeather % 23];
	WindState w;
	w.wind = a + (b - a) * wf;
	float wc = std::min(w.wind, 1.0f);
	auto lerpf = [](float x, float y, float t) { return x + (y - x) * t; };
	auto off = [&](size_t i, float t) { return lerpf(kOff[i % 16], kOff[(i + 1) % 16], t); };
	size_t slow = (timeMs / 1024) % 16;
	float slowT = 0.5f - std::cos((float)(timeMs % 1024) / 1024.0f * 3.14159265f) * 0.5f;
	float wx = off(slow, slowT) * wc * 0.4f + wc * 0.7f, wy = off(slow + 3, slowT) * wc * 0.4f + wc * 0.7f;
	float wz = off(slow + 6, slowT) * wc * 0.2f;
	float gust = (wc - 0.5f) * 0.4f;
	if (gust > 0.0f) {
		size_t fast = (timeMs / 256) % 16;
		float fastT = (float)(timeMs % 256) / 256.0f;
		wx += off(fast, fastT) * gust; wy += off(fast + 3, fastT) * gust; wz += off(fast + 6, fastT) * gust;
	}
	size_t si = (timeMs / 2048) % 16;
	float sT = 0.5f - std::cos((float)(timeMs % 2048) / 2048.0f * 3.14159265f) * 0.5f;
	float sc = lerpf(kScale[si], kScale[(si + 1) % 16], sT);
	w.dir = {sc * wx, sc * wy, sc * wz};
	return w;
}

// CWeather::RenderRainStreaks (0x72AF70) + the Rain step in CWeather::Update (0x72BBE1): 32 line streaks in a 5 m box 6 m ahead of the camera.
// DEVIATIONS: FogReduction as 0, no rain particles/splashes/puddles/wet roads, no CullZone no-rain test.
float gRain = 0.0f;
int gRainStrength = 0;

void DrawRain(float dt, unsigned tms) {
	auto rainy = [](int w) { return w == 8 || w == 16; };
	float f = std::fmod(gGameMinutes, 60.0f) / 60.0f;
	if (gWeatherForced) f = 1.0f;
	float a = rainy(gOldWeather) ? 1.0f : 0.0f, b = rainy(gWeather) ? 1.0f : 0.0f;
	float intensity = (float)((tms >> 13) % 4) * 0.1f + 0.7f;
	float target = intensity * (a == b ? b : a + (b - a) * f), step = dt * 50.0f * 0.005f;
	gRain = gRain < target ? std::min(gRain + step, target) : std::max(gRain - step, target);

	int strength = (int)(gRain * 110.0f);
	if (gRainStrength < strength) gRainStrength++; else if (gRainStrength > 0) gRainStrength--;
	if (!gRainStrength || camPos.z > 900.0f) return;

	static int px[32], py[32], pz[32];
	static uint8_t pa[32];
	auto rnd = [](float lo, float hi) { return lo + (hi - lo) * ((float)rand() / (float)RAND_MAX); };
	rw::Matrix *m = &gCamera->getFrame()->matrix;
	WindState ws = WeatherWind(tms);
	std::vector<rw::RWDEVICE::Im3DVertex> verts;
	std::vector<rw::uint16> idx;
	for (int s = 0; s < 32; s++) {
		float dx = camPos.x - px[s], dy = camPos.y - py[s], dz = camPos.z - pz[s];
		if (!pa[s] || pz[s] <= 0 || dx * dx + dy * dy + dz * dz > 64.0f) {
			px[s] = (int)(rnd(0.0f, 5.0f) + m->at.x * 6.0f + camPos.x - 2.5f);
			py[s] = (int)(rnd(0.0f, 5.0f) + m->at.y * 6.0f + camPos.y - 2.5f);
			pz[s] = (int)(rnd(0.0f, 5.0f) + m->at.z * 6.0f + camPos.z - 2.5f);
			pa[s] = (uint8_t)((float)gRainStrength * 0.6f);
		}
		float ox = rnd(-0.2f, 0.2f), oy = rnd(-0.2f, 0.2f), oz = rnd(-0.1f, 0.1f);
		float o1z = oz + rnd(0.1f, 0.5f);
		uint8_t al[2] = {pa[s], (uint8_t)(pa[s] / 2u)};
		float oz2[2] = {oz, o1z};
		float posMul = (s % 2) ? ws.wind * 0.1f : ws.wind * gRain * 0.1f;
		float ex[2] = {ox, ox - ws.dir.x * posMul}, ey[2] = {oy, oy - ws.dir.y * posMul};
		oz2[1] -= ws.dir.z * posMul;
		for (int v = 0; v < 2; v++) {
			rw::RWDEVICE::Im3DVertex iv;
			iv.setX((float)px[s] + ex[v]); iv.setY((float)py[s] + ey[v]); iv.setZ((float)pz[s] + oz2[v]);
			if (getenv("SA_RAINDEBUG")) iv.setColor(255, 0, 0, 255); else iv.setColor(210, 210, 230, al[v]);
			iv.setU(0.0f); iv.setV(0.0f);
			idx.push_back((rw::uint16)verts.size());
			verts.push_back(iv);
		}
		pz[s] -= (int)rnd(0.01f, 0.1f);
		int dec = 2 + rand() % 4;
		pa[s] = (uint8_t)std::max(0, (int)pa[s] - dec);
	}
	rw::SetRenderState(rw::ZWRITEENABLE, 0);
	rw::SetRenderState(rw::ZTESTENABLE, 1);
	rw::SetRenderState(rw::FOGENABLE, 0);
	rw::SetRenderState(rw::SRCBLEND, rw::BLENDSRCALPHA);
	rw::SetRenderState(rw::DESTBLEND, rw::BLENDINVSRCALPHA);
	rw::SetRenderState(rw::VERTEXALPHA, 1);
	rw::SetRenderStatePtr(rw::TEXTURERASTER, nil);
	rw::im3d::Transform(verts.data(), (rw::int32)verts.size(), nil, rw::im3d::EVERYTHING);
	rw::im3d::RenderIndexedPrimitive(rw::PRIMTYPELINELIST, idx.data(), (rw::int32)idx.size());
	rw::im3d::End();
	rw::SetRenderState(rw::ZWRITEENABLE, 1);
	rw::SetRenderState(rw::VERTEXALPHA, 0);
}

void Idle(float dt) {
	static auto start = std::chrono::steady_clock::now();
	double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	bool anyMove = sk::keys['W'] || sk::keys['A'] || sk::keys['S'] || sk::keys['D'];
	float speed = (gWalk ? ClipSpeed(PedClip(anyMove)) : (sk::keys[sk::KEY_LSHIFT] ? 200.0f : 40.0f)) * dt;
	float yr = camYaw * 3.14159265f / 180.0f;
	rw::V3d fwd = {std::sin(yr), std::cos(yr), 0.0f}, right = {std::cos(yr), -std::sin(yr), 0.0f};
	static bool jumpDown = false;
	static bool autoJumped = false; // test aid: SA_AUTOJUMP=1 jumps once after 12 s
	bool autoJump = getenv("SA_AUTOJUMP") && now > 12.0 && !autoJumped;
	if (autoJump) autoJumped = true;
	if (gWalk && gGrounded && !gSwimming && ((sk::keys[sk::KEY_LSHIFT] && !jumpDown) || autoJump)) {
		// CTaskSimpleJump::Launch (0x679B80): player force 8.5 (@0x8703C0) on a 70 kg ped (CPed ctor, 0x5E82A4) = 8.5/70 per tick (x50 ticks/s);
		// horizontal 0.1 / 0.17 / 0.22 (@0x8703B0..B8) standing / running / sprinting. All read in the exe. ASSUMED: fat/muscle modifier 1.
		float h = !anyMove ? 0.1f : sk::keys[sk::KEY_LALT] ? 0.1f : sk::keys[' '] ? 0.22f : 0.17f;
		gVelZ = 8.5f / 70.0f * 50.0f;
		gAirVx = fwd.x * h * 50.0f;
		gAirVy = fwd.y * h * 50.0f;
		gGrounded = false;
		auto lj = gAnims.find("jump_launch");
		gLaunchTimer = lj != gAnims.end() ? lj->second.duration : 0.0f;
	}
	jumpDown = sk::keys[sk::KEY_LSHIFT];
	if (gWalk && gSwimming) {
		float sp = ClipSpeed(sk::keys[' '] ? "swim_crawl" : "swim_breast") * dt; // root motion of the swim clips: breast 2.32, crawl 2.78 m/s
		if (sk::keys['W']) { camPos.x += fwd.x * sp; camPos.y += fwd.y * sp; }
		if (sk::keys['S']) { camPos.x -= fwd.x * sp; camPos.y -= fwd.y * sp; }
		if (sk::keys['D']) { camPos.x += right.x * sp; camPos.y += right.y * sp; }
		if (sk::keys['A']) { camPos.x -= right.x * sp; camPos.y -= right.y * sp; }
	} else if (gWalk && !gGrounded) {
		camPos.x += gAirVx * dt;
		camPos.y += gAirVy * dt;
	} else {
		if (sk::keys['W']) { camPos.x += fwd.x * speed; camPos.y += fwd.y * speed; }
		if (sk::keys['S']) { camPos.x -= fwd.x * speed; camPos.y -= fwd.y * speed; }
		if (sk::keys['D']) { camPos.x += right.x * speed; camPos.y += right.y * speed; }
		if (sk::keys['A']) { camPos.x -= right.x * speed; camPos.y -= right.y * speed; }
	}
	static bool gDown = false;
	if (sk::keys['G'] && !gDown) { gWalk = !gWalk; gVelZ = 0.0f; }
	gDown = sk::keys['G'];
	if (!gWalk) {
		if (sk::keys['E']) camPos.z += speed;
		if (sk::keys['Q']) camPos.z -= speed;
	} else {
		CollideWalker();
		UpdateWalk(dt);
	}
	if (const char *s = getenv("SA_AUTOFLY")) camPos.x += (float)atof(s) * dt; // scripted flight for testing
	if (const char *s = getenv("SA_AUTOFLYY")) camPos.y += (float)atof(s) * dt;
	if (!(getenv("SA_CLOCK") && atoi(getenv("SA_CLOCK")) == 0)) gGameMinutes = std::fmod(gGameMinutes + dt * 1000.0f / kMsPerGameMinute, 1440.0f);
	ApplyTimeOfDay();
	UpdateStreaming(now);
	UpdatePed();
	AnimatePed(dt);
	UpdateCamera();
	gCamera->clear(&Background, rw::Camera::CLEARIMAGE | rw::Camera::CLEARZ);
	gCamera->beginUpdate();
	DrawSky();
	rw::SetRenderState(rw::FOGENABLE, gTc.ok);
	rw::SetRenderState(rw::FOGCOLOR, (uint32_t)(Background.red | Background.green << 8 | Background.blue << 16 | 0xFF000000u));
	rw::SetRenderState(rw::ALPHATESTFUNC, rw::ALPHAGREATEREQUAL);
	rw::SetRenderState(rw::ALPHATESTREF, 100);
	gWorld->render();
	DrawWater();
	DrawRain(dt, (unsigned)(now * 1000.0));
	gCamera->endUpdate();
	gCamera->showRaster(0);
}

} // namespace

sk::EventStatus AppEventHandler(sk::Event e, void *param) {
	using namespace sk;
	switch (e) {
	case INITIALIZE:
		sk::globals.windowtitle = "sa_stream";
		sk::globals.width = 1280;
		sk::globals.height = 720;
		sk::globals.quit = 0;
		if (sk::args.argc > 1) gameDir = sk::args.argv[1];
		if (sk::args.argc > 3) { centerX = (float)atof(sk::args.argv[2]); centerY = (float)atof(sk::args.argv[3]); }
		if (sk::args.argc > 4) loadRadius = (float)atof(sk::args.argv[4]);
		gLog = fopen("E:/openSA/sa_view/sa_stream.log", "w");
		if (!gLog) gLog = stderr;
		return gameDir.empty() ? EVENTERROR : EVENTPROCESSED;
	case RWINITIALIZE: return Initialize3D() ? EVENTPROCESSED : EVENTERROR;
	case RWTERMINATE: Terminate3D(); return EVENTPROCESSED;
	case PLUGINATTACH: return AttachPlugins() ? EVENTPROCESSED : EVENTERROR;
	case KEYDOWN:
		if (*(int *)param == KEY_ESC) sk::globals.quit = 1;
		return EVENTPROCESSED;
	case MOUSEMOVE: {
		MouseState *m = (MouseState *)param;
		if (m->buttons & 1) {
			camYaw += (m->posx - sk::prevmouse.posx) * 0.25f;
			camPitch -= (m->posy - sk::prevmouse.posy) * 0.25f;
			camPitch = std::max(-85.0f, std::min(85.0f, camPitch));
		}
		return EVENTPROCESSED;
	}
	case RESIZE: {
		Rect *r = (Rect *)param;
		if (r->w == 0) r->w = 1;
		if (r->h == 0) r->h = 1;
		sk::globals.width = r->w;
		sk::globals.height = r->h;
		if (gCamera) {
			sk::CameraSize(gCamera, r);
			gCamera->setFOV(gFovDeg, (float)r->w / r->h);
		}
		break;
	}
	case IDLE:
		Idle(*(float *)param);
		return EVENTPROCESSED;
	}
	return sk::EVENTNOTPROCESSED;
}
