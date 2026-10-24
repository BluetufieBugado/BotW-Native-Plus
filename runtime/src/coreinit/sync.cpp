// coreinit synchronization: OSMutex, OSFastMutex, OSEvent, OSSemaphore, OSRendezvous,
// OSMessageQueue and the GHS C-library locks. State lives in the guest structs where games may
// look; waiting uses per-object host condition variables under threads::g_klock.
#include <chrono>
#include <condition_variable>
#include <stdlib.h>

#include "diag.h"
#include "hle.h"
#include "thread.h"

using threads::g_klock;
using threads::GuestThread;

namespace {
// Power-of-two open-addressed flat hash table with a 64-entry direct-mapped MRU front cache.
// All accesses are guarded by threads::g_klock.
template <typename V>
struct FlatSyncMap {
    struct Slot {
        uint32_t key;
        V* val;
    };
    Slot* slots = nullptr;
    uint32_t mask = 0;
    uint32_t used = 0;
    Slot mru0{0, nullptr};
    Slot mru1{0, nullptr};

    static inline uint32_t hash_key(uint32_t k) {
        k ^= k >> 16;
        k *= 0x7feb352du;
        k ^= k >> 15;
        return k;
    }

    __attribute__((noinline)) void grow() {
        uint32_t new_cap = mask ? (mask + 1u) * 2u : 4096u;
        uint32_t new_mask = new_cap - 1u;
        Slot* ns = (Slot*)calloc(new_cap, sizeof(Slot));
        if (slots) {
            uint32_t old_cap = mask + 1u;
            for (uint32_t i = 0; i < old_cap; i++) {
                if (slots[i].key) {
                    uint32_t idx = hash_key(slots[i].key) & new_mask;
                    while (ns[idx].key) idx = (idx + 1u) & new_mask;
                    ns[idx] = slots[i];
                }
            }
            free(slots);
        }
        slots = ns;
        mask = new_mask;
    }

    inline V* find_existing(uint32_t k) {
        if (mru0.key == k && mru0.val) return mru0.val;
        if (mru1.key == k && mru1.val) return mru1.val;
        if (!slots) return nullptr;
        uint32_t idx = hash_key(k) & mask;
        for (;;) {
            uint32_t sk = slots[idx].key;
            if (sk == k) {
                V* v = slots[idx].val;
                mru1 = mru0;
                mru0 = {k, v};
                return v;
            }
            if (sk == 0) return nullptr;
            idx = (idx + 1u) & mask;
        }
    }

    __attribute__((noinline)) V& insert_new(uint32_t k) {
        if (!slots || (used + 1u) * 2u > mask + 1u) grow();
        uint32_t idx = hash_key(k) & mask;
        while (slots[idx].key) {
            if (slots[idx].key == k) {
                V* v = slots[idx].val;
                mru1 = mru0;
                mru0 = {k, v};
                return *v;
            }
            idx = (idx + 1u) & mask;
        }
        V* v = new V();
        slots[idx].key = k;
        slots[idx].val = v;
        used++;
        mru1 = mru0;
        mru0 = {k, v};
        return *v;
    }

