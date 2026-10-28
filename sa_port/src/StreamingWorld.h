// Result of the startup sequence that fills the streaming slots (see stream_info.cpp history / StreamingWorld.cpp):
// default.dat IDEs, archive directories (CStreaming::LoadCdDirectory emulation), remaining gta.dat IDEs.
#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "ImgV2.h"
#include "StreamingInfo.h"
#include "TextData.h"

namespace sa {

struct SlotTable {
	int first = 0;
	std::unordered_map<std::string, int> slots;
	int FindOrAdd(const std::string &lowerName);
	int Find(const std::string &lowerName) const; // -1 if unknown
};

struct StreamingWorld {
	IdeDb ide;                       // every IDE of default.dat + gta.dat
	StreamingStore store;
	std::vector<ImgV2> archives;     // index = StreamingInfo::imgId
	SlotTable txd, col, ipl, ifp, rrr, scm;
	std::unordered_map<std::string, int> modelByName; // lower-case model name -> id
	std::unordered_map<int, std::string> txdNameBySlot;

	// Streaming id of the TXD a model uses, or -1.
	int ModelTxdId(int modelId) const;
	// Streaming id of the TXD parent of a TXD streaming id, or -1.
	int TxdParentId(int txdStreamId) const;
	bool Build(const std::string &gameDir, std::string *error = nullptr);
};

} // namespace sa
