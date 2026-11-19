// High-level emulation of the PS1 kernel (BIOS) services used by the game.
#include "psx.h"
#include "state.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <string>

namespace psx {
bool g_log_bios = false;

// (declared in psx.h)
void memcard_init(const std::string &path);
int mc_open(const std::string &name, u32 mode);
int mc_read(int fd, u32 dst, u32 len);
int mc_write(int fd, u32 src, u32 len);
int mc_seek(int fd, s32 off, int whence);
int mc_close(int fd);
bool mc_is_async(int fd);
u32 mc_firstfile(const std::string &pattern, u32 dirent);
u32 mc_nextfile(u32 dirent);
bool mc_erase(const std::string &name);
bool mc_rename(const std::string &a, const std::string &b);
void mc_format();
void mc_raw_read(int sector, u32 dst);
void mc_raw_write(int sector, u32 src);
void pad_bios_vblank(u32 dest);
void pad_bios_initpad_vblank(u32 buf1, u32 buf2);

#define R M.cpu.r
enum { V0 = 2, V1, A0, A1, A2, A3, T0 = 8, T1 = 9, SP = 29, RA = 31 };

static std::string guest_str(u32 a, size_t max = 256) {
    std::string s;
    for (size_t i = 0; i < max; i++) {
        char ch = (char)read8(a + (u32)i);
        if (!ch) break;
        s += ch;
    }
    return s;
}

// ---------------------------------------------------------------- tables
// The kernel function tables live in RAM in a real system and some libraries patch them.
// We fill them with sentinels; a patched entry makes us jump to guest code instead.
constexpr u32 TBL_A = 0x200, TBL_B = 0x874, TBL_C = 0x674;
constexpr u32 KERNEL_STACK = 0x8000c000;  // interrupt handlers' stack (grows down into 0x1000..0xbfff, unused)
static u32 sentinel(int t, int n) { return 0xbfc10000u + (u32)t * 0x400 + (u32)n * 4; }

// ---------------------------------------------------------------- events
struct Event {
    u32 cls, spec, mode, func;
    u32 status;  // 0 = free, 0x1000 disabled, 0x2000 enabled, 0x4000 ready
};
static Event events[64];

static u32 open_event(u32 cls, u32 spec, u32 mode, u32 func) {
    for (int i = 0; i < 64; i++) {
        if (events[i].status == 0) {
            events[i] = {cls, spec, mode, func, 0x1000};
            return 0xf1000000u | i;
        }
    }
    return 0xffffffff;
}

static Event *ev(u32 h) {
    u32 i = h & 0xffff;
    if ((h & 0xffff0000u) != 0xf1000000u || i >= 64 || events[i].status == 0) return nullptr;
    return &events[i];
}

void deliver_event(u32 cls, u32 spec) {
    for (auto &e : events) {
        if (e.status != 0x2000 || e.cls != cls || e.spec != spec) continue;
        if (e.mode == 0x2000) e.status = 0x4000;
        else if (e.mode == 0x1000 && e.func) cpu_call(e.func, 0, 0);
    }
}

// Events requested by asynchronous operations, delivered on the next interrupt.
static std::vector<std::pair<u32, u32>> deferred_events;
void defer_event(u32 cls, u32 spec) { deferred_events.push_back({cls, spec}); }

// ---------------------------------------------------------------- heap
static u32 heap_base = 0, heap_size = 0;
static std::map<u32, u32> heap_used;  // addr -> size

static u32 do_malloc(u32 size) {
    size = (size + 7) & ~7u;
    u32 a = heap_base;
    for (auto &kv : heap_used) {
        if (kv.first >= a + size) break;
        a = std::max(a, kv.first + kv.second);
    }
    if (a + size > heap_base + heap_size) {
        logf("malloc(%u) failed", size);
        return 0;
    }
    heap_used[a] = size;
    return a;
}

static void do_free(u32 a) { heap_used.erase(a); }

// ---------------------------------------------------------------- interrupt state
static u32 hook_jmpbuf = 0;   // SetCustomExitFromException
static bool clear_pad = true;
static bool clear_rcnt[4] = {true, true, true, true};
static u32 pad_dest = 0;      // OutdatedPadInitAndStart buffer
static u32 initpad_buf[2] = {0, 0};  // InitPad(buf1, siz1, buf2, siz2): raw controller replies per port
static bool pad_started = false;
static Cpu saved_ctx;
static bool in_exception = false;

static void return_from_exception() {
    Cpu &c = M.cpu;
    u32 sr = saved_ctx.sr;
    u32 epc = saved_ctx.epc;
    c = saved_ctx;
    c.sr = (sr & ~0xfu) | ((sr >> 2) & 0xf);  // rfe
    c.pc = epc;
    in_exception = false;
}

static void run_intrp_chains();
void bios_exception() {
    Cpu &c = M.cpu;
    u32 code = (c.cause >> 2) & 31;
    if (code == 8) {  // syscall
        u32 fn = c.r[A0];
        u32 sr = (c.sr & ~0xfu) | ((c.sr >> 2) & 0xf);  // as after rfe
        if (fn == 1) {  // EnterCriticalSection
            c.r[V0] = (sr & 0x401) == 0x401;
            sr &= ~0x401u;
        } else if (fn == 2) {  // ExitCriticalSection
            sr |= 0x401;
        } else if (fn != 0) {
            logf("syscall %u", fn);
        }
        c.sr = sr;
        c.pc = c.epc + 4;
        return;
    }
    if (code != 0) {
        logf("exception %u at %08x", code, c.epc);
        c.sr = (c.sr & ~0xfu) | ((c.sr >> 2) & 0xf);
        c.pc = c.epc + 4;
        return;
    }
    // Hardware interrupt
    saved_ctx = c;
    in_exception = true;
    // The handlers run on the BIOS's own stack, as on the PS1 (in the kernel's first 64 KB, unused by the HLE): on the
    // interrupted code's stack, a game that had moved its stack into the 1 KB scratchpad (Resident Evil Survivor's
    // heavy rooms) had the handlers' frames pushed below 0x1f800000, where nothing is stored: a handler read back its
    // return address as 0xffffffff and the game ran into nowhere (the crashes after the lift). The interrupted
    // registers come back with return_from_exception.
    c.r[SP] = KERNEL_STACK;
    u32 pend = M.i_stat & M.i_mask;
    run_intrp_chains();

    if (pend & (1u << IRQ_VBLANK)) {
        if (pad_started && pad_dest) pad_bios_vblank(pad_dest);
        if (pad_started && initpad_buf[0]) pad_bios_initpad_vblank(initpad_buf[0], initpad_buf[1]);
        deliver_event(0xf2000003, 2);
        if (clear_pad) M.i_stat &= ~(1u << IRQ_VBLANK);
        auto evs = deferred_events;
        deferred_events.clear();
        for (auto &e : evs) deliver_event(e.first, e.second);
    }
    for (int t = 0; t < 3; t++) {
        if (pend & (1u << (IRQ_TMR0 + t))) {
            deliver_event(0xf2000000u + t, 2);
            if (clear_rcnt[t]) M.i_stat &= ~(1u << (IRQ_TMR0 + t));
        }
    }

    if (hook_jmpbuf) {
        // longjmp into the hook buffer; the handler ends with ReturnFromException
        u32 b = hook_jmpbuf;
        c.pc = read32(b + 0);
        c.r[SP] = read32(b + 4);
        c.r[30] = read32(b + 8);
        for (int i = 0; i < 8; i++) c.r[16 + i] = read32(b + 12 + i * 4);
        c.r[28] = read32(b + 44);
        c.r[V0] = 1;
        return;
    }
    return_from_exception();
}

// ---------------------------------------------------------------- printf
static std::string guest_printf(u32 fmt_addr, int first_arg) {
    std::string fmt = guest_str(fmt_addr, 1024), out;
    int argn = first_arg;
    auto arg = [&]() -> u32 {
        u32 v = argn < 4 ? R[A0 + argn] : read32(R[SP] + argn * 4);
        argn++;
        return v;
    };
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') { out += fmt[i]; continue; }
        std::string spec = "%";
        i++;
        while (i < fmt.size() && strchr("-+ #0123456789.lh", fmt[i])) {
            if (fmt[i] != 'l' && fmt[i] != 'h') spec += fmt[i];
            i++;
        }
        if (i >= fmt.size()) break;
        char cv = fmt[i];
        char buf[512];
        if (cv == 's') { spec += 's'; snprintf(buf, sizeof buf, spec.c_str(), guest_str(arg()).c_str()); }
        else if (cv == '%') { buf[0] = '%'; buf[1] = 0; }
        else { spec += cv; snprintf(buf, sizeof buf, spec.c_str(), arg()); }
        out += buf;
    }
    return out;
}