    inline V& get(uint32_t k) {
        if (mru0.key == k && mru0.val) return *mru0.val;
        if (mru1.key == k && mru1.val) {
            Slot tmp = mru0;
            mru0 = mru1;
            mru1 = tmp;
            return *mru0.val;
        }
        if (__builtin_expect(slots != nullptr, 1)) {
            uint32_t idx = hash_key(k) & mask;
            for (;;) {
                uint32_t sk = slots[idx].key;
                if (__builtin_expect(sk == k, 1)) {
                    V* v = slots[idx].val;
                    mru1 = mru0;
                    mru0 = {k, v};
                    return *v;
                }
                if (sk == 0) break;
                idx = (idx + 1u) & mask;
            }
        }
        return insert_new(k);
    }
};

struct SyncCV {
    std::condition_variable cv;
    int waiters = 0;
};
FlatSyncMap<SyncCV> g_cvs;

struct WaiterScope {
    int& count;
    WaiterScope(int& c) : count(c) { ++count; }
    ~WaiterScope() { --count; }
};

inline void notify_one_if_waiting(uint32_t obj) {
    if (SyncCV* s = g_cvs.find_existing(obj)) {
        if (s->waiters > 0) s->cv.notify_one();
    }
}

inline void notify_all_if_waiting(uint32_t obj) {
    if (SyncCV* s = g_cvs.find_existing(obj)) {
        if (s->waiters > 0) s->cv.notify_all();
    }
}

// ---- recursive mutexes (OSMutex and OSFastMutex share the implementation) ----------------
struct MutexState {
    GuestThread* owner = nullptr;
    int count = 0;
    int waiters = 0;
    std::condition_variable cv;
};
FlatSyncMap<MutexState> g_mutexes;

void mutex_lock(uint32_t m) {
    GuestThread* me = threads::current();
    threads::CoreHandoff handoff;
    std::unique_lock<std::mutex> l(g_klock);
    MutexState& s = g_mutexes.get(m);
    for (;;) {
        if (!s.owner || s.owner == me) {
            s.owner = me;
            s.count++;
            return;
        }
        l.unlock();
        handoff.yield();
        l.lock();
        if (!s.owner || s.owner == me) continue;
        threads::WaitMark w("mutex", m);
        WaiterScope ws(s.waiters);
        s.cv.wait(l, [&] { return s.owner == nullptr; });
    }
}
bool mutex_trylock(uint32_t m) {
    GuestThread* me = threads::current();
    std::lock_guard<std::mutex> l(g_klock);
    MutexState& s = g_mutexes.get(m);
    if (s.owner && s.owner != me) return false;
    s.owner = me;
    s.count++;
    return true;
}
void mutex_unlock(uint32_t m) {
    GuestThread* me = threads::current();
    std::lock_guard<std::mutex> l(g_klock);
    MutexState& s = g_mutexes.get(m);
    if (__builtin_expect(s.owner != me, 0)) {
        LOG("mutex %08x unlocked by a non-owner (%s)", m, me ? me->name.c_str() : "?");
        return;
    }
    if (--s.count == 0) {
        s.owner = nullptr;
        if (__builtin_expect(s.waiters > 0, 0)) s.cv.notify_one();
    }
}
void mutex_init(uint32_t m) {
    std::lock_guard<std::mutex> l(g_klock);
    MutexState& s = g_mutexes.get(m);
    s.owner = nullptr;
    s.count = 0;
}
}  // namespace

HLE_EXPORT(coreinit, OSInitMutex) {
    uint32_t m = ARG(0);
    mem::commit(m, 0x2C, true);
    memset(mem::host(m), 0, 0x2C);
    mem::w32(m, 0x6D557458);  // 'mUtX'
    mutex_init(m);
}
HLE_EXPORT(coreinit, OSLockMutex) { mutex_lock(ARG(0)); }
HLE_EXPORT(coreinit, OSTryLockMutex) { RET(mutex_trylock(ARG(0))); }
HLE_EXPORT(coreinit, OSUnlockMutex) { mutex_unlock(ARG(0)); }
HLE_EXPORT(coreinit, OSFastMutex_Init) {
    mem::commit(ARG(0), 0x2C, true);
    memset(mem::host(ARG(0)), 0, 0x2C);
    mem::w32(ARG(0), 0x664D7458);  // 'fMtX'
    mutex_init(ARG(0));
}
HLE_EXPORT(coreinit, OSFastMutex_Lock) { mutex_lock(ARG(0)); }
HLE_EXPORT(coreinit, OSFastMutex_TryLock) { RET(mutex_trylock(ARG(0))); }
HLE_EXPORT(coreinit, OSFastMutex_Unlock) { mutex_unlock(ARG(0)); }

