#include "thread.h"

#include "botw_target.h"
#if !defined(_WIN32)
#include <dlfcn.h>
#include <execinfo.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>
#endif
#include <string.h>
#include <time.h>

#include <unordered_map>
#include <utility>
#include <vector>

#include "diag.h"
#include "hle.h"
#include "loader.h"

#if TARGET_OS_IOS  // iOS port: stack size vs. the VA limit without extended VA (ios/src/vm_ios.cpp)
size_t botw_ios_stack_size(size_t want);
#endif

namespace threads {
std::mutex g_klock;
namespace {
thread_local GuestThread* t_current = nullptr;
thread_local int t_exec_depth = 0;
thread_local int t_exec_core = -1;
std::mutex g_exec[3];
std::unordered_map<uint32_t, GuestThread*> g_threads;  // OSThread* -> thread
uint16_t g_next_id = 1;
constexpr size_t kHostStack = 64u << 20;  // guest recursion maps onto the host stack

int core_index(int core) { return core >= 0 && core <= 2 ? core : 1; }

void run_thread(GuestThread* t) {
    t_current = t;
    t->host_tid = pthread_self();
    // The game's main thread and the light timing threads (audio, alarms, GX2 callbacks) get the highest QoS so
    // the scheduler keeps them on performance cores (a phone has two); worker threads one step below.
    const bool critical = t->is_main || t->name == "AX" || t->name == "alarms" || t->name == "GX2 callbacks";
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(critical ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_USER_INITIATED, 0);
#else
    (void)critical;
#endif
    botw_set_thread_name(t->name.empty() ? "guest" : t->name.c_str());
    int core = core_index(t->core);
    g_exec[core].lock();
    t_exec_core = core;
    t_exec_depth = 1;
    LOG("thread %s running (entry %08x core %d prio %d)", t->name.empty() ? "guest" : t->name.c_str(), t->entry, t->core,
        t->os ? (int)mem::r32(t->os + T_BASEPRIO) : -1);
    PPCContext* c = &t->ctx;
    c->lr = 0;
    rt_call(c, t->entry);
    uint32_t v = c->r[3];
    bool sound = false;
    {
        std::lock_guard<std::mutex> l(g_klock);
        t->exited = true;
        t->exit_value = v;
        mem::w32(t->os + T_EXITVAL, v);
        mem::w8(t->os + T_STATE, 8);  // moribund
        t->cv.notify_all();
        LOG("thread %s exited (%08x)", t->name.c_str(), v);
        sound = t->name == "SoundCalcThread";
    }
    if (t_exec_depth > 0) {
        int held = t_exec_core;
        t_exec_depth = 0;
        t_exec_core = -1;
        g_exec[held].unlock();
    }
    // After the core is released, so the dump does not sit in the scheduler lock.
    // The guest function has already returned; this cannot change what it wrote.
    if (sound) diag_dump("SoundCalcThread exit");
}

void launch(GuestThread* t) {
    t->started = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
#if TARGET_OS_IOS
    pthread_attr_setstacksize(&attr, botw_ios_stack_size(kHostStack));
#else
    pthread_attr_setstacksize(&attr, kHostStack);
#endif
    pthread_t th;
    pthread_create(&th, &attr, [](void* p) -> void* { run_thread((GuestThread*)p); return nullptr; }, t);
    pthread_detach(th);
}
}  // namespace

GuestThread* current() { return t_current; }
const char* current_name() { return t_current ? t_current->name.c_str() : nullptr; }

bool exec_yield() {
    if (t_exec_depth == 0) return false;
    int core = t_exec_core;
    t_exec_depth = 0;
    g_exec[core].unlock();
    return true;
}

void exec_resume() {
    if (t_exec_depth > 0 || t_exec_core < 0) return;
    GuestThread* self = t_current;
    int core = t_exec_core;
    if (self) core = core_index(self->core);
    t_exec_core = core;
    g_exec[core].lock();
    t_exec_depth = 1;
}

void set_blocked(GuestThread* t, bool blocked) {
    if (!t || !t->os || t->exited) return;
    mem::w8(t->os + T_STATE, blocked ? 4 : 2);
}

GuestThread* get(uint32_t os) {
    auto it = g_threads.find(os);
    return it == g_threads.end() ? nullptr : it->second;
}

uint32_t sda_base() { return loader::main_module().sda_base; }

namespace {
std::mutex g_wait_stats_lock;
std::vector<std::pair<const char*, uint64_t>> g_wait_stats;  // kind -> ns
auto g_wait_stats_t0 = std::chrono::steady_clock::now();
}  // namespace
void note_main_wait(const char* kind, uint64_t ns) {
    std::lock_guard<std::mutex> l(g_wait_stats_lock);
    for (auto& [k, v] : g_wait_stats)
        if (k == kind) {
            v += ns;
            return;
        }
    g_wait_stats.push_back({kind, ns});
}
std::string main_wait_report() {
    std::lock_guard<std::mutex> l(g_wait_stats_lock);
    auto now = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(now - g_wait_stats_t0).count();
    g_wait_stats_t0 = now;
    std::string s;
    char buf[96];
    for (auto& [k, v] : g_wait_stats) {
        snprintf(buf, sizeof buf, "%s%s %.0f", s.empty() ? "" : ", ", k, v / 1e6 / secs);
        s += buf;
        v = 0;
    }
    return s + " (ms/s)";
}

#if !defined(_WIN32)
namespace {
std::atomic<const char*> g_sample_label{"?"};  // name for non-guest threads (sample_host)

void log_host_frames(const char* label, void* const* frames, int n) {
    char line[4096];
    int k = snprintf(line, sizeof line, "  %s host frames (base 0x%llx):", label ? label : "?",
                     (unsigned long long)(uintptr_t)botw_image_base());
    for (int i = 0; i < n && k < (int)sizeof line - 24; i++)
        k += snprintf(line + k, sizeof line - k, " %p", frames[i]);
    LOG("%s", line);
    for (int i = 0; i < n; i++) {
        Dl_info info{};
        const char* name = nullptr;
        unsigned long long off = (unsigned long long)(uintptr_t)frames[i];
        if (dladdr(frames[i], &info) && info.dli_fbase) {
            name = info.dli_fname;
            if (name) {
                const char* slash = strrchr(name, '/');
                if (slash) name = slash + 1;
            }
            off = (unsigned long long)((uintptr_t)frames[i] - (uintptr_t)info.dli_fbase);
        }
        if (name) LOG("    %s+0x%llx", name, off);
        else LOG("    0x%llx", off);
    }
}

void on_stack_signal(int) {  // runs on the interrupted thread
#if defined(__APPLE__) || defined(__linux__)
    void* frames[64];
    int n = backtrace(frames, 64);
    // backtrace's first frames are the signal trampoline and this handler.
    int skip = n > 2 ? 2 : 0;
    const char* label = current_name() ? current_name() : g_sample_label.load();
    log_host_frames(label, frames + skip, n - skip);
#else
    LOG("  %s (host stack sample unavailable)", current_name() ? current_name() : g_sample_label.load());
#endif
}
}  // namespace

void install_stack_signal() {
    static bool installed = [] {
        struct sigaction sa = {};
        sa.sa_handler = on_stack_signal;
        sigaction(SIGUSR2, &sa, nullptr);
        return true;
    }();
    (void)installed;
}

void sample_host(pthread_t th, const char* label) {
    install_stack_signal();
    g_sample_label = label;
    pthread_kill(th, SIGUSR2);
}

void host_stack_log(const char* label, void*) {
#if defined(__APPLE__) || defined(__linux__)
    void* frames[64];
    int n = backtrace(frames, 64);
    int skip = n > 1 ? 1 : 0;
    log_host_frames(label, frames + skip, n - skip);
#else
    LOG("  %s (host stack sample unavailable)", label ? label : "?");
#endif
}
#else
namespace {
constexpr int kMaxFrames = 64;

// x64 unwind data, not the frame pointer. MinGW omits RBP, so a chain walk stops at the
// first system frame (the single KERNELBASE address in the wake-up stall).
int capture_frames(CONTEXT ctx, uint64_t* pcs, int max) {
    UNWIND_HISTORY_TABLE history{};
    int n = 0;
    for (; n < max;) {
        DWORD64 pc = ctx.Rip;
        if (!pc) break;
        pcs[n++] = pc;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(pc, &imageBase, &history);
        if (!entry) {
            DWORD64 ret = 0;
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)ctx.Rsp, &ret, sizeof ret, &got) || got != sizeof ret || !ret)
                break;
            ctx.Rip = ret;
            ctx.Rsp += 8;
        } else {
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, pc, entry, &ctx, &handlerData, &establisher, nullptr);
        }
        if (ctx.Rip == pc) break;
    }
    return n;
}

