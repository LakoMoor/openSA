// Streaming id space of GTA SA 1.0 US. Bases verified in CStreaming::LoadCdDirectory (0x5B6170):
// TXD +20000, COL +25000, IPL +0x62A7, DAT +0x63A7, IFP +0x63E7, RRR +0x649B, SCM +0x6676. The sizes of the ranges
// match the real data (251 COL, 190 IPL, 64 DAT, 153 IFP, 426 RRR, 79 SCM). The slot array initialised in
// CStreaming::Init2 (0x5B8AD0) has 26230 entries, so SCM ids (>= 26230) must be backed by storage elsewhere.
#pragma once
#include "Pools.h"

namespace sa {

enum StreamBase : int {
	kDffBase = 0,       // model ids 0..19999 (objects, vehicles, peds, weapons)
	kTxdBase = 20000,
	kColBase = 25000,
	kIplBase = 25255,
	kDatBase = 25511,   // path node files
	kIfpBase = 25575,   // animation blocks
	kRrrBase = 25755,   // recorded car paths
	kScmBase = 26230,   // mission scripts (first id past the info array)
};

static_assert(kScmBase == kStreamingInfoCount, "SCM ids start right after the slots initialised by Init2");

enum class StreamKind { Model, Txd, Col, Ipl, Dat, Ifp, Rrr, Scm, Invalid };

inline StreamKind ClassifyStreamId(int id) {
	if (id < 0) return StreamKind::Invalid;
	if (id < kTxdBase) return StreamKind::Model;
	if (id < kColBase) return StreamKind::Txd;
	if (id < kIplBase) return StreamKind::Col;
	if (id < kDatBase) return StreamKind::Ipl;
	if (id < kIfpBase) return StreamKind::Dat;
	if (id < kRrrBase) return StreamKind::Ifp;
	if (id < kScmBase) return StreamKind::Rrr;
	return StreamKind::Invalid; // ids >= kScmBase are not streaming slots
}

} // namespace sa
