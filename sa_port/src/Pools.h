// Pool capacities and streaming constants of GTA SA 1.0 US, read from gta_sa.exe:
//   CPools::Initialise            0x550F10 (constructor arguments, in call order)
//   CStreaming::Init2             0x5B8AD0 (model info array 0x8E4CC0..0x9654B0, 20-byte entries)
#pragma once

namespace sa {

struct PoolSizes {
	static constexpr int PtrNodeSingle = 70000;
	static constexpr int PtrNodeDouble = 3200;   // 0xC80
	static constexpr int EntryInfoNode = 500;
	static constexpr int Peds = 140;             // 0x8C
	static constexpr int Vehicles = 110;         // 0x6E
	static constexpr int Buildings = 13000;
	static constexpr int Objects = 350;          // 0x15E
	static constexpr int Dummies = 2500;         // 0x9C4
	static constexpr int ColModels = 10150;      // 0x27A6
	static constexpr int Tasks = 500;            // pool at 0x550890; name string not resolved
	static constexpr int Events = 200;
	static constexpr int PointRoutes = 64;       // 0x40
	static constexpr int PatrolRoutes = 32;      // 0x20
	static constexpr int NodeRoutes = 64;
	static constexpr int TaskAllocators = 16;
	static constexpr int PedIntelligences = 140; // 0x8C
	static constexpr int PedAttractors = 64;
};

// (0x9654B0 - 0x8E4CC0) / 20 = 26230 streaming slots, equal to the SCM id base of the IMG id ranges.
constexpr int kStreamingInfoCount = 26230;
constexpr int kStreamingInfoEntrySize = 20;
// DAT_008A5A80 is set to 0x3200000 in Init2 (50 MiB memory budget, exact meaning not yet confirmed).
constexpr unsigned kStreamingMemoryBudget = 0x3200000;

// CTxdStore::Initialise (0x731F20): TexDictionary pool of 5000 slots (12 bytes each); the first 4 slots are
// allocated up front with a placeholder name, so named slots (CTxdStore::AddTxdSlot 0x731C80, called from
// CFileLoader::LoadObject via 0x4C4B40 when FindTxdSlot fails) start at index 4. Streaming id = kTxdBase + slot.
constexpr int kTxdSlotPoolSize = 5000;
constexpr int kReservedTxdSlots = 4;

} // namespace sa