// ---------------------------------------------------------------- dispatch
static void ret(u32 v) { R[V0] = v; }

// Exec: the executable returns to a small trampoline in kernel RAM that calls A(0xFE) (internal: back to Exec's caller)
constexpr u32 EXEC_RETURN = 0x80000600;
static std::vector<u32> exec_stack;
static void exec_return() {
    if (exec_stack.empty()) { logf("Exec return without Exec"); return; }
    u32 h = exec_stack.back();
    exec_stack.pop_back();
    R[RA] = read32(h + 0x28); R[SP] = read32(h + 0x2c); R[30] = read32(h + 0x30);
    R[28] = read32(h + 0x34); R[16] = read32(h + 0x38);
    R[V0] = 1;
    M.cpu.pc = R[RA];
}

// Threads (OpenThread / ChangeThread): each one keeps a full register set; thread 0 is the boot thread.
struct Thread { bool used; Cpu regs; };
static Thread threads[16];
static int cur_thread = 0;
static void change_thread(u32 h) {
    u32 i = h & 0xffff;
    if ((h & 0xffff0000u) != 0xff000000u || i >= 16 || !threads[i].used) { R[V0] = 0; M.cpu.pc = R[RA]; return; }
    Cpu &c = M.cpu;
    threads[cur_thread].regs = c;
    threads[cur_thread].regs.r[V0] = 1;
    threads[cur_thread].regs.pc = c.r[RA];
    u32 sr = c.sr;
    cur_thread = (int)i;
    c = threads[i].regs;
    c.sr = sr;
}