void format_module(uint64_t pc, char* buf, size_t n) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)pc, &mod) &&
        mod) {
        char path[MAX_PATH];
        if (GetModuleFileNameA(mod, path, MAX_PATH)) {
            const char* name = path;
            for (const char* p = path; *p; p++)
                if (*p == '\\' || *p == '/') name = p + 1;
            snprintf(buf, n, "%s+0x%llx", name, (unsigned long long)(pc - (uint64_t)(uintptr_t)mod));
            return;
        }
    }
    snprintf(buf, n, "0x%llx", (unsigned long long)pc);
}

void log_pcs(const char* label, const uint64_t* pcs, int n) {
    char line[4096];
    int k = snprintf(line, sizeof line, "  %s host frames (base 0x%llx):", label ? label : "?",
                     (unsigned long long)(uintptr_t)botw_image_base());
    for (int i = 0; i < n && k < (int)sizeof line - 24; i++)
        k += snprintf(line + k, sizeof line - k, " 0x%llx", (unsigned long long)pcs[i]);
    LOG("%s", line);
    for (int i = 0; i < n; i++) {
        char mod[192];
        format_module(pcs[i], mod, sizeof mod);
        LOG("    %s", mod);
    }
}
}  // namespace

void install_stack_signal() {}

void host_stack_log(const char* label, void* host_context) {
    CONTEXT ctx{};
    if (host_context) ctx = *(CONTEXT*)host_context;
    else RtlCaptureContext(&ctx);
    uint64_t pcs[kMaxFrames];
    int n = capture_frames(ctx, pcs, kMaxFrames);
    log_pcs(label, pcs, n);
}

