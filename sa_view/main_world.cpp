// sa_world: static view of a slice of the GTA SA map built from IPL/IDE/IMG data with librw (D3D9).
// usage: sa_world.exe <SA game dir> [x y radius]     default: Grove Street, LS (2495 -1690), radius 250
// Controls: W/A/S/D move, Q/E down/up, Shift = fast, left mouse drag = look, Esc = quit.
#include <rw.h>
#include <skeleton.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "BinaryIpl.h"
#include "ImgV2.h"
#include "TextData.h"

rw::EngineOpenParams engineOpenParams;

namespace {

struct Placement {
	int id;
	rw::V3d pos;
	rw::Quat rot; // already converted to RW convention
	float lodDist;
};

rw::World *gWorld;
rw::Camera *gCamera;
rw::RGBA Background = {120, 160, 210, 255};
std::map<int, int> gFilterHist, gAddrUHist;

std::string gameDir;
float centerX = 2495.0f, centerY = -1690.0f, radius = 250.0f;
rw::V3d camPos;
float camYaw = 0.0f, camPitch = -12.0f; // degrees; yaw 0 looks along +Y
FILE *gLog;

sa::IdeDb gIde;
sa::ImgV2 gImg;
std::map<std::string, rw::TexDictionary *> gTxdCache; // lower-case name -> dictionary (kept alive)
std::unordered_map<std::string, std::string> gTxdParent;

std::string Lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
	return s;
}

std::string Join(const std::string &rel) {
	std::string r = rel;
	std::replace(r.begin(), r.end(), '\\', '/');
	return gameDir + "/" + r;
}

bool IsLodName(const std::string &n) { return Lower(n).compare(0, 3, "lod") == 0; }

// Hour 12: a timed object is visible if 12 is inside [on, off) (the interval may wrap midnight).
bool TimedVisibleAtNoon(int on, int off) { return on <= off ? (on <= 12 && 12 < off) : (12 >= on || 12 < off); }

rw::TexDictionary *LoadTxdRaw(const std::string &name) {
	const sa::ImgEntry *e = gImg.Find(name + ".txd");
	if (!e) return nil;
	std::vector<uint8_t> buf;
	if (!gImg.Read(*e, buf)) return nil;
	rw::StreamMemory s;
	s.open(buf.data(), (rw::uint32)buf.size());
	rw::TexDictionary *txd = nil;
	if (rw::findChunk(&s, rw::ID_TEXDICTIONARY, nil, nil)) txd = rw::TexDictionary::streamRead(&s);
	s.close();
	return txd;
}

// Returns a dictionary holding this TXD's textures plus clones of those from its parent chain.
// Raw dictionaries are never destroyed, so the clones can share their rasters.
rw::TexDictionary *GetTxd(const std::string &name) {
	std::string key = Lower(name);
	auto it = gTxdCache.find(key);
	if (it != gTxdCache.end()) return it->second;
	rw::TexDictionary *own = LoadTxdRaw(key);
	gTxdCache[key] = own;
	if (!own) return nil;
	auto p = gTxdParent.find(key);
	for (int depth = 0; p != gTxdParent.end() && depth < 4; depth++, p = gTxdParent.find(p->second)) {
		rw::TexDictionary *par = LoadTxdRaw(p->second);
		if (!par) continue;
		FORLIST(lnk, par->textures) {
			rw::Texture *src = rw::Texture::fromDict(lnk);
			if (own->find(src->name)) continue;
			rw::Texture *c = rw::Texture::create(src->raster);
			strncpy(c->name, src->name, 32);
			strncpy(c->mask, src->mask, 32);
			c->filterAddressing = src->filterAddressing;
			own->add(c);
		}
	}
	// Many SA TXDs store NEAREST (0x1101); the game does not show them blocky, so force bilinear (+mips where present).
	FORLIST(lnk, own->textures) {
		rw::Texture *t = rw::Texture::fromDict(lnk);
		gFilterHist[t->getFilter()]++;
		gAddrUHist[t->getAddressU()]++;
		t->setFilter(rw::Texture::LINEARMIPLINEAR);
	}
	return own;
}