// Interrupt handler chains (SysEnqIntRP): {next, func2, func1, pad}; func1 tests, func2 handles.
static u32 intrp_head[4] = {0, 0, 0, 0};
static void run_intrp_chains() {
    for (int p = 0; p < 4; p++) {
        int guard = 0;
        for (u32 e = intrp_head[p]; e && guard < 32; e = read32(e), guard++) {
            u32 f1 = read32(e + 8), f2 = read32(e + 4);
            u32 r = 1;
            if (f1) { cpu_call(f1, 0, 0); r = R[V0]; }
            if (r && f2) cpu_call(f2, r, 0);
        }
    }
}

static u32 rand_seed = 0x24040001;

static void bios_a(u32 n) {
    switch (n) {
    case 0x13: {  // setjmp
        u32 b = R[A0];
        write32(b, R[RA]); write32(b + 4, R[SP]); write32(b + 8, R[30]);
        for (int i = 0; i < 8; i++) write32(b + 12 + i * 4, R[16 + i]);
        write32(b + 44, R[28]);
        ret(0);
        break;
    }
    case 0x14: {  // longjmp
        u32 b = R[A0];
        R[RA] = read32(b); R[SP] = read32(b + 4); R[30] = read32(b + 8);
        for (int i = 0; i < 8; i++) R[16 + i] = read32(b + 12 + i * 4);
        R[28] = read32(b + 44);
        ret(R[A1]);
        break;
    }
    case 0x17: {  // strcmp
        std::string a = guest_str(R[A0], 4096), b = guest_str(R[A1], 4096);
        int r = strcmp(a.c_str(), b.c_str());
        ret((u32)(r < 0 ? -1 : r > 0 ? 1 : 0));
        break;
    }
    case 0x19: {  // strcpy
        u32 d = R[A0], s = R[A1];
        u8 ch;
        do { ch = read8(s++); write8(d++, ch); } while (ch);
        ret(R[A0]);
        break;
    }
    case 0x1b: ret((u32)guest_str(R[A0], 1 << 20).size()); break;  // strlen
    case 0x25: ret((u32)toupper((int)(R[A0] & 0xff))); break;
    case 0x26: ret((u32)tolower((int)(R[A0] & 0xff))); break;
    // bcopy(src, dst, len): memcpy with the arguments the other way round (Resident Evil Survivor calls it in play, e.g.
    // from 0x800c7c44; unimplemented, nothing was copied and the game later jumped into garbage and hung)
    case 0x27: for (u32 i = 0; i < R[A2]; i++) write8(R[A1] + i, read8(R[A0] + i)); ret(R[A1]); break;
    case 0x28: for (u32 i = 0; i < R[A1]; i++) write8(R[A0] + i, 0); ret(R[A0]); break;  // bzero
    case 0x29: {  // bcmp(a, b, len): 0 if equal, else the difference of the first different bytes
        u32 r = 0;
        for (u32 i = 0; i < R[A2] && !r; i++) r = (u32)((int)read8(R[A0] + i) - (int)read8(R[A1] + i));
        ret(r);
        break;
    }
    case 0x2a: for (u32 i = 0; i < R[A2]; i++) write8(R[A0] + i, read8(R[A1] + i)); ret(R[A0]); break;
    case 0x2b: for (u32 i = 0; i < R[A2]; i++) write8(R[A0] + i, (u8)R[A1]); ret(R[A0]); break;
    case 0x33: ret(do_malloc(R[A0])); break;
    case 0x34: do_free(R[A0]); break;
    case 0x37: {  // calloc
        u32 sz = R[A0] * R[A1];
        u32 p = do_malloc(sz);
        for (u32 i = 0; i < sz; i++) write8(p + i, 0);
        ret(p);
        break;
    }
    case 0x38: {  // realloc
        u32 old = R[A0], sz = R[A1];
        u32 osz = heap_used.count(old) ? heap_used[old] : 0;
        u32 p = do_malloc(sz);
        for (u32 i = 0; i < std::min(sz, osz); i++) write8(p + i, read8(old + i));
        if (old) do_free(old);
        ret(p);
        break;
    }
    case 0x39:  // InitHeap
        heap_base = R[A0];
        heap_size = R[A1];
        heap_used.clear();
        break;
    case 0x3f: logf("printf: %s", guest_printf(R[A0], 1).c_str()); break;
    case 0x43: {  // Exec(headerbuf, param1, param2): run a loaded executable, return 1 when it returns
        u32 h = R[A0];
        u32 pc0 = read32(h + 0x00), gp0 = read32(h + 0x04);
        u32 b_addr = read32(h + 0x18), b_size = read32(h + 0x1c);
        u32 s_addr = read32(h + 0x20), s_size = read32(h + 0x24);
        for (u32 i = 0; i < b_size; i += 4) write32(b_addr + i, 0);
        // saved context, as the real kernel does (ra, sp, fp, gp, s0 at +0x28)
        write32(h + 0x28, R[RA]); write32(h + 0x2c, R[SP]); write32(h + 0x30, R[30]);
        write32(h + 0x34, R[28]); write32(h + 0x38, R[16]);
        exec_stack.push_back(h);
        if (s_addr) { R[SP] = s_addr + s_size; R[30] = R[SP]; }
        R[28] = gp0;
        R[A0] = R[A1];
        R[A1] = R[A2];
        R[RA] = EXEC_RETURN;
        logf("Exec pc=%08x gp=%08x sp=%08x", pc0, gp0, R[SP]);
        M.cpu.pc = pc0;
        return;
    }
    case 0x44: break;  // FlushCache
    case 0x2f: rand_seed = rand_seed * 0x41c64e6d + 0x3039; ret((rand_seed >> 16) & 0x7fff); break;  // rand
    case 0x30: rand_seed = R[A0]; break;  // srand
    case 0x9f: break;  // SetMemSize
    case 0xad: break;  // set_card_auto_format
    case 0x15: {  // strcat
        u32 d = R[A0], e = d;
        while (read8(e)) e++;
        for (u32 i = 0;; i++) { u8 ch = read8(R[A1] + i); write8(e + i, ch); if (!ch) break; }
        ret(d);
        break;
    }
    case 0x49: gpu_write_gp1(0x04000000); gpu_write_gp0(R[A0]); break;  // GPU_cw
    case 0x70: break;  // _bu_init
    case 0x72: break;  // CdRemove
    case 0xab:  // _card_info
    case 0xac:  // _card_load
        defer_event(0xf4000001, 4);
        defer_event(0xf0000011, 4);
        ret(1);
        break;
    default: logf("BIOS A(%02x) unimplemented, ra=%08x", n, R[RA]); break;
    }
}

