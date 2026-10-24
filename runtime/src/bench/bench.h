#pragma once
// Benchmark mode (--bench N): reaches gameplay from the title screen with no user input (presses A
// until the frame looks like gameplay), lets the scene settle, measures game frame times for N
// seconds, writes <user root>/bench.txt and exits. --bench-uncap raises the virtual vsync rate so
// the frame rate is limited by the emulation work, not by the 60 Hz pacing. --bench-profile adds a
// sampling profiler (Windows) that writes <user root>/bench_profile.txt.
#include <cstdint>

namespace bench {
struct Options {
    int seconds = 0;     // measured window
    int settle = 10;     // seconds of gameplay before measuring
    int timeout = 240;   // seconds to reach gameplay
    int uncap_hz = 0;    // virtual vsync rate while benchmarking (0 = normal pacing)
    bool profile = false;
    int min_draws = 1500;  // draws per frame that count as gameplay
};
void configure(const Options& o);
bool active();
void start();                // after the GPU backend is up (sets the vsync rate)
void on_swap();              // GX2SwapScanBuffers (game thread)
void on_stats(int draws);    // once a second from the GPU thread's statistics
uint32_t buttons();          // buttons the bench wants pressed right now (host loop)
bool tick();                 // host loop; true when the run is finished and the process should exit
}  // namespace bench