void sample_host(pthread_t th, const char* label) {
    // winpthreads: the pthread_t is not a HANDLE. pthread_gethandle returns the OS thread.
    HANDLE h = (HANDLE)pthread_gethandle(th);
    if (!h) {
        LOG("  %s (no host thread handle)", label ? label : "?");
        return;
    }
    if (GetThreadId(h) == GetCurrentThreadId()) {
        host_stack_log(label, nullptr);
        return;
    }
    if (SuspendThread(h) == (DWORD)-1) {
        LOG("  %s (SuspendThread failed %lu)", label ? label : "?", GetLastError());
        return;
    }
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    BOOL ok = GetThreadContext(h, &ctx);
    uint64_t pcs[kMaxFrames];
    int n = 0;
    if (ok) n = capture_frames(ctx, pcs, kMaxFrames);
    ResumeThread(h);
    if (!ok) {
        LOG("  %s (GetThreadContext failed %lu)", label ? label : "?", GetLastError());
        return;
    }
    log_pcs(label, pcs, n);
}
#endif

void dump(bool stacks) {
    std::vector<std::pair<pthread_t, std::string>> running;
    {
        std::lock_guard<std::mutex> l(g_klock);
        int suspended = 0;
        for (auto& [os, t] : g_threads) {
            if (t->exited) continue;
            if (!t->started) {
                suspended++;
                continue;
            }
            const char* k = t->wait_kind;
            if (k) LOG("thread %-32s %08x entry %08x core %d: %s %08x (lr %08x)", t->name.c_str(), os, t->entry, t->core, k,
                       t->wait_obj, t->wait_lr);
            else {
                LOG("thread %-32s %08x entry %08x core %d: running (lr %08x)", t->name.c_str(), os, t->entry, t->core, t->ctx.lr);
                if (t->host_tid) running.emplace_back(t->host_tid, t->name);
            }
        }
        LOG("%d threads created but never started", suspended);
    }
    if (!stacks) return;
#if defined(_WIN32)
    for (auto& [th, name] : running) sample_host(th, name.c_str());
#else
    install_stack_signal();
    for (auto& [th, name] : running) {
        (void)name;
        pthread_kill(th, SIGUSR2);
        usleep(50000);
    }
#endif
}