static void bios_b(u32 n) {
    switch (n) {
    case 0x07: deliver_event(R[A0], R[A1]); break;
    case 0x0e: {  // OpenThread(pc, sp, gp)
        u32 h = 0xffffffff;
        for (int i = 1; i < 16; i++) {
            if (threads[i].used) continue;
            threads[i].used = true;
            Cpu &t = threads[i].regs;
            t = M.cpu;
            memset(t.r, 0, sizeof t.r);
            t.pc = R[A0]; t.r[SP] = R[A1]; t.r[30] = R[A1]; t.r[28] = R[A2];
            h = 0xff000000u | (u32)i;
            break;
        }
        ret(h);
        break;
    }
    case 0x0f: { u32 i = R[A0] & 0xffff; if (i < 16 && i) threads[i].used = false; ret(1); break; }  // CloseThread
    case 0x10: if (g_log_bios) logf("ChangeThread %08x ra=%08x", R[A0], R[RA]); change_thread(R[A0]); return;  // ChangeThread
    case 0x08: ret(open_event(R[A0], R[A1], R[A2], R[A3])); break;
    case 0x09: if (Event *e = ev(R[A0])) e->status = 0; ret(1); break;
    case 0x0a: {  // WaitEvent
        Event *e = ev(R[A0]);
        if (e && e->status == 0x4000) { e->status = 0x2000; ret(1); }
        else ret(0);
        break;
    }
    case 0x0b: {  // TestEvent
        Event *e = ev(R[A0]);
        if (e && e->status == 0x4000) { e->status = 0x2000; ret(1); }
        else ret(0);
        break;
    }
    case 0x0c: if (Event *e = ev(R[A0])) e->status = 0x2000; ret(1); break;
    case 0x0d: if (Event *e = ev(R[A0])) e->status = 0x1000; ret(1); break;
    case 0x12:  // InitPad(buf1, siz1, buf2, siz2)
        initpad_buf[0] = R[A0];
        initpad_buf[1] = R[A2];
        logf("InitPad buf1=%08x/%u buf2=%08x/%u", R[A0], R[A1], R[A2], R[A3]);
        pad_started = true;  // this game never calls StartPad through the B table
        ret(1);
        break;
    case 0x13: pad_started = true; logf("StartPad"); ret(1); break;
    case 0x14: pad_started = false; break;
    case 0x15:  // OutdatedPadInitAndStart(type, dest, ...)
        pad_dest = R[A1];
        pad_started = true;
        ret(2);
        break;
    case 0x16: ret(pad_dest ? ~read32(pad_dest) : 0); break;
    case 0x17: return_from_exception(); return;  // no "jr ra"
    case 0x18: hook_jmpbuf = 0; break;
    case 0x19: hook_jmpbuf = R[A0]; break;
    case 0x32: {
        int r = mc_open(guest_str(R[A0]), R[A1]);
        logf("card open(%s, %x) -> %d", guest_str(R[A0]).c_str(), R[A1], r);
        ret((u32)r);
        break;
    }
    case 0x33: ret((u32)mc_seek((int)R[A0], (s32)R[A1], (int)R[A2])); break;
    case 0x34: {
        int fd = (int)R[A0];
        int r = mc_read(fd, R[A1], R[A2]);
        if (mc_is_async(fd)) { defer_event(0xf0000011, r >= 0 ? 4 : 0x8000); ret(0); }
        else ret((u32)r);
        break;
    }
    case 0x35: {
        int fd = (int)R[A0];
        if (fd == 0 || fd == 1) {  // tty
            static std::string line;
            for (u32 i = 0; i < R[A2]; i++) {
                char ch = (char)read8(R[A1] + i);
                if (ch == '\n') { logf("tty: %s", line.c_str()); line.clear(); }
                else line += ch;
            }
            ret(R[A2]);
            break;
        }
        int r = mc_write(fd, R[A1], R[A2]);
        if (mc_is_async(fd)) { defer_event(0xf0000011, r >= 0 ? 4 : 0x8000); ret(0); }
        else ret((u32)r);
        break;
    }
    case 0x36: ret((u32)mc_close((int)R[A0])); break;
    case 0x38: logf("exit(%d)", R[A0]); M.quit = true; break;
    case 0x3f: logf("puts: %s", guest_str(R[A0]).c_str()); break;
    case 0x41: mc_format(); ret(1); break;
    case 0x42: ret(mc_firstfile(guest_str(R[A0]), R[A1])); break;
    case 0x43: ret(mc_nextfile(R[A0])); break;
    case 0x44: ret(mc_rename(guest_str(R[A0]), guest_str(R[A1])) ? 1 : 0); break;
    case 0x45: ret(mc_erase(guest_str(R[A0])) ? 1 : 0); break;
    case 0x4a: case 0x4b: case 0x4c: break;  // Init/Start/StopCard
    case 0x4e:  // write_card_sector(port, sector, src)
        mc_raw_write((int)R[A1], R[A2]);
        defer_event(0xf4000001, 4);
        defer_event(0xf0000011, 4);
        ret(1);
        break;
    case 0x4f:
        mc_raw_read((int)R[A1], R[A2]);
        defer_event(0xf4000001, 4);
        defer_event(0xf0000011, 4);
        ret(1);
        break;
    case 0x50: break;  // allow_new_card
    // _card_status(slot) / _card_wait(slot): 1 = ready (no hardware wait here: reads and writes are done at once). The
    // game's save (after the ending: "Data save failed") calls _card_info, _card_wait, then tests its events: the
    // card's events are delivered here rather than at the next interrupt.
    case 0x5c: ret(1); break;
    case 0x5d: {
        auto evs = deferred_events;
        deferred_events.clear();
        for (auto &e : evs) deliver_event(e.first, e.second);
        ret(1);
        break;
    }
    case 0x56: ret(TBL_C); break;
    case 0x57: ret(TBL_B); break;
    case 0x5b: clear_pad = R[A0] != 0; break;
    default: logf("BIOS B(%02x) unimplemented, ra=%08x", n, R[RA]); break;
    }
    M.cpu.pc = R[RA];
}

