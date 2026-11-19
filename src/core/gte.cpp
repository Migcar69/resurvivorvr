// Geometry Transformation Engine (COP2), bit-accurate per the nocash PSX specs.
#include "psx.h"
#include "pgxp.h"
#include <algorithm>

namespace psx {

u16 g_gte_h_override = 0;  // VR: force a wider projection (smaller H) than the game asks for
u16 g_gte_h_game = 0;
s32 g_world_ofx = 160 << 16, g_world_ofy = 120 << 16;  // screen centre of the game's world view

// Data register indices
enum {
    VXY0 = 0, VZ0, VXY1, VZ1, VXY2, VZ2, RGBC, OTZ, IR0, IR1, IR2, IR3,
    SXY0, SXY1, SXY2, SXYP, SZ0, SZ1, SZ2, SZ3, RGB0, RGB1, RGB2, RES1,
    MAC0, MAC1, MAC2, MAC3, IRGB, ORGB, LZCS, LZCR
};
// Control register indices
enum {
    RT11RT12 = 0, RT13RT21, RT22RT23, RT31RT32, RT33, TRX, TRY, TRZ,
    L11L12, L13L21, L22L23, L31L32, L33, RBK, GBK, BBK,
    LR1LR2, LR3LG1, LG2LG3, LB1LB2, LB3, RFC, GFC, BFC,
    OFX, OFY, H, DQA, DQB, ZSF3, ZSF4, FLAG
};

#define D M.gte.data
#define C M.gte.ctrl

static u32 &flag() { return (u32 &)C[FLAG]; }

static const u8 unr_table[0x101] = {
    0xFF, 0xFD, 0xFB, 0xF9, 0xF7, 0xF5, 0xF3, 0xF1, 0xEF, 0xEE, 0xEC, 0xEA, 0xE8, 0xE6, 0xE4, 0xE3,
    0xE1, 0xDF, 0xDD, 0xDC, 0xDA, 0xD8, 0xD6, 0xD5, 0xD3, 0xD1, 0xD0, 0xCE, 0xCD, 0xCB, 0xC9, 0xC8,
    0xC6, 0xC5, 0xC3, 0xC1, 0xC0, 0xBE, 0xBD, 0xBB, 0xBA, 0xB8, 0xB7, 0xB5, 0xB4, 0xB2, 0xB1, 0xB0,
    0xAE, 0xAD, 0xAB, 0xAA, 0xA9, 0xA7, 0xA6, 0xA4, 0xA3, 0xA2, 0xA0, 0x9F, 0x9E, 0x9C, 0x9B, 0x9A,
    0x99, 0x97, 0x96, 0x95, 0x94, 0x92, 0x91, 0x90, 0x8F, 0x8D, 0x8C, 0x8B, 0x8A, 0x89, 0x87, 0x86,
    0x85, 0x84, 0x83, 0x82, 0x81, 0x7F, 0x7E, 0x7D, 0x7C, 0x7B, 0x7A, 0x79, 0x78, 0x77, 0x75, 0x74,
    0x73, 0x72, 0x71, 0x70, 0x6F, 0x6E, 0x6D, 0x6C, 0x6B, 0x6A, 0x69, 0x68, 0x67, 0x66, 0x65, 0x64,
    0x63, 0x62, 0x61, 0x60, 0x5F, 0x5E, 0x5D, 0x5D, 0x5C, 0x5B, 0x5A, 0x59, 0x58, 0x57, 0x56, 0x55,
    0x54, 0x53, 0x53, 0x52, 0x51, 0x50, 0x4F, 0x4E, 0x4D, 0x4D, 0x4C, 0x4B, 0x4A, 0x49, 0x48, 0x48,
    0x47, 0x46, 0x45, 0x44, 0x43, 0x43, 0x42, 0x41, 0x40, 0x3F, 0x3F, 0x3E, 0x3D, 0x3C, 0x3C, 0x3B,
    0x3A, 0x39, 0x39, 0x38, 0x37, 0x36, 0x36, 0x35, 0x34, 0x33, 0x33, 0x32, 0x31, 0x31, 0x30, 0x2F,
    0x2E, 0x2E, 0x2D, 0x2C, 0x2C, 0x2B, 0x2A, 0x2A, 0x29, 0x28, 0x28, 0x27, 0x26, 0x26, 0x25, 0x24,
    0x24, 0x23, 0x22, 0x22, 0x21, 0x20, 0x20, 0x1F, 0x1E, 0x1E, 0x1D, 0x1D, 0x1C, 0x1B, 0x1B, 0x1A,
    0x19, 0x19, 0x18, 0x18, 0x17, 0x16, 0x16, 0x15, 0x15, 0x14, 0x14, 0x13, 0x12, 0x12, 0x11, 0x11,
    0x10, 0x0F, 0x0F, 0x0E, 0x0E, 0x0D, 0x0D, 0x0C, 0x0C, 0x0B, 0x0A, 0x0A, 0x09, 0x09, 0x08, 0x08,
    0x07, 0x07, 0x06, 0x06, 0x05, 0x05, 0x04, 0x04, 0x03, 0x03, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00,
    0x00};

static u32 gte_divide(u16 h, u16 sz3) {
    if (h < sz3 * 2) {
        int z = 0;
        u16 t = sz3;
        while (!(t & 0x8000) && z < 16) { t <<= 1; z++; }
        u32 n = (u32)h << z;
        u32 d = (u32)sz3 << z;
        u32 u = unr_table[(d - 0x7fc0) >> 7] + 0x101;
        d = (0x2000080 - (d * u)) >> 8;
        d = (0x0000080 + (d * u)) >> 8;
        u32 r = (u32)(((u64)n * d + 0x8000) >> 16);
        return std::min<u32>(0x1ffff, r);
    }
    flag() |= 1u << 17;
    return 0x1ffff;
}

static inline s16 lo16(s32 v) { return (s16)(v & 0xffff); }
static inline s16 hi16(s32 v) { return (s16)((u32)v >> 16); }

// MAC overflow checks (44-bit for MAC1..3)
static s64 mac_check(int i, s64 v) {
    if (v > 0x7ffffffffffLL) flag() |= 1u << (31 - i);   // 30,29,28 positive
    else if (v < -0x80000000000LL) flag() |= 1u << (28 - i);  // 27,26,25 negative
    // sign-extend to 44 bits
    return (v << 20) >> 20;
}

static void set_mac0(s64 v) {
    if (v > 0x7fffffffLL) flag() |= 1u << 16;
    else if (v < -0x80000000LL) flag() |= 1u << 15;
    D[MAC0] = (s32)v;
}

static int g_sf, g_lm;

static s32 set_mac(int i, s64 v) {
    v = mac_check(i, v);
    D[MAC0 + i] = (s32)(v >> g_sf);
    return D[MAC0 + i];
}

static s16 sat_ir(int i, s32 v, bool lm) {
    s32 lo = lm ? 0 : -0x8000;
    if (v < lo) { flag() |= 1u << (25 - i); return (s16)lo; }
    if (v > 0x7fff) { flag() |= 1u << (25 - i); return 0x7fff; }
    return (s16)v;
}

static void set_ir(int i, s32 v, bool lm) { D[IR0 + i] = sat_ir(i, v, lm); }

static void set_ir0(s64 v) {
    if (v < 0) { flag() |= 1u << 12; v = 0; }
    else if (v > 0x1000) { flag() |= 1u << 12; v = 0x1000; }
    D[IR0] = (s32)v;
}

static void push_sz(s64 v) {
    if (v < 0) { flag() |= 1u << 18; v = 0; }
    else if (v > 0xffff) { flag() |= 1u << 18; v = 0xffff; }
    D[SZ0] = D[SZ1]; D[SZ1] = D[SZ2]; D[SZ2] = D[SZ3];
    D[SZ3] = (s32)v;
}

static void push_sxy(s64 x, s64 y, u32 vid = 0) {
    g_sxy_vid[0] = g_sxy_vid[1];
    g_sxy_vid[1] = g_sxy_vid[2];
    g_sxy_vid[2] = vid;
    if (x < -0x400) { flag() |= 1u << 14; x = -0x400; }
    else if (x > 0x3ff) { flag() |= 1u << 14; x = 0x3ff; }
    if (y < -0x400) { flag() |= 1u << 13; y = -0x400; }
    else if (y > 0x3ff) { flag() |= 1u << 13; y = 0x3ff; }
    D[SXY0] = D[SXY1];
    D[SXY1] = D[SXY2];
    D[SXY2] = (s32)(((u32)x & 0xffff) | ((u32)y << 16));
}

static u8 sat_color(int i, s32 v) {
    if (v < 0) { flag() |= 1u << (21 - i); return 0; }
    if (v > 0xff) { flag() |= 1u << (21 - i); return 0xff; }
    return (u8)v;
}

static void push_color() {
    u8 r = sat_color(0, D[MAC1] >> 4);
    u8 g = sat_color(1, D[MAC2] >> 4);
    u8 b = sat_color(2, D[MAC3] >> 4);
    u8 code = (u8)((u32)D[RGBC] >> 24);
    D[RGB0] = D[RGB1];
    D[RGB1] = D[RGB2];
    D[RGB2] = (s32)(r | (g << 8) | (b << 16) | ((u32)code << 24));
}

// Matrix access helpers
static s16 mat(int base, int idx) {
    // base: 0 = rotation, 8 = light, 16 = color
    int reg = base + idx / 2;
    return (idx & 1) ? hi16(C[reg]) : lo16(C[reg]);
}

static s16 vx(int n) { return lo16(D[n * 2]); }
static s16 vy(int n) { return hi16(D[n * 2]); }
static s16 vz(int n) { return lo16(D[n * 2 + 1]); }

static s16 ir(int i) { return (s16)D[IR0 + i]; }

static void mat_mul_vec(int mbase, const s16 v[3], const s32 tr[3], bool lm) {
    for (int i = 0; i < 3; i++) {
        s64 acc = (s64)tr[i] << 12;
        acc = mac_check(i + 1, acc + (s64)mat(mbase, i * 3 + 0) * v[0]);
        acc = mac_check(i + 1, acc + (s64)mat(mbase, i * 3 + 1) * v[1]);
        acc = mac_check(i + 1, acc + (s64)mat(mbase, i * 3 + 2) * v[2]);
        set_mac(i + 1, acc);
        set_ir(i + 1, D[MAC1 + i], lm);
    }
}

static void rtp(int n, bool last) {
    s16 v[3] = {vx(n), vy(n), vz(n)};
    s32 tr[3] = {C[TRX], C[TRY], C[TRZ]};
    s64 mz = 0, accs[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
        s64 acc = (s64)tr[i] << 12;
        acc = mac_check(i + 1, acc + (s64)mat(0, i * 3 + 0) * v[0]);
        acc = mac_check(i + 1, acc + (s64)mat(0, i * 3 + 1) * v[1]);
        acc = mac_check(i + 1, acc + (s64)mat(0, i * 3 + 2) * v[2]);
        set_mac(i + 1, acc);
        accs[i] = acc;
        if (i == 2) mz = acc;
    }
    set_ir(1, D[MAC1], g_lm);
    set_ir(2, D[MAC2], g_lm);
    // IR3 saturation flag quirk: flag is set based on (MAC3>>12) regardless of sf
    {
        s32 m3 = D[MAC3];
        s32 lo = g_lm ? 0 : -0x8000;
        D[IR3] = m3 < lo ? lo : (m3 > 0x7fff ? 0x7fff : m3);
        s64 z12 = mz >> 12;
        if (z12 < -0x8000 || z12 > 0x7fff) flag() |= 1u << 22;
    }
    push_sz(mz >> 12);
    // VR: widen the field of view only for the world view (projection centred on the screen); HUD models like
    // the compass are projected around other centres and keep the game's own H.
    bool world_view = C[OFX] == g_world_ofx && C[OFY] == g_world_ofy;
    u16 h = (g_gte_h_override && world_view) ? g_gte_h_override : (u16)C[H];
    u32 div = gte_divide(h, (u16)D[SZ3]);
    s64 x = (s64)div * ir(1) + C[OFX];
    s64 y = (s64)div * ir(2) + C[OFY];
    set_mac0(x);
    set_mac0(y);
    u32 vid = 0;
    if (g_pgxp) {
        s64 gx = x, gy = y;
        if (h != (u16)C[H]) {  // where the game's own projection would have put it (HUD models drawn as in the original)
            u32 gdiv = gte_divide((u16)C[H], (u16)D[SZ3]);
            gx = (s64)gdiv * ir(1) + C[OFX];
            gy = (s64)gdiv * ir(2) + C[OFY];
        }
        vid = vid_new(accs[0] / 4096.0f, accs[1] / 4096.0f, accs[2] / 4096.0f, world_view ? 1 : 2,
                      (s16)std::clamp<s64>(gx >> 16, -1024, 1023), (s16)std::clamp<s64>(gy >> 16, -1024, 1023));
    }
    push_sxy(x >> 16, y >> 16, vid);
    if (last) {
        s64 dq = (s64)div * (s16)C[DQA] + C[DQB];
        set_mac0(dq);
        set_ir0(dq >> 12);
    }
}

static void ncds_core(int n, int mode);

// Color pipeline helpers
static void light_col(const s16 v[3]) {
    s32 bk[3] = {C[RBK], C[GBK], C[BBK]};
    s32 zero[3] = {0, 0, 0};
    mat_mul_vec(8, v, zero, g_lm);  // light matrix
    s16 l[3] = {ir(1), ir(2), ir(3)};
    mat_mul_vec(16, l, bk, g_lm);   // color matrix + background
}

static void depth_cue_color(s32 r, s32 g, s32 b) {
    // MAC = (FC<<12 - col) saturated into IR, then col + IR*IR0
    s32 fc[3] = {C[RFC], C[GFC], C[BFC]};
    s32 col[3] = {r, g, b};
    s32 t[3];
    for (int i = 0; i < 3; i++) {
        s64 v = ((s64)fc[i] << 12) - col[i];
        v = mac_check(i + 1, v);
        t[i] = sat_ir(i + 1, (s32)(v >> g_sf), false);
    }
    for (int i = 0; i < 3; i++) {
        s64 v = (s64)t[i] * ir(0) + col[i];
        set_mac(i + 1, mac_check(i + 1, v));
        set_ir(i + 1, D[MAC1 + i], g_lm);
    }
}

static void ncds_core(int n, int mode) {
    // mode: 0 = NCS, 1 = NCCS, 2 = NCDS
    s16 v[3] = {vx(n), vy(n), vz(n)};
    light_col(v);
    if (mode >= 1) {
        u32 rgbc = (u32)D[RGBC];
        s32 r = (s32)((rgbc & 0xff) << 4) * ir(1);
        s32 g = (s32)(((rgbc >> 8) & 0xff) << 4) * ir(2);
        s32 b = (s32)(((rgbc >> 16) & 0xff) << 4) * ir(3);
        if (mode == 2) {
            depth_cue_color(r, g, b);
        } else {
            set_mac(1, mac_check(1, r));
            set_mac(2, mac_check(2, g));
            set_mac(3, mac_check(3, b));
            set_ir(1, D[MAC1], g_lm);
            set_ir(2, D[MAC2], g_lm);
            set_ir(3, D[MAC3], g_lm);
        }
    }
    push_color();
}

void Gte::command(u32 op) {
    g_sf = (op & (1 << 19)) ? 12 : 0;
    g_lm = (op >> 10) & 1;
    flag() = 0;
    switch (op & 0x3f) {
    case 0x01: rtp(0, true); break;  // RTPS
    case 0x06: {  // NCLIP
        s32 x0 = lo16(D[SXY0]), y0 = hi16(D[SXY0]);
        s32 x1 = lo16(D[SXY1]), y1 = hi16(D[SXY1]);
        s32 x2 = lo16(D[SXY2]), y2 = hi16(D[SXY2]);
        if (g_gte_h_override && g_pgxp) {
            // VR: decide on the points as the game's own H places them. The widened view only scales the picture
            // around the centre (same winding), but small HUD models (compass) lose faces to rounding otherwise.
            // Only when all three are known and not saturated (far off-screen points would flip the winding).
            s32 gx[3], gy[3];
            bool ok = true;
            for (int i = 0; i < 3 && ok; i++) {
                u32 vid = g_sxy_vid[i];
                ok = !(vid & VID_DERIVED) && vid_valid(vid);
                if (!ok) break;
                const Vert3 &p = vid_get(vid);
                gx[i] = p.gsx;
                gy[i] = p.gsy;
                ok = gx[i] > -1024 && gx[i] < 1023 && gy[i] > -1024 && gy[i] < 1023;
            }
            if (ok) { x0 = gx[0]; y0 = gy[0]; x1 = gx[1]; y1 = gy[1]; x2 = gx[2]; y2 = gy[2]; }
            // Sub-pixel: a small polygon far away (a face 8 m off: under a pixel on the screen) had zero area in
            // whole pixels and the game dropped it as facing away; VR looks at it closely (holes in the faces).
            // The winding from the tracked camera-space points, projected as the game does, decides instead; a
            // front face keeps at least 1 (the game only looks at the sign).
            if (ok) {
                float px[3], py[3];
                for (int i = 0; i < 3 && ok; i++) {
                    const Vert3 &p = vid_get(g_sxy_vid[i]);
                    if (p.z < 1.0f) { ok = false; break; }
                    px[i] = p.x / p.z, py[i] = p.y / p.z;
                }
                if (ok) {
                    float hh = (float)(g_gte_h_game ? g_gte_h_game : 256);
                    double a = ((double)px[0] * py[1] + (double)px[1] * py[2] + (double)px[2] * py[0] - (double)px[0] * py[2] -
                                (double)px[1] * py[0] - (double)px[2] * py[1]) * hh * hh;
                    s64 iv = (s64)x0 * y1 + (s64)x1 * y2 + (s64)x2 * y0 - (s64)x0 * y2 - (s64)x1 * y0 - (s64)x2 * y1;
                    if (iv == 0 && a != 0) {
                        set_mac0(a > 0 ? 1 : -1);
                        break;
                    }
                }
            }
        }
        s64 v = (s64)x0 * y1 + (s64)x1 * y2 + (s64)x2 * y0 - (s64)x0 * y2 - (s64)x1 * y0 - (s64)x2 * y1;
        set_mac0(v);
        break;
    }
    case 0x0c: {  // OP
        s32 d1 = lo16(C[RT11RT12]), d2 = lo16(C[RT22RT23]), d3 = lo16(C[RT33]);
        set_mac(1, mac_check(1, (s64)ir(3) * d2 - (s64)ir(2) * d3));
        set_mac(2, mac_check(2, (s64)ir(1) * d3 - (s64)ir(3) * d1));
        set_mac(3, mac_check(3, (s64)ir(2) * d1 - (s64)ir(1) * d2));
        set_ir(1, D[MAC1], g_lm); set_ir(2, D[MAC2], g_lm); set_ir(3, D[MAC3], g_lm);
        break;
    }
    case 0x10: {  // DPCS
        u32 rgbc = (u32)D[RGBC];
        depth_cue_color((rgbc & 0xff) << 16, ((rgbc >> 8) & 0xff) << 16, ((rgbc >> 16) & 0xff) << 16);
        push_color();
        break;
    }
    case 0x11: {  // INTPL
        depth_cue_color((s32)ir(1) << 12, (s32)ir(2) << 12, (s32)ir(3) << 12);
        push_color();
        break;
    }
    case 0x12: {  // MVMVA
        int mx = (op >> 17) & 3, vsel = (op >> 15) & 3, cv = (op >> 13) & 3;
        s16 v[3];
        if (vsel < 3) { v[0] = vx(vsel); v[1] = vy(vsel); v[2] = vz(vsel); }
        else { v[0] = ir(1); v[1] = ir(2); v[2] = ir(3); }
        s32 tr[3] = {0, 0, 0};
        if (cv == 0) { tr[0] = C[TRX]; tr[1] = C[TRY]; tr[2] = C[TRZ]; }
        else if (cv == 1) { tr[0] = C[RBK]; tr[1] = C[GBK]; tr[2] = C[BBK]; }
        else if (cv == 2) { tr[0] = C[RFC]; tr[1] = C[GFC]; tr[2] = C[BFC]; }
        if (mx == 3) {
            // garbage matrix: R=-0x60<<4, R<<4, IR0 ... (rarely used)
            s16 r = (s16)(D[RGBC] & 0xff) << 4;
            s16 m[9] = {(s16)-r, r, ir(0), mat(0, 3), mat(0, 3), mat(0, 3), mat(0, 5), mat(0, 5), mat(0, 5)};
            for (int i = 0; i < 3; i++) {
                s64 acc = (s64)tr[i] << 12;
                acc = mac_check(i + 1, acc + (s64)m[i * 3] * v[0]);
                acc = mac_check(i + 1, acc + (s64)m[i * 3 + 1] * v[1]);
                acc = mac_check(i + 1, acc + (s64)m[i * 3 + 2] * v[2]);
                set_mac(i + 1, acc);
                set_ir(i + 1, D[MAC1 + i], g_lm);
            }
        } else if (cv == 2) {
            // FC bugged: only the last two columns are added properly
            int mb = mx * 8;
            for (int i = 0; i < 3; i++) {
                s64 a1 = mac_check(i + 1, ((s64)tr[i] << 12) + (s64)mat(mb, i * 3) * v[0]);
                sat_ir(i + 1, (s32)(a1 >> g_sf), false);
                s64 acc = mac_check(i + 1, (s64)mat(mb, i * 3 + 1) * v[1]);
                acc = mac_check(i + 1, acc + (s64)mat(mb, i * 3 + 2) * v[2]);
                set_mac(i + 1, acc);
                set_ir(i + 1, D[MAC1 + i], g_lm);
            }
        } else {
            mat_mul_vec(mx * 8, v, tr, g_lm);
        }
        break;
    }
    case 0x13: ncds_core(0, 2); break;  // NCDS
    case 0x14: {  // CDP
        s16 l[3] = {ir(1), ir(2), ir(3)};
        s32 bk[3] = {C[RBK], C[GBK], C[BBK]};
        mat_mul_vec(16, l, bk, g_lm);
        u32 rgbc = (u32)D[RGBC];
        depth_cue_color((s32)((rgbc & 0xff) << 4) * ir(1), (s32)(((rgbc >> 8) & 0xff) << 4) * ir(2),
                        (s32)(((rgbc >> 16) & 0xff) << 4) * ir(3));
        push_color();
        break;
    }
    case 0x16: ncds_core(0, 2); ncds_core(1, 2); ncds_core(2, 2); break;  // NCDT
    case 0x1b: ncds_core(0, 1); break;  // NCCS
    case 0x1c: {  // CC
        s16 l[3] = {ir(1), ir(2), ir(3)};
        s32 bk[3] = {C[RBK], C[GBK], C[BBK]};
        mat_mul_vec(16, l, bk, g_lm);
        u32 rgbc = (u32)D[RGBC];
        set_mac(1, mac_check(1, (s64)((rgbc & 0xff) << 4) * ir(1)));
        set_mac(2, mac_check(2, (s64)(((rgbc >> 8) & 0xff) << 4) * ir(2)));
        set_mac(3, mac_check(3, (s64)(((rgbc >> 16) & 0xff) << 4) * ir(3)));
        set_ir(1, D[MAC1], g_lm); set_ir(2, D[MAC2], g_lm); set_ir(3, D[MAC3], g_lm);
        push_color();
        break;
    }
    case 0x1e: ncds_core(0, 0); break;  // NCS
    case 0x20: ncds_core(0, 0); ncds_core(1, 0); ncds_core(2, 0); break;  // NCT
    case 0x28: {  // SQR
        for (int i = 1; i <= 3; i++) {
            set_mac(i, mac_check(i, (s64)ir(i) * ir(i)));
            set_ir(i, D[MAC0 + i], g_lm);
        }
        break;
    }
    case 0x29: {  // DCPL
        u32 rgbc = (u32)D[RGBC];
        depth_cue_color((s32)((rgbc & 0xff) << 4) * ir(1), (s32)(((rgbc >> 8) & 0xff) << 4) * ir(2),
                        (s32)(((rgbc >> 16) & 0xff) << 4) * ir(3));
        push_color();
        break;
    }
    case 0x2a: {  // DPCT
        for (int k = 0; k < 3; k++) {
            u32 c0 = (u32)D[RGB0];
            depth_cue_color((c0 & 0xff) << 16, ((c0 >> 8) & 0xff) << 16, ((c0 >> 16) & 0xff) << 16);
            push_color();
        }
        break;
    }
    case 0x2d: {  // AVSZ3
        s64 v = (s64)(s16)C[ZSF3] * ((u16)D[SZ1] + (u16)D[SZ2] + (u16)D[SZ3]);
        set_mac0(v);
        s64 o = v >> 12;
        if (o < 0) { flag() |= 1u << 18; o = 0; } else if (o > 0xffff) { flag() |= 1u << 18; o = 0xffff; }
        D[OTZ] = (s32)o;
        break;
    }
    case 0x2e: {  // AVSZ4
        s64 v = (s64)(s16)C[ZSF4] * ((u16)D[SZ0] + (u16)D[SZ1] + (u16)D[SZ2] + (u16)D[SZ3]);
        set_mac0(v);
        s64 o = v >> 12;
        if (o < 0) { flag() |= 1u << 18; o = 0; } else if (o > 0xffff) { flag() |= 1u << 18; o = 0xffff; }
        D[OTZ] = (s32)o;
        break;
    }
    case 0x30: rtp(0, false); rtp(1, false); rtp(2, true); break;  // RTPT
    case 0x3d: {  // GPF
        for (int i = 1; i <= 3; i++) {
            set_mac(i, mac_check(i, (s64)ir(0) * ir(i)));
            set_ir(i, D[MAC0 + i], g_lm);
        }
        push_color();
        break;
    }
    case 0x3e: {  // GPL
        for (int i = 1; i <= 3; i++) {
            s64 base = (s64)D[MAC0 + i] << g_sf;
            set_mac(i, mac_check(i, base + (s64)ir(0) * ir(i)));
            set_ir(i, D[MAC0 + i], g_lm);
        }
        push_color();
        break;
    }
    case 0x3f: ncds_core(0, 1); ncds_core(1, 1); ncds_core(2, 1); break;  // NCCT
    default: logf("GTE: unknown op %07x", op); break;
    }
    if (flag() & 0x7f87e000) flag() |= 0x80000000u;
}

static u32 lzc(u32 v) {
    u32 x = (v & 0x80000000) ? ~v : v;
    u32 n = 0;
    while (n < 32 && !(x & 0x80000000)) { x <<= 1; n++; }
    return n;
}

u32 Gte::read_data(int r) {
    switch (r) {
    case VZ0: case VZ1: case VZ2: case IR0: case IR1: case IR2: case IR3:
        return (u32)(s32)(s16)data[r];
    case OTZ: case SZ0: case SZ1: case SZ2: case SZ3:
        return (u32)(u16)data[r];
    case SXYP: return (u32)data[SXY2];
    case IRGB: case ORGB: {
        auto c5 = [](s32 v) { v >>= 7; return (u32)(v < 0 ? 0 : v > 0x1f ? 0x1f : v); };
        return c5(data[IR1]) | (c5(data[IR2]) << 5) | (c5(data[IR3]) << 10);
    }
    default: return (u32)data[r];
    }
}

void Gte::write_data(int r, u32 v) {
    switch (r) {
    case SXYP:
        g_sxy_vid[0] = g_sxy_vid[1];
        g_sxy_vid[1] = g_sxy_vid[2];
        g_sxy_vid[2] = g_gte_in_vid;
        data[SXY0] = data[SXY1];
        data[SXY1] = data[SXY2];
        data[SXY2] = (s32)v;
        break;
    case IRGB:
        data[IRGB] = (s32)(v & 0x7fff);
        data[IR1] = (s32)((v & 0x1f) << 7);
        data[IR2] = (s32)(((v >> 5) & 0x1f) << 7);
        data[IR3] = (s32)(((v >> 10) & 0x1f) << 7);
        break;
    case ORGB: break;
    case LZCS:
        data[LZCS] = (s32)v;
        data[LZCR] = (s32)lzc(v);
        break;
    case LZCR: break;
    default:
        if (r >= SXY0 && r <= SXY2) g_sxy_vid[r - SXY0] = g_gte_in_vid;
        data[r] = (s32)v;
        break;
    }
    g_gte_in_vid = 0;
}

u32 Gte::read_ctrl(int r) {
    switch (r) {
    case RT33: case L33: case LB3: case H: case DQA: case ZSF3: case ZSF4:
        if (r == H) return (u32)(s32)(s16)ctrl[r];  // hardware bug: H reads sign-extended
        return (u32)(s32)(s16)ctrl[r];
    default: return (u32)ctrl[r];
    }
}

void Gte::write_ctrl(int r, u32 v) {
    switch (r) {
    case RT33: case L33: case LB3: case DQA: case ZSF3: case ZSF4:
        ctrl[r] = (s32)(s16)v;
        break;
    case H:
        g_gte_h_game = (u16)v;
        ctrl[r] = (s32)(u16)v;  // the override is applied per projection in rtp()
        break;
    case FLAG:
        ctrl[r] = (s32)(v & 0x7ffff000);
        if (v & 0x7f87e000) ctrl[r] |= 0x80000000;
        break;
    default: ctrl[r] = (s32)v; break;
    }
}

}  // namespace psx
