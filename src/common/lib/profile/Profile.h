#pragma once
#include <array>
#include <atomic>
#include <cstdint>
namespace coop::profile {
enum Phase { NETWORK_POLL, NETWORK_DISPATCH, STREAMING, NATIVE_FRAME, SAVE_WORLD, LOG_IO, SCRIPT_PROCESS, ENTITY_CREATE, PHASE_COUNT };
inline const char* Name(int p){static const char*n[]={"network_poll","network_dispatch","streaming","game_interval","save_world","log_io","script_process","entity_create"};return n[p];}
uint64_t Micros();
struct Metric {
    std::atomic<uint64_t> count{0},total{0},maximum{0};
    void Add(uint64_t us){++count;total+=us;auto old=maximum.load();while(old<us&&!maximum.compare_exchange_weak(old,us)){} }
};
inline std::array<Metric,PHASE_COUNT>& Metrics(){static std::array<Metric,PHASE_COUNT> m;return m;}
struct Scope { Phase phase;uint64_t begin;Scope(Phase p):phase(p),begin(Micros()){}~Scope(){Metrics()[phase].Add(Micros()-begin);} };
}
