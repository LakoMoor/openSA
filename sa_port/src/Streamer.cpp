#include "Streamer.h"

namespace sa {

bool Streamer::RequestOne(int id, uint8_t flags) {
	StreamingInfo *si = m_w.store.At(id);
	if (!si || !si->HasCdPosAndSize()) return false;
	si->flags |= flags;
	if (si->loadState == LoadState::NotLoaded) {
		si->loadState = LoadState::Requested;
		m_queue.push_back(id);
	}
	return true;
}

bool Streamer::Request(int id, uint8_t flags) {
	if (!RequestOne(id, flags)) return false;
	if (ClassifyStreamId(id) == StreamKind::Model) {
		// the TXD of the model and its parents must be available before the model can be set up
		for (int t = m_w.ModelTxdId(id); t >= 0; t = m_w.TxdParentId(t)) RequestOne(t, flags);
	}
	return true;
}

int Streamer::Process(int maxLoads) {
	int n = 0;
	while (n < maxLoads && !m_queue.empty()) {
		int id = m_queue.front();
		m_queue.pop_front();
		StreamingInfo *si = m_w.store.At(id);
		if (!si || si->loadState != LoadState::Requested) continue;
		si->loadState = LoadState::Reading;
		ImgEntry e{si->cdOffset, (uint16_t)si->cdSize, 0, ""};
		std::vector<uint8_t> buf;
		if (si->imgId >= m_w.archives.size() || !m_w.archives[si->imgId].Read(e, buf)) {
			si->loadState = LoadState::NotLoaded;
			continue;
		}
		m_bytes += buf.size();
		si->loadState = LoadState::Loaded;
		auto &slot = m_data[id] = std::move(buf);
		if (m_onLoaded) m_onLoaded(id, slot);
		n++;
	}
	return n;
}

bool Streamer::Remove(int id) {
	StreamingInfo *si = m_w.store.At(id);
	auto it = m_data.find(id);
	if (!si || it == m_data.end()) return false;
	if (si->flags & (STREAMING_GAME_REQUIRED | STREAMING_MISSION_REQUIRED | STREAMING_KEEP_IN_MEMORY)) return false;
	m_bytes -= it->second.size();
	m_data.erase(it);
	si->loadState = LoadState::NotLoaded;
	return true;
}

const std::vector<uint8_t> *Streamer::Data(int id) const {
	auto it = m_data.find(id);
	return it == m_data.end() ? nullptr : &it->second;
}

} // namespace sa
