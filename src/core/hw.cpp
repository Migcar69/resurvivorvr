// I/O dispatch, interrupt controller, DMA, root counters and the event scheduler.
#include "psx.h"
#include "state.h"
#include "capture.h"
namespace psx { extern u32 g_gp0_addr; }
#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace psx {

u64 g_frame_end = 0;      // cycle at which the current frame (vblank) ends
static u64 frame_start = 0;

// ---------------------------------------------------------------- DMA
struct DmaChan {
    u32 madr, bcr, chcr;
};
static DmaChan dma[7];
static u32 dpcr = 0x07654321, dicr = 0;

static void dma_update_master() {
    bool force = dicr & (1u << 15);
    bool master = dicr & (1u << 23);
    bool flags = ((dicr >> 24) & (dicr >> 16) & 0x7f) != 0;
    bool old = dicr & 0x80000000u;
    bool now = force || (master && flags);
    dicr = (dicr & 0x7fffffff) | (now ? 0x80000000u : 0);
    if (now && !old) M.irq(IRQ_DMA);
}

static void dma_finish(int ch) {
    dma[ch].chcr &= ~0x01000000u;
    if (dicr & (1u << (16 + ch))) dicr |= 1u << (24 + ch);
    dma_update_master();
}

static void dma_run(int ch) {
    DmaChan &d = dma[ch];
    u32 mode = (d.chcr >> 9) & 3;
    bool from_ram = d.chcr & 1;
    s32 step = (d.chcr & 2) ? -4 : 4;
    u32 addr = d.madr & 0x1ffffc;

    if (mode == 2) {  // linked list (GPU)
        if (ch != 2) { dma_finish(ch); return; }
        int guard = 0;
        for (;;) {
            u32 hdr = *(u32 *)(M.ram + (addr & 0x1ffffc));
            u32 n = hdr >> 24;
            if (n == 0) g_cap_node++;  // empty node = ordering-table entry
            for (u32 i = 0; i < n; i++) {
                addr = (addr + 4) & 0x1ffffc;
                g_gp0_addr = 0x80000000u | addr;
                gpu_write_gp0(*(u32 *)(M.ram + addr));
            }
            g_gp0_addr = 0;
            u32 next = hdr & 0xffffff;
            M.cycles += n + 1;
            if (next & 0x800000) break;
            addr = next & 0x1ffffc;
            if (++guard > 0x100000) { logf("GPU DMA list loop"); break; }
        }
        d.madr = 0xffffff;
        dma_finish(ch);
        return;
    }

    u32 words;
    if (mode == 0) words = (d.bcr & 0xffff) ? (d.bcr & 0xffff) : 0x10000;
    else words = (d.bcr & 0xffff) * (d.bcr >> 16);

    if (ch == 6) {  // OTC: reverse-clear ordering table
        for (u32 i = 0; i < words; i++) {
            u32 v = (i == words - 1) ? 0xffffff : ((addr - 4) & 0x1fffff);
            *(u32 *)(M.ram + addr) = v;
            addr = (addr - 4) & 0x1ffffc;
        }
        M.cycles += words;
        dma_finish(ch);
        return;
    }

    for (u32 i = 0; i < words; i++) {
        u32 *p = (u32 *)(M.ram + addr);
        if (from_ram) {
            u32 v = *p;
            switch (ch) {
            case 0: mdec_write(0, v); break;
            case 2: gpu_write_gp0(v); break;
            case 4: spu_dma_write(v); break;
            default: break;
            }
        } else {
            u32 v = 0;
            switch (ch) {
            case 1: v = mdec_dma_out(); break;
            case 2: v = gpu_read(); break;
            case 3: v = cdrom_dma_read(); break;
            case 4: v = spu_dma_read(); break;
            default: break;
            }
            *p = v;
        }
        addr = (addr + step) & 0x1ffffc;
    }
    M.cycles += words;
    if (mode == 1) {
        d.madr = addr;
        d.bcr &= 0xffff;
    }
    dma_finish(ch);
}

static void dma_write(u32 off, u32 v) {
    int ch = (off >> 4) & 7;
    int reg = (off >> 2) & 3;
    if (ch == 7) {
        if (reg == 0) dpcr = v;
        else if (reg == 1) {
            u32 ack = v & 0x7f000000u;
            dicr = (dicr & ~0x00ffffffu) | (v & 0x00ff803fu);
            dicr &= ~ack;
            dma_update_master();
        }
        return;
    }
    DmaChan &d = dma[ch];
    if (reg == 0) d.madr = v & 0xffffff;
    else if (reg == 1) d.bcr = v;
    else if (reg == 2) {
        d.chcr = v;
        bool enabled = (dpcr >> (ch * 4 + 3)) & 1;
        bool start = v & 0x01000000u;
        bool trigger = ((v >> 9) & 3) != 0 || (v & 0x10000000u);
        if (enabled && start && trigger) dma_run(ch);
        else if (ch == 6 && start) dma_run(ch);
        else if (enabled && start && (ch == 0 || ch == 1 || ch == 3)) dma_run(ch);
    }
}

