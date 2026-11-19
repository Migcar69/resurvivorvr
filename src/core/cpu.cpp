// Reference MIPS R3000A interpreter. The recompiled code falls back to this for
// addresses it could not resolve statically, and it is the oracle used to validate it.
#include "psx.h"
#include "state.h"
#include "pgxp.h"
#include <algorithm>
#include <cstdio>
#include <map>

namespace psx {

bool g_trace = false;

// Emulated cycles per CPU instruction: 2 is the PS1 (as before); 1 runs the game's code twice as fast against the video
// timing - an overclock, as emulators offer: a game whose frames got heavier (the VR's wider view) keeps its frame rate.
// Timers, video and sound keep their own time. (As in the MOH VR port.)
u32 g_cpu_instr_cycles = 2;
// finer: cycles per instruction x 256 (0 = use g_cpu_instr_cycles), e.g. 384 = 1.5
u32 g_cpu_cpi_q8 = 0;
static u32 cpi_acc = 0;
static inline u32 instr_cycles() {
    if (!g_cpu_cpi_q8) return g_cpu_instr_cycles;
    cpi_acc += g_cpu_cpi_q8;
    u32 n = cpi_acc >> 8;
    cpi_acc &= 255;
    return n;
}

// Illegal instructions executed so far: the game has run into data (a crash; frontends may react)
u64 g_illegal_count = 0;

// Code hooks: host callbacks run just before the guest instruction at a given address (VR camera etc.)
constexpr int MAX_HOOKS = 32;
u32 g_hook_pc[MAX_HOOKS];  // 0 = free slot (the guest never executes address 0)
static bool hook_bucket[4096];  // quick test before the list: any hook whose (pc >> 2) & 4095 lands here
static void (*hook_fn[MAX_HOOKS])();
void cpu_add_hook(u32 pc, void (*fn)()) {
    for (int i = 0; i < MAX_HOOKS; i++)
        if (g_hook_pc[i] == 0) {
            g_hook_pc[i] = pc;
            hook_fn[i] = fn;
            hook_bucket[(pc >> 2) & 4095] = true;
            return;
        }
}
static void run_hooks(u32 pc) {
    for (int i = 0; i < MAX_HOOKS; i++)
        if (g_hook_pc[i] == pc) hook_fn[i]();
}

// Pending load (R3000 load delay slot)
static int ld_reg = 0;
static u32 ld_val = 0;

void cpu_reset(u32 pc, u32 gp, u32 sp) {
    memset(&M.cpu, 0, sizeof(M.cpu));
    M.cpu.pc = pc;
    M.cpu.r[28] = gp;
    M.cpu.r[29] = sp;
    M.cpu.r[30] = sp;
    M.cpu.sr = 0x10900000;  // CU0, CU2 (GTE), BEV off
    ld_reg = 0;
}

// Propagate a vertex tag through a register move (PGXP)
static inline void pg_copy(int rd, int src, u32 v) {
    if (!rd) return;
    u32 vid = src ? reg_vid(src, M.cpu.r[src]) : 0;
    g_reg_tag[rd] = {v, vid};
}

// Partial stores keep the word's vertex tag, marked as derived (sprite corners built from a projected point)
static inline u32 shadowed_word(u32 a) {  // a is RAM or scratchpad (shadow_at succeeded)
    u32 cur;
    if ((a & 0x1fffffff) >= 0x1f800000) memcpy(&cur, M.scratch + (a & 0x3fc), 4);
    else memcpy(&cur, ram_ptr(a & ~3u), 4);
    return cur;
}
static inline u32 pg_partial_before(u32 a) {
    Shadow *s = shadow_at(a);
    return (s && s->val == shadowed_word(a)) ? s->vid : 0;
}
static inline void pg_partial_after(u32 a, u32 vid) {
    Shadow *s = shadow_at(a);
    if (!s) return;
    s->val = shadowed_word(a);
    s->vid = vid ? (vid | VID_DERIVED) : 0;
}

// Halfword loads: the tag of the word they come from (a screen coordinate's x or y half)
static inline u32 pg_half_load(u32 a) {
    Shadow *s = shadow_at(a);
    if (s && s->vid && s->val == shadowed_word(a)) return s->vid;
    HalfShadow *h = half_shadow_at(a);
    if (h && h->vid) {
        u32 w = shadowed_word(a);
        u16 cur = (u16)((a & 2) ? (w >> 16) : w);
        if (h->val == cur) return h->vid;
    }
    return 0;
}
// Halfword stores of a tagged value: when both halves of the word now hold the same vertex
// (x and y written separately), the whole word becomes that vertex again.
static inline void pg_half_store(u32 a, u16 v, u32 vid) {
    HalfShadow *h = half_shadow_at(a);
    if (!h) return;
    h->val = v;
    h->vid = vid;
    if (!vid) return;
    HalfShadow *o = half_shadow_at(a ^ 2);
    u32 w = shadowed_word(a);
    u16 other = (u16)((a & 2) ? w : (w >> 16));
    if (o->vid && o->val == other && ((o->vid ^ vid) & ~VID_DERIVED) == 0)
        if (Shadow *s = shadow_at(a)) *s = {w, (o->vid | vid) & ~0u};
}
static inline void set_reg(int r, u32 v) {
    if (r) M.cpu.r[r] = v;
}

// Execute exactly one instruction (plus its delay slot if it branches).
// Returns the next pc.
static void step();

void cpu_exception(u32 code) {
    Cpu &c = M.cpu;
    c.cause = (c.cause & ~0x7c) | (code << 2);
    c.epc = c.pc;
    // push interrupt-enable / kernel-mode stack
    c.sr = (c.sr & ~0x3f) | ((c.sr << 2) & 0x3f);
    bios_exception();
}

bool irq_pending() {
    if (!(M.i_stat & M.i_mask)) return false;
    return (M.cpu.sr & 0x401) == 0x401;  // IEc and IM2 (hardware interrupt line)
}

static inline u32 fetch(u32 pc) {
    return read32(pc);
}

static void exec(u32 w, u32 pc, u32 &npc, bool &branched, u32 &target);
extern bool g_pc_sample;
extern std::map<u32, u32> g_gte_hist;

// Executes instructions until the next scheduler event.
static void run_until_event() {
    Cpu &c = M.cpu;
    while (M.cycles < M.next_event) {
        u32 pc = c.pc;
        u32 p = pc & 0x1fffffff;
        if (p == 0xa0 || p == 0xb0 || p == 0xc0) {
            bios_call(p);
            continue;
        }
        if (g_trace) logf("%08x", pc);
        if (hook_bucket[(pc >> 2) & 4095]) {
            run_hooks(pc);
            if (c.pc != pc) continue;  // a hook sent the CPU elsewhere (e.g. straight back to the caller: pc = ra)
        }
        u32 w = fetch(pc);
        u32 npc = pc + 4;
        bool br = false;
        u32 tgt = 0;
        exec(w, pc, npc, br, tgt);
        M.cycles += instr_cycles();
        if (br) {
            // execute the delay slot
            u32 dw = fetch(pc + 4);
            u32 dn = pc + 8;
            bool dbr = false;
            u32 dt = 0;
            c.pc = pc + 4;
            exec(dw, pc + 4, dn, dbr, dt);
            M.cycles += instr_cycles();
            c.pc = tgt;
        } else if (c.pc == pc) {
            c.pc = npc;
        }
    }
}

// Call a guest function from host code (BIOS event callbacks). Runs until it returns.
constexpr u32 CALL_RETURN_MAGIC = 0x1fc0f000;
int g_in_cpu_call = 0;
void cpu_call(u32 addr, u32 a0, u32 a1, bool free_time) {
    Cpu &c = M.cpu;
    g_in_cpu_call++;
    Cpu saved = c;
    u64 cycles0 = M.cycles;
    c.r[4] = a0;
    c.r[5] = a1;
    c.r[31] = CALL_RETURN_MAGIC;
    c.r[29] -= 0x100;  // keep caller's frame intact
    c.pc = addr;
    while ((c.pc & 0x1fffffff) != CALL_RETURN_MAGIC && !M.quit) {
        u64 limit = M.next_event;
        M.next_event = M.cycles + 64;
        u32 pc = c.pc;
        u32 p = pc & 0x1fffffff;
        if (p == 0xa0 || p == 0xb0 || p == 0xc0) {
            bios_call(p);
        } else {
            // game code called from a hook still gets the capture hooks (they only read / adjust registers)
            if (hook_bucket[(pc >> 2) & 4095]) {
                run_hooks(pc);
                if (c.pc != pc) { M.next_event = limit; continue; }  // (as in the main loop)
            }
            u32 w = fetch(pc);
            u32 npc = pc + 4;
            bool br = false;
            u32 tgt = 0;
            exec(w, pc, npc, br, tgt);
            M.cycles += instr_cycles();
            if (br) {
                u32 dw = fetch(pc + 4);
                u32 dn = pc + 8;
                bool dbr = false;
                u32 dt = 0;
                c.pc = pc + 4;
                exec(dw, pc + 4, dn, dbr, dt);
                M.cycles += instr_cycles();
                c.pc = tgt;
            } else if (c.pc == pc) {
                c.pc = npc;
            }
        }
        M.next_event = limit;
    }
    g_in_cpu_call--;
    u32 v0 = c.r[2];
    c = saved;
    c.r[2] = v0;
    if (free_time) M.cycles = cycles0;  // extra work for the VR view must not slow the game down
}

std::map<u32, u32> g_pc_hist;
std::map<u32, u32> g_gte_hist;  // debug: GTE command address -> executions (with --pcs)
bool g_pc_sample = false;
std::map<u32, u32> g_call_hist;  // debug: call targets (JAL/JALR) -> count, while g_call_log is set
bool g_call_log = false;
void cpu_run_frame() {
    while (!M.quit) {
        run_until_event();
        if (g_pc_sample) g_pc_hist[M.cpu.pc]++;
        hw_tick();
        if (irq_pending()) cpu_exception(0);
        if (hw_take_vblank()) break;
    }
}

#define RS c.r[rs]
#define RT c.r[rt]
static inline void load_commit() {}

static void exec(u32 w, u32 pc, u32 &npc, bool &branched, u32 &target) {
    Cpu &c = M.cpu;
    u32 op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31;
    u32 imm = w & 0xffff;
    u32 simm = (u32)(s32)(s16)imm;

    // Load-delay: values loaded by the previous instruction become visible now,
    // after this instruction's operands were read. We approximate by committing
    // before execution unless this instruction reads the same register; GCC code
    // never relies on the stale value, so commit immediately for simplicity.
    if (ld_reg) {
        set_reg(ld_reg, ld_val);
        ld_reg = 0;
    }
    auto branch = [&](bool cond, u32 t) {
        branched = true;
        target = cond ? t : pc + 8;
    };
    switch (op) {
    case 0x00:
        switch (w & 63) {
        case 0x00: set_reg(rd, RT << sa); break;
        case 0x02: set_reg(rd, RT >> sa); break;
        case 0x03: set_reg(rd, (u32)((s32)RT >> sa)); break;
        case 0x04: set_reg(rd, RT << (RS & 31)); break;
        case 0x06: set_reg(rd, RT >> (RS & 31)); break;
        case 0x07: set_reg(rd, (u32)((s32)RT >> (RS & 31))); break;
        case 0x08: branch(true, RS); break;
        case 0x09: {
            u32 t = RS;
            if (g_call_log) g_call_hist[t]++;
            set_reg(rd, pc + 8);
            branch(true, t);
            break;
        }
        case 0x0c:
            c.pc = pc;
            cpu_exception(8);
            break;
        case 0x0d:  // break: division-by-zero traps etc. The BIOS just returns.
            logf("break %x at %08x", (w >> 6) & 0xfffff, pc);
            break;
        case 0x10: set_reg(rd, c.hi); break;
        case 0x11: c.hi = RS; break;
        case 0x12: set_reg(rd, c.lo); break;
        case 0x13: c.lo = RS; break;
        case 0x18: {
            s64 r = (s64)(s32)RS * (s64)(s32)RT;
            c.lo = (u32)r;
            c.hi = (u32)(r >> 32);
            break;
        }
        case 0x19: {
            u64 r = (u64)RS * (u64)RT;
            c.lo = (u32)r;
            c.hi = (u32)(r >> 32);
            break;
        }
        case 0x1a: {
            s32 n = (s32)RS, d = (s32)RT;
            if (d == 0) {
                c.hi = (u32)n;
                c.lo = n >= 0 ? 0xffffffff : 1;
            } else if ((u32)n == 0x80000000 && d == -1) {
                c.hi = 0;
                c.lo = 0x80000000;
            } else {
                c.lo = (u32)(n / d);
                c.hi = (u32)(n % d);
            }
            break;
        }
        case 0x1b: {
            u32 n = RS, d = RT;
            if (d == 0) {
                c.hi = n;
                c.lo = 0xffffffff;
            } else {
                c.lo = n / d;
                c.hi = n % d;
            }
            break;
        }
        case 0x20: case 0x21: { u32 v = RS + RT; if (g_pgxp) pg_copy(rd, rt == 0 ? rs : rs == 0 ? rt : 0, v); set_reg(rd, v); break; }
        case 0x22: case 0x23: set_reg(rd, RS - RT); break;
        case 0x24: set_reg(rd, RS & RT); break;
        case 0x25: { u32 v = RS | RT; if (g_pgxp) pg_copy(rd, rt == 0 ? rs : rs == 0 ? rt : 0, v); set_reg(rd, v); break; }
        case 0x26: set_reg(rd, RS ^ RT); break;
        case 0x27: set_reg(rd, ~(RS | RT)); break;
        case 0x2a: set_reg(rd, (s32)RS < (s32)RT); break;
        case 0x2b: set_reg(rd, RS < RT); break;
        default: if (++g_illegal_count <= 20) logf("illegal SPECIAL %08x at %08x", w, pc); break;
        }
        break;
    case 0x01: {
        bool cond = (rt & 1) ? (s32)RS >= 0 : (s32)RS < 0;
        if ((rt & 0x1e) == 0x10) set_reg(31, pc + 8);
        branch(cond, pc + 4 + (simm << 2));
        break;
    }
    case 0x02: branch(true, ((pc + 4) & 0xf0000000) | ((w & 0x3ffffff) << 2)); break;
    case 0x03:
        if (g_call_log) g_call_hist[((pc + 4) & 0xf0000000) | ((w & 0x3ffffff) << 2)]++;
        set_reg(31, pc + 8);
        branch(true, ((pc + 4) & 0xf0000000) | ((w & 0x3ffffff) << 2));
        break;
    case 0x04: branch(RS == RT, pc + 4 + (simm << 2)); break;
    case 0x05: branch(RS != RT, pc + 4 + (simm << 2)); break;
    case 0x06: branch((s32)RS <= 0, pc + 4 + (simm << 2)); break;
    case 0x07: branch((s32)RS > 0, pc + 4 + (simm << 2)); break;
    case 0x08: case 0x09: { u32 v = RS + simm; if (g_pgxp && simm == 0) pg_copy(rt, rs, v); set_reg(rt, v); break; }
    case 0x0a: set_reg(rt, (s32)RS < (s32)simm); break;
    case 0x0b: set_reg(rt, RS < simm); break;
    case 0x0c: set_reg(rt, RS & imm); break;
    case 0x0d: set_reg(rt, RS | imm); break;
    case 0x0e: set_reg(rt, RS ^ imm); break;
    case 0x0f: set_reg(rt, imm << 16); break;
    case 0x10:  // COP0
        if (rs == 0) set_reg(rt, rd == 12 ? c.sr : rd == 13 ? c.cause : rd == 14 ? c.epc : rd == 8 ? c.badvaddr : rd == 15 ? 2 : 0);
        else if (rs == 4) {
            if (rd == 12) c.sr = RT;
            else if (rd == 13) c.cause = (c.cause & ~0x300) | (RT & 0x300);
        } else if (rs == 0x10 && (w & 63) == 0x10) {  // rfe
            c.sr = (c.sr & ~0xf) | ((c.sr >> 2) & 0xf);
        }
        break;
    case 0x12:  // COP2 (GTE)
        if (rs & 0x10) {
            if (g_pc_sample) g_gte_hist[pc]++;
            M.gte.command(w & 0x1ffffff);
        }
        else if (rs == 0) {
            u32 v = M.gte.read_data(rd);
            if (g_pgxp && rt && rd >= 12 && rd <= 15) g_reg_tag[rt] = {v, g_sxy_vid[rd == 15 ? 2 : rd - 12]};
            set_reg(rt, v);
        }
        else if (rs == 2) set_reg(rt, M.gte.read_ctrl(rd));
        else if (rs == 4) {
            if (g_pgxp) g_gte_in_vid = reg_vid(rt, RT);
            M.gte.write_data(rd, RT);
        }
        else if (rs == 6) M.gte.write_ctrl(rd, RT);
        M.cycles += 6;
        break;
    case 0x20: { u32 v = (u32)(s32)(s8)read8(RS + simm); set_reg(rt, v); break; }
    case 0x21: {
        u32 a = RS + simm;
        u32 v = (u32)(s32)(s16)read16(a);
        if (g_pgxp && rt) g_reg_tag[rt] = {v, pg_half_load(a)};
        set_reg(rt, v);
        break;
    }
    case 0x22: {  // lwl
        u32 a = RS + simm;
        u32 m = read32(a & ~3);
        u32 cur = RT;
        u32 v;
        switch (a & 3) {
        case 0: v = (cur & 0x00ffffff) | (m << 24); break;
        case 1: v = (cur & 0x0000ffff) | (m << 16); break;
        case 2: v = (cur & 0x000000ff) | (m << 8); break;
        default: v = m; break;
        }
        set_reg(rt, v);
        break;
    }
    case 0x23: {
        u32 a = RS + simm;
        u32 v = read32(a);
        if (g_pgxp && rt) g_reg_tag[rt] = {v, shadow_vid(a, v)};
        set_reg(rt, v);
        break;
    }
    case 0x24: { u32 v = read8(RS + simm); set_reg(rt, v); break; }
    case 0x25: {
        u32 a = RS + simm;
        u32 v = read16(a);
        if (g_pgxp && rt) g_reg_tag[rt] = {v, pg_half_load(a)};
        set_reg(rt, v);
        break;
    }
    case 0x26: {  // lwr
        u32 a = RS + simm;
        u32 m = read32(a & ~3);
        u32 cur = RT;
        u32 v;
        switch (a & 3) {
        case 0: v = m; break;
        case 1: v = (cur & 0xff000000) | (m >> 8); break;
        case 2: v = (cur & 0xffff0000) | (m >> 16); break;
        default: v = (cur & 0xffffff00) | (m >> 24); break;
        }
        set_reg(rt, v);
        break;
    }
    case 0x28: {
        u32 a = RS + simm;
        u32 vid = g_pgxp ? pg_partial_before(a) : 0;
        write8(a, (u8)RT);
        if (g_pgxp) pg_partial_after(a, vid);
        break;
    }
    case 0x29: {
        u32 a = RS + simm;
        u32 vid = g_pgxp ? pg_partial_before(a) : 0;
        u32 hv = g_pgxp ? reg_vid(rt, RT) : 0;
        write16(a, (u16)RT);
        if (g_pgxp) {
            pg_partial_after(a, vid);
            pg_half_store(a, (u16)RT, hv);
        }
        break;
    }
    case 0x2a: {  // swl
        u32 a = RS + simm;
        u32 m = read32(a & ~3);
        u32 v;
        switch (a & 3) {
        case 0: v = (m & 0xffffff00) | (RT >> 24); break;
        case 1: v = (m & 0xffff0000) | (RT >> 16); break;
        case 2: v = (m & 0xff000000) | (RT >> 8); break;
        default: v = RT; break;
        }
        write32(a & ~3, v);
        break;
    }
    case 0x2b: if (g_pgxp) pgxp_store(RS + simm, RT, reg_vid(rt, RT)); write32(RS + simm, RT); break;
    case 0x2e: {  // swr
        u32 a = RS + simm;
        u32 m = read32(a & ~3);
        u32 v;
        switch (a & 3) {
        case 0: v = RT; break;
        case 1: v = (m & 0x000000ff) | (RT << 8); break;
        case 2: v = (m & 0x0000ffff) | (RT << 16); break;
        default: v = (m & 0x00ffffff) | (RT << 24); break;
        }
        write32(a & ~3, v);
        break;
    }
    case 0x32: {
        u32 a = RS + simm;
        u32 v = read32(a);
        if (g_pgxp) g_gte_in_vid = shadow_vid(a, v);
        M.gte.write_data(rt, v);
        break;
    }
    case 0x3a: {
        u32 v = M.gte.read_data(rt);
        if (g_pgxp) pgxp_store(RS + simm, v, (rt >= 12 && rt <= 15) ? g_sxy_vid[rt == 15 ? 2 : rt - 12] : 0);
        write32(RS + simm, v);
        break;
    }
    default: if (++g_illegal_count <= 20) logf("illegal opcode %08x at %08x", w, pc); break;
    }
}


void st_cpu(StateIO &s) {
    s.pod(ld_reg); s.pod(ld_val);
    s.pod(M.cpu); s.pod(M.gte); s.raw(M.ram, RAM_SIZE); s.pod(M.scratch); s.pod(M.cycles); s.pod(M.next_event);
    s.pod(M.i_stat); s.pod(M.i_mask);
}

static const u32 STATE_MAGIC = 0x31545350;  // "PST1"
static bool state_io(const char *path, bool saving) {
    StateIO s;
    s.f = fopen(path, saving ? "wb" : "rb");
    if (!s.f) return false;
    s.saving = saving;
    u32 magic = STATE_MAGIC, size = 0;
    s.pod(magic);
    if (magic != STATE_MAGIC) { fclose(s.f); return false; }
    st_cpu(s); st_hw(s); st_gpu(s); st_spu(s); st_cdrom(s); st_mdec(s); st_pad(s); st_bios(s); st_memcard(s);
    (void)size;
    fclose(s.f);
    return s.ok;
}
bool state_save(const char *path) { return state_io(path, true); }
bool state_load(const char *path) { return state_io(path, false); }

}  // namespace psx
