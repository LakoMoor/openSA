// sa_view: shows one GTA SA model (DFF + TXD) with librw's D3D9 platform.
// usage: sa_view.exe <SA game dir> [model name without extension, default infernus]
// Controls: left mouse drag = orbit, wheel = zoom, Esc = quit.
#include <rw.h>
#include <skeleton.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ImgV2.h"

rw::EngineOpenParams engineOpenParams;

namespace {

rw::World *World;
rw::Camera *gCamera;
rw::Clump *Model;
rw::TexDictionary *Txd;

rw::V3d Xaxis = {1, 0, 0}, Yaxis = {0, 1, 0}, Zaxis = {0, 0, 1};
rw::RGBA Background = {70, 80, 95, 255};

float yaw = 30.0f, pitch = 15.0f, dist = 8.0f;
bool autoSpin = true;
std::string gameDir, modelName = "infernus", wheelName = "wheel_sport_l1";
float wheelScale = 0.7f;

bool ReadFile(const std::string &path, std::vector<uint8_t> &out) {
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	out.resize((size_t)n);
	bool ok = fread(out.data(), 1, out.size(), f) == out.size();
	fclose(f);
	return ok;
}

rw::TexDictionary *ReadTxd(std::vector<uint8_t> &buf) {
	rw::StreamMemory s;
	s.open(buf.data(), (rw::uint32)buf.size());
	rw::TexDictionary *txd = nil;
	if (rw::findChunk(&s, rw::ID_TEXDICTIONARY, nil, nil)) txd = rw::TexDictionary::streamRead(&s);
	s.close();
	return txd;
}

// Moves all textures of src into dst so one dictionary can resolve every material.
void MergeInto(rw::TexDictionary *dst, rw::TexDictionary *src) {
	std::vector<rw::Texture *> list;
	FORLIST(lnk, src->textures) list.push_back(rw::Texture::fromDict(lnk));
	for (rw::Texture *t : list) {
		src->remove(t);
		dst->add(t);
	}
}

// SA stores each frame's name in a 0x253F2FE extension chunk; librw has no plugin for it.
struct NodeName { char name[24]; };
int nodeNameOffset = -1;
constexpr rw::uint32 ID_NODENAME = 0x253F2FE;

void *CreateNodeName(void *obj, rw::int32 off, rw::int32) { PLUGINOFFSET(NodeName, obj, off)->name[0] = 0; return obj; }
void *DestroyNodeName(void *obj, rw::int32, rw::int32) { return obj; }
void *CopyNodeName(void *dst, void *src, rw::int32 off, rw::int32) {
	*PLUGINOFFSET(NodeName, dst, off) = *PLUGINOFFSET(NodeName, src, off);
	return dst;
}
rw::Stream *ReadNodeName(rw::Stream *s, rw::int32 len, void *obj, rw::int32 off, rw::int32) {
	NodeName *n = PLUGINOFFSET(NodeName, obj, off);
	char tmp[256] = {};
	s->read8(tmp, len < 255 ? len : 255);
	if (len > 255) s->seek(len - 255);
	strncpy(n->name, tmp, 23);
	n->name[23] = 0;
	return s;
}
rw::Stream *WriteNodeName(rw::Stream *s, rw::int32, void *, rw::int32, rw::int32) { return s; }
rw::int32 SizeNodeName(void *, rw::int32, rw::int32) { return 0; }

const char *FrameName(rw::Frame *f) { return PLUGINOFFSET(NodeName, f, nodeNameOffset)->name; }

rw::Frame *FindFrame(rw::Frame *f, const char *name) {
	if (!f) return nil;
	if (!_stricmp(FrameName(f), name)) return f;
	for (rw::Frame *c = f->child; c; c = c->next)
		if (rw::Frame *r = FindFrame(c, name)) return r;
	return nil;
}

void DumpFrames(rw::Frame *f, int depth) {
	printf("%*s%s\n", depth * 2, "", FrameName(f));
	for (rw::Frame *c = f->child; c; c = c->next) DumpFrames(c, depth + 1);
}

// SA marks paintable materials with fixed colours; carcols.dat supplies the real ones.
const rw::RGBA kMarkers[4] = {{60, 255, 0, 255}, {255, 0, 175, 255}, {0, 255, 255, 255}, {255, 255, 0, 255}};

// Returns the palette (carcols.dat "col" section) and the first colour pair of `model`.
bool ReadCarcols(const std::string &model, std::vector<rw::RGBA> &pal, int pair[4]) {
	std::vector<uint8_t> buf;
	if (!ReadFile(gameDir + "/data/carcols.dat", buf)) return false;
	std::string text(buf.begin(), buf.end());
	std::string section;
	size_t pos = 0;
	bool found = false;
	while (pos < text.size()) {
		size_t e = text.find('\n', pos);
		if (e == std::string::npos) e = text.size();
		std::string line = text.substr(pos, e - pos);
		pos = e + 1;
		size_t h = line.find('#');
		if (h != std::string::npos) line.resize(h);
		for (char &c : line) if (c == ',' || c == '\r' || c == '\t') c = ' ';
		char a[64];
		int n[16];
		if (sscanf(line.c_str(), "%63s", a) != 1) continue;
		if (!_stricmp(a, "col") || !_stricmp(a, "car") || !_stricmp(a, "car4")) { section = a; continue; }
		if (!_stricmp(a, "end")) { section.clear(); continue; }
		if (section == "col") {
			int r, g, b;
			if (sscanf(line.c_str(), "%d %d %d", &r, &g, &b) == 3) pal.push_back({(rw::uint8)r, (rw::uint8)g, (rw::uint8)b, 255});
		} else if ((section == "car" || section == "car4") && !_stricmp(a, model.c_str())) {
			int cnt = sscanf(line.c_str() + strlen(a) + line.find(a), "%d %d %d %d", &n[0], &n[1], &n[2], &n[3]);
			if (cnt >= 2) {
				for (int i = 0; i < 4; i++) pair[i] = i < cnt ? n[i] : 0;
				found = true;
			}
		}
	}
	return found;
}

void PaintClump(rw::Clump *c, const rw::RGBA *cols /* 4 */) {
	FORLIST(lnk, c->atomics) {
		rw::Atomic *a = rw::Atomic::fromClump(lnk);
		if (!a->geometry) continue;
		for (int i = 0; i < a->geometry->matList.numMaterials; i++) {
			rw::Material *m = a->geometry->matList.materials[i];
			if (!getenv("SA_FX")) rw::MatFX::setEffects(m, rw::MatFX::NOTHING);
			if (getenv("SA_DUMP"))
				printf("  [%s] mat %d col=%d,%d,%d tex=%s\n", FrameName(a->getFrame()), i, m->color.red, m->color.green, m->color.blue,
				       m->texture ? m->texture->name : "-");
			for (int k = 0; k < 4; k++)
				if (m->color.red == kMarkers[k].red && m->color.green == kMarkers[k].green && m->color.blue == kMarkers[k].blue) {
					m->color = cols[k];
					m->color.alpha = 255;
				}
		}
	}
}

void AttachWheels(sa::ImgV2 &) {
	std::vector<uint8_t> buf;
	if (!ReadFile(gameDir + "/models/generic/wheels.dff", buf)) { printf("wheels.dff missing\n"); return; }
	rw::StreamMemory s;
	s.open(buf.data(), (rw::uint32)buf.size());
	rw::Clump *wc = nil;
	if (rw::findChunk(&s, rw::ID_CLUMP, nil, nil)) wc = rw::Clump::streamRead(&s);
	s.close();
	if (!wc) { printf("wheels.dff load failed\n"); return; }
	if (getenv("SA_DUMP")) DumpFrames(wc->getFrame(), 0);

	rw::Atomic *proto = nil;
	FORLIST(lnk, wc->atomics) {
		rw::Atomic *a = rw::Atomic::fromClump(lnk);
		if (!_stricmp(FrameName(a->getFrame()), wheelName.c_str())) proto = a;
	}
	if (!proto) { printf("wheel %s not found\n", wheelName.c_str()); return; }

	// The car carries a single placeholder "wheel" atomic under wheel_rf_dummy; the game swaps it for real wheels.
	FORLIST(lnk, Model->atomics) {
		rw::Atomic *a = rw::Atomic::fromClump(lnk);
		const char *n = FrameName(a->getFrame());
		size_t nl = strlen(n);
		// placeholder wheel, damaged-part variants and the very-low-detail body are not drawn in the intact, close-up state
		if (!_stricmp(n, "wheel") || (nl > 4 && (!_stricmp(n + nl - 4, "_dam") || !_stricmp(n + nl - 4, "_vlo"))))
			a->object.object.flags &= ~rw::Atomic::RENDER;
	}
	FORLIST(lnk, Model->atomics) {
		rw::Atomic *a = rw::Atomic::fromClump(lnk);
		printf("car atomic '%s' verts=%d flags=%x\n", FrameName(a->getFrame()), a->geometry ? a->geometry->numVertices : 0, a->object.object.flags);
	}
	printf("wheel proto: %d verts, %d tris\n", proto->geometry->numVertices, proto->geometry->numTriangles);
	{
		rw::Geometry *g = proto->geometry;
		for (int i = 0; i < g->matList.numMaterials; i++) {
			rw::Material *m = g->matList.materials[i];
			printf("  mat %d tex=%s col=%d,%d,%d,%d\n", i, m->texture ? m->texture->name : "(none)", m->color.red, m->color.green,
			       m->color.blue, m->color.alpha);
		}
		rw::BBox bb;
		float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
		for (int i = 0; i < g->numVertices; i++) {
			float *v = &g->morphTargets[0].vertices[i].x;
			for (int k = 0; k < 3; k++) { if (v[k] < mn[k]) mn[k] = v[k]; if (v[k] > mx[k]) mx[k] = v[k]; }
		}
		printf("  bbox %.2f %.2f %.2f .. %.2f %.2f %.2f\n", mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
		rw::Frame *pf = proto->getFrame();
		printf("  frame pos %.2f %.2f %.2f right %.2f %.2f %.2f up %.2f %.2f %.2f\n", pf->matrix.pos.x, pf->matrix.pos.y, pf->matrix.pos.z,
		       pf->matrix.right.x, pf->matrix.right.y, pf->matrix.right.z, pf->matrix.up.x, pf->matrix.up.y, pf->matrix.up.z);
	}

	struct { const char *dummy; bool right; } slots[4] = {
		{"wheel_lf_dummy", false}, {"wheel_rf_dummy", true}, {"wheel_lb_dummy", false}, {"wheel_rb_dummy", true}};
	for (auto &sl : slots) {
		rw::Frame *d = FindFrame(Model->getFrame(), sl.dummy);
		if (!d) { printf("no frame %s\n", sl.dummy); continue; }
		rw::Atomic *a = proto->clone();
		rw::Frame *f = rw::Frame::create();
		rw::V3d sc = {wheelScale, wheelScale, wheelScale};
		f->scale(&sc, rw::COMBINEREPLACE);
		if (sl.right) f->rotate(&Zaxis, 180.0f, rw::COMBINEPOSTCONCAT);
		d->addChild(f);
		a->setFrame(f);
		Model->addAtomic(a);
	}
}

bool LoadModel() {
	sa::ImgV2 img;
	if (!img.Open(gameDir + "/models/gta3.img")) { fprintf(stderr, "cannot open gta3.img\n"); return false; }
	const sa::ImgEntry *dff = img.Find(modelName + ".dff");
	const sa::ImgEntry *own = img.Find(modelName + ".txd");
	if (!dff) { fprintf(stderr, "%s.dff not found\n", modelName.c_str()); return false; }

	Txd = rw::TexDictionary::create();
	std::vector<uint8_t> buf;
	if (ReadFile(gameDir + "/models/generic/vehicle.txd", buf))
		if (rw::TexDictionary *g = ReadTxd(buf)) { MergeInto(Txd, g); g->destroy(); }
	if (ReadFile(gameDir + "/models/generic/wheels.txd", buf))
		if (rw::TexDictionary *g = ReadTxd(buf)) { MergeInto(Txd, g); g->destroy(); }
	if (own && img.Read(*own, buf))
		if (rw::TexDictionary *t = ReadTxd(buf)) { MergeInto(Txd, t); t->destroy(); }
	printf("textures in dictionary: %d\n", Txd->count());
	rw::TexDictionary::setCurrent(Txd);

	if (!img.Read(*dff, buf)) return false;
	rw::StreamMemory s;
	s.open(buf.data(), (rw::uint32)buf.size());
	if (!rw::findChunk(&s, rw::ID_CLUMP, nil, nil)) { s.close(); return false; }
	Model = rw::Clump::streamRead(&s);
	s.close();
	if (!Model) return false;

	// SA is Z-up, RenderWare's camera is Y-up: stand the model up.
	if (getenv("SA_DUMP")) DumpFrames(Model->getFrame(), 0);
	AttachWheels(img);
	{
		std::vector<rw::RGBA> pal;
		int pr[4] = {0, 0, 0, 0};
		if (ReadCarcols(modelName, pal, pr)) {
			rw::RGBA c[4];
			for (int i = 0; i < 4; i++) c[i] = pr[i] < (int)pal.size() ? pal[pr[i]] : rw::RGBA{128, 128, 128, 255};
			printf("carcols: palette=%d colours, pair %d,%d -> (%d,%d,%d)/(%d,%d,%d)\n", (int)pal.size(), pr[0], pr[1],
			       c[0].red, c[0].green, c[0].blue, c[1].red, c[1].green, c[1].blue);
			PaintClump(Model, c);
		} else printf("no carcols entry for %s\n", modelName.c_str());
	}
	Model->getFrame()->rotate(&Xaxis, -90.0f, rw::COMBINEREPLACE);
	World->addClump(Model);
	return true;
}

void UpdateCamera() {
	rw::Frame *f = gCamera->getFrame();
	f->rotate(&Xaxis, pitch, rw::COMBINEREPLACE);
	f->rotate(&Yaxis, yaw, rw::COMBINEPOSTCONCAT);
	rw::V3d target = {0.0f, 0.4f, 0.0f};
	rw::V3d p = {target.x - f->matrix.at.x * dist, target.y - f->matrix.at.y * dist, target.z - f->matrix.at.z * dist};
	f->translate(&p, rw::COMBINEPOSTCONCAT);
}

bool Initialize3D() {
	if (!sk::InitRW()) return false;
	rw::BBox bb;
	bb.inf.x = bb.inf.y = bb.inf.z = -500.0f;
	bb.sup.x = bb.sup.y = bb.sup.z = 500.0f;
	World = rw::World::create(&bb);

	gCamera = sk::CameraCreate(sk::globals.width, sk::globals.height, 1);
	gCamera->setNearPlane(0.1f);
	gCamera->setFarPlane(300.0f);
	gCamera->setFOV(60.0f, (float)sk::globals.width / sk::globals.height);
	World->addCamera(gCamera);

	rw::Light *amb = rw::Light::create(rw::Light::AMBIENT);
	amb->setColor(0.3f, 0.3f, 0.3f);
	World->addLight(amb);
	rw::Light *sun = rw::Light::create(rw::Light::DIRECTIONAL);
	sun->setColor(0.6f, 0.6f, 0.6f);
	rw::Frame *sf = rw::Frame::create();
	sf->rotate(&Xaxis, 40.0f, rw::COMBINEREPLACE);
	sf->rotate(&Yaxis, 35.0f, rw::COMBINEPOSTCONCAT);
	sun->setFrame(sf);
	World->addLight(sun);

	if (!LoadModel()) return false;
	UpdateCamera();
	return true;
}

void Terminate3D() {
	if (Model) { World->removeClump(Model); Model->destroy(); Model = nil; }
	if (gCamera) { World->removeCamera(gCamera); sk::CameraDestroy(gCamera); gCamera = nil; }
	if (World) { World->destroy(); World = nil; }
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
	nodeNameOffset = rw::Frame::registerPlugin(sizeof(NodeName), ID_NODENAME, CreateNodeName, DestroyNodeName, CopyNodeName);
	rw::Frame::registerPluginStream(ID_NODENAME, ReadNodeName, WriteNodeName, SizeNodeName);
	rw::registerMatFXPlugin();
	rw::registerUVAnimPlugin();
	return true;
}

void Idle(float dt) {
	if (autoSpin) yaw += 20.0f * dt;
	UpdateCamera();
	gCamera->clear(&Background, rw::Camera::CLEARIMAGE | rw::Camera::CLEARZ);
	gCamera->beginUpdate();
	World->render();
	gCamera->endUpdate();
	gCamera->showRaster(0);
}

} // namespace

sk::EventStatus AppEventHandler(sk::Event e, void *param) {
	using namespace sk;
	switch (e) {
	case INITIALIZE:
		sk::globals.windowtitle = "sa_view";
		sk::globals.width = 1280;
		sk::globals.height = 720;
		sk::globals.quit = 0;
		if (sk::args.argc > 1) gameDir = sk::args.argv[1];
		if (sk::args.argc > 2) modelName = sk::args.argv[2];
			if (const char *e = getenv("SA_WHEEL")) wheelName = e;
			if (const char *e = getenv("SA_WSCALE")) wheelScale = (float)atof(e);
			freopen("sa_view.log", "w", stdout);
			setvbuf(stdout, nullptr, _IONBF, 0);
			if (const char *e = getenv("SA_YAW")) { yaw = (float)atof(e); autoSpin = false; }
			if (const char *e = getenv("SA_PITCH")) pitch = (float)atof(e);
		if (gameDir.empty()) { fprintf(stderr, "usage: sa_view <SA game dir> [model]\n"); return EVENTERROR; }
		return EVENTPROCESSED;
	case RWINITIALIZE: return Initialize3D() ? EVENTPROCESSED : EVENTERROR;
	case RWTERMINATE: Terminate3D(); return EVENTPROCESSED;
	case PLUGINATTACH: return AttachPlugins() ? EVENTPROCESSED : EVENTERROR;
	case KEYDOWN:
		if (*(int *)param == KEY_ESC) sk::globals.quit = 1;
		return EVENTPROCESSED;
	case MOUSEMOVE: {
		MouseState *m = (MouseState *)param;
		if (m->buttons & 1) {
			autoSpin = false;
			yaw += (m->posx - sk::prevmouse.posx) * 0.4f;
			pitch += (m->posy - sk::prevmouse.posy) * 0.4f;
			if (pitch > 85.0f) pitch = 85.0f;
			if (pitch < -85.0f) pitch = -85.0f;
		}
		return EVENTPROCESSED;
	}
	case MOUSEWHEEL:
		dist *= (*(float *)param > 0) ? 0.9f : 1.1f;
		return EVENTPROCESSED;
	case RESIZE: {
		Rect *r = (Rect *)param;
		if (r->w == 0) r->w = 1;
		if (r->h == 0) r->h = 1;
		sk::globals.width = r->w;
		sk::globals.height = r->h;
		if (gCamera) {
			sk::CameraSize(gCamera, r);
			gCamera->setFOV(60.0f, (float)r->w / r->h);
		}
		break;
	}
	case IDLE:
		Idle(*(float *)param);
		return EVENTPROCESSED;
	}
	return sk::EVENTNOTPROCESSED;
}