static u32 dma_read(u32 off) {
    int ch = (off >> 4) & 7;
    int reg = (off >> 2) & 3;
    if (ch == 7) return reg == 0 ? dpcr : reg == 1 ? dicr : 0;
    DmaChan &d = dma[ch];
    return reg == 0 ? d.madr : reg == 1 ? d.bcr : d.chcr;
}

// ---------------------------------------------------------------- Root counters
struct Timer {
    u32 mode = 0, target = 0;
    u64 base = 0;  // cycle where counter was 0 (in timer clock units scaled)
    u32 value_at_base = 0;
    bool irq_done = false;
};
static Timer tmr[3];

static u64 timer_ticks(int i, u64 cyc) {
    const Timer &t = tmr[i];
    u32 src = (t.mode >> 8) & 3;
    u64 dt = cyc - t.base;
    if (i == 1 && (src & 1)) return dt / CYCLES_PER_LINE;  // hblank
    if (i == 2 && (src & 2)) return dt / 8;
    if (i == 0 && (src & 1)) return dt * 2 / 5;  // ~dot clock (320px mode approx)
    return dt;
}

static u64 timer_cycles_for(int i, u64 ticks) {
    const Timer &t = tmr[i];
    u32 src = (t.mode >> 8) & 3;
    if (i == 1 && (src & 1)) return ticks * CYCLES_PER_LINE;
    if (i == 2 && (src & 2)) return ticks * 8;
    if (i == 0 && (src & 1)) return ticks * 5 / 2;
    return ticks;
}

static u32 timer_value(int i) {
    Timer &t = tmr[i];
    u64 v = t.value_at_base + timer_ticks(i, M.cycles);
    u32 period = (t.mode & 8) ? (t.target ? t.target + 1 : 0x10000) : 0x10000;
    return (u32)(v % period) & 0xffff;
}

static u64 timer_next_irq(int i) {
    Timer &t = tmr[i];
    if (!(t.mode & 0x30)) return ~0ull;
    if (i == 2 && (t.mode & 1) && ((t.mode >> 1) & 3) % 3 == 0) return ~0ull;  // stopped
    u64 now = t.value_at_base + timer_ticks(i, M.cycles);
    u32 target = t.target ? t.target : 0x10000;
    u64 period = (t.mode & 8) ? (u64)target + 1 : 0x10000;
    u64 cur = now % period;
    u64 to_target = (t.mode & 0x10) ? (target > cur ? target - cur : period - cur + target) : ~0ull;
    u64 to_wrap = (t.mode & 0x20) ? (0x10000 - (now % 0x10000)) : ~0ull;
    u64 ticks = std::min(to_target, to_wrap);
    if (ticks == 0) ticks = period;
    return t.base + timer_cycles_for(i, (now - t.value_at_base) + ticks) + 1;
}

static u64 tmr_due[3] = {~0ull, ~0ull, ~0ull};

static void timer_reschedule(int i) { tmr_due[i] = timer_next_irq(i); }

static void timer_write(int i, int reg, u32 v) {
    Timer &t = tmr[i];
    if (reg == 0) {
        t.value_at_base = v & 0xffff;
        t.base = M.cycles;
    } else if (reg == 1) {
        t.value_at_base = timer_value(i);
        t.base = M.cycles;
        t.mode = (v & 0x3ff) | 0x400;  // irq not requested (bit10=1)
        t.value_at_base = 0;
    } else if (reg == 2) {
        t.value_at_base = timer_value(i);
        t.base = M.cycles;
        t.target = v & 0xffff;
    }
    timer_reschedule(i);
    schedule_update();
}

static u32 timer_read(int i, int reg) {
    Timer &t = tmr[i];
    if (reg == 0) return timer_value(i);
    if (reg == 1) {
        u32 m = t.mode;
        u32 val = timer_value(i);
        if (t.target && val == t.target) m |= 0x800;
        t.mode &= ~0x1800u;
        return m;
    }
    return t.target;
}

// ---------------------------------------------------------------- Scheduler
void schedule_update() {
    u64 n = g_frame_end;
    for (int i = 0; i < 3; i++) n = std::min(n, tmr_due[i]);
    n = std::min(n, cdrom_next_event());
    n = std::min(n, sio_next_event());
    M.next_event = std::max(n, M.cycles + 1);
    // Keep interrupt latency low so polling loops see IRQs promptly
    M.next_event = std::min(M.next_event, M.cycles + 2048);
}

void hw_init() {
    frame_start = M.cycles;
    g_frame_end = M.cycles + CYCLES_PER_FRAME;
    for (int i = 0; i < 3; i++) {
        tmr[i] = Timer();
        tmr[i].base = M.cycles;
        tmr_due[i] = ~0ull;
    }
    schedule_update();
}

static bool vblank_flag = false;
bool hw_take_vblank() {
    bool v = vblank_flag;
    vblank_flag = false;
    return v;
}