static void bios_c(u32 n) {
    switch (n) {
    case 0x0a:  // ChangeClearRCnt(t, flag) -> old
        if (R[A0] < 4) { ret(clear_rcnt[R[A0]]); clear_rcnt[R[A0]] = R[A1] != 0; }
        break;
    case 0x02: {  // SysEnqIntRP(prio, struct)
        u32 p = R[A0] & 3;
        u32 e = R[A1];
        for (u32 x = intrp_head[p]; x; x = read32(x)) if (x == e) { ret(0); return; }
        logf("SysEnqIntRP(%u, %08x) f2=%08x f1=%08x ra=%08x", p, e, read32(e + 4), read32(e + 8), R[RA]);
        write32(e, intrp_head[p]);
        intrp_head[p] = e;
        ret(0);
        break;
    }
    case 0x03: {  // SysDeqIntRP(prio, struct)
        u32 p = R[A0] & 3, e = R[A1];
        if (intrp_head[p] == e) intrp_head[p] = read32(e);
        else for (u32 x = intrp_head[p]; x; x = read32(x)) if (read32(x) == e) { write32(x, read32(e)); break; }
        ret(0);
        break;
    }
    default: logf("BIOS C(%02x) unimplemented, ra=%08x", n, R[RA]); break;
    }
}


