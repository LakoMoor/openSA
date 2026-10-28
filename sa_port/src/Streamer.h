// Synchronous request/load/remove layer over the streaming slots. Reads raw resource bytes from the archives;
// turning them into RenderWare objects is left to the caller (a "loaded" callback).
// Modelled on CStreaming::RequestModel (0x4087E0) / LoadAllRequestedModels (0x40EA10): requesting a model also
// requests its TXD and the TXD parents. Not modelled yet: the loading channels, priority list, memory budget eviction.
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_map>
#include <vector>
#include "StreamingWorld.h"

namespace sa {

class Streamer {
public:
	using LoadedFn = std::function<void(int id, const std::vector<uint8_t> &bytes)>;

	explicit Streamer(StreamingWorld &w) : m_w(w) {}
	void SetLoadedCallback(LoadedFn fn) { m_onLoaded = std::move(fn); }

	// Marks `id` (and, for models, its TXD chain) as requested. Returns false if the id has no archive entry.
	bool Request(int id, uint8_t flags = 0);
	// Loads up to `maxLoads` queued resources in request order; returns the number loaded.
	int Process(int maxLoads = 1 << 30);
	// Drops the cached bytes of `id` unless it is flagged to stay in memory.
	bool Remove(int id);

	bool IsLoaded(int id) const { const StreamingInfo *s = m_w.store.At(id); return s && s->IsLoaded(); }
	const std::vector<uint8_t> *Data(int id) const;
	size_t LoadedBytes() const { return m_bytes; }
	size_t LoadedCount() const { return m_data.size(); }
	size_t PendingCount() const { return m_queue.size(); }

private:
	bool RequestOne(int id, uint8_t flags);

	StreamingWorld &m_w;
	LoadedFn m_onLoaded;
	std::deque<int> m_queue;
	std::unordered_map<int, std::vector<uint8_t>> m_data;
	size_t m_bytes = 0;
};

} // namespace sa
