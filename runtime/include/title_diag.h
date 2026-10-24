// BOTW_DIAG=1 counters for the title screen: where draws land, whether the command
// processor dropped any, whether AX voices reach the mixer, and whether VPADRead
// keeps being called. Every function is a no-op unless BOTW_DIAG is set.
//
// GPU functions are called on the GPU thread only. AX and VPAD functions are called
// from their own threads; title_diag_log() (GPU thread, about once a second) prints
// and resets the interval.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
#include <atomic>
extern std::atomic<int> g_diag_on_flag;
bool diag_init_slow();
inline bool title_diag_enabled() {
    int v = g_diag_on_flag.load(std::memory_order_relaxed);
    if (__builtin_expect(v >= 0, 1)) return v == 1;
    return diag_init_slow();
}
extern "C" {
#endif

// color_base / color_size are CB_COLOR0_BASE and CB_COLOR0_SIZE at the draw.
void title_diag_draw_slow(uint32_t color_base, uint32_t color_size);
// why: 1 = indexed draw with a null index buffer (not counted as a draw),
//      2 = auto draw skipped as a compute shader.
void title_diag_draw_skip_slow(int why);
// One indirect buffer, including the per-submit command buffer and display lists.
void title_diag_indirect_slow(uint32_t dwords);
void title_diag_scanout_slow(uint32_t addr, uint32_t width, uint32_t height, uint32_t format);
void title_diag_clear_slow(uint32_t mask, uint32_t width, uint32_t height);

// playing: voices whose playback state is running.
// linked_ppc / linked_dsp: voices actually chained into that mixer list.
// mask_tv: linked PPC voices with a non-zero TV device mix mask.
// peak_mix: max abs sample in the 32 kHz mix buffer (before the 32->48 step).
// peak_lr / peak_other: max abs sample in the 48 kHz buffer sent to the host,
// stereo channels 0-1 vs the other four.
// flags bit 0: TV final-mix callback set. bit 1: frame callback. bit 2: an app frame callback.
void title_diag_ax_slow(uint32_t playing, uint32_t linked_ppc, uint32_t linked_dsp, uint32_t mask_tv,
                        uint32_t peak_mix, uint32_t peak_lr, uint32_t peak_other, uint32_t flags);

void title_diag_vpad_read_slow(uint32_t samples, uint32_t buttons);
void title_diag_vpad_callback_slow(uint32_t addr);
void title_diag_vpad_invoked_slow(void);

void title_diag_log_slow(void);

#ifdef __cplusplus
}

inline void title_diag_draw(uint32_t color_base, uint32_t color_size) {
    if (__builtin_expect(title_diag_enabled(), 0)) title_diag_draw_slow(color_base, color_size);
}
inline void title_diag_draw_skip(int why) {
    if (__builtin_expect(title_diag_enabled(), 0)) title_diag_draw_skip_slow(why);
}
inline void title_diag_indirect(uint32_t dwords) {
    if (__builtin_expect(title_diag_enabled(), 0)) title_diag_indirect_slow(dwords);
}
inline void title_diag_scanout(uint32_t addr, uint32_t width, uint32_t height, uint32_t format) {
    if (__builtin_expect(title_diag_enabled(), 0)) title_diag_scanout_slow(addr, width, height, format);
}
inline void title_diag_clear(uint32_t mask, uint32_t width, uint32_t height) {
    if (__builtin_expect(title_diag_enabled(), 0)) title_diag_clear_slow(mask, width, height);
}
inline void title_diag_ax(uint32_t playing, uint32_t linked_ppc, uint32_t linked_dsp, uint32_t mask_tv,
                          uint32_t peak_mix, uint32_t peak_lr, uint32_t peak_other, uint32_t flags) {
    if (__builtin_expect(title_diag_enabled(), 0))
        title_diag_ax_slow(playing, linked_ppc, linked_dsp, mask_tv, peak_mix, peak_lr, peak_other, flags);
}
inline void title_diag_vpad_read(uint32_t samples, uint32_t buttons) {
    if (__builtin_expect(title_diag_enabled(), 0)) title_diag_vpad_read_slow(samples, buttons);
}
inline void title_diag_vpad_callback(uint32_t addr) {
    if (__builtin_expect(title_diag_enabled(), 0)) title_diag_vpad_callback_slow(addr);
}
inline void title_diag_vpad_invoked(void) {
    if (__builtin_expect(title_diag_enabled(), 0)) title_diag_vpad_invoked_slow();
}
inline void title_diag_log(void) {
    if (__builtin_expect(title_diag_enabled(), 0)) title_diag_log_slow();
}
#endif
