// MDEC motion decoder (used by PsyQ libpress for the STR movies).
#include "psx.h"
#include "state.h"
#include <algorithm>

namespace psx {

static u8 qt_y[64], qt_c[64];
static s16 scale[64];
static std::vector<u32> in;
static u32 in_need = 0;
static u32 cur_cmd = 0;
static std::vector<u32> out;
static size_t out_pos = 0;
static u32 ctrl = 0;

static const u8 zigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                              12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                              35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                              58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

void mdec_init() {
    in.clear();
    out.clear();
    out_pos = 0;
    in_need = 0;
}

static void idct(s32 *blk) {
    s32 tmp[64];
    s32 *src = blk, *dst = tmp;
    for (int pass = 0; pass < 2; pass++) {
        for (int x = 0; x < 8; x++)
            for (int y = 0; y < 8; y++) {
                s64 sum = 0;
                for (int z = 0; z < 8; z++) sum += (s64)src[y + z * 8] * (scale[x + z * 8] / 8);
                dst[x + y * 8] = (s32)((sum + 0xfff) / 0x2000);
            }
        std::swap(src, dst);
    }
    // after two passes result is back in blk
}

// Decode one 8x8 block from the halfword stream; returns false at end of data
static bool decode_block(const u16 *&p, const u16 *end, const u8 *qt, s32 *blk) {
    memset(blk, 0, 64 * sizeof(s32));
    while (p < end && *p == 0xfe00) p++;  // padding
    if (p >= end) return false;
    u16 n = *p++;
    int q = n >> 10;
    s32 dc = (s32)((s16)(n << 6) >> 6);
    int k = 0;
    s32 val = dc * qt[0];
    for (;;) {
        if (q == 0) val = dc * 2;
        val = std::clamp(val, -0x400, 0x3ff);
        if (q > 0) blk[zigzag[k]] = val;
        else blk[k] = val;
        if (p >= end) break;
        n = *p++;
        if (n == 0xfe00) break;
        k += (n >> 10) + 1;
        if (k > 63) break;
        dc = (s32)((s16)(n << 6) >> 6);
        val = (dc * qt[k] * q + 4) / 8;
    }
    idct(blk);
    return true;
}

static void put_rgb(std::vector<u8> &rgb, int x, int y, s32 r, s32 g, s32 b, bool sign) {
    r = std::clamp(r, -128, 127);
    g = std::clamp(g, -128, 127);
    b = std::clamp(b, -128, 127);
    if (!sign) { r += 128; g += 128; b += 128; }
    size_t o = ((size_t)y * 16 + x) * 3;
    rgb[o] = (u8)r; rgb[o + 1] = (u8)g; rgb[o + 2] = (u8)b;
}

static void decode_macroblocks() {
    int depth = (cur_cmd >> 27) & 3;
    bool sign = (cur_cmd >> 26) & 1;
    bool bit15 = (cur_cmd >> 25) & 1;
    const u16 *p = (const u16 *)in.data();
    const u16 *end = p + in.size() * 2;
    s32 blk[6][64];
    if (depth >= 2) {
        std::vector<u8> rgb(16 * 16 * 3);
        for (;;) {
            bool ok = true;
            for (int b = 0; b < 6 && ok; b++) ok = decode_block(p, end, b < 2 ? qt_c : qt_y, blk[b]);
            if (!ok) break;
            s32 *cr = blk[0], *cb = blk[1];
            for (int yb = 0; yb < 4; yb++) {
                int ox = (yb & 1) * 8, oy = (yb >> 1) * 8;
                s32 *yy = blk[2 + yb];
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) {
                        int cx = (ox + x) / 2, cy = (oy + y) / 2;
                        s32 R = cr[cy * 8 + cx], B = cb[cy * 8 + cx];
                        s32 G = (-0x0581 * B - 0x0b6c * R) >> 12;
                        s32 rr = (0x166e * R) >> 12;
                        s32 bb = (0x1c5a * B) >> 12;
                        s32 Y = yy[y * 8 + x];
                        put_rgb(rgb, ox + x, oy + y, Y + rr, Y + G, Y + bb, sign);
                    }
            }
            if (depth == 2) {
                const u8 *s = rgb.data();
                for (size_t i = 0; i < rgb.size(); i += 4) {
                    u32 w = s[i] | (s[i + 1] << 8) | (s[i + 2] << 16) | ((u32)s[i + 3] << 24);
                    out.push_back(w);
                }
            } else {
                std::vector<u16> px(256);
                for (int i = 0; i < 256; i++)
                    px[i] = (u16)((rgb[i * 3] >> 3) | ((rgb[i * 3 + 1] >> 3) << 5) | ((rgb[i * 3 + 2] >> 3) << 10) |
                                  (bit15 ? 0x8000 : 0));
                for (int i = 0; i < 256; i += 2) out.push_back(px[i] | ((u32)px[i + 1] << 16));
            }
        }
    } else {
        for (;;) {
            if (!decode_block(p, end, qt_y, blk[0])) break;
            std::vector<u8> pix(64);
            for (int i = 0; i < 64; i++) {
                s32 v = std::clamp(blk[0][i], -128, 127);
                pix[i] = (u8)(sign ? v : v + 128);
            }
            if (depth == 1) {
                for (int i = 0; i < 64; i += 4)
                    out.push_back(pix[i] | (pix[i + 1] << 8) | (pix[i + 2] << 16) | ((u32)pix[i + 3] << 24));
            } else {
                for (int i = 0; i < 64; i += 8) {
                    u32 w = 0;
                    for (int j = 0; j < 8; j++) w |= (u32)(pix[i + j] >> 4) << (j * 4);
                    out.push_back(w);
                }
            }
        }
    }
}