// GHS C/C++ runtime locks: one global recursive lock plus mutexes allocated per object.
HLE_EXPORT(coreinit, __ghsLock) { mutex_lock(0xFFFF0001u); }
HLE_EXPORT(coreinit, __ghsUnlock) { mutex_unlock(0xFFFF0001u); }
HLE_EXPORT(coreinit, __ghs_flock_file) { mutex_lock(0xFFFE0000u + (ARG(0) & 0xFFFF)); }
HLE_EXPORT(coreinit, __ghs_funlock_file) { mutex_unlock(0xFFFE0000u + (ARG(0) & 0xFFFF)); }
// __ghs_mtx_init(void** m): allocate; lock/unlock(void* m); dst(void** m)
HLE_EXPORT(coreinit, __ghs_mtx_init) {
    uint32_t m = mem::sys_alloc(0x2C, 8);
    mutex_init(m);
    mem::w32(ARG(0), m);
}
HLE_EXPORT(coreinit, __ghs_mtx_dst) {}
HLE_EXPORT(coreinit, __ghs_mtx_lock) { mutex_lock(ARG(0)); }
HLE_EXPORT(coreinit, __ghs_mtx_unlock) { mutex_unlock(ARG(0)); }

// ---- events: +0x0C value, +0x20 mode (0 manual reset, 1 auto reset) ----------------------
HLE_EXPORT(coreinit, OSInitEvent) {
    uint32_t e = ARG(0);
    mem::commit(e, 0x24, true);
    memset(mem::host(e), 0, 0x24);
    mem::w32(e, 0x65566E54);  // 'eVnT'
    mem::w32(e + 0x0C, ARG(1));
    mem::w32(e + 0x20, ARG(2));
}
HLE_EXPORT(coreinit, OSSignalEvent) {
    uint32_t e = ARG(0);
    std::lock_guard<std::mutex> l(g_klock);
    mem::w32(e + 0x0C, 1);
    if (mem::r32(e + 0x20) == 1) notify_one_if_waiting(e);
    else notify_all_if_waiting(e);
}
HLE_EXPORT(coreinit, OSResetEvent) {
    std::lock_guard<std::mutex> l(g_klock);
    mem::w32(ARG(0) + 0x0C, 0);
}
HLE_EXPORT(coreinit, OSWaitEvent) {
    uint32_t e = ARG(0);
    threads::CoreHandoff handoff;
    std::unique_lock<std::mutex> l(g_klock);
    if (mem::r32(e + 0x0C) == 0) {
        diag_sync("event wait %08x mode %u (clear)", e, mem::r32(e + 0x20));
        l.unlock();
        handoff.yield();
        l.lock();
        if (mem::r32(e + 0x0C) == 0) {
            threads::WaitMark w("event", e);
            SyncCV& scv = g_cvs.get(e);
            WaiterScope ws(scv.waiters);
            scv.cv.wait(l, [&] { return mem::r32(e + 0x0C) != 0; });
        }
        diag_sync("event wake %08x value %u", e, mem::r32(e + 0x0C));
    }
    if (mem::r32(e + 0x20) == 1) mem::w32(e + 0x0C, 0);
}
// BOOL OSWaitEventWithTimeout(OSEvent*, OSTime timeout_ns)
HLE_EXPORT(coreinit, OSWaitEventWithTimeout) {
    uint32_t e = ARG(0);
    // Cafe passes OSTime ticks. This runtime has always treated the value as
    // nanoseconds (see the historical comment); keep that so a timed wait does
    // not get 16x longer on the platforms that already boot.
    uint64_t ns = ARG64(2);
    threads::CoreHandoff handoff;
    std::unique_lock<std::mutex> l(g_klock);
    bool ok = mem::r32(e + 0x0C) != 0;
    if (!ok) {
        diag_sync("event timeout %08x raw %llu treated as ns", e, (unsigned long long)ns);
        l.unlock();
        handoff.yield();
        l.lock();
        ok = mem::r32(e + 0x0C) != 0;
        if (!ok) {
            threads::WaitMark w("event/timeout", e);
            SyncCV& scv = g_cvs.get(e);
            WaiterScope ws(scv.waiters);
            ok = scv.cv.wait_for(l, std::chrono::nanoseconds(ns), [&] { return mem::r32(e + 0x0C) != 0; });
        }
        diag_sync("event timeout %08x -> %d", e, (int)ok);
    }
    if (ok && mem::r32(e + 0x20) == 1) mem::w32(e + 0x0C, 0);
    RET(ok);
}

