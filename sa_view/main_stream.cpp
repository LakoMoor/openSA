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
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "BinaryIpl.h"
#include "ColBounds.h"
#include "Timecyc.h"
#include "Streamer.h"
#include "StreamingWorld.h"

rw::EngineOpenParams engineOpenParams;

namespace {

struct Inst {
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
	if (!ms.proto) ms.failed = true;
	else if (txd >= 0)
		for (int t : TxdChain(txd)) gTxdUsers[t]++;
	gPending.erase(id);
}

const float kMaxRange = 1000.0f;

// Text IPLs come first in a "name -> local index -> global index" table because the binary streaming IPLs
// (X_streamN) reference their LOD parent by index into the text IPL X (CIplStore::LoadIpl, see BinaryIpl.h).
std::unordered_map<std::string, sa::ColBound> gColBounds; // lower-case model name -> COL bound

void LoadColBounds() {
	const sa::ImgV2 &gta3 = gW.archives[0];
	size_t files = 0;
	for (auto &e : gta3.Entries()) {
		auto dot = e.name.rfind('.');
		if (dot == std::string::npos || Lower(e.name.substr(dot)) != ".col") continue;
		std::vector<uint8_t> buf;
		if (gta3.Read(e, buf)) { sa::ParseColBounds(buf.data(), buf.size(), gColBounds); files++; }
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
		if (cb != gColBounds.end()) i.colRadius = cb->second.radius;
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
// CCamera::Process (0x52B730): m_fLODDistMultiplier = ms_lodDistScale (0x8CD800 = 1.2) * 70 / CDraw::ms_fFOV (0x8D5038, 45 in the exe image),
// or 1.0 while the flags at 0xB5F851/0xB5F853 select the other branch. ASSUMPTION: the game runs with FOV 70, as this viewer does.
constexpr float kFovDeg = 70.0f, kLodDistScale = 1.2f, kLodDistMult = kLodDistScale * 70.0f / kFovDeg;

// CFileLoader (0x5B51E0, decompiled): SetupBigBuilding is called for an instance with LOD children or lodMult * drawDist > 300.
bool IsBigBuilding(const Inst &in) { return in.numLodChildren > 0 || kLodDistMult * in.drawDist > 300.0f; }

float DrawLimit(const Inst &in, float &fading) {
	float farClip = gFarClip;
	float d = std::min(kLodDistMult * in.drawDist, farClip + in.colRadius);
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
		fprintf(gLog, "t=%.0fs cam=(%.0f,%.0f) active=%zu protos=%d pending=%zu streamer: loaded=%zu %.1f MB queued=%zu\n", now, camPos.x,
		        camPos.y, gActive.size(), (int)std::count_if(gModels.begin(), gModels.end(), [](auto &kv) { return kv.second.proto != nil; }),
		        gPending.size(), gStreamer->LoadedCount(), gStreamer->LoadedBytes() / 1048576.0, gStreamer->PendingCount());
		fflush(gLog);
	}
}

void UpdateCamera() {
	float yr = camYaw * 3.14159265f / 180.0f, pr = camPitch * 3.14159265f / 180.0f;
	rw::V3d dir = {std::sin(yr) * std::cos(pr), std::cos(yr) * std::cos(pr), std::sin(pr)};
	rw::V3d up = {0.0f, 0.0f, 1.0f};
	rw::Frame *f = gCamera->getFrame();
	f->matrix.lookAt(dir, up);
	f->matrix.pos = camPos;
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
	{
		// time of day / weather: SA_HOUR (default 12), SA_WEATHER (default 0 = EXTRASUNNY_LA)
		sa::Timecyc tc;
		float hour = getenv("SA_HOUR") ? (float)atof(getenv("SA_HOUR")) : 12.0f;
		int weather = getenv("SA_WEATHER") ? atoi(getenv("SA_WEATHER")) : 0;
		if (tc.Load(gameDir + "/data/timecyc.dat")) gTc = tc.Sample(weather, hour);
		if (gTc.ok) {
			gFarClip = std::min(gTc.farClip, 1000.0f);
			Background = {(uint8_t)gTc.skyBottom[0], (uint8_t)gTc.skyBottom[1], (uint8_t)gTc.skyBottom[2], 255};
		}
		fprintf(gLog, "timecyc: hour %.1f weather %d ok=%d farClip=%.0f fogStart=%.0f sky bottom=(%.0f,%.0f,%.0f)\n", hour, weather, (int)gTc.ok, gFarClip,
		        gTc.fogStart, gTc.skyBottom[0], gTc.skyBottom[1], gTc.skyBottom[2]);
	}
	gCamera->setFarPlane(gFarClip);
	gCamera->fogPlane = gTc.ok ? std::min(gTc.fogStart, gFarClip - 1.0f) : gFarClip * 0.5f;
	gCamera->setFOV(kFovDeg, (float)sk::globals.width / sk::globals.height);
	gWorld->addCamera(gCamera);

	rw::Light *amb = rw::Light::create(rw::Light::AMBIENT);
	if (gTc.ok) amb->setColor(gTc.ambientObj[0] / 255.0f, gTc.ambientObj[1] / 255.0f, gTc.ambientObj[2] / 255.0f);
	else amb->setColor(0.65f, 0.65f, 0.7f);
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
	return true;
}

void Idle(float dt) {
	static auto start = std::chrono::steady_clock::now();
	double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	float speed = (sk::keys[sk::KEY_LSHIFT] ? 200.0f : 40.0f) * dt;
	float yr = camYaw * 3.14159265f / 180.0f;
	rw::V3d fwd = {std::sin(yr), std::cos(yr), 0.0f}, right = {std::cos(yr), -std::sin(yr), 0.0f};
	if (sk::keys['W']) { camPos.x += fwd.x * speed; camPos.y += fwd.y * speed; }
	if (sk::keys['S']) { camPos.x -= fwd.x * speed; camPos.y -= fwd.y * speed; }
	if (sk::keys['D']) { camPos.x += right.x * speed; camPos.y += right.y * speed; }
	if (sk::keys['A']) { camPos.x -= right.x * speed; camPos.y -= right.y * speed; }
	if (sk::keys['E']) camPos.z += speed;
	if (sk::keys['Q']) camPos.z -= speed;
	if (const char *s = getenv("SA_AUTOFLY")) camPos.x += (float)atof(s) * dt; // scripted flight for testing
	UpdateStreaming(now);
	UpdateCamera();
	gCamera->clear(&Background, rw::Camera::CLEARIMAGE | rw::Camera::CLEARZ);
	gCamera->beginUpdate();
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
		gLog = fopen("sa_stream.log", "w");
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
			gCamera->setFOV(kFovDeg, (float)r->w / r->h);
		}
		break;
	}
	case IDLE:
		Idle(*(float *)param);
		return EVENTPROCESSED;
	}
	return sk::EVENTNOTPROCESSED;
}