void mdec_write(u32 reg, u32 v) {
    if (reg == 4) {  // control
        if (v & 0x80000000u) mdec_init();
        ctrl = v;
        return;
    }
    if (in_need == 0) {
        cur_cmd = v;
        in.clear();
        u32 cmd = v >> 29;
        if (cmd == 1) {
            in_need = v & 0xffff;
            out.clear();
            out_pos = 0;
        } else if (cmd == 2) {
            in_need = (v & 1) ? 32 : 16;
        } else if (cmd == 3) {
            in_need = 32;
        } else {
            in_need = 0;
        }
        return;
    }
    in.push_back(v);
    if (in.size() < in_need) return;
    in_need = 0;
    u32 cmd = cur_cmd >> 29;
    if (cmd == 1) {
        decode_macroblocks();
    } else if (cmd == 2) {
        const u8 *b = (const u8 *)in.data();
        memcpy(qt_y, b, 64);
        if (cur_cmd & 1) memcpy(qt_c, b + 64, 64);
    } else if (cmd == 3) {
        memcpy(scale, in.data(), 128);
    }
}

u32 mdec_read(u32 reg) {
    if (reg == 0) return mdec_dma_out();
    u32 s = 0;
    if (out_pos >= out.size()) s |= 1u << 31;
    if (in_need) s |= 1u << 29;
    if (ctrl & 0x40000000u) s |= 1u << 28;
    if ((ctrl & 0x20000000u) && out_pos < out.size()) s |= 1u << 27;
    s |= ((cur_cmd >> 25) & 0xf) << 23;
    s |= (in_need ? (in_need - (u32)in.size() - 1) : 0xffff) & 0xffff;
    return s;
}

u32 mdec_dma_out() { return out_pos < out.size() ? out[out_pos++] : 0; }
bool mdec_dma_out_ready() { return out_pos < out.size(); }


void st_mdec(StateIO &s) {
    s.pod(qt_y); s.pod(qt_c); s.pod(scale); s.vec(in); s.pod(in_need); s.pod(cur_cmd); s.vec(out); s.pod(out_pos);
    s.pod(ctrl);
}

}  // namespace psx
