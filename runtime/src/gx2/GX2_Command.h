// GX2 command writing (Cemu-compatible API, own implementation in gx2_core.cpp).
// Commands go to the calling thread's current write target: a chunk of the GX2 command pool
// or, between GX2BeginDisplayList/GX2EndDisplayList, the game's display list buffer.
#pragma once
#include "../compat/cemu.h"
#include "../latte/isa/LatteReg.h"
#include "../latte/latte.h"

#define pm4HeaderType3(__itCode, __dataDWordCount) (0xC0000000 | ((uint32)(__itCode) << 8) | ((uint32)((__dataDWordCount)-1) << 16))
#define pm4HeaderType2Filler() (0x80000000)
using namespace latte;  // IT_* opcodes

namespace GX2 {
struct GX2PerCoreCBState {
    uint32be* bufferPtr = nullptr;
    uint32 bufferSizeInU32s = 0;
    uint32be* currentWritePtr = nullptr;
    bool isDisplayList = false;
};
extern thread_local GX2PerCoreCBState* t_cb_ptr;
GX2PerCoreCBState& cb_slow();
inline GX2PerCoreCBState& cb() {
    GX2PerCoreCBState* p = t_cb_ptr;
    return __builtin_expect(p != nullptr, 1) ? *p : cb_slow();
}

void GX2Command_Flush(uint32 numU32sForNextBuffer, bool triggerMarkerInterrupt = true);
void GX2ReserveCmdSpace_Slow(uint32 reservedFreeSpaceInU32);
inline void GX2ReserveCmdSpace(uint32 n) {
    GX2PerCoreCBState& c = cb();
    uint32be* wp = c.currentWritePtr;
    if (__builtin_expect(wp != nullptr, 1)) {
        uint32 used = (uint32)(wp - c.bufferPtr);
        uint32 pad = c.isDisplayList ? 0u : 8u;
        if (__builtin_expect(used + n + pad <= c.bufferSizeInU32s, 1)) return;
    }
    GX2ReserveCmdSpace_Slow(n);
}
uint64 GX2GetLastSubmittedTimeStamp();
uint64 GX2GetRetiredTimeStamp();
bool GX2WaitTimeStamp(uint64 tsWait);
void GX2BeginDisplayList(MEMPTR<void> displayListAddr, uint32 size);
void GX2BeginDisplayListEx(MEMPTR<void> displayListAddr, uint32 size, bool profiling);
uint32 GX2EndDisplayList(MEMPTR<void> displayListAddr);
void GX2CallDisplayList(MPTR addr, uint32 size);
void GX2DirectCallDisplayList(void* addr, uint32 size);
bool GX2GetDisplayListWriteStatus();
bool GX2DrawDone();
void GX2Flush();
extern uint32 sGX2MainCoreIndex;
}  // namespace GX2

inline void gx2WriteGather_submitU32AsBE(uint32 v) {
    GX2::GX2PerCoreCBState& c = GX2::cb();
    uint32be* wp = c.currentWritePtr;
    if (__builtin_expect(wp != nullptr, 1)) {
        *wp = v;
        c.currentWritePtr = wp + 1;
    }
}
inline void gx2WriteGather_submitU32AsLE(uint32 v) {
    GX2::GX2PerCoreCBState& c = GX2::cb();
    uint32be* wp = c.currentWritePtr;
    if (__builtin_expect(wp != nullptr, 1)) {
        *(uint32*)wp = v;
        c.currentWritePtr = wp + 1;
    }
}
void gx2WriteGather_submitU32AsLEArray(uint32* v, uint32 numValues);

template <typename T>
inline uint32 gx2CmdWordToBE(const betype<T>& arg) {
    static_assert(sizeof(betype<T>) == sizeof(uint32));
    uint32 raw;
    memcpy(&raw, &arg, 4);
    return raw;
}
template <typename T>
    requires std::is_floating_point_v<T>
inline uint32 gx2CmdWordToBE(const T& arg) {
    static_assert(sizeof(T) == sizeof(uint32));
    uint32 u;
    memcpy(&u, &arg, 4);
    return _swapEndianU32(u);
}
template <typename T>
    requires std::is_base_of_v<Latte::LATTEREG, T>
inline uint32 gx2CmdWordToBE(const T& arg) {
    return _swapEndianU32(arg.getRawValue());
}
template <typename T>
    requires(!std::is_base_of_v<Latte::LATTEREG, T>) && (!std::is_floating_point_v<T>)
inline uint32 gx2CmdWordToBE(const T& arg) {
    return _swapEndianU32((uint32)arg);
}

inline void gx2WriteGather_submit_(uint32* writePtr) {}

template <typename T0>
inline void gx2WriteGather_submit_(uint32* writePtr, const T0& a0) {
    writePtr[0] = gx2CmdWordToBE(a0);
}

template <typename T0, typename T1, typename... Targs>
inline void gx2WriteGather_submit_(uint32* writePtr, const T0& a0, const T1& a1, const Targs&... rest) {
    uint64 pair = (uint64)gx2CmdWordToBE(a0) | ((uint64)gx2CmdWordToBE(a1) << 32);
    memcpy(writePtr, &pair, 8);
    gx2WriteGather_submit_(writePtr + 2, rest...);
}

template <typename... Targs>
inline void gx2WriteGather_submit(const Targs&... args) {
    GX2::GX2PerCoreCBState& c = GX2::cb();
    uint32be* p = c.currentWritePtr;
    if (__builtin_expect(!p, 0)) return;
    gx2WriteGather_submit_((uint32*)p, args...);
    c.currentWritePtr = p + sizeof...(Targs);
}
