// Controller ports: a real SIO0 serial-port emulation (the games that drive the controllers themselves through
// libpad / their own code, byte by byte, with /ACK and IRQ7) plus the BIOS pad services (games that read the pads
// through the kernel: OutdatedPadInitAndStart / InitPad, see bios.cpp).
//
// Devices: digital pad (ID 41), DualShock (41 digital / 73 analog / F3 config mode, commands 42-4D), GunCon
// (ID 63: buttons + beam position counted like the real gun: 8 MHz clocks from HSYNC and scanlines from VSYNC).
// The memory card is served by the BIOS file API; on the serial port the card slots read as empty.
#include "psx.h"
#include "state.h"
#include <algorithm>
#include <cstdlib>
#include <initializer_list>

namespace psx {

u16 g_pad_buttons = 0;  // pressed = 1, PsyQ PadRead layout (see platform input mapping): ORed into port 1
PadPort g_pads[2];
bool g_sio_log = getenv("PSXVR_SIOLOG") != nullptr;  // debug: log every byte exchanged on the controller port

void gpu_display_timing(int *h1, int *v1, int *dot_div, bool *interlace);

void pad_init() {
    pad_reset_ports();
    g_pad_buttons = 0;
    for (auto &p : g_pads) p = PadPort();
    g_pads[0].type = PAD_DUALSHOCK;
}

static u16 port_buttons(int port) {
    u16 b = g_pads[port].buttons;
    if (port == 0) b |= g_pad_buttons;
    return b;
}

// ---------------------------------------------------------------- BIOS pad services
// Called by the HLE BIOS each vblank: the kernel stores the inverted button word.
void pad_bios_vblank(u32 dest) {
    // PsyQ PadRead returns ~(*dest) with pad 1 in the low half; unused pad 2 reads released.
    u32 v = ~((u32)port_buttons(0));
    write32(dest, v);
}

// InitPad/StartPad: the kernel stores each port's raw reply: status (0 = ok), id (0x41 = digital pad),
// then the two button bytes (active low; first byte = Select..Left, i.e. the high half of the PsyQ word).
void pad_bios_initpad_vblank(u32 buf1, u32 buf2) {
    u16 b = (u16)~port_buttons(0);
    write8(buf1 + 0, 0x00);
    write8(buf1 + 1, 0x41);
    write8(buf1 + 2, (u8)(b >> 8));
    write8(buf1 + 3, (u8)(b & 0xff));
    if (buf2) write8(buf2 + 0, 0xff);  // port 2: nothing connected
}

// ---------------------------------------------------------------- devices on the serial port
struct Device {
    int step = 0;        // bytes exchanged since /JOY was asserted
    bool pad = false;    // the controller answered the address byte
    u8 cmd = 0;
    u8 reply[12];        // reply bytes from index 0 (the ID, sent with the command byte)
    int len = 0;         // number of reply bytes from the ID on
    bool config = false;         // DualShock configuration (escape) mode
    bool analog_locked = false;
    u8 rumble_map[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    bool analog_set = false;     // the game chose the mode itself (then the default is no longer applied)
};
static Device dev[2];

static void gun_position(const PadPort &p, u16 &x, u16 &y) {
    if (!p.gun_onscreen) { x = 0x01; y = 0x0a; return; }  // what the real gun reports when it sees no light
    if (p.gun_raw) { x = p.gun_raw_x; y = p.gun_raw_y; return; }  // exact counters chosen by the game module
    int h1 = 0x260, v1 = 16, div = 8;
    bool il = false;
    gpu_display_timing(&h1, &v1, &div, &il);
    // horizontal: GPU video clocks (53.2 MHz PAL / 53.69 MHz NTSC) from HSYNC -> the gun's 8 MHz counter
#ifdef PSX_PAL
    const double video_hz = 53203425.0;
#else
    const double video_hz = 53693175.0;
#endif
    double vclk = h1 + p.gun_x * div;
    double gx = vclk * 8000000.0 / video_hz;
    // vertical: scanlines from VSYNC = the display's first line (GP1 07) + the row; gun_dy lets a game module move
    // the reading (a game whose shots do not land where its calibrated sight is)
    double gy = v1 + (il ? (p.gun_y + p.gun_dy) * 0.5 : p.gun_y + p.gun_dy);
    x = (u16)(gx < 1 ? 1 : gx > 0xffff ? 0xffff : gx);
    y = (u16)(gy < 1 ? 1 : gy > 0x3ff ? 0x3ff : gy);
}

static void build_poll(int port, Device &d) {
    PadPort &p = g_pads[port];
    u16 b = (u16)~port_buttons(port);
    u8 lo = (u8)(b >> 8), hi = (u8)(b & 0xff);  // wire order: Select..Left first, then L2..Square
    if (p.type == PAD_GUNCON) {
        u16 x, y;
        gun_position(p, x, y);
        u8 r[] = {0x63, 0x5a, lo, hi, (u8)x, (u8)(x >> 8), (u8)y, (u8)(y >> 8)};
        memcpy(d.reply, r, sizeof r);
        d.len = 8;
        return;
    }
    if (p.type == PAD_DUALSHOCK && (p.analog_mode || d.config)) {
        u8 r[] = {(u8)(d.config ? 0xf3 : 0x73), 0x5a, lo, hi, p.rx, p.ry, p.lx, p.ly};
        memcpy(d.reply, r, sizeof r);
        d.len = 8;
        return;
    }
    u8 r[] = {0x41, 0x5a, lo, hi};
    memcpy(d.reply, r, sizeof r);
    d.len = 4;
}

static void set_reply(Device &d, u8 id, std::initializer_list<u8> data) {
    d.reply[0] = id;
    d.reply[1] = 0x5a;
    int i = 2;
    for (u8 v : data) d.reply[i++] = v;
    d.len = i;
}

// One byte exchanged with the selected port; returns the reply byte and whether the device pulses /ACK.
static u8 device_exchange(int port, u8 in, bool &ack) {
    Device &d = dev[port];
    PadPort &p = g_pads[port];
    int s = d.step++;
    ack = false;
    if (s == 0) {
        if (in == 0x01 && p.type != PAD_NONE) { d.pad = true; ack = true; }
        return 0xff;  // memory card address (0x81) and empty ports: no answer
    }
    if (!d.pad) return 0xff;
    if (s == 1) {
        d.cmd = in;
        bool ds = p.type == PAD_DUALSHOCK;
        if (in == 0x42) build_poll(port, d);
        else if (ds && in == 0x43) {
            if (d.config) set_reply(d, 0xf3, {0, 0, 0, 0, 0, 0});
            else build_poll(port, d);
        } else if (ds && d.config) {
            switch (in) {
            case 0x44: set_reply(d, 0xf3, {0, 0, 0, 0, 0, 0}); break;
            case 0x45: set_reply(d, 0xf3, {0x03, 0x02, (u8)(p.analog_mode ? 1 : 0), 0x02, 0x01, 0x00}); break;
            case 0x46: case 0x47: case 0x4c: set_reply(d, 0xf3, {0, 0, 0, 0, 0, 0}); break;
            case 0x4d: set_reply(d, 0xf3, {d.rumble_map[0], d.rumble_map[1], d.rumble_map[2], d.rumble_map[3],
                                            d.rumble_map[4], d.rumble_map[5]}); break;
            default: d.pad = false; return 0xff;
            }
        } else {
            d.pad = false;  // command this device does not know
            return 0xff;
        }
        ack = d.len > 1;
        return d.reply[0];
    }
    int i = s - 1;  // reply index sent with this byte
    int arg = s - 3; // argument index (0 = first byte after the 0x5A)
    if (i >= d.len) { d.pad = false; return 0xff; }
    u8 out = d.reply[i];
    // arguments that change the rest of the reply or the pad state
    if (arg == 0) {
        switch (d.cmd) {
        case 0x46: if (in == 1) { d.reply[4] = 0x01; d.reply[5] = 0x01; d.reply[6] = 0x01; d.reply[7] = 0x14; }
                   else { d.reply[4] = 0x01; d.reply[5] = 0x02; d.reply[6] = 0x00; d.reply[7] = 0x0a; } break;
        case 0x47: d.reply[4] = 0x02; d.reply[5] = 0x00; d.reply[6] = 0x01; d.reply[7] = 0x00; break;
        case 0x4c: d.reply[5] = in == 1 ? 0x07 : 0x04; break;
        default: break;
        }
    }
    if (d.cmd == 0x42 || (d.cmd == 0x43 && !d.config)) {
        if (arg == 0) p.rumble_small = in;
        if (arg == 1) p.rumble_large = in;
    }
    if (d.cmd == 0x43 && arg == 0) d.config = in == 1 ? true : in == 0 ? false : d.config;
    if (d.cmd == 0x44 && arg == 0 && !d.analog_locked) { p.analog_mode = in == 1; d.analog_set = true; }
    if (d.cmd == 0x44 && arg == 1) d.analog_locked = in == 3;
    if (d.cmd == 0x4d && arg >= 0 && arg < 6) d.rumble_map[arg] = in;
    ack = i < d.len - 1;
    return out;
}

// ---------------------------------------------------------------- SIO0 registers and timing
static struct {
    u16 mode = 0, ctrl = 0, baud = 0x88;
    bool rx_full = false;
    u8 rx = 0xff;
    bool tx_busy = false;
    u8 tx = 0;
    u64 tx_done = ~0ull;    // cycle when the byte being shifted is complete
    u64 ack_start = ~0ull;  // /ACK pulse from the device
    u64 ack_end = ~0ull;
    bool irq = false;       // STAT bit 9
} S;

constexpr u64 ACK_DELAY = 120, ACK_LEN = 100;  // cycles after the last bit / pulse length (as measured on pads)

static u64 byte_cycles() {
    static const u32 mul[4] = {1, 1, 16, 64};
    u64 c = (u64)(S.baud ? S.baud : 0x88) * mul[S.mode & 3] * 8;
    return c < 64 ? 64 : c;
}

static void sio_update() {
    for (;;) {
        u64 t = std::min(S.tx_done, std::min(S.ack_start, S.ack_end));
        if (t > M.cycles) break;
        if (t == S.tx_done) {
            S.tx_done = ~0ull;
            S.tx_busy = false;
            bool ack = false;
            u8 r = 0xff;
            if (S.ctrl & 2) r = device_exchange((S.ctrl >> 13) & 1, S.tx, ack);
            if (g_sio_log) logf("sio port%d %s %02x -> %02x%s", (S.ctrl >> 13) & 1, (S.ctrl & 2) ? "sel" : "---", S.tx, r, ack ? " ack" : "");
            S.rx = r;
            S.rx_full = true;
            if (ack) { S.ack_start = t + ACK_DELAY; S.ack_end = S.ack_start + ACK_LEN; }
        } else if (t == S.ack_start) {
            S.ack_start = ~0ull;
            if (S.ctrl & 0x1000) {
                if (!S.irq) M.irq(IRQ_SIO0);
                S.irq = true;
            }
        } else {
            S.ack_end = ~0ull;
        }
    }
}

static void deselect() {
    for (auto &d : dev) {
        d.step = 0;
        d.pad = false;
    }
}

u32 sio_read(u32 addr, int size) {
    if ((addr & 0xff) >= 0x50) return 0;  // SIO1 (serial link): nothing connected
    sio_update();
    switch (addr & 0xf) {
    case 0x0: {
        u8 v = S.rx_full ? S.rx : 0xff;
        S.rx_full = false;
        if (size >= 2) return v | 0xffffff00u;
        return v;
    }
    case 0x4: {
        u32 s = 0;
        if (!S.tx_busy) s |= 1 | 4;       // TX ready, TX finished
        else s |= 1;                       // one byte of TX FIFO free
        if (S.rx_full) s |= 2;
        bool ack_low = S.ack_end != ~0ull && S.ack_start == ~0ull;  // inside the pulse
        if (ack_low) s |= 0x80;
        if (S.irq) s |= 0x200;
        return s;
    }
    case 0x8: return S.mode;
    case 0xa: return S.ctrl;
    case 0xe: return S.baud;
    default: return 0;
    }
}

void sio_write(u32 addr, u32 v, int size) {
    if ((addr & 0xff) >= 0x50) return;
    sio_update();
    switch (addr & 0xf) {
    case 0x0:
        S.tx = (u8)v;
        S.tx_busy = true;
        S.tx_done = M.cycles + byte_cycles();
        break;
    case 0x8: S.mode = (u16)v; break;
    case 0xa: {
        u16 old = S.ctrl;
        S.ctrl = (u16)v;
        if (v & 0x40) {  // reset
            S.ctrl = 0;
            S.mode = 0;
            S.rx_full = false;
            S.tx_busy = false;
            S.tx_done = S.ack_start = S.ack_end = ~0ull;
            S.irq = false;
            deselect();
            break;
        }
        if (v & 0x10) S.irq = false;  // acknowledge
        bool sel_now = (S.ctrl & 2) != 0, sel_old = (old & 2) != 0;
        if (!sel_now || (sel_old && ((old ^ S.ctrl) & 0x2000))) deselect();
        if (sel_now && !sel_old) deselect();
        if (!sel_now) S.ack_start = S.ack_end = ~0ull;
        break;
    }
    case 0xe: S.baud = (u16)v; break;
    default: break;
    }
    (void)size;
    schedule_update();
}

void sio_event() { sio_update(); }
u64 sio_next_event() { return std::min(S.tx_done, std::min(S.ack_start, S.ack_end)); }

// Called on power-on; also applied when a pad type changes (fresh device state).
void pad_reset_ports() {
    S = {};
    S.baud = 0x88;
    for (auto &d : dev) d = Device();
}


void st_pad(StateIO &s) { s.pod(S); s.pod(dev); }

}  // namespace psx