void init_context(PPCContext* c, uint32_t stack_top, int core) {
    memset(c, 0, sizeof *c);
    c->r[1] = (stack_top - 0x20) & ~0xFu;
    mem::w32(c->r[1], 0);  // back chain terminator
    c->r[2] = loader::main_module().sda2_base;
    c->r[13] = loader::main_module().sda_base;
    c->gqr[2] = 0x00040004;
    c->gqr[3] = 0x00050005;
    c->gqr[4] = 0x00060006;
    c->gqr[5] = 0x00070007;
    c->res_addr = 0xFFFFFFFFu;
    c->core = core;
}

static uint32_t new_os_thread(const char* name, uint32_t entry, uint32_t stack_top, uint32_t stack_size, int prio,
                              int core) {
    uint32_t os = mem::sys_alloc(kOSThreadSize, 8);
    mem::w32(os + T_MAGIC, 0x74487244);
    mem::w8(os + T_STATE, 2);
    mem::w16(os + T_ID, g_next_id++);
    mem::w32(os + T_EFFPRIO, prio);
    mem::w32(os + T_BASEPRIO, prio);
    mem::w32(os + T_STACKBASE, stack_top);
    mem::w32(os + T_STACKEND, stack_top - stack_size);
    mem::w32(os + T_ENTRY, entry);
    mem::w32(os + T_AFFINITY, 1u << core);
    mem::w32(os + T_NAME, mem::sys_strdup(name));
    mem::w32(os + 0x1DC, core);  // context.upir
    return os;
}

GuestThread* adopt_host_thread(const char* name, uint32_t stack_size, int core) {
    GuestThread* t = new GuestThread;
    uint32_t stack = mem::sys_alloc(stack_size, 16);
    t->name = name;
    t->core = core;
    t->started = true;
    t->suspend = 0;
    std::lock_guard<std::mutex> l(g_klock);
    t->os = new_os_thread(name, 0, stack + stack_size, stack_size, 0, core);
    init_context(&t->ctx, stack + stack_size, core);
    t->ctx.thread = t;
    g_threads[t->os] = t;
    t_current = t;
    return t;
}

void start_main(uint32_t entry, uint32_t argc, uint32_t argv, uint32_t stack_size) {
    GuestThread* t = new GuestThread;
    uint32_t stack = mem::sys_alloc(stack_size, 16);
    t->name = "main";
    t->is_main = true;
    t->core = 1;
    t->entry = entry;
    t->suspend = 0;
    {
        std::lock_guard<std::mutex> l(g_klock);
        t->os = new_os_thread("Default Thread 1", entry, stack + stack_size, stack_size, 16, 1);
        init_context(&t->ctx, stack + stack_size, 1);
        t->ctx.thread = t;
        t->ctx.r[3] = argc;
        t->ctx.r[4] = argv;
        g_threads[t->os] = t;
    }
    launch(t);
}
}  // namespace threads

extern "C" void botw_assert_fail(const char* file, int line, const char* expr) {
    static thread_local int depth = 0;
    if (depth) return;
    depth++;
    const char* show = file ? file : "?";
    if (file) {
        const char* p = strstr(file, "runtime/");
        if (!p) p = strstr(file, "runtime\\");
        if (p) show = p;
    }
    static std::mutex seen_mu;
    struct Site {
        const char* file;
        int line;
        int count;
    };
    static Site seen[48];
    static int nseen = 0;
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(seen_mu);
        int slot = -1;
        for (int i = 0; i < nseen; i++)
            if (seen[i].line == line && seen[i].file == file) {
                slot = i;
                break;
            }
        if (slot >= 0) {
            int count = ++seen[slot].count;
            depth--;
            if (count == 2 || count == 100 || count == 10000)
                LOG("assert: %s:%d repeated (%d)", show, line, count);
            return;
        }
        if (nseen >= 48) {
            depth--;
            LOG("assert: %s:%d: %s", show, line, expr ? expr : "?");
            return;
        }
        seen[nseen++] = {file, line, 1};
        first = true;
    }
    LOG("assert: %s:%d: %s", show, line, expr ? expr : "?");
    threads::host_stack_log("assert", nullptr);
    if (first) LOG("assert: continuing");
    depth--;
}

