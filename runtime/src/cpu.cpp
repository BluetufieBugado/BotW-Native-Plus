// CPU helpers for generated code: time base, reciprocal estimates, quantization scales.
#include <math.h>
#include <time.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

#include "ppc.h"

// Espresso time base runs at bus clock / 4 = 248.625 MHz / 4.
static constexpr uint64_t kTimerHz = 62156250;

extern "C" uint64_t rt_timebase(void) {
#if defined(_WIN32)
    static const uint64_t freq = []() -> uint64_t {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return (uint64_t)f.QuadPart;
    }();
    static const uint64_t qpc0 = []() -> uint64_t {
        LARGE_INTEGER c{};
        QueryPerformanceCounter(&c);
        return (uint64_t)c.QuadPart;
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    uint64_t ticks = (uint64_t)now.QuadPart - qpc0;
#if defined(__SIZEOF_INT128__)
    return (uint64_t)((__uint128_t)ticks * kTimerHz / freq);
#else
    return ticks / freq * kTimerHz + (ticks % freq) * kTimerHz / freq;
#endif
#else
    struct timespec ts{};
    const clockid_t clk = CLOCK_MONOTONIC_RAW;
    if (clock_gettime(clk, &ts) != 0) clock_gettime(CLOCK_MONOTONIC, &ts);
    static const uint64_t t0 = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec - t0;
#if defined(__SIZEOF_INT128__)
    return (uint64_t)((__uint128_t)ns * kTimerHz / 1000000000ull);
#else
    return ns / 1000000000ull * kTimerHz + (ns % 1000000000ull) * kTimerHz / 1000000000ull;
#endif
#endif
}

// fres / frsqrte lookup tables from the Espresso hardware (via Cemu PPCInterpreterFPU.cpp).
namespace {
struct Entry {
    uint32_t offset, step;
};
const Entry kRsqrte[32] = {
    {0x1a7e800, 0x568}, {0x17cb800, 0x4f3}, {0x1552800, 0x48d}, {0x130c000, 0x435}, {0x10f2000, 0x3e7}, {0xeff000, 0x3a2},
    {0xd2e000, 0x365},  {0xb7c000, 0x32e},  {0x9e5000, 0x2fc},  {0x867000, 0x2d0},  {0x6ff000, 0x2a8},  {0x5ab800, 0x283},
    {0x46a000, 0x261},  {0x339800, 0x243},  {0x218800, 0x226},  {0x105800, 0x20b},  {0x3ffa000, 0x7a4}, {0x3c29000, 0x700},
    {0x38aa000, 0x670}, {0x3572000, 0x5f2}, {0x3279000, 0x584}, {0x2fb7000, 0x524}, {0x2d26000, 0x4cc}, {0x2ac0000, 0x47e},
    {0x2881000, 0x43a}, {0x2665000, 0x3fa}, {0x2468000, 0x3c2}, {0x2287000, 0x38e}, {0x20c1000, 0x35e}, {0x1f12000, 0x332},
    {0x1d79000, 0x30a}, {0x1bf4000, 0x2e6},
};
const Entry kRes[32] = {
    {0x7ff800, 0x3e1}, {0x783800, 0x3a7}, {0x70ea00, 0x371}, {0x6a0800, 0x340}, {0x638800, 0x313}, {0x5d6200, 0x2ea},
    {0x579000, 0x2c4}, {0x520800, 0x2a0}, {0x4cc800, 0x27f}, {0x47ca00, 0x261}, {0x430800, 0x245}, {0x3e8000, 0x22a},
    {0x3a2c00, 0x212}, {0x360800, 0x1fb}, {0x321400, 0x1e5}, {0x2e4a00, 0x1d1}, {0x2aa800, 0x1be}, {0x272c00, 0x1ac},
    {0x23d600, 0x19b}, {0x209e00, 0x18b}, {0x1d8800, 0x17c}, {0x1a9000, 0x16e}, {0x17ae00, 0x15b}, {0x14f800, 0x15b},
    {0x124400, 0x143}, {0xfbe00, 0x143},  {0xd3800, 0x12d},  {0xade00, 0x12d},  {0x88400, 0x11a},  {0x65000, 0x11a},
    {0x41c00, 0x108},  {0x20c00, 0x106},
};
}  // namespace

extern "C" double ppc_frsqrte(double input) {
    uint64_t x = dbits(input);
    if ((x << 1) == 0) return bitsd((x & ~0x7FFFFFFFFFFFFFFFULL) | 0x7FF0000000000000ULL);
    uint32_t e = (x >> 52) & 0x7FF;
    if (e == 0x7FF) {
        if ((x & ((1ull << 52) - 1)) == 0) return (int64_t)x < 0 ? bitsd(0x7FF8000000000000ULL) : 0.0;
        return input;
    }
    if ((int64_t)x < 0) return bitsd(0x7FF8000000000000ULL);
    uint32_t idx = (x >> 48) & 31;
    uint32_t stepMul = (x >> 37) & 0x7FF;
    int32_t sum = (int32_t)(kRsqrte[idx].offset - kRsqrte[idx].step * stepMul);
    e = 1023 - ((e - 1021) >> 1);
    x &= ~(0x7FFULL << 52);
    x |= (uint64_t)e << 52;
    x &= ~((1ull << 52) - 1);
    x += (uint64_t)(int64_t)sum << 26;
    return bitsd(x);
}

extern "C" double ppc_fres(double input) {
    uint64_t x = dbits(input);
    uint32_t idx = (x >> 47) & 31;
    uint32_t stepMul = (x >> 37) & 0x3FF;
    uint32_t sum = kRes[idx].offset - (kRes[idx].step * stepMul + 1) / 2;
    uint32_t e = (x >> 52) & 0x7FF;
    if (e == 0) return bitsd(x | 0x7FF0000000000000ull);
    if (e == 0x7FF) {
        if ((x & ((1ull << 52) - 1)) == 0) return (int64_t)x < 0 ? bitsd(0x8000000000000000ull) : 0.0;
        return input;
    }
    e = 2045 - e;
    x &= ~(0x7FFULL << 52);
    x |= (uint64_t)e << 52;
    x &= ~((1ull << 52) - 1);
    x += (uint64_t)sum << 29;
    return bitsd(x);
}

// GQR scale field is a 6-bit signed exponent s: dequantize multiplies by 2^-s, quantize by 2^s.
extern "C" const float g_dequant_scale[64] = {0x1p0f, 0x1p-1f, 0x1p-2f, 0x1p-3f, 0x1p-4f, 0x1p-5f, 0x1p-6f, 0x1p-7f, 0x1p-8f, 0x1p-9f, 0x1p-10f, 0x1p-11f, 0x1p-12f, 0x1p-13f, 0x1p-14f, 0x1p-15f, 0x1p-16f, 0x1p-17f, 0x1p-18f, 0x1p-19f, 0x1p-20f, 0x1p-21f, 0x1p-22f, 0x1p-23f, 0x1p-24f, 0x1p-25f, 0x1p-26f, 0x1p-27f, 0x1p-28f, 0x1p-29f, 0x1p-30f, 0x1p-31f, 0x1p32f, 0x1p31f, 0x1p30f, 0x1p29f, 0x1p28f, 0x1p27f, 0x1p26f, 0x1p25f, 0x1p24f, 0x1p23f, 0x1p22f, 0x1p21f, 0x1p20f, 0x1p19f, 0x1p18f, 0x1p17f, 0x1p16f, 0x1p15f, 0x1p14f, 0x1p13f, 0x1p12f, 0x1p11f, 0x1p10f, 0x1p9f, 0x1p8f, 0x1p7f, 0x1p6f, 0x1p5f, 0x1p4f, 0x1p3f, 0x1p2f, 0x1p1f};
extern "C" const float g_quant_scale[64] = {0x1p0f, 0x1p1f, 0x1p2f, 0x1p3f, 0x1p4f, 0x1p5f, 0x1p6f, 0x1p7f, 0x1p8f, 0x1p9f, 0x1p10f, 0x1p11f, 0x1p12f, 0x1p13f, 0x1p14f, 0x1p15f, 0x1p16f, 0x1p17f, 0x1p18f, 0x1p19f, 0x1p20f, 0x1p21f, 0x1p22f, 0x1p23f, 0x1p24f, 0x1p25f, 0x1p26f, 0x1p27f, 0x1p28f, 0x1p29f, 0x1p30f, 0x1p31f, 0x1p-32f, 0x1p-31f, 0x1p-30f, 0x1p-29f, 0x1p-28f, 0x1p-27f, 0x1p-26f, 0x1p-25f, 0x1p-24f, 0x1p-23f, 0x1p-22f, 0x1p-21f, 0x1p-20f, 0x1p-19f, 0x1p-18f, 0x1p-17f, 0x1p-16f, 0x1p-15f, 0x1p-14f, 0x1p-13f, 0x1p-12f, 0x1p-11f, 0x1p-10f, 0x1p-9f, 0x1p-8f, 0x1p-7f, 0x1p-6f, 0x1p-5f, 0x1p-4f, 0x1p-3f, 0x1p-2f, 0x1p-1f};

// Runtime-dispatched IEEE-754 double-precision FMA for guest PowerPC fmadd/fmsub/fnmadd/fnmsub.
// Prebuilt guest shards compiled with generic x86-64 baseline emit calls to external symbol `fma`.
// Providing `fma` in the main executable avoids the DLL import jump into ucrtbase.dll and uses a
// single hardware FMA3 instruction when supported at runtime, with a portable software fallback.
#if defined(__x86_64__) || defined(_M_X64)
static const int g_cpu_has_fma = __builtin_cpu_supports("fma");
#endif

__attribute__((noinline)) static double fma_software_fallback(double x, double y, double z) {
#if defined(_WIN32)
    using FmaFn = double (*)(double, double, double);
    static const FmaFn ucrt_fma = []() -> FmaFn {
        HMODULE m = GetModuleHandleW(L"ucrtbase.dll");
        return m ? (FmaFn)(void*)GetProcAddress(m, "fma") : nullptr;
    }();
    if (ucrt_fma) return ucrt_fma(x, y, z);
#endif
    return (double)((long double)x * (long double)y + (long double)z);
}

extern "C" double fma(double x, double y, double z) {
#if defined(__x86_64__) || defined(_M_X64)
    if (__builtin_expect(g_cpu_has_fma, 1)) {
        __asm__("vfmadd213sd %2, %1, %0" : "+x"(x) : "x"(y), "x"(z));
        return x;
    }
#endif
    return fma_software_fallback(x, y, z);
}
