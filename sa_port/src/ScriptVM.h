#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

// SCM interpreter for main.scm (CRunningScript equivalent). Argument counts come from ScmTable.inc.
// Commands without a handler consume their arguments and are counted in unhandled().

struct ScmArg {
	uint8_t type = 0;      // raw SCM type byte
	int32_t value = 0;     // immediate value, or the byte address inside the variable space
	bool isVar = false;
	std::string str;       // string immediates
};

struct ScmThread {
	uint32_t ip = 0;
	uint32_t wakeMs = 0;
	bool active = true;
	bool condFlag = false;
	int andorCount = 0;     // remaining conditions in the current IF
	int andorMode = 0;      // 0 single, 1 AND, 2 OR
	bool notFlag = false;
	std::vector<uint32_t> stack;
	int32_t locals[34] = {};
	std::string name;
	int32_t swValue = 0, swTotal = 0, swDefault = 0;   // SWITCH_START / SWITCH_CONTINUED state
	std::vector<std::pair<int32_t, int32_t>> swCases;
	bool isMission = false;
	uint32_t base = 0;      // mission scripts: negative labels are relative to this offset
};

class ScriptVM {
public:
	using Handler = std::function<bool(ScriptVM &, ScmThread &, std::vector<ScmArg> &)>;
	void setHandler(uint16_t op, Handler h) { m_handlers[op] = std::move(h); }
	ScriptVM();
	// argument access for handlers
	int32_t getI(ScmThread &t, const ScmArg &a);
	float getF(ScmThread &t, const ScmArg &a);
	void setI(ScmThread &t, const ScmArg &a, int32_t v);
	void setF(ScmThread &t, const ScmArg &a, float v);
	void setCond(ScmThread &t, bool v) { condResult(t, v, t.notFlag); }
	bool load(const std::string &path);
	void tick(uint32_t deltaMs);   // advance the clock and run due threads
	static const char *opName(uint16_t op);
	uint32_t nowMs() const { return m_now; }
	size_t threadCount() const { return m_threads.size(); }

	// zone_key (8 chars upper-case) -> population type, filled by SET_ZONE_POPULATION_TYPE
	std::map<std::string, int> zonePopType;
	std::map<uint16_t, uint32_t> unhandled;   // opcode -> times executed without a handler
	std::map<uint16_t, uint32_t> handled;
	uint64_t commandsRun = 0;
	std::vector<uint32_t> spawned;   // start offsets of threads created by START_NEW_SCRIPT

private:
	std::vector<uint8_t> m_mem;
	std::deque<ScmThread> m_threads;
	uint32_t m_now = 0;
	float m_timeStep = 1.0f;

	void runThread(ScmThread &t);
	bool readArgs(ScmThread &t, int fixed, bool vararg, std::vector<ScmArg> &out);
	bool readOne(ScmThread &t, ScmArg &a);
	int32_t &ref(ScmThread &t, const ScmArg &a);
	bool execute(ScmThread &t, uint16_t op, bool neg, std::vector<ScmArg> &args);
	void condResult(ScmThread &t, bool v, bool neg);
	uint32_t label(const ScmThread &t, int32_t v) const;
	uint32_t m_rng = 12345;
	std::map<uint16_t, Handler> m_handlers;
};
