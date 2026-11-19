// Core state of the PS1 machine that runs the original game executable.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

namespace psx {

constexpr u32 RAM_SIZE = 2 * 1024 * 1024;
constexpr u32 CPU_HZ = 33868800;
// Timing: PAL discs 50 fields / 314 lines, NTSC 60 / 263 (CMake option PSX_PAL)
#ifdef PSX_PAL
constexpr u32 FRAME_HZ = 50;
#else
constexpr u32 FRAME_HZ = 60;
#endif
constexpr u32 CYCLES_PER_FRAME = CPU_HZ / FRAME_HZ;
#ifdef PSX_PAL
constexpr u32 SCANLINES = 314;
#else
constexpr u32 SCANLINES = 263;
#endif
constexpr u32 CYCLES_PER_LINE = CYCLES_PER_FRAME / SCANLINES;

// Interrupt lines (I_STAT / I_MASK bits)
enum Irq : u32 {
    IRQ_VBLANK = 0, IRQ_GPU = 1, IRQ_CDROM = 2, IRQ_DMA = 3,
    IRQ_TMR0 = 4, IRQ_TMR1 = 5, IRQ_TMR2 = 6, IRQ_SIO0 = 7, IRQ_SIO = 8, IRQ_SPU = 9,
};

struct Cpu {
    u32 r[32];
    u32 hi, lo;
    u32 pc;
    // COP0
    u32 sr, cause, epc, badvaddr;
    // Delayed-load emulation is not needed by GCC-generated code; kept simple.
};

struct Gte {
    s32 data[32];
    s32 ctrl[32];
    void write_data(int r, u32 v);
    u32 read_data(int r);
    void write_ctrl(int r, u32 v);
    u32 read_ctrl(int r);
    void command(u32 op);
};

// Global machine
struct Machine {
    Cpu cpu;
    Gte gte;
    u8 *ram;          // 2MB
    u8 scratch[1024];
    u64 cycles = 0;   // global cycle counter
    u64 next_event = 0;

    u32 i_stat = 0, i_mask = 0;
    bool quit = false;

