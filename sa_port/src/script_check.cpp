#include "ScriptVM.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <string>
extern "C" {}
#include <cstring>
extern volatile uint32_t gDbgIp, gDbgOp, gDbgStage;
#include <thread>
#include <chrono>
#include <process.h>
int main(int argc, char **argv)
{
	// watchdog: report where the VM is stuck if no command is executed for 15 s
	std::thread([] {
		uint32_t last = 0; int same = 0;
		for (;;) {
			std::this_thread::sleep_for(std::chrono::seconds(5));
			uint32_t cur = gDbgIp * 31 + gDbgOp;
			if (cur == last) { if (++same >= 3) { std::fprintf(stderr, "WATCHDOG stuck at ip %u op %04X stage %u\n", gDbgIp, gDbgOp, gDbgStage); std::fflush(stderr); _exit(3); } }
			else same = 0;
			last = cur;
		}
	}).detach();
	if (argc < 2) { std::printf("usage: script_check <main.scm> [seconds]\n"); return 1; }
	ScriptVM vm;
	if (!vm.load(argv[1])) { std::printf("cannot load\n"); return 1; }
	int secs = argc > 2 ? atoi(argv[2]) : 20;
	for (int f = 0; f < secs * 50; f++)
		vm.tick(20);
	std::printf("threads=%zu commands=%llu zones with population type=%zu\n", vm.threadCount(), (unsigned long long)vm.commandsRun, vm.zonePopType.size());
	std::vector<std::pair<uint32_t, uint16_t>> u;
	for (auto &kv : vm.unhandled) u.push_back({kv.second, kv.first});
	std::sort(u.rbegin(), u.rend());
	std::printf("unhandled opcodes: %zu distinct\n", u.size());
	for (size_t i = 0; i < u.size() && i < 25; i++)
		std::printf("  %04X %s x%u\n", u[i].second, ScriptVM::opName(u[i].second), u[i].first);
	for (uint32_t ip : vm.spawned) std::printf("spawned at %u\n", ip);
	int k = 0;
	for (auto &kv : vm.zonePopType) { if (k++ < 8) std::printf("zone %s -> %d\n", kv.first.c_str(), kv.second); }
	return 0;
}