using namespace threads;

// ---- coreinit thread API ---------------------------------------------------------------------
// BOOL OSCreateThread(OSThread*, entry, argc, argv, stackTop, stackSize, priority, attr)
HLE_EXPORT(coreinit, OSCreateThread) {
    uint32_t os = ARG(0), entry = ARG(1), argc = ARG(2), argv = ARG(3), stack_top = ARG(4), stack_size = ARG(5);
    int32_t prio = (int32_t)ARG(6);
    uint32_t attr = ARG(7);
    GuestThread* me = current();
    int core = me ? me->core : 1;
    uint32_t aff = attr & 7;
    if (aff == 1) core = 0;
    else if (aff == 2) core = 1;
    else if (aff == 4) core = 2;
    GuestThread* t = new GuestThread;
    t->os = os;
    t->entry = entry;
    t->core = core;
    t->name = "thread";
    {
        std::lock_guard<std::mutex> l(g_klock);
        mem::commit(os, kOSThreadSize, true);
        memset(mem::host(os), 0, kOSThreadSize);
        mem::w32(os + T_MAGIC, 0x74487244);
        mem::w8(os + T_STATE, 1);
        mem::w8(os + T_ATTR, (uint8_t)attr);
        mem::w16(os + T_ID, g_next_id++);
        mem::w32(os + T_SUSPEND, 1);
        mem::w32(os + T_EFFPRIO, prio);
        mem::w32(os + T_BASEPRIO, prio);
        mem::w32(os + T_STACKBASE, stack_top);
        mem::w32(os + T_STACKEND, stack_top - stack_size);
        mem::w32(os + T_ENTRY, entry);
        mem::w32(os + T_AFFINITY, aff ? aff : 7);
        mem::w32(os + 0x1DC, core);
        init_context(&t->ctx, stack_top, core);
        t->ctx.thread = t;
        t->ctx.r[3] = argc;
        t->ctx.r[4] = argv;
        if (GuestThread* old = get(os)) {
            if (!old->exited) LOG("warning: OSCreateThread reuses a live OSThread %08x", os);
        }
        g_threads[os] = t;
    }
    TRACE("OSCreateThread %08x entry %08x stack %08x+%x prio %d attr %x core %d", os, entry, stack_top, stack_size,
          prio, attr, core);
    RET(1);
}

HLE_EXPORT(coreinit, OSResumeThread) {
    uint32_t os = ARG(0);
    std::lock_guard<std::mutex> l(g_klock);
    GuestThread* t = get(os);
    if (!t) {
        LOG("OSResumeThread: unknown thread %08x", os);
        RET(0);
        return;
    }
    int prev = t->suspend;
    if (t->suspend > 0) t->suspend--;
    mem::w32(os + T_SUSPEND, t->suspend);
    if (t->suspend == 0 && !t->started) {
        mem::w8(os + T_STATE, 2);
        // The creator keeps this core. The new thread blocks in run_thread until we
        // sleep, yield, or wait, so stores that follow OSResumeThread are visible
        // before it runs. Switching here races that setup.
        launch(t);
    }
    RET(prev);
}

HLE_EXPORT(coreinit, OSJoinThread) {
    uint32_t os = ARG(0), out = ARG(1);
    threads::CoreHandoff handoff;
    std::unique_lock<std::mutex> l(g_klock);
    GuestThread* t = get(os);
    if (!t) {
        RET(0);
        return;
    }
    if (!t->exited) {
        l.unlock();
        handoff.yield();
        l.lock();
        if (!t->exited) {
            threads::WaitMark w("join", os);
            t->cv.wait(l, [&] { return t->exited; });
        }
    }
    if (out) mem::w32(out, t->exit_value);
    RET(1);
}

HLE_EXPORT(coreinit, OSGetCurrentThread) { RET(current() ? current()->os : 0); }

HLE_EXPORT(coreinit, OSSetThreadName) {
    uint32_t os = ARG(0), name = ARG(1);
    mem::w32(os + T_NAME, name);
    std::lock_guard<std::mutex> l(g_klock);
    if (GuestThread* t = get(os)) {
        t->name = name ? mem::str(name) : "";
        if (t == current()) botw_set_thread_name(t->name.c_str());
    }
}