// ---- semaphores: +0x0C count -----------------------------------------------------------------
HLE_EXPORT(coreinit, OSInitSemaphore) {
    uint32_t s = ARG(0);
    mem::commit(s, 0x20, true);
    memset(mem::host(s), 0, 0x20);
    mem::w32(s, 0x73506852);  // 'sPhR'
    mem::w32(s + 0x0C, ARG(1));
}
HLE_EXPORT(coreinit, OSWaitSemaphore) {
    uint32_t s = ARG(0);
    threads::CoreHandoff handoff;
    std::unique_lock<std::mutex> l(g_klock);
    if ((int32_t)mem::r32(s + 0x0C) <= 0) {
        l.unlock();
        handoff.yield();
        l.lock();
        if ((int32_t)mem::r32(s + 0x0C) <= 0) {
            threads::WaitMark w("semaphore", s);
            SyncCV& scv = g_cvs.get(s);
            WaiterScope ws(scv.waiters);
            scv.cv.wait(l, [&] { return (int32_t)mem::r32(s + 0x0C) > 0; });
        }
    }
    int32_t v = (int32_t)mem::r32(s + 0x0C);
    mem::w32(s + 0x0C, v - 1);
    RET(v);
}
HLE_EXPORT(coreinit, OSSignalSemaphore) {
    uint32_t s = ARG(0);
    std::lock_guard<std::mutex> l(g_klock);
    int32_t v = (int32_t)mem::r32(s + 0x0C);
    mem::w32(s + 0x0C, v + 1);
    notify_one_if_waiting(s);
    RET(v);
}
HLE_EXPORT(coreinit, OSGetSemaphoreCount) {
    std::lock_guard<std::mutex> l(g_klock);
    RET(mem::r32(ARG(0) + 0x0C));
}

// ---- rendezvous: core[3] arrival flags ---------------------------------------------------------
HLE_EXPORT(coreinit, OSInitRendezvous) {
    mem::commit(ARG(0), 0x10, true);
    memset(mem::host(ARG(0)), 0, 0x10);
}
// BOOL OSWaitRendezvous(OSRendezvous*, uint32_t coreMask)
HLE_EXPORT(coreinit, OSWaitRendezvous) {
    uint32_t r = ARG(0), mask = ARG(1);
    int core = c->core;
    auto ready = [&] {
        for (int k = 0; k < 3; k++)
            if ((mask & (1u << k)) && !mem::r32(r + 4 * k)) return false;
        return true;
    };
    threads::CoreHandoff handoff;
    std::unique_lock<std::mutex> l(g_klock);
    mem::w32(r + 4 * core, 1);
    notify_all_if_waiting(r);
    if (!ready()) {
        l.unlock();
        handoff.yield();
        l.lock();
        if (!ready()) {
            threads::WaitMark w("rendezvous", r);
            SyncCV& scv = g_cvs.get(r);
            WaiterScope ws(scv.waiters);
            scv.cv.wait(l, ready);
        }
    }
    RET(1);
}