rw::Clump *LoadClump(const std::string &dffName, const std::string &txdName) {
	const sa::ImgEntry *e = gImg.Find(dffName + ".dff");
	if (!e) return nil;
	std::vector<uint8_t> buf;
	if (!gImg.Read(*e, buf)) return nil;
	rw::TexDictionary::setCurrent(GetTxd(txdName));
	rw::StreamMemory s;
	s.open(buf.data(), (rw::uint32)buf.size());
	rw::Clump *c = nil;
	if (rw::findChunk(&s, rw::ID_CLUMP, nil, nil)) c = rw::Clump::streamRead(&s);
	s.close();
	return c;
}

// The first instance of a model is the prototype and is added to the world explicitly; Clump::clone() adds the
// clone to the prototype's world by itself, so adding it again would put it in the list twice and corrupt it.
void PlaceInstance(rw::Clump *proto, const Placement &p, bool first) {
	rw::Clump *c = first ? proto : proto->clone();
	rw::Frame *f = c->getFrame();
	f->rotate(&p.rot, rw::COMBINEREPLACE);
	rw::V3d pos = p.pos;
	f->translate(&pos, rw::COMBINEPOSTCONCAT);
	if (first) gWorld->addClump(c);
}

void CollectPlacements(std::vector<Placement> &out, int &skippedLod, int &skippedNoDef, int &skippedNight, int &skippedInterior) {
	auto consider = [&](int id, const std::string &nameHint, float x, float y, float z, const float q[4], uint32_t interiorByte) {
		float dx = x - centerX, dy = y - centerY;
		if (dx * dx + dy * dy > radius * radius) return;
		if (interiorByte) { skippedInterior++; return; }
		const sa::ObjectDef *d = gIde.Find(id);
		if (!d) { skippedNoDef++; return; }
		if (IsLodName(d->model)) { skippedLod++; return; }
		if (d->timed && !TimedVisibleAtNoon(d->timeOn, d->timeOff)) { skippedNight++; return; }
		Placement p;
		p.id = id;
		p.pos = {x, y, z};
		// IPL quaternions are stored conjugated (xyz negated), see CFileLoader::LoadObjectInstance (0x538090).
		p.rot = {-q[0], -q[1], -q[2], q[3]};
		p.lodDist = d->drawDist[0];
		out.push_back(p);
		(void)nameHint;
	};

	// text IPLs on disk
	std::vector<sa::DatDirective> dat;
	sa::LoadGtaDat(gameDir + "/data/gta.dat", dat);
	for (auto &d : dat) {
		if (d.kind != "IPL") continue;
		sa::IplText ipl;
		bool bin = false;
		if (!sa::LoadIplText(Join(d.arg), ipl, bin) || bin) continue;
		for (auto &i : ipl.insts) consider(i.id, i.model, i.pos[0], i.pos[1], i.pos[2], i.rot, (uint32_t)i.interior);
	}
	// binary streaming IPLs inside gta3.img
	for (auto &e : gImg.Entries()) {
		auto dot = e.name.rfind('.');
		if (dot == std::string::npos || Lower(e.name.substr(dot)) != ".ipl") continue;
		std::vector<uint8_t> buf;
		sa::BinaryIpl b;
		if (!gImg.Read(e, buf) || !sa::ParseBinaryIpl(buf.data(), buf.size(), b, nullptr)) continue;
		for (auto &i : b.insts) consider(i.modelId, "", i.pos[0], i.pos[1], i.pos[2], i.rot, i.flags & 0xFF);
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
	gCamera->setFarPlane(700.0f);
	gCamera->setFOV(70.0f, (float)sk::globals.width / sk::globals.height);
	gWorld->addCamera(gCamera);

	rw::Light *amb = rw::Light::create(rw::Light::AMBIENT);
	amb->setColor(0.65f, 0.65f, 0.7f);
	gWorld->addLight(amb);
	rw::Light *sun = rw::Light::create(rw::Light::DIRECTIONAL);
	sun->setColor(0.6f, 0.6f, 0.55f);
	rw::Frame *sf = rw::Frame::create();
	rw::V3d xa = {1, 0, 0}, ya = {0, 1, 0};
	sf->rotate(&xa, 50.0f, rw::COMBINEREPLACE);
	sf->rotate(&ya, 30.0f, rw::COMBINEPOSTCONCAT);
	sun->setFrame(sf);
	gWorld->addLight(sun);

	// Data
	std::vector<sa::DatDirective> dat;
	sa::LoadGtaDat(gameDir + "/data/default.dat", dat);
	sa::LoadGtaDat(gameDir + "/data/gta.dat", dat);
	for (auto &d : dat)
		if (d.kind == "IDE") gIde.LoadFile(Join(d.arg));
	gTxdParent = gIde.TxdParents();
	if (!gImg.Open(gameDir + "/models/gta3.img")) { fprintf(gLog, "cannot open gta3.img\n"); return false; }

	std::vector<Placement> places;
	int skLod = 0, skNoDef = 0, skNight = 0, skInt = 0;
	CollectPlacements(places, skLod, skNoDef, skNight, skInt);

	std::map<int, rw::Clump *> proto;
	std::map<int, bool> failed;
	int placed = 0, noModel = 0;
	for (auto &p : places) {
		if (failed[p.id]) { noModel++; continue; }
		auto it = proto.find(p.id);
		const sa::ObjectDef *d = gIde.Find(p.id);
		if (it == proto.end()) {
			rw::Clump *c = LoadClump(d->model, d->txd);
			if (!c) { failed[p.id] = true; noModel++; continue; }
			proto[p.id] = c;
			PlaceInstance(c, p, true);
		} else {
			PlaceInstance(it->second, p, false);
		}
		placed++;
	}
	fprintf(gLog, "centre (%.0f, %.0f) radius %.0f: selected=%zu placed=%d unique models=%zu | skipped: lod=%d no-def=%d night=%d interior=%d, dff-missing=%d\n",
	        centerX, centerY, radius, places.size(), placed, proto.size(), skLod, skNoDef, skNight, skInt, noModel);
	fprintf(gLog, "texture filter modes:");
	for (auto &kv : gFilterHist) fprintf(gLog, " %d=%d", kv.first, kv.second);
	fprintf(gLog, " | address U:");
	for (auto &kv : gAddrUHist) fprintf(gLog, " %d=%d", kv.first, kv.second);
	fprintf(gLog, "\n");
	fflush(gLog);

	camPos = {centerX - 30.0f, centerY - 30.0f, 40.0f};
	camYaw = 45.0f;
	UpdateCamera();
	return true;
}

void Terminate3D() {
	if (gCamera) { gWorld->removeCamera(gCamera); sk::CameraDestroy(gCamera); gCamera = nil; }
	// Clumps/textures are left to process exit; this is a viewer.
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
	float speed = (sk::keys[sk::KEY_LSHIFT] ? 200.0f : 40.0f) * dt;
	float yr = camYaw * 3.14159265f / 180.0f;
	rw::V3d fwd = {std::sin(yr), std::cos(yr), 0.0f}, right = {std::cos(yr), -std::sin(yr), 0.0f};
	if (sk::keys['W']) { camPos.x += fwd.x * speed; camPos.y += fwd.y * speed; }
	if (sk::keys['S']) { camPos.x -= fwd.x * speed; camPos.y -= fwd.y * speed; }
	if (sk::keys['D']) { camPos.x += right.x * speed; camPos.y += right.y * speed; }
	if (sk::keys['A']) { camPos.x -= right.x * speed; camPos.y -= right.y * speed; }
	if (sk::keys['E']) camPos.z += speed;
	if (sk::keys['Q']) camPos.z -= speed;
	UpdateCamera();
	gCamera->clear(&Background, rw::Camera::CLEARIMAGE | rw::Camera::CLEARZ);
	gCamera->beginUpdate();
	// Cut out transparent texels (leaves, fences) instead of blending them, so they do not hide geometry
	// drawn later through the depth buffer.
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
		sk::globals.windowtitle = "sa_world";
		sk::globals.width = 1280;
		sk::globals.height = 720;
		sk::globals.quit = 0;
		if (sk::args.argc > 1) gameDir = sk::args.argv[1];
		if (sk::args.argc > 3) { centerX = (float)atof(sk::args.argv[2]); centerY = (float)atof(sk::args.argv[3]); }
		if (sk::args.argc > 4) radius = (float)atof(sk::args.argv[4]);
		gLog = fopen("sa_world.log", "w");
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
			gCamera->setFOV(70.0f, (float)r->w / r->h);
		}
		break;
	}
	case IDLE:
		Idle(*(float *)param);
		return EVENTPROCESSED;
	}
	return sk::EVENTNOTPROCESSED;
}