bool bios_call(u32 p) {
    u32 n = R[T1] & 0xff;
    int t = p == 0xa0 ? 0 : p == 0xb0 ? 1 : 2;
    if (g_log_bios) logf("%c(%02x) a0=%08x a1=%08x a2=%08x ra=%08x", 'A' + t, n, R[A0], R[A1], R[A2], R[RA]);
    if (t == 0 && n == 0xfe && (M.cpu.pc & 0x1fffffff) == 0xa0 && (R[31] & 0x1fffffff) != 0) {
        if (!exec_stack.empty()) { exec_return(); return true; }
    }
    u32 tbl = t == 0 ? TBL_A : t == 1 ? TBL_B : TBL_C;
    u32 entry = read32(tbl + n * 4);
    if (entry != sentinel(t, (int)n) && entry != 0) {
        M.cpu.pc = entry;  // patched by the game
        return true;
    }
    M.cycles += 50;
    if (t == 0 && n == 0x43) { bios_a(n); return true; }  // Exec sets its own pc
    if (t == 0) { bios_a(n); M.cpu.pc = R[RA]; }
    else if (t == 1) bios_b(n);
    else { bios_c(n); M.cpu.pc = R[RA]; }
    return true;
}

void bios_init() {
    for (int i = 0; i < 0xc0; i++) write32(TBL_A + i * 4, sentinel(0, i));
    for (int i = 0; i < 0x60; i++) write32(TBL_B + i * 4, sentinel(1, i));
    for (int i = 0; i < 0x20; i++) write32(TBL_C + i * 4, sentinel(2, i));
    memset(events, 0, sizeof events);
    write32(EXEC_RETURN + 0, 0x240900fe);  // addiu t1, zero, 0xfe
    write32(EXEC_RETURN + 4, 0x08000028);  // j 0xa0
    write32(EXEC_RETURN + 8, 0x00000000);
    exec_stack.clear();
    memset(threads, 0, sizeof threads);
    threads[0].used = true;
    cur_thread = 0;
    memset(intrp_head, 0, sizeof intrp_head);
    memcard_init("memcard0.mcd");
}


void st_bios(StateIO &s) {
    s.pod(events); s.vec(deferred_events); s.pod(heap_base); s.pod(heap_size); s.map(heap_used);
    s.pod(hook_jmpbuf); s.pod(clear_pad); s.pod(clear_rcnt); s.pod(pad_dest); s.pod(initpad_buf); s.pod(pad_started);
    s.pod(saved_ctx); s.pod(in_exception); s.vec(exec_stack); s.pod(threads); s.pod(cur_thread); s.pod(intrp_head);
    s.pod(rand_seed);
}

}  // namespace psx