    void irq(Irq line) { i_stat |= 1u << line; }
};

extern Machine M;

// Memory access (mem.cpp)
u8 read8(u32 addr);
u16 read16(u32 addr);
u32 read32(u32 addr);
void write8(u32 addr, u8 v);
void write16(u32 addr, u16 v);
void write32(u32 addr, u32 v);
u8 *ram_ptr(u32 addr);
// Trial runs of game code: record RAM / scratchpad writes from begin, put the old contents back on restore.
void mem_undo_begin();
void mem_undo_restore();  // host pointer into RAM for a guest address (no bounds beyond 2MB mirror)

// Scheduler (hw.cpp)
void hw_init();
void hw_tick();               // run due events; called by the CPU core when cycles >= next_event
void schedule_update();
u32 io_read(u32 addr, int size);
void io_write(u32 addr, u32 v, int size);
extern bool g_io_mute;        // ignore hardware register writes (trial runs of game code)
bool irq_pending();           // interrupt requested and enabled by COP0
bool hw_take_vblank();        // true once per emulated vblank

// CPU (cpu.cpp)
void cpu_reset(u32 pc, u32 gp, u32 sp);
void cpu_run_frame();         // run until the next vblank
void cpu_exception(u32 code);  // raise exception (syscall/break/irq)
extern bool g_trace;
extern u32 g_cpu_instr_cycles;
extern u32 g_cpu_cpi_q8;  // cycles per instruction x 256 (0: g_cpu_instr_cycles)
extern u64 g_illegal_count;  // illegal instructions executed (> 0: the game has crashed into data)  // emulated cycles per CPU instruction (2 = the PS1; 1 = twice as fast)
void cpu_add_hook(u32 pc, void (*fn)());  // run fn before executing the guest instruction at pc
// run a guest function to its return (registers are restored); free_time: its cycles are not counted
void cpu_call(u32 addr, u32 a0, u32 a1, bool free_time = false);
extern int g_in_cpu_call;  // > 0 while host code runs a guest function (cpu_call)
// Debug: remember the last writes into a RAM range (physical addresses) in a ring, to find who wrote something.
struct WriteRec { u32 pc, ra, addr, value; u8 size, in_call; u64 frame; };
void write_ring_watch(u32 lo, u32 hi);
int write_ring_get(WriteRec *out, int max);  // newest last
extern u16 g_gte_h_override, g_gte_h_game;
extern s32 g_world_ofx, g_world_ofy;

// BIOS HLE (bios.cpp)
void bios_init();
bool bios_call(u32 pc);       // pc is 0xA0/0xB0/0xC0 -> handled, returns true
void bios_exception();        // exception vector entry (IRQ/syscall)

// GPU (gpu.cpp)
void gpu_init();
void gpu_write_gp0(u32 v);
void gpu_write_gp1(u32 v);
u32 gpu_read();
u32 gpu_stat();
void gpu_vblank();
u16 *gpu_vram();
struct DisplayInfo { int x, y, w, h; bool rgb24; bool enabled; };
DisplayInfo gpu_display();

// DMA (hw.cpp)
// CD-ROM (cdrom.cpp)
bool cdrom_open(const std::string &cue_path);
void cdrom_init();
u8 cdrom_read(u32 reg);
void cdrom_write(u32 reg, u8 v);
void cdrom_event();
u64 cdrom_next_event();
u32 cdrom_dma_read();
void cdrom_get_audio(s16 *out, int frames);  // CD-DA/XA mixed into SPU

// SPU (spu.cpp)
void spu_init();
u16 spu_read(u32 addr);
void spu_write(u32 addr, u16 v);
void spu_dma_write(u32 v);
u32 spu_dma_read();
void spu_run();                            // generate samples up to M.cycles (called by the scheduler)
int spu_fetch(s16 *out, int frames);       // audio thread: pull 44.1kHz stereo frames

// MDEC (mdec.cpp)
void mdec_init();
void mdec_write(u32 reg, u32 v);
u32 mdec_read(u32 reg);
u32 mdec_dma_out();
bool mdec_dma_out_ready();

// Pad / memory card (pad.cpp)
void pad_init();
u32 sio_read(u32 addr, int size);
void sio_write(u32 addr, u32 v, int size);
void sio_event();
u64 sio_next_event();
extern u16 g_pad_buttons;  // pressed = 1, PsyQ PadRead layout; ORed into port 1
// What is plugged into each controller port (SIO0 emulation in pad.cpp) and its live state.
enum PadType { PAD_NONE = 0, PAD_DIGITAL, PAD_DUALSHOCK, PAD_GUNCON };
struct PadPort {
    int type = PAD_NONE;
    u16 buttons = 0;                         // pressed = 1, PsyQ layout. GunCon: trigger = Circle, A = Start, B = Cross
    u8 lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;  // DualShock sticks (0 = left / up, 0x80 centre, 0xff right / down)
    bool analog_mode = true;                 // DualShock: analog (ID 73) or digital (ID 41); the game can change it
    bool gun_onscreen = false;               // GunCon: the gun sees the screen
    float gun_x = 0, gun_y = 0;              // GunCon: aimed point in pixels of the displayed image (gpu_display w x h)
    float gun_dy = 0;                        // GunCon: rows added to what the gun reports (game-specific correction)
    bool gun_raw = false;                    // GunCon: report these counters as they are (a game module that knows
    u16 gun_raw_x = 0, gun_raw_y = 0;        // how its game converts them computes the exact ones)
    u8 rumble_small = 0, rumble_large = 0;   // DualShock motors as last sent by the game
};
extern PadPort g_pads[2];
void pad_reset_ports();

// Logging
void logf(const char *fmt, ...);

}  // namespace psx