// ---- message queues --------------------------------------------------------------------------
// +0x2C messages, +0x30 capacity, +0x34 first, +0x38 used; message = 16 bytes
namespace {
constexpr uint32_t Q_MSGS = 0x2C, Q_CAP = 0x30, Q_FIRST = 0x34, Q_USED = 0x38;
}
HLE_EXPORT(coreinit, OSInitMessageQueue) {
    uint32_t q = ARG(0), msgs = ARG(1), cap = ARG(2);
    mem::commit(q, 0x3C, true);
    if (msgs && cap) mem::commit(msgs, (size_t)cap * 16, true);
    memset(mem::host(q), 0, 0x3C);
    mem::w32(q, 0x6D536751);  // 'mSgQ'
    mem::w32(q + Q_MSGS, msgs);
    mem::w32(q + Q_CAP, cap);
}
// BOOL OSSendMessage(q, msg, flags): flags 1 = block, 2 = jam (front)
HLE_EXPORT(coreinit, OSSendMessage) {
    uint32_t q = ARG(0), msg = ARG(1), flags = ARG(2);
    threads::CoreHandoff handoff;
    std::unique_lock<std::mutex> l(g_klock);
    uint32_t cap = mem::r32(q + Q_CAP);
    if (cap == 0) {
        diag_sync("msg send q %08x flags %x cap 0", q, flags);
        RET(0);
        return;
    }
    if (mem::r32(q + Q_USED) >= cap) {
        if (!(flags & 1)) {
            diag_sync("msg send q %08x flags %x used %u cap %u full", q, flags, mem::r32(q + Q_USED), cap);
            RET(0);
            return;
        }
        l.unlock();
        handoff.yield();
        l.lock();
        cap = mem::r32(q + Q_CAP);
        if (mem::r32(q + Q_USED) >= cap) {
            threads::WaitMark w("msgq send", q);
            SyncCV& scv = g_cvs.get(q);
            WaiterScope ws(scv.waiters);
            scv.cv.wait(l, [&] { return mem::r32(q + Q_USED) < mem::r32(q + Q_CAP); });
        }
    }
    uint32_t first = mem::r32(q + Q_FIRST), used = mem::r32(q + Q_USED);
    uint32_t slot;
    if (flags & 2) {
        first = (first + cap - 1) % cap;
        mem::w32(q + Q_FIRST, first);
        slot = first;
    } else {
        slot = (first + used) % cap;
    }
    uint32_t slot_addr = mem::r32(q + Q_MSGS) + slot * 16;
    mem::commit(slot_addr, 16, true);
    mem::commit(msg, 16, false);
    memcpy(mem::host(slot_addr), mem::host(msg), 16);
    mem::w32(q + Q_USED, used + 1);
    notify_all_if_waiting(q);
    diag_sync("msg send q %08x flags %x slot %u used %u cap %u word %08x", q, flags, slot, used + 1, cap, mem::r32(msg));
    RET(1);
}
// BOOL OSReceiveMessage(q, msg, flags): flags 1 = block
HLE_EXPORT(coreinit, OSReceiveMessage) {
    uint32_t q = ARG(0), msg = ARG(1), flags = ARG(2);
    threads::CoreHandoff handoff;
    std::unique_lock<std::mutex> l(g_klock);
    if (mem::r32(q + Q_USED) == 0) {
        if (!(flags & 1)) {
            diag_sync("msg recv q %08x flags %x used 0 cap %u empty", q, flags, mem::r32(q + Q_CAP));
            RET(0);
            return;
        }
        diag_sync("msg recv q %08x flags %x used 0 cap %u block", q, flags, mem::r32(q + Q_CAP));
        l.unlock();
        handoff.yield();
        l.lock();
        if (mem::r32(q + Q_USED) == 0) {
            threads::WaitMark w("msgq receive", q);
            SyncCV& scv = g_cvs.get(q);
            WaiterScope ws(scv.waiters);
            scv.cv.wait(l, [&] { return mem::r32(q + Q_USED) != 0; });
        }
    }
    uint32_t first = mem::r32(q + Q_FIRST), cap = mem::r32(q + Q_CAP);
    if (cap == 0) {
        diag_sync("msg recv q %08x cap 0 used %u", q, mem::r32(q + Q_USED));
        RET(0);
        return;
    }
    uint32_t slot_addr = mem::r32(q + Q_MSGS) + first * 16;
    mem::commit(msg, 16, true);
    mem::commit(slot_addr, 16, false);
    memcpy(mem::host(msg), mem::host(slot_addr), 16);
    diag_sync("msg recv q %08x flags %x used %u cap %u word %08x", q, flags, mem::r32(q + Q_USED), cap, mem::r32(msg));
    mem::w32(q + Q_FIRST, (first + 1) % cap);
    mem::w32(q + Q_USED, mem::r32(q + Q_USED) - 1);
    notify_all_if_waiting(q);
    RET(1);
}
HLE_EXPORT(coreinit, OSPeekMessage) {
    uint32_t q = ARG(0), msg = ARG(1);
    std::lock_guard<std::mutex> l(g_klock);
    if (mem::r32(q + Q_USED) == 0) {
        RET(0);
        return;
    }
    if (msg) {
        uint32_t slot_addr = mem::r32(q + Q_MSGS) + mem::r32(q + Q_FIRST) * 16;
        mem::commit(msg, 16, true);
        mem::commit(slot_addr, 16, false);
        memcpy(mem::host(msg), mem::host(slot_addr), 16);
    }
    RET(1);
}
