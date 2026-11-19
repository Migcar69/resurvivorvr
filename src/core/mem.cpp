// Guest memory bus: 2MB RAM (mirrored), 1KB scratchpad, I/O, BIOS area (HLE, reads zero).
#include "psx.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace psx {

Machine M;

static inline u32 phys(u32 a) { return a & 0x1fffffff; }

u8 *ram_ptr(u32 addr) { return M.ram + (phys(addr) & (RAM_SIZE - 1)); }

u32 g_rwatch_lo = 1, g_rwatch_hi = 0;  // debug read watchpoint range (physical)
bool g_rwatch_on = false;

template <typename T>
static inline T rd(u32 addr) {
    u32 p = phys(addr);
    if (g_rwatch_on && p >= g_rwatch_lo && p <= g_rwatch_hi) {
        T v;
        memcpy(&v, M.ram + (p & (RAM_SIZE - 1)), sizeof(T));
        logf("rwatch: pc=%08x [%08x] -> %08x (size %d)", M.cpu.pc, addr, (u32)v, (int)sizeof(T));
    }
    if (p < 0x00800000) {
        T v;
        memcpy(&v, M.ram + (p & (RAM_SIZE - 1)), sizeof(T));
        return v;
    }
    if ((p & 0xfffffc00) == 0x1f800000) {
        T v;
        memcpy(&v, M.scratch + (p & 0x3ff), sizeof(T));
        return v;
    }
    if (p >= 0x1f801000 && p < 0x1f803000) return (T)io_read(p, sizeof(T));
    if (p >= 0x1f000000 && p < 0x1f800000) return (T)~0u;  // expansion 1: nothing connected
    return 0;  // BIOS ROM / cache control
}

u32 g_watch_lo = 1, g_watch_hi = 0;  // debug write watchpoint range (physical)

// Undo log for trial runs of game code: every RAM / scratchpad write records the previous contents.
struct UndoEntry {
    u8 *at;
    u32 old;
    u8 size;
};
static std::vector<UndoEntry> undo_log;
static bool undo_on = false;

void mem_undo_begin() {
    undo_log.clear();
    undo_on = true;
}

void mem_undo_restore() {
    undo_on = false;
    for (size_t i = undo_log.size(); i-- > 0;) memcpy(undo_log[i].at, &undo_log[i].old, undo_log[i].size);
    undo_log.clear();
}

static u32 ring_lo = 1, ring_hi = 0;
static WriteRec wring[64];
static int wring_n = 0;
void write_ring_watch(u32 lo, u32 hi) { ring_lo = lo & 0x1fffffff; ring_hi = hi & 0x1fffffff; wring_n = 0; }
int write_ring_get(WriteRec *out, int max) {
    int n = std::min(wring_n, std::min(max, 64));
    for (int i = 0; i < n; i++) out[i] = wring[(wring_n - n + i) & 63];
    return n;
}

template <typename T>
static inline void wr(u32 addr, T v) {
    if ((addr & 0x1fffffff) >= ring_lo && (addr & 0x1fffffff) <= ring_hi) {
        WriteRec &w = wring[wring_n++ & 63];
        w = {M.cpu.pc, M.cpu.r[31], addr, (u32)v, (u8)sizeof(T), (u8)(g_in_cpu_call > 0), M.cycles / CYCLES_PER_FRAME};
    }
    if ((addr & 0x1fffffff) >= g_watch_lo && (addr & 0x1fffffff) <= g_watch_hi)
        logf("watch: pc=%08x [%08x] <- %08x (size %d) ra=%08x", M.cpu.pc, addr, (u32)v, (int)sizeof(T), M.cpu.r[31]);
    // Cache isolation (SR.IsC) is used by the BIOS to flush the i-cache; ignore such writes.
    if (M.cpu.sr & 0x10000) return;
    u32 p = phys(addr);
    if (p < 0x00800000) {
        u8 *at = M.ram + (p & (RAM_SIZE - 1));
        if (undo_on) { UndoEntry e = {at, 0, (u8)sizeof(T)}; memcpy(&e.old, at, sizeof(T)); undo_log.push_back(e); }
        memcpy(at, &v, sizeof(T));
        return;
    }
    if ((p & 0xfffffc00) == 0x1f800000) {
        u8 *at = M.scratch + (p & 0x3ff);
        if (undo_on) { UndoEntry e = {at, 0, (u8)sizeof(T)}; memcpy(&e.old, at, sizeof(T)); undo_log.push_back(e); }
        memcpy(at, &v, sizeof(T));
        return;
    }
    if (p >= 0x1f801000 && p < 0x1f803000) io_write(p, (u32)v, sizeof(T));
}

u8 read8(u32 a) { return rd<u8>(a); }
u16 read16(u32 a) { return rd<u16>(a); }
u32 read32(u32 a) { return rd<u32>(a); }
void write8(u32 a, u8 v) { wr<u8>(a, v); }
void write16(u32 a, u16 v) { wr<u16>(a, v); }
void write32(u32 a, u32 v) { wr<u32>(a, v); }

}  // namespace psx

u32 g_watch_lo_pub(u32 lo, u32 hi) {
    psx::g_watch_lo = lo;
    psx::g_watch_hi = hi;
    return 0;
}

u32 g_rwatch_pub(u32 lo, u32 hi) {
    psx::g_rwatch_lo = lo;
    psx::g_rwatch_hi = hi;
    psx::g_rwatch_on = true;
    return 0;
}