void hw_tick() {
    if (M.cycles >= g_frame_end) {
        frame_start = g_frame_end;
        g_frame_end += CYCLES_PER_FRAME;
        M.irq(IRQ_VBLANK);
        gpu_vblank();
        vblank_flag = true;
    }
    for (int i = 0; i < 3; i++) {
        if (M.cycles >= tmr_due[i]) {
            Timer &t = tmr[i];
            M.irq((Irq)(IRQ_TMR0 + i));
            t.mode |= (timer_value(i) < 0x8000 && (t.mode & 0x20)) ? 0x1000 : 0x800;
            if (!(t.mode & 0x40)) {  // one-shot
                t.mode &= ~0x30u;
            }
            tmr_due[i] = timer_next_irq(i);
            if (tmr_due[i] <= M.cycles) tmr_due[i] = M.cycles + 64;
        }
    }
    if (M.cycles >= cdrom_next_event()) cdrom_event();
    if (M.cycles >= sio_next_event()) sio_event();
    spu_run();
    schedule_update();
}

// Frame-relative scanline (for GPUSTAT interlace bit and timer gates)
int hw_scanline() { return (int)((M.cycles - frame_start) / CYCLES_PER_LINE); }

// ---------------------------------------------------------------- I/O
u32 io_read(u32 a, int size) {
    u32 off = a - 0x1f801000;
    if (off < 0x40) return 0;  // memory control
    if (off >= 0x40 && off < 0x60) return sio_read(a, size);
    if (off == 0x60) return 0x00000b88;  // RAM size
    if (off == 0x70) { sio_event(); return M.i_stat; }  // lazy devices first: polled from inside handlers
    if (off == 0x74) return M.i_mask;
    if (off >= 0x80 && off < 0x100) {
        u32 v = dma_read(off - 0x80);
        if (size == 1) return (v >> ((a & 3) * 8)) & 0xff;
        if (size == 2) return (v >> ((a & 2) * 8)) & 0xffff;
        return v;
    }
    if (off >= 0x100 && off < 0x130) return timer_read((off >> 4) & 3, (off >> 2) & 3);
    if (off >= 0x800 && off < 0x804) return cdrom_read(off & 3);
    if (off == 0x810) return gpu_read();
    if (off == 0x814) return gpu_stat();
    if (off == 0x820 || off == 0x824) return mdec_read(off & 4);
    if (off >= 0xc00 && off < 0x1000) {
        if (size == 4) return spu_read(a) | ((u32)spu_read(a + 2) << 16);
        return spu_read(a);
    }
    return 0;
}

bool g_io_mute = false;  // trial runs of game code: hardware is not touched

void io_write(u32 a, u32 v, int size) {
    if (g_io_mute) return;
    u32 off = a - 0x1f801000;
    if (off < 0x40) return;
    if (off >= 0x40 && off < 0x60) { sio_write(a, v, size); return; }
    if (off == 0x60) return;
    if (off == 0x70) { M.i_stat &= v; return; }
    if (off == 0x74) { M.i_mask = v & 0x7ff; return; }
    if (off >= 0x80 && off < 0x100) {
        if (size == 1) {
            u32 base = (off - 0x80) & ~3u;
            u32 cur = dma_read(base);
            int sh = (a & 3) * 8;
            v = (cur & ~(0xffu << sh)) | ((v & 0xff) << sh);
            dma_write(base, v);
            return;
        }
        if (size == 2 && (a & 2)) {
            u32 base = (off - 0x80) & ~3u;
            u32 cur = dma_read(base);
            v = (cur & 0xffff) | (v << 16);
            dma_write(base, v);
            return;
        }
        dma_write(off - 0x80, v);
        return;
    }
    if (off >= 0x100 && off < 0x130) { timer_write((off >> 4) & 3, (off >> 2) & 3, v); return; }
    if (off >= 0x800 && off < 0x804) { cdrom_write(off & 3, (u8)v); return; }
    if (off == 0x810) { gpu_write_gp0(v); return; }
    if (off == 0x814) { gpu_write_gp1(v); return; }
    if (off == 0x820 || off == 0x824) { mdec_write(off & 4, v); return; }
    if (off >= 0xc00 && off < 0x1000) {
        if (size == 4) {
            spu_write(a, (u16)v);
            spu_write(a + 2, (u16)(v >> 16));
        } else {
            spu_write(a, (u16)v);
        }
        return;
    }
}

// ---------------------------------------------------------------- log
static FILE *logfile = nullptr;
const char *g_log_name = "psxvr.log";  // headless test runs use psxvr_test.log (the game's own log is kept)
void logf(const char *fmt, ...) {
    if (!logfile) logfile = fopen(g_log_name, "w");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logfile, fmt, ap);
    va_end(ap);
    fputc('\n', logfile);
    fflush(logfile);
}


void st_hw(StateIO &s) {
    s.pod(g_frame_end); s.pod(frame_start); s.pod(dma); s.pod(dpcr); s.pod(dicr); s.pod(tmr); s.pod(tmr_due);
    s.pod(vblank_flag);
}

}  // namespace psx