HLE_EXPORT(coreinit, OSSetThreadPriority) {
    uint32_t os = ARG(0);
    int32_t prev = (int32_t)mem::r32(os + T_BASEPRIO);
    mem::w32(os + T_BASEPRIO, ARG(1));
    mem::w32(os + T_EFFPRIO, ARG(1));
    (void)prev;
    RET(1);
}
HLE_EXPORT(coreinit, OSGetThreadPriority) { RET(mem::r32(ARG(0) + T_BASEPRIO)); }

HLE_EXPORT(coreinit, OSSetThreadAffinity) {
    uint32_t os = ARG(0), aff = ARG(1) & 7;
    mem::w32(os + T_AFFINITY, aff);
    std::lock_guard<std::mutex> l(g_klock);
    if (GuestThread* t = get(os)) {
        if (aff == 1) t->core = 0;
        else if (aff == 2) t->core = 1;
        else if (aff == 4) t->core = 2;
        t->ctx.core = t->core;
    }
    RET(1);
}
HLE_EXPORT(coreinit, OSGetThreadAffinity) { RET(mem::r32(ARG(0) + T_AFFINITY)); }

HLE_EXPORT(coreinit, OSGetThreadSpecific) {
    uint32_t i = ARG(0);
    GuestThread* t = current();
    RET(t && i < 16 ? mem::r32(t->os + T_SPECIFIC + 4 * i) : 0);
}
HLE_EXPORT(coreinit, OSSetThreadSpecific) {
    uint32_t i = ARG(0);
    GuestThread* t = current();
    if (t && i < 16) mem::w32(t->os + T_SPECIFIC + 4 * i, ARG(1));
}

HLE_EXPORT(coreinit, OSGetCoreId) { RET(current() ? current()->core : 1); }
HLE_EXPORT(coreinit, OSGetStackPointer) { RET(c->r[1]); }
HLE_EXPORT(coreinit, OSYieldThread) {
    diag_sync("yield");
    threads::exec_yield();
#if defined(_WIN32)
    SwitchToThread();
#else
    sched_yield();
#endif
    threads::exec_resume();
}
HLE_EXPORT(coreinit, OSBlockThreadsOnExit) {}

#if defined(_WIN32)
static void host_sleep_ns(uint64_t ns) {
    if (ns == 0) {
        SwitchToThread();
        return;
    }
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    thread_local HANDLE timer = [] {
        HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!t) t = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        return t;
    }();
    if (!timer) {
        Sleep((DWORD)((ns + 999999ull) / 1000000ull));
        return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)((ns + 99) / 100);  // 100 ns units, relative
    if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        Sleep((DWORD)((ns + 999999ull) / 1000000ull));
        return;
    }
    WaitForSingleObject(timer, INFINITE);
}
#endif

// void OSSleepTicks(OSTime ticks)
HLE_EXPORT(coreinit, OSSleepTicks) {
    int64_t ticks = (int64_t)ARG64(0);
    diag_sync("sleep ticks %lld", (long long)ticks);
    if (ticks <= 0) {
        threads::exec_yield();
#if defined(_WIN32)
        SwitchToThread();
#else
        sched_yield();
#endif
        threads::exec_resume();
        return;
    }
    uint64_t ns = (uint64_t)((__int128)ticks * 1000000000 / 62156250);
    threads::CoreHandoff handoff;
    handoff.yield();
    threads::WaitMark w("sleep", (uint32_t)(ns / 1000));
#if defined(_WIN32)
    host_sleep_ns(ns);
#else
    struct timespec ts = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)};
    nanosleep(&ts, nullptr);
#endif
}

HLE_EXPORT(coreinit, OSInitThreadQueue) {
    uint32_t q = ARG(0);
    mem::w32(q, 0);
    mem::w32(q + 4, 0);
    mem::w32(q + 8, 0);
    mem::w32(q + 12, 0);
}

// Cemu-compat: guest callback on the calling guest thread.
uint32_t cemu_core_callback(uint32_t fn, std::initializer_list<uint32_t> args) {
    threads::GuestThread* t = threads::current();
    if (!t) {
        LOG("cemu_core_callback(%08x) from a non-guest thread", fn);
        return 0;
    }
    return guest_call(&t->ctx, fn, args);
}
