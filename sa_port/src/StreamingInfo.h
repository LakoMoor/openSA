// CStreamingInfo (20 bytes, stride verified in CStreaming::Init2 0x5B8AD0) and the slot array.
// Field order follows the SA layout; flag/load-state values come from the gta-reversed reconstruction.
#pragma once
#include <cstdint>
#include <vector>
#include "Pools.h"
#include "StreamingIds.h"

namespace sa {

enum StreamingFlags : uint8_t {
	STREAMING_GAME_REQUIRED = 0x2,
	STREAMING_MISSION_REQUIRED = 0x4,
	STREAMING_KEEP_IN_MEMORY = 0x8,
	STREAMING_PRIORITY_REQUEST = 0x10,
	STREAMING_LOADING_SCENE = 0x20,
};

enum class LoadState : uint8_t { NotLoaded = 0, Loaded = 1, Requested = 2, Reading = 3, Finishing = 4 };

struct StreamingInfo {
	int16_t next = -1, prev = -1;      // links of the loaded / requested lists
	int16_t nextOnCd = -1;             // id following this resource inside its archive
	uint8_t flags = 0;
	uint8_t imgId = 0;                 // index into the list of opened archives
	uint32_t cdOffset = 0;             // sectors
	uint32_t cdSize = 0;               // sectors; 0 = no archive entry
	LoadState loadState = LoadState::NotLoaded;

	bool HasCdPosAndSize() const { return cdSize != 0; }
	bool IsLoaded() const { return loadState == LoadState::Loaded; }
};
static_assert(sizeof(StreamingInfo) == kStreamingInfoEntrySize, "must match the 20-byte SA entry");

class StreamingStore {
public:
	StreamingStore() : m_info(kStreamingInfoCount) {}
	StreamingInfo *At(int id) { return id >= 0 && id < kStreamingInfoCount ? &m_info[id] : nullptr; }
	const StreamingInfo *At(int id) const { return id >= 0 && id < kStreamingInfoCount ? &m_info[id] : nullptr; }
	int CountWithArchiveEntry() const {
		int n = 0;
		for (auto &i : m_info) n += i.HasCdPosAndSize();
		return n;
	}
private:
	std::vector<StreamingInfo> m_info;
};

} // namespace sa
