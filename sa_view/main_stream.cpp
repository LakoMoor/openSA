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
	if (gTc.ok) {
		gFarClip = std::min(gTc.farClip, 1000.0f);
		Background = {(uint8_t)gTc.skyBottom[0], (uint8_t)gTc.skyBottom[1], (uint8_t)gTc.skyBottom[2], 255};
		gCamera->setFarPlane(gFarClip);
		gCamera->fogPlane = std::min(gTc.fogStart, gFarClip - 1.0f);
		if (gAmbient) gAmbient->setColor(gTc.ambientObj[0] / 255.0f, gTc.ambientObj[1] / 255.0f, gTc.ambientObj[2] / 255.0f);
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
bool PushOutSphere(rw::V3d &c, float r) {
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
// the feet (box min z = -1.0). kEye (camera above feet) is still ASSUMED.
void CollideWalker() {
	constexpr float kRadius = 0.35f, kEye = 1.7f;
	const float heights[3] = {0.8f, 1.2f, 1.6f}; // above the feet
	for (int iter = 0; iter < 3; iter++) {
		bool any = false;
		for (float h : heights) {
			rw::V3d c = {camPos.x, camPos.y, camPos.z - kEye + h};
			if (PushOutSphere(c, kRadius)) { camPos.x = c.x; camPos.y = c.y; any = true; }
		}
		if (!any) break;
	}
}

// Walk mode (G): the camera is a point 1.7 m above the ground with gravity. DEVIATION: the original uses a ped with a collision
// capsule, animations and physics; horizontal collision uses the exe's three ped spheres (CollideWalker); only the eye height is assumed.
bool gWalk = false;
float gVelZ = 0.0f;

void UpdateWalk(float dt) {
	constexpr float kEye = 1.7f, kGravity = 20.0f, kStep = 1.0f, kMaxDrop = 60.0f;
	float gz;
	bool ground = GroundHeight(camPos.x, camPos.y, camPos.z - kEye + kStep, kMaxDrop, gz);
	if (!ground) { gVelZ = 0.0f; return; } // no collision loaded here (yet): hold the height
	float floorZ = gz + kEye;
	if (camPos.z > floorZ + 0.02f) {
		gVelZ -= kGravity * dt;
		camPos.z += gVelZ * dt;
		if (camPos.z < floorZ) { camPos.z = floorZ; gVelZ = 0.0f; }
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
		fprintf(gLog, "z=%.2f walk=%d ", camPos.z, (int)gWalk);
			fprintf(gLog, "clock %02d:%02d balance %.2f  ", (int)(gGameMinutes / 60), (int)gGameMinutes % 60, gDayNightBalance);
			fprintf(gLog, "t=%.0fs cam=(%.0f,%.0f) active=%zu protos=%d pending=%zu streamer: loaded=%zu %.1f MB queued=%zu\n", now, camPos.x,
		        camPos.y, gActive.size(), (int)std::count_if(gModels.begin(), gModels.end(), [](auto &kv) { return kv.second.proto != nil; }),
		        gPending.size(), gStreamer->LoadedCount(), gStreamer->LoadedBytes() / 1048576.0, gStreamer->PendingCount());
		fflush(gLog);
	}
}

// Visible ped (walk mode). DEVIATIONS: the model is the generic male01 ped (CJ needs the clothes builder: player.dff + player.img parts);
// the follow camera uses an assumed distance/height instead of CCam::Process_FollowPed_SA, with a simple ray test against COL geometry.
rw::Clump *gPed = nil;
rw::HAnimHierarchy *gPedHier = nil;
constexpr float kPedFollowDist = 4.0f, kPedFollowUp = 0.8f;

// ped.ifp animation on the ped's hierarchy. Rotation keys replace the bone's local rotation, translation keys (root only) its position.
// Key quaternions are used as stored (checked visually: conjugating them lays the ped on its side). ASSUMED: bones without a translation
// key keep their bind position.
std::unordered_map<std::string, sa::IfpAnim> gAnims;
const sa::IfpAnim *gPedAnim = nil;
std::vector<rw::V3d> gBindPos;
float gAnimTime = 0.0f;

void LoadPedAnims() {
	std::ifstream f(gameDir + "/anim/ped.ifp", std::ios::binary);
	std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	size_t n = sa::ParseIfp(b, gAnims);
	fprintf(gLog, "ped.ifp: %zu animations\n", n);
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

// PC controls: moving runs by default, Shift walks, Space sprints. ASSUMED: the sprint clip is sprint_civi.
const char *PedClip(bool moving) {
	if (!moving) return "idle_stance";
	if (sk::keys[sk::KEY_LSHIFT]) return "walk_player";
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

void SelectPedAnim(const char *name) {
	auto it = gAnims.find(name);
	if (it != gAnims.end() && gPedAnim != &it->second) { gPedAnim = &it->second; gAnimTime = 0.0f; }
}

void AnimatePed(float dt) {
	// DEVIATION: the original blends clips through the ped task system; this just switches between idle / walk / run
	bool moving = sk::keys['W'] || sk::keys['A'] || sk::keys['S'] || sk::keys['D'];
	SelectPedAnim(PedClip(moving));
	if (!gPed || !gPedHier || !gPedAnim || gPedAnim->duration <= 0) return;
	gAnimTime = std::fmod(gAnimTime + dt, gPedAnim->duration);
	for (const sa::IfpSeq &sq : gPedAnim->seqs) {
		int ni = gPedHier->getIndex(sq.boneTag);
		if (ni < 0 || sq.keys.empty()) continue;
		rw::Frame *fr = gPedHier->nodeInfo[ni].frame;
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
	gPed->getFrame()->updateObjects();
	// the hierarchy has no interpolator (updateMatrices would crash), so take the bone matrices straight from the frames' LTMs
	for (int i = 0; i < gPedHier->numNodes; i++)
		if (gPedHier->nodeInfo[i].frame) gPedHier->matrices[i] = *gPedHier->nodeInfo[i].frame->getLTM();
}

void EnsurePed() {
	if (gPed) return;
	const sa::ImgV2 &gta3 = gW.archives[0];
	const sa::ImgEntry *ed = gta3.Find("male01.dff"), *et = gta3.Find("male01.txd");
	std::vector<uint8_t> dff, txd;
	if (!ed || !et || !gta3.Read(*ed, dff) || !gta3.Read(*et, txd)) { fprintf(gLog, "ped: male01 not found\n"); return; }
	rw::TexDictionary *d = ReadTxd(txd);
	gChain.clear();
	if (d) gChain.push_back(d);
	rw::TexDictionary::setCurrent(nil);
	rw::Texture::findCB = FindInChain;
	rw::StreamMemory sm;
	sm.open(dff.data(), (rw::uint32)dff.size());
	if (rw::findChunk(&sm, rw::ID_CLUMP, nil, nil)) gPed = rw::Clump::streamRead(&sm);
	sm.close();
	fprintf(gLog, "ped: male01 %s\n", gPed ? "loaded" : "failed");
	if (gPed) {
		gWorld->addClump(gPed);
		gPedHier = rw::HAnimHierarchy::find(gPed->getFrame());
		fprintf(gLog, "ped: hierarchy %s (%d nodes, flags %x, matrices %p, interp %p)\n", gPedHier ? "found" : "missing", gPedHier ? gPedHier->numNodes : 0,
		        gPedHier ? gPedHier->flags : 0, gPedHier ? (void *)gPedHier->matrices : nullptr, gPedHier ? (void *)gPedHier->interpolator : nullptr);
		if (gPedHier) gPedHier->attach(); // binds each node to the frame with the same hanim id
		if (gPedHier) FORLIST(lnk, gPed->atomics) {
			rw::Atomic *at = rw::Atomic::fromClump(lnk);
			if (rw::Skin::get(at->geometry)) rw::Skin::setHierarchy(at, gPedHier);
		}
		if (gAnims.empty()) LoadPedAnims();
	}
}

void UpdatePed() {
	const bool show = getenv("SA_PEDSHOW") != nullptr; // test aid: draw the ped 6 m ahead of the free camera
	if (!gWalk && !show) { if (gPed) { gWorld->removeClump(gPed); gPed->destroy(); gPed = nil; gPedHier = nil; } return; }
	EnsurePed();
	if (!gPed) return;
	rw::V3d zaxis = {0, 0, 1};
	rw::Frame *f = gPed->getFrame();
	f->rotate(&zaxis, -camYaw, rw::COMBINEREPLACE);
	rw::V3d feet = {camPos.x, camPos.y, camPos.z - 1.7f + 1.0f};
	if (show && !gWalk) feet = {camPos.x + std::sin(camYaw * 0.0174533f) * 6.0f, camPos.y + std::cos(camYaw * 0.0174533f) * 6.0f, camPos.z - 2.0f}; // ped origin is 1.0 above the feet (verified); 1.7 = assumed eye height
	f->translate(&feet, rw::COMBINEPOSTCONCAT);
}

void UpdateCamera() {
	float yr = camYaw * 3.14159265f / 180.0f, pr = camPitch * 3.14159265f / 180.0f;
	rw::V3d dir = {std::sin(yr) * std::cos(pr), std::cos(yr) * std::cos(pr), std::sin(pr)};
	rw::V3d up = {0.0f, 0.0f, 1.0f};
	rw::Frame *f = gCamera->getFrame();
	f->matrix.lookAt(dir, up);
	f->matrix.pos = camPos;
	if (gWalk && gPed) { // third-person: back off along the view direction, stopping short of geometry in between
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
	fprintf(gLog, "timecyc: minute-of-day %.0f weather %d ok=%d farClip=%.0f fogStart=%.0f\n", gGameMinutes, gWeather, (int)gTc.ok, gFarClip, gTc.fogStart);
	gWorld->addLight(amb);
	rw::Light *sun = rw::Light::create(rw::Light::DIRECTIONAL);
	sun->setColor(0.6f, 0.6f, 0.55f);
	rw::Frame *sf = rw::Frame::create();
	rw::V3d xa = {1, 0, 0}, ya = {0, 1, 0};
	sf->rotate(&xa, 50.0f, rw::COMBINEREPLACE);
	sf->rotate(&ya, 30.0f, rw::COMBINEPOSTCONCAT);
	sun->setFrame(sf);
	gWorld->addLight(sun);

	auto t0 = std::chrono::steady_clock::now();
	std::string err;
	if (!gW.Build(gameDir, &err)) { fprintf(gLog, "streaming world: %s\n", err.c_str()); return false; }
	gStreamer = new sa::Streamer(gW);
	CollectPlacements();
	double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	fprintf(gLog, "startup %.0f ms: %zu instances in %zu cells, %d slots with archive entry\n", ms, gInsts.size(), gGrid.size(),
	        gW.store.CountWithArchiveEntry());
	fflush(gLog);

	camPos = {centerX - 30.0f, centerY - 30.0f, 40.0f};
	camYaw = 45.0f;
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

// Sky gradient: timecyc sky-top colour at the top of the screen blending to sky-bottom at the horizon (the clear colour fills below it).
// DEVIATION: the original (CClouds::Render) also draws the sun, moon, cloud layers and low-level clouds; none of that is implemented here.
void DrawSky() {
	if (!gTc.ok) return;
	rw::Raster *r = gCamera->frameBuffer;
	float W = (float)r->width, H = (float)r->height;
	float tanHalf = gCamera->viewWindow.y;
	float pitch = camPitch * 3.14159265f / 180.0f;
	float horizon = H * 0.5f + std::tan(pitch) / tanHalf * H * 0.5f;
	horizon = std::max(0.0f, std::min(H, horizon));
	if (horizon <= 0.0f) return;
	rw::RWDEVICE::Im2DVertex v[4];
	float xs[4] = {0, W, 0, W}, ys[4] = {0, 0, horizon, horizon};
	const float *col[4] = {gTc.skyTop, gTc.skyTop, gTc.skyBottom, gTc.skyBottom};
	float recipZ = 1.0f / gCamera->nearPlane;
	for (int i = 0; i < 4; i++) {
		v[i].setScreenX(xs[i]);
		v[i].setScreenY(ys[i]);
		v[i].setScreenZ(rw::im2d::GetNearZ());
		v[i].setCameraZ(gCamera->nearPlane);
		v[i].setRecipCameraZ(recipZ);
		v[i].setColor((uint8_t)col[i][0], (uint8_t)col[i][1], (uint8_t)col[i][2], 255);
		v[i].setU(0, recipZ);
		v[i].setV(0, recipZ);
	}
	rw::uint16 idx[6] = {0, 1, 2, 1, 3, 2};
	rw::SetRenderStatePtr(rw::TEXTURERASTER, nil);
	rw::SetRenderState(rw::ZTESTENABLE, 0);
	rw::SetRenderState(rw::ZWRITEENABLE, 0);
	rw::SetRenderState(rw::CULLMODE, rw::CULLNONE);
	rw::SetRenderState(rw::FOGENABLE, 0);
	rw::im2d::RenderIndexedPrimitive(rw::PRIMTYPETRILIST, v, 4, idx, 6);
	rw::SetRenderState(rw::ZTESTENABLE, 1);
	rw::SetRenderState(rw::ZWRITEENABLE, 1);
	rw::SetRenderState(rw::CULLMODE, rw::CULLBACK);
}

void Idle(float dt) {
	static auto start = std::chrono::steady_clock::now();
	double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	bool anyMove = sk::keys['W'] || sk::keys['A'] || sk::keys['S'] || sk::keys['D'];
	float speed = (gWalk ? ClipSpeed(PedClip(anyMove)) : (sk::keys[sk::KEY_LSHIFT] ? 200.0f : 40.0f)) * dt;
	float yr = camYaw * 3.14159265f / 180.0f;
	rw::V3d fwd = {std::sin(yr), std::cos(yr), 0.0f}, right = {std::cos(yr), -std::sin(yr), 0.0f};
	if (sk::keys['W']) { camPos.x += fwd.x * speed; camPos.y += fwd.y * speed; }
	if (sk::keys['S']) { camPos.x -= fwd.x * speed; camPos.y -= fwd.y * speed; }
	if (sk::keys['D']) { camPos.x += right.x * speed; camPos.y += right.y * speed; }
	if (sk::keys['A']) { camPos.x -= right.x * speed; camPos.y -= right.y * speed; }
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
