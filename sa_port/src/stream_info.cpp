// Builds the streaming world (slot assignment emulating CStreaming::LoadCdDirectory, see StreamingWorld.cpp),
// then requests a few resources through the Streamer and checks the loaded bytes.
// usage: stream_info <SA game dir>
#include "Streamer.h"
#include <cstdio>
#include <cstring>

static uint32_t Chunk(const std::vector<uint8_t> &b) {
	uint32_t t = 0;
	if (b.size() >= 4) memcpy(&t, b.data(), 4);
	return t;
}

int main(int argc, char **argv) {
	if (argc < 2) { puts("usage: stream_info <SA game dir>"); return 2; }
	sa::StreamingWorld w;
	std::string err;
	if (!w.Build(argv[1], &err)) { printf("build failed: %s\n", err.c_str()); return 1; }
	printf("slots with archive entry=%d of %d; TXD slots=%zu COL=%zu IPL=%zu IFP=%zu RRR=%zu SCM=%zu\n",
	       w.store.CountWithArchiveEntry(), sa::kStreamingInfoCount, w.txd.slots.size(), w.col.slots.size(),
	       w.ipl.slots.size(), w.ifp.slots.size(), w.rrr.slots.size(), w.scm.slots.size());

	sa::Streamer st(w);
	int loadedDff = 0, loadedTxd = 0, badMagic = 0;
	st.SetLoadedCallback([&](int id, const std::vector<uint8_t> &b) {
		sa::StreamKind k = sa::ClassifyStreamId(id);
		uint32_t t = Chunk(b);
		if (k == sa::StreamKind::Model) { loadedDff++; if (t != 0x10 && t != 0x2B) badMagic++; }
		else if (k == sa::StreamKind::Txd) { loadedTxd++; if (t != 0x16) badMagic++; }
	});

	// 411 = infernus, 496 = blista compact; a few map objects around Grove Street
	int ids[] = {411, 496, 1412, 700, 3425};
	for (int id : ids) {
		bool ok = st.Request(id);
		printf("request %d: %s, txd id=%d\n", id, ok ? "ok" : "no archive entry", w.ModelTxdId(id));
	}
	int n = st.Process();
	printf("loaded %d resources: dff=%d txd=%d, %zu bytes, bad chunk magic=%d, pending=%zu\n", n, loadedDff, loadedTxd,
	       st.LoadedBytes(), badMagic, st.PendingCount());

	// Removing frees the bytes, requesting again reloads them.
	bool removed = st.Remove(411);
	printf("remove 411: %s, now loaded=%d, bytes=%zu\n", removed ? "ok" : "refused", st.IsLoaded(411), st.LoadedBytes());
	st.Request(411, sa::STREAMING_GAME_REQUIRED);
	st.Process();
	printf("re-request 411 (game required): loaded=%d, remove allowed=%d\n", st.IsLoaded(411), st.Remove(411));
	return badMagic == 0 ? 0 : 1;
}
