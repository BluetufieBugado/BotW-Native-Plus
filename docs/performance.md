# Performance guide

Lowering the resolution (`--scale`, resolution mods) only shrinks the **GPU** work. If the frame
rate barely moves when you do that, the limit is the **CPU side**: the recompiled game code, the
GX2/Latte command processing and the Vulkan driver's per-draw cost. The on-screen counter shows
that cost directly: the draw count (`4973 draws`) is paid on the CPU every frame, at any
resolution.

## 1. Build for your CPU (biggest free win)

The default build targets a generic x86-64 CPU so one binary runs everywhere. The price is that
every PowerPC `fmadd`/`fmsub` in the recompiled game becomes a real call into `fma()`
(`runtime/src/cpu.cpp`, which first checks the CPU), and every big-endian load/store is a
`mov` + `bswap`. Game physics and animation are full of both.

Tell the compiler which CPU it is building for and it emits `vfmadd`/`vsqrtsd`/`movbe` inline:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBOTW_CPU_ARCH=native
cmake --build build -j12
```

- `native`: best for a build you run on the same PC. Do not give that binary to other people.
- `x86-64-v3`: portable to any CPU with AVX2/FMA (Intel Haswell 2013+, AMD Zen). Use this for a
  build you share. It will not start on older CPUs, for example Intel Atom/Celeron/Pentium
  "Silver" parts without AVX. Leave the option empty for those machines.

The recompiled game is also built with `-fno-math-errno` now, so `sqrt` is a single instruction
on every target.

Changing the architecture changes every compile flag, so the whole `generated/` tree is rebuilt
(allow the same time as the first build; ccache does not help across a flag change).

### Optional: more optimisation for the large functions

Functions that the recompiler splits into `big_*.cpp` are built at `-O1` and `huge_*.cpp` at
`-O0`, to save RAM and build time. If the profiler below shows time in them, raise them:

```bash
cmake -B build -DBOTW_BIG_OPT=-O2 -DBOTW_HUGE_OPT=-O1
```

## 2. Measure instead of guessing

`--bench` runs a repeatable test from the title screen (it presses A until gameplay, lets the
scene settle, measures, writes `bench.txt` in `%APPDATA%\BotWRecomp` and exits).
`--bench-uncap` removes the 60 Hz pacing so the number is the machine's real speed.

```bash
./build/BotWRecomp.exe --game /path/to/game --bench 60 --bench-uncap
```

Run it once on the old build and once on the new one and compare `avg_fps`, `low1_fps` and
`p99_ms` in `bench.txt`. Add `--bench-profile` to also get `bench_profile.txt`: its `[modules]`
section tells you whether the time is in `BotWRecomp.exe` (game code, Latte) or in the GPU driver
DLL, and the per-thread sections show which thread is the bottleneck.

`BOTW_DIAG=1` prints draw counts per render target and how many draws were dropped (see the
README).

## 3. Things that look like low performance but are not

- **A cold shader cache.** Pipelines are compiled the first time a scene is drawn, so a fresh
  install, a new driver or a new area is slow for a while and then recovers. The cache lives in
  `%APPDATA%\BotWRecomp\cache`. Judge performance on the second pass through the same place, or
  with `--bench`. Keep that folder on an SSD and do not clear it.
- **Antivirus real-time scanning** of the game folder and of the cache folder.
- **Laptops on battery or in a power-saver plan.** The CPU clock drops a lot. Plug in and pick the
  "High performance" plan. The runtime now also opts out of Windows' per-process efficiency
  throttling (EcoQoS).
- **Hybrid graphics laptops.** Make sure Windows runs `BotWRecomp.exe` on the dedicated GPU
  (Settings > System > Display > Graphics).
- **Too few cores.** The three Wii U cores run as separate host threads, plus the GPU thread and
  audio. A 2-core CPU is oversubscribed and is the usual reason resolution has no effect.
- **`--fps 30`** gives the game's original pacing. It does not make a slow machine faster, but if
  you can hold 30 and not 60 it gives steadier frame times than a 60 target that keeps missing.
