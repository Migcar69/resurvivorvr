// Sound Processing Unit: 24 ADPCM voices, ADSR, pitch modulation, noise, reverb, CD audio mix.
#include "psx.h"
#include "state.h"
#include <algorithm>
#include <mutex>

namespace psx {

static u8 sram[512 * 1024];
static u16 regs[0x200];  // raw register mirror (0x1f801c00..0x1f801dff)

#define REG(off) regs[((off) - 0x1c00) >> 1]

enum Phase { OFF, ATTACK, DECAY, SUSTAIN, RELEASE };

struct Voice {
    u32 addr;       // current block address (bytes)
    u32 repeat;     // repeat address (bytes)
    u32 counter;    // pitch counter (12.12 within decoded block)
    s16 buf[28];
    s16 hist[4];    // last decoded samples for interpolation
    s32 p1, p2;     // ADPCM filter history
    u8 flags;
    Phase phase;
    s32 env;        // 0..0x7fff
    u32 env_ctr;
    bool repeat_set_by_game;
    s32 out_last;   // for pitch modulation
};
static Voice voices[24];
static u32 endx = 0;
static u32 transfer_addr = 0;
static u32 noise_level = 1;
static u32 noise_ctr = 0;
static u64 last_cycle = 0;
static u32 reverb_cur = 0;
static bool reverb_phase = false;
static s32 reverb_in_l = 0, reverb_in_r = 0, reverb_out_l = 0, reverb_out_r = 0;

// output ring
static std::mutex ring_mtx;
static s16 ring[65536];
static u32 ring_w = 0, ring_r = 0;

bool g_log_spu = false;
}  // namespace psx
bool g_log_spu_pub() { return psx::g_log_spu = true; }  // --logspu (development)
namespace psx {
void spu_dump_voices() {
    for (int n = 0; n < 24; n++) {
        Voice &v = voices[n];
        if (v.phase == OFF) continue;
        u32 b = 0x1c00 + n * 16;
        logf("voice %2d phase %d env %5d volL %04x volR %04x pitch %04x start %05x addr %05x rep %05x adsr %04x/%04x", n, v.phase, v.env,
             REG(b), REG(b + 2), REG(b + 4), REG(b + 6) * 8, v.addr, v.repeat, REG(b + 8), REG(b + 10));
    }
    logf("main vol %04x %04x cnt %04x cdvol %04x %04x rvbvol %04x %04x mbase %04x", REG(0x1d80), REG(0x1d82), REG(0x1daa), REG(0x1db0), REG(0x1db2), REG(0x1d84), REG(0x1d86), REG(0x1da2));
}

void spu_init() {
    memset(sram, 0, sizeof sram);
    memset(regs, 0, sizeof regs);
    memset(voices, 0, sizeof voices);
    last_cycle = M.cycles;
}

static const s32 pos_tbl[5] = {0, 60, 115, 98, 122}, neg_tbl[5] = {0, 0, -52, -55, -60};

static void check_irq(u32 addr) {
    u16 cnt = REG(0x1daa);
    if (!(cnt & 0x40)) return;
    u32 irq_addr = (u32)REG(0x1da4) * 8;
    if ((addr & ~15u) == (irq_addr & ~15u)) {
        if (!(REG(0x1dae) & 0x40)) {
            REG(0x1dae) |= 0x40;
            M.irq(IRQ_SPU);
        }
    }
}

static void decode_block(Voice &v) {
    const u8 *b = sram + (v.addr & 0x7fff0);
    check_irq(v.addr);
    int shift = 12 - (b[0] & 15);
    if (shift < 0) shift = 3;  // ranges 13..15 behave like 9
    int f = std::min((b[0] >> 4) & 7, 4);
    v.flags = b[1];
    if (v.flags & 4) {
        if (!v.repeat_set_by_game) v.repeat = v.addr;
    }
    for (int i = 0; i < 28; i++) {
        int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 15;
        s32 s = (s32)(s16)(u16)(nib << 12) >> (12 - shift);
        s32 val = s + ((v.p1 * pos_tbl[f] + v.p2 * neg_tbl[f] + 32) >> 6);
        val = std::clamp(val, -32768, 32767);
        v.p2 = v.p1;
        v.p1 = val;
        v.buf[i] = (s16)val;
    }
}

static void key_on(int n) {
    Voice &v = voices[n];
    u32 base = 0x1c00 + n * 16;
    v.addr = (u32)REG(base + 6) * 8;
    v.repeat = (u32)REG(base + 14) * 8;
    v.repeat_set_by_game = false;
    v.counter = 0;
    v.p1 = v.p2 = 0;
    memset(v.hist, 0, sizeof v.hist);
    v.phase = ATTACK;
    v.env = 0;
    v.env_ctr = 0;
    endx &= ~(1u << n);
    decode_block(v);
}

static void key_off(int n) {
    if (voices[n].phase != OFF) voices[n].phase = RELEASE;
}

// ADSR step per nocash: returns new level
static void env_tick(Voice &v, int n) {
    u32 base = 0x1c00 + n * 16;
    u16 lo = REG(base + 8), hi = REG(base + 10);
    bool exp = false, dec = false;
    int shift = 0, step = 0;
    switch (v.phase) {
    case ATTACK: exp = lo & 0x8000; shift = (lo >> 10) & 31; step = 7 - ((lo >> 8) & 3); break;
    case DECAY: exp = true; dec = true; shift = (lo >> 4) & 15; step = -8; break;
    case SUSTAIN:
        exp = hi & 0x8000; dec = hi & 0x4000; shift = (hi >> 8) & 31;
        step = dec ? -8 + ((hi >> 6) & 3) : 7 - ((hi >> 6) & 3);
        break;
    case RELEASE: exp = hi & 0x20; dec = true; shift = hi & 31; step = -8; break;
    default: return;
    }
    u32 cycles = 1u << std::max(0, shift - 11);
    s32 st = step << std::max(0, 11 - shift);
    if (exp && !dec && v.env > 0x6000) cycles *= 4;
    if (exp && dec) st = (st * v.env) >> 15;
    if (++v.env_ctr < cycles) return;
    v.env_ctr = 0;
    v.env = std::clamp(v.env + st, 0, 0x7fff);
    if (v.phase == ATTACK && v.env >= 0x7fff) v.phase = DECAY;
    else if (v.phase == DECAY) {
        s32 sl = ((lo & 15) + 1) * 0x800;
        if (v.env <= sl) v.phase = SUSTAIN;
    } else if (v.phase == RELEASE && v.env == 0) v.phase = OFF;
}

static s32 voice_sample(Voice &v, int n, s32 prev_out) {
    if (v.phase == OFF) return 0;
    u32 base = 0x1c00 + n * 16;
    u32 pitch = REG(base + 4);
    u32 pmon = (u32)REG(0x1d90) | ((u32)REG(0x1d92) << 16);
    if (n > 0 && (pmon & (1u << n))) {
        s32 f = (s32)pitch * (prev_out + 0x8000) >> 15;
        pitch = (u32)std::clamp(f, 0, 0xffff);
    }
    pitch = std::min(pitch, 0x4000u);

    u32 idx = v.counter >> 12;
    // 4-point interpolation over decoded samples (hist holds the previous block's tail)
    auto smp = [&](int i) -> s32 {
        int k = (int)idx + i;
        if (k < 0) return v.hist[4 + k];
        return v.buf[std::min(k, 27)];
    };
    s32 s0 = smp(-3), s1 = smp(-2), s2 = smp(-1), s3 = smp(0);
    float t = (v.counter & 0xfff) / 4096.0f;
    // Catmull-Rom between s2 and s3
    float a = -0.5f * s0 + 1.5f * s1 - 1.5f * s2 + 0.5f * s3;
    float b = s0 - 2.5f * s1 + 2.0f * s2 - 0.5f * s3;
    float c = -0.5f * s0 + 0.5f * s2;
    float d = (float)s1;
    s32 out = (s32)(((a * t + b) * t + c) * t + d);
    (void)s3;
    out = std::clamp(out, -32768, 32767);

    u32 non = (u32)REG(0x1d94) | ((u32)REG(0x1d96) << 16);
    if (non & (1u << n)) out = (s16)noise_level;

    env_tick(v, n);
    out = (out * v.env) >> 15;

    v.counter += pitch;
    while ((v.counter >> 12) >= 28) {
        v.counter -= 28 << 12;
        for (int i = 0; i < 4; i++) v.hist[i] = v.buf[24 + i];
        u8 fl = v.flags;
        if (fl & 1) {
            endx |= 1u << n;
            v.addr = v.repeat;
            if (!(fl & 2)) {
                v.phase = OFF;
                v.env = 0;
            }
        } else {
            v.addr = (v.addr + 16) & 0x7ffff;
        }
        decode_block(v);
    }
    return out;
}

static s16 vol_of(u16 r) {
    if (r & 0x8000) return 0x3fff;  // sweep mode: approximate as max sustained level
    return (s16)(r << 1) >> 1;
}

// --- reverb (nocash algorithm, runs at 22.05kHz)
// Reverb work area addressing: offsets are in bytes relative to the current buffer address and
// wrap inside [mBASE, 0x80000).
static u32 rv_addr(s32 byte_off) {
    u32 base = (u32)REG(0x1da2) * 8;
    s64 size = 0x80000 - (s64)base;
    s64 rel = ((s64)reverb_cur - base + byte_off) % size;
    if (rel < 0) rel += size;
    return (base + (u32)rel) & 0x7fffe;
}
static s16 rv_read(s32 off) {
    s16 v;
    memcpy(&v, sram + rv_addr(off), 2);
    return v;
}
static void rv_write(s32 off, s32 val) {
    s16 v = (s16)std::clamp(val, -32768, 32767);
    memcpy(sram + rv_addr(off), &v, 2);
}
static void reverb(s32 in_l, s32 in_r, s32 &out_l, s32 &out_r) {
    auto R16 = [](int i) -> s32 { return (s16)regs[(0x1dc0 - 0x1c00) / 2 + i]; };
    auto U16 = [](int i) -> s32 { return (s32)(u16)regs[(0x1dc0 - 0x1c00) / 2 + i] * 8; };  // address regs, in bytes
    s32 dAPF1 = U16(0), dAPF2 = U16(1), vIIR = R16(2), vCOMB1 = R16(3), vCOMB2 = R16(4), vCOMB3 = R16(5),
        vCOMB4 = R16(6), vWALL = R16(7), vAPF1 = R16(8), vAPF2 = R16(9), mLSAME = U16(10), mRSAME = U16(11),
        mLCOMB1 = U16(12), mRCOMB1 = U16(13), mLCOMB2 = U16(14), mRCOMB2 = U16(15), dLSAME = U16(16),
        dRSAME = U16(17), mLDIFF = U16(18), mRDIFF = U16(19), mLCOMB3 = U16(20), mRCOMB3 = U16(21),
        mLCOMB4 = U16(22), mRCOMB4 = U16(23), dLDIFF = U16(24), dRDIFF = U16(25), mLAPF1 = U16(26),
        mRAPF1 = U16(27), mLAPF2 = U16(28), mRAPF2 = U16(29), vLIN = R16(30), vRIN = R16(31);
    auto mul = [](s32 a, s32 b) { return (a * b) >> 15; };
    bool enable = REG(0x1daa) & 0x80;
    s32 Lin = mul(in_l, vLIN), Rin = mul(in_r, vRIN);
    if (enable) {
        s32 ls = mul(Lin + mul(rv_read(dLSAME), vWALL) - rv_read(mLSAME - 2), vIIR) + rv_read(mLSAME - 2);
        s32 rs = mul(Rin + mul(rv_read(dRSAME), vWALL) - rv_read(mRSAME - 2), vIIR) + rv_read(mRSAME - 2);
        s32 ld = mul(Lin + mul(rv_read(dRDIFF), vWALL) - rv_read(mLDIFF - 2), vIIR) + rv_read(mLDIFF - 2);
        s32 rd = mul(Rin + mul(rv_read(dLDIFF), vWALL) - rv_read(mRDIFF - 2), vIIR) + rv_read(mRDIFF - 2);
        rv_write(mLSAME, ls); rv_write(mRSAME, rs); rv_write(mLDIFF, ld); rv_write(mRDIFF, rd);
    }
    s32 Lout = mul(vCOMB1, rv_read(mLCOMB1)) + mul(vCOMB2, rv_read(mLCOMB2)) + mul(vCOMB3, rv_read(mLCOMB3)) +
               mul(vCOMB4, rv_read(mLCOMB4));
    s32 Rout = mul(vCOMB1, rv_read(mRCOMB1)) + mul(vCOMB2, rv_read(mRCOMB2)) + mul(vCOMB3, rv_read(mRCOMB3)) +
               mul(vCOMB4, rv_read(mRCOMB4));
    Lout = Lout - mul(vAPF1, rv_read(mLAPF1 - dAPF1));
    Rout = Rout - mul(vAPF1, rv_read(mRAPF1 - dAPF1));
    if (enable) { rv_write(mLAPF1, Lout); rv_write(mRAPF1, Rout); }
    Lout = mul(Lout, vAPF1) + rv_read(mLAPF1 - dAPF1);
    Rout = mul(Rout, vAPF1) + rv_read(mRAPF1 - dAPF1);
    Lout = Lout - mul(vAPF2, rv_read(mLAPF2 - dAPF2));
    Rout = Rout - mul(vAPF2, rv_read(mRAPF2 - dAPF2));
    if (enable) { rv_write(mLAPF2, Lout); rv_write(mRAPF2, Rout); }
    Lout = mul(Lout, vAPF2) + rv_read(mLAPF2 - dAPF2);
    Rout = mul(Rout, vAPF2) + rv_read(mRAPF2 - dAPF2);
    out_l = std::clamp(Lout, -32768, 32767);
    out_r = std::clamp(Rout, -32768, 32767);
    u32 base = (u32)REG(0x1da2) * 8;
    reverb_cur += 2;
    if (reverb_cur >= 0x80000 || reverb_cur < base) reverb_cur = base;
}

static void gen_sample(s16 cd_l, s16 cd_r) {
    u16 cnt = REG(0x1daa);
    // noise generator
    {
        u32 shift = (cnt >> 10) & 15, step = ((cnt >> 8) & 3) + 4;
        noise_ctr += step << 16 >> shift;
        if (noise_ctr >= 0x10000) {
            noise_ctr &= 0xffff;
            u32 parity = ((noise_level >> 15) ^ (noise_level >> 12) ^ (noise_level >> 11) ^ (noise_level >> 10) ^ 1) & 1;
            noise_level = ((noise_level << 1) | parity) & 0xffff;
        }
    }
    s32 l = 0, r = 0, rl = 0, rr = 0;
    u32 eon = (u32)REG(0x1d98) | ((u32)REG(0x1d9a) << 16);
    s32 prev = 0;
    for (int n = 0; n < 24; n++) {
        Voice &v = voices[n];
        s32 s = voice_sample(v, n, prev);
        prev = s;
        u32 base = 0x1c00 + n * 16;
        REG(base + 12) = (u16)v.env;
        s32 vl = (s * vol_of(REG(base + 0))) >> 15;
        s32 vr = (s * vol_of(REG(base + 2))) >> 15;
        l += vl;
        r += vr;
        if (eon & (1u << n)) { rl += vl; rr += vr; }
    }
    if (cnt & 1) {  // CD audio
        s32 cl = (cd_l * (s16)REG(0x1db0)) >> 15, cr = (cd_r * (s16)REG(0x1db2)) >> 15;
        l += cl;
        r += cr;
        if (cnt & 4) { rl += cl; rr += cr; }
    }
    // reverb at half rate
    reverb_phase = !reverb_phase;
    if (reverb_phase) {
        reverb_in_l = rl;
        reverb_in_r = rr;
    } else {
        reverb((reverb_in_l + rl) / 2, (reverb_in_r + rr) / 2, reverb_out_l, reverb_out_r);
    }
    if (getenv("UW_NOREVERB")) reverb_out_l = reverb_out_r = 0;
    l += (reverb_out_l * (s16)REG(0x1d84)) >> 15;
    r += (reverb_out_r * (s16)REG(0x1d86)) >> 15;
    if (!(cnt & 0x4000)) { l = 0; r = 0; }  // mute bit (0 = muted)
    l = (std::clamp(l, -32768, 32767) * vol_of(REG(0x1d80))) >> 14;
    r = (std::clamp(r, -32768, 32767) * vol_of(REG(0x1d82))) >> 14;
    if (!(cnt & 0x8000)) { l = 0; r = 0; }  // SPU disabled
    std::lock_guard<std::mutex> lk(ring_mtx);
    if (((ring_w + 2) & 65535) != ring_r) {
        ring[ring_w] = (s16)std::clamp(l, -32768, 32767);
        ring[ring_w + 1] = (s16)std::clamp(r, -32768, 32767);
        ring_w = (ring_w + 2) & 65535;
    }
}

void spu_run() {
    const u64 CPS = 768;  // 33.8688MHz / 44100
    while (M.cycles - last_cycle >= CPS) {
        last_cycle += CPS;
        s16 cd[2];
        cdrom_get_audio(cd, 1);
        gen_sample(cd[0], cd[1]);
    }
}

int spu_fetch(s16 *out, int frames) {
    std::lock_guard<std::mutex> lk(ring_mtx);
    int got = 0;
    while (got < frames && ring_r != ring_w) {
        out[got * 2] = ring[ring_r];
        out[got * 2 + 1] = ring[ring_r + 1];
        ring_r = (ring_r + 2) & 65535;
        got++;
    }
    for (int i = got; i < frames; i++) out[i * 2] = out[i * 2 + 1] = 0;
    return got;
}

u16 spu_read(u32 addr) {
    u32 off = addr & 0xfff;
    if (off >= 0xc00 && off < 0xd80 && (off & 15) == 12) return (u16)voices[(off - 0xc00) >> 4].env;
    switch (off) {
    case 0xd9c: return (u16)endx;
    case 0xd9e: return (u16)(endx >> 16);
    case 0xda6: return (u16)(transfer_addr / 8);
    case 0xdae: {
        u16 cnt = REG(0x1daa);
        return (u16)((cnt & 0x3f) | (REG(0x1dae) & 0x40) | ((cnt & 0x20) ? 0x80 : 0));
    }
    default: return regs[(off - 0xc00) >> 1];
    }
}

void spu_write(u32 addr, u16 v) {
    spu_run();
    u32 off = addr & 0xfff;
    regs[(off - 0xc00) >> 1] = v;
    if (off >= 0xc00 && off < 0xd80) {
        int n = (off - 0xc00) >> 4;
        if ((off & 15) == 14) voices[n].repeat = (u32)v * 8, voices[n].repeat_set_by_game = true;
        return;
    }
    switch (off) {
    case 0xd88: if (g_log_spu) logf("KON lo %04x @%llu", v, (unsigned long long)(M.cycles / CYCLES_PER_FRAME)); for (int i = 0; i < 16; i++) if (v & (1 << i)) key_on(i); break;
    case 0xd8a: if (g_log_spu && v) logf("KON hi %04x", v); for (int i = 0; i < 8; i++) if (v & (1 << i)) key_on(16 + i); break;
    case 0xd8c: if (g_log_spu && v) logf("KOFF lo %04x @%llu", v, (unsigned long long)(M.cycles / CYCLES_PER_FRAME)); for (int i = 0; i < 16; i++) if (v & (1 << i)) key_off(i); break;
    case 0xd8e: if (g_log_spu && v) logf("KOFF hi %04x", v); for (int i = 0; i < 8; i++) if (v & (1 << i)) key_off(16 + i); break;
    case 0xda2: reverb_cur = (u32)v * 8; break;
    case 0xda6: transfer_addr = (u32)v * 8; break;
    case 0xda8:
        check_irq(transfer_addr);
        memcpy(sram + (transfer_addr & 0x7fffe), &v, 2);
        transfer_addr = (transfer_addr + 2) & 0x7ffff;
        break;
    case 0xdaa:
        if (!(v & 0x40)) REG(0x1dae) &= ~0x40;  // IRQ acknowledge
        break;
    default: break;
    }
}

void spu_dma_write(u32 v) {
    check_irq(transfer_addr);
    memcpy(sram + (transfer_addr & 0x7fffc), &v, 4);
    transfer_addr = (transfer_addr + 4) & 0x7ffff;
}

u32 spu_dma_read() {
    u32 v;
    memcpy(&v, sram + (transfer_addr & 0x7fffc), 4);
    transfer_addr = (transfer_addr + 4) & 0x7ffff;
    return v;
}


void st_spu(StateIO &s) {
    s.pod(sram); s.pod(regs); s.pod(voices); s.pod(endx); s.pod(transfer_addr); s.pod(noise_level); s.pod(noise_ctr);
    s.pod(last_cycle); s.pod(reverb_cur); s.pod(reverb_phase); s.pod(reverb_in_l); s.pod(reverb_in_r);
    s.pod(reverb_out_l); s.pod(reverb_out_r);
}

}  // namespace psx
