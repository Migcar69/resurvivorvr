// PS1 GPU: GP0/GP1 command processing and a software rasterizer into 1024x512 VRAM.
#include "psx.h"
#include "state.h"
#include "capture.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstdint>

namespace psx {

static u16 vram[1024 * 512];

// (Development, PSXVR_UPLOADLOG=1) each CPU -> VRAM upload with the CRC32 of its rows (zlib's, over the 16-bit words
// row by row): the texture key of Beetle PSX HW texture packs (<texture crc>-<palette crc>.png).
static uint32_t crc32_update(uint32_t crc, const void *data, size_t n) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    const uint8_t *b = (const uint8_t *)data;
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ b[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}
static void vram_upload_done(int x, int y, int w, int h) {
    static const bool log_on = getenv("PSXVR_UPLOADLOG") != nullptr;
    if (!log_on) return;
    uint32_t crc = 0;
    u16 row[1024];
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) row[i] = vram[((y + j) & 511) * 1024 + ((x + i) & 1023)];
        crc = crc32_update(crc, row, (size_t)w * 2);
    }
    static FILE *f = fopen("vram_uploads.txt", "a");
    if (f) fprintf(f, "%d %d %d %d %x\n", x, y, w, h, crc), fflush(f);
}
u16 *gpu_vram() { return vram; }

// Hook so the VR renderer can observe primitives (set by the frontend; may be null)
u32 g_gp0_addr = 0;  // RAM address of the GP0 word being written (DMA), 0 for CPU writes

static struct {
    // draw state
    u32 texpage = 0;       // E1 bits
    u32 tw_mask_x = 0, tw_mask_y = 0, tw_off_x = 0, tw_off_y = 0;
    int clip_x1 = 0, clip_y1 = 0, clip_x2 = 1023, clip_y2 = 511;
    int off_x = 0, off_y = 0;
    bool mask_set = false, mask_check = false;
    bool tex_disable = false;
    // display state
    bool display_off = true;
    u32 dma_dir = 0;
    int disp_x = 0, disp_y = 0;
    int h1 = 0x200, h2 = 0xc00, v1 = 0x10, v2 = 0x100;
    u32 dispmode = 0;
    bool irq = false;
    // command FIFO
    u32 cmd[16];
    u32 cmd_addr[16];
    int cmd_len = 0, cmd_need = 0;
    // VRAM transfers
    enum { NONE, TO_VRAM, FROM_VRAM, POLYLINE } mode = NONE;
    int tx, ty, tw, th, tcx, tcy;
    u32 read_latch = 0;
    bool odd = false;
    // polyline
    u32 pl_cmd;
    u32 pl_color, pl_pos;
    bool pl_need_color;
    bool pl_have_first;
} G;

void gpu_init() {
    memset(vram, 0, sizeof vram);
    G = {};
    G.mode = G.NONE;
    G.display_off = true;
}

// ------------------------------------------------------------- pixel helpers
static const s8 dither_tbl[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

static inline u16 tex_fetch(int u, int v, u32 tpage, u32 clut) {
    u = (u & ~(G.tw_mask_x * 8)) | ((G.tw_off_x & G.tw_mask_x) * 8);
    v = (v & ~(G.tw_mask_y * 8)) | ((G.tw_off_y & G.tw_mask_y) * 8);
    u &= 0xff;
    v &= 0xff;
    int tx = (tpage & 0xf) * 64, ty = ((tpage >> 4) & 1) * 256;
    int depth = (tpage >> 7) & 3;
    int cx = (clut & 0x3f) * 16, cy = (clut >> 6) & 0x1ff;
    if (depth == 0) {
        u16 w = vram[((ty + v) & 511) * 1024 + ((tx + u / 4) & 1023)];
        int idx = (w >> ((u & 3) * 4)) & 0xf;
        return vram[cy * 1024 + ((cx + idx) & 1023)];
    } else if (depth == 1) {
        u16 w = vram[((ty + v) & 511) * 1024 + ((tx + u / 2) & 1023)];
        int idx = (w >> ((u & 1) * 8)) & 0xff;
        return vram[cy * 1024 + ((cx + idx) & 1023)];
    }
    return vram[((ty + v) & 511) * 1024 + ((tx + u) & 1023)];
}

static inline u16 blend(u16 back, u16 front, int mode) {
    int br = back & 31, bg = (back >> 5) & 31, bb = (back >> 10) & 31;
    int fr = front & 31, fg = (front >> 5) & 31, fb = (front >> 10) & 31;
    int r, g, b;
    switch (mode) {
    case 0: r = (br + fr) >> 1; g = (bg + fg) >> 1; b = (bb + fb) >> 1; break;
    case 1: r = br + fr; g = bg + fg; b = bb + fb; break;
    case 2: r = br - fr; g = bg - fg; b = bb - fb; break;
    default: r = br + (fr >> 2); g = bg + (fg >> 2); b = bb + (fb >> 2); break;
    }
    r = std::clamp(r, 0, 31); g = std::clamp(g, 0, 31); b = std::clamp(b, 0, 31);
    return (u16)(r | (g << 5) | (b << 10) | (front & 0x8000));
}

// Plot one pixel. col is 24-bit shading color (or texture modulation), tex = texel or -1.
static inline void plot(int x, int y, int r, int g, int b, int texel, bool semi, bool raw, bool dither) {
    u16 *dst = &vram[y * 1024 + x];
    if (G.mask_check && (*dst & 0x8000)) return;
    u16 out;
    bool st = semi;
    if (texel >= 0) {
        if (texel == 0) return;  // fully transparent
        if (raw) {
            out = (u16)texel;
        } else {
            int tr = (texel & 31) << 3, tg = ((texel >> 5) & 31) << 3, tb = ((texel >> 10) & 31) << 3;
            tr = (tr * r) >> 7; tg = (tg * g) >> 7; tb = (tb * b) >> 7;
            if (dither) {
                int d = dither_tbl[y & 3][x & 3];
                tr += d; tg += d; tb += d;
            }
            tr = std::clamp(tr, 0, 255); tg = std::clamp(tg, 0, 255); tb = std::clamp(tb, 0, 255);
            out = (u16)((tr >> 3) | ((tg >> 3) << 5) | ((tb >> 3) << 10) | (texel & 0x8000));
        }
        st = semi && (texel & 0x8000);
    } else {
        if (dither) {
            int d = dither_tbl[y & 3][x & 3];
            r = std::clamp(r + d, 0, 255); g = std::clamp(g + d, 0, 255); b = std::clamp(b + d, 0, 255);
        }
        out = (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
    }
    if (st) out = blend(*dst, out, (G.texpage >> 5) & 3);
    if (G.mask_set) out |= 0x8000;
    *dst = out;
}

struct Vtx {
    int x, y;
    int r, g, b;
    int u, v;
};

static void draw_triangle(const Vtx *v0, const Vtx *v1, const Vtx *v2, bool textured, bool gouraud, bool semi,
                          bool raw, u32 tpage, u32 clut) {
    // Reject triangles exceeding hardware size limits
    int minx = std::min({v0->x, v1->x, v2->x}), maxx = std::max({v0->x, v1->x, v2->x});
    int miny = std::min({v0->y, v1->y, v2->y}), maxy = std::max({v0->y, v1->y, v2->y});
    if (maxx - minx >= 1024 || maxy - miny >= 512) return;
    s64 area = (s64)(v1->x - v0->x) * (v2->y - v0->y) - (s64)(v2->x - v0->x) * (v1->y - v0->y);
    if (area == 0) return;
    if (area < 0) { std::swap(v1, v2); area = -area; }
    int x0 = std::max(minx, G.clip_x1), x1 = std::min(maxx, G.clip_x2 + 1);
    int y0 = std::max(miny, G.clip_y1), y1 = std::min(maxy, G.clip_y2 + 1);
    if (x0 >= x1 || y0 >= y1) return;
    bool dither = (G.texpage & 0x200) && (gouraud || (textured && !raw));

    // edge functions with top-left fill rule
    auto edge = [](const Vtx *a, const Vtx *b, int px, int py) -> s64 {
        return (s64)(b->x - a->x) * (py - a->y) - (s64)(b->y - a->y) * (px - a->x);
    };
    auto bias = [](const Vtx *a, const Vtx *b) -> int {
        // top edge or left edge -> inclusive
        int dy = b->y - a->y, dx = b->x - a->x;
        bool top_left = (dy < 0) || (dy == 0 && dx > 0);
        return top_left ? 0 : -1;
    };
    int b0 = bias(v1, v2), b1 = bias(v2, v0), b2 = bias(v0, v1);
    double inv = 1.0 / (double)area;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            s64 w0 = edge(v1, v2, x, y), w1 = edge(v2, v0, x, y), w2 = edge(v0, v1, x, y);
            if (w0 + b0 < 0 || w1 + b1 < 0 || w2 + b2 < 0) continue;
            // with our orientation edges are negative inside; flip sign handling
            double l0 = w0 * inv, l1 = w1 * inv, l2 = w2 * inv;
            int r, g, b;
            if (gouraud) {
                r = (int)(v0->r * l0 + v1->r * l1 + v2->r * l2);
                g = (int)(v0->g * l0 + v1->g * l1 + v2->g * l2);
                b = (int)(v0->b * l0 + v1->b * l1 + v2->b * l2);
            } else {
                r = v0->r; g = v0->g; b = v0->b;
            }
            int texel = -1;
            if (textured) {
                int u = (int)floor(v0->u * l0 + v1->u * l1 + v2->u * l2 + 0.0001);
                int vv = (int)floor(v0->v * l0 + v1->v * l1 + v2->v * l2 + 0.0001);
                texel = tex_fetch(u, vv, tpage, clut);
            }
            plot(x, y, r, g, b, texel, semi, raw, dither);
        }
    }
}

// The rasterizer above expects counter-clockwise edges to be positive; build wrapper
static void triangle(Vtx a, Vtx b, Vtx c, bool textured, bool gouraud, bool semi, bool raw, u32 tpage, u32 clut) {
    s64 area = (s64)(b.x - a.x) * (c.y - a.y) - (s64)(c.x - a.x) * (b.y - a.y);
    if (area < 0) std::swap(b, c);
    draw_triangle(&a, &b, &c, textured, gouraud, semi, raw, tpage, clut);
}

static inline int sext11(u32 v) { return ((s32)(v << 21)) >> 21; }

static void cmd_polygon(const u32 *c) {
    u32 op = c[0] >> 24;
    bool gouraud = op & 0x10, quad = op & 0x08, textured = op & 0x04, semi = op & 0x02, raw = op & 0x01;
    int n = quad ? 4 : 3;
    Vtx v[4];
    int idx = 0;
    u32 tpage = G.texpage, clut = 0;
    u32 color = c[idx++] & 0xffffff;
    for (int i = 0; i < n; i++) {
        if (i > 0 && gouraud) color = c[idx++] & 0xffffff;
        u32 pos = c[idx++];
        v[i].x = sext11(pos) + G.off_x;
        v[i].y = sext11(pos >> 16) + G.off_y;
        v[i].r = color & 0xff;
        v[i].g = (color >> 8) & 0xff;
        v[i].b = (color >> 16) & 0xff;
        v[i].u = v[i].v = 0;
        if (textured) {
            u32 t = c[idx++];
            v[i].u = t & 0xff;
            v[i].v = (t >> 8) & 0xff;
            if (i == 0) clut = t >> 16;
            if (i == 1) {
                tpage = (t >> 16) & 0x9ff;
                G.texpage = (G.texpage & ~0x9ffu) | tpage;
            }
        }
    }
    if (textured && G.tex_disable) textured = false;
    if (textured && raw) { for (auto &vv : v) vv.r = vv.g = vv.b = 128; }
    triangle(v[0], v[1], v[2], textured, gouraud, semi, raw, tpage, clut);
    if (quad) triangle(v[1], v[2], v[3], textured, gouraud, semi, raw, tpage, clut);
}

static void draw_line(int x0, int y0, u32 c0, int x1, int y1, u32 c1, bool gouraud, bool semi) {
    int dx = x1 - x0, dy = y1 - y0;
    if (std::abs(dx) >= 1024 || std::abs(dy) >= 512) return;
    int steps = std::max(std::abs(dx), std::abs(dy));
    bool dither = (G.texpage & 0x200) && gouraud;
    for (int i = 0; i <= steps; i++) {
        int x = steps ? x0 + (dx * i + (dx >= 0 ? steps / 2 : -steps / 2)) / steps : x0;
        int y = steps ? y0 + (dy * i + (dy >= 0 ? steps / 2 : -steps / 2)) / steps : y0;
        if (x < G.clip_x1 || x > G.clip_x2 || y < G.clip_y1 || y > G.clip_y2) continue;
        int r = c0 & 0xff, g = (c0 >> 8) & 0xff, b = (c0 >> 16) & 0xff;
        if (gouraud && steps) {
            r += (((int)(c1 & 0xff) - r) * i) / steps;
            g += (((int)((c1 >> 8) & 0xff) - g) * i) / steps;
            b += (((int)((c1 >> 16) & 0xff) - b) * i) / steps;
        }
        plot(x, y, r, g, b, -1, semi, false, dither);
    }
}

static void cmd_rect(const u32 *c) {
    u32 op = c[0] >> 24;
    bool textured = op & 0x04, semi = op & 0x02, raw = op & 0x01;
    int idx = 1;
    u32 pos = c[idx++];
    int x = sext11(pos) + G.off_x, y = sext11(pos >> 16) + G.off_y;
    int u0 = 0, v0 = 0;
    u32 clut = 0;
    if (textured) {
        u32 t = c[idx++];
        u0 = t & 0xff;
        v0 = (t >> 8) & 0xff;
        clut = t >> 16;
    }
    int w, h;
    switch ((op >> 3) & 3) {
    case 0: { u32 s = c[idx++]; w = s & 0x3ff; h = (s >> 16) & 0x1ff; break; }
    case 1: w = h = 1; break;
    case 2: w = h = 8; break;
    default: w = h = 16; break;
    }
    if (G.tex_disable) textured = false;
    int r = c[0] & 0xff, g = (c[0] >> 8) & 0xff, b = (c[0] >> 16) & 0xff;
    if (raw) r = g = b = 128;
    bool flipx = G.texpage & 0x1000, flipy = G.texpage & 0x2000;
    for (int yy = 0; yy < h; yy++) {
        int py = y + yy;
        if (py < G.clip_y1 || py > G.clip_y2) continue;
        for (int xx = 0; xx < w; xx++) {
            int px = x + xx;
            if (px < G.clip_x1 || px > G.clip_x2) continue;
            int texel = -1;
            if (textured) {
                int u = flipx ? u0 - xx : u0 + xx;
                int v = flipy ? v0 - yy : v0 + yy;
                texel = tex_fetch(u, v, G.texpage, clut);
            }
            plot(px, py, r, g, b, texel, semi, raw, false);
        }
    }
}

static int cmd_length(u32 w) {
    u32 op = w >> 24;
    if (op >= 0x20 && op < 0x40) {
        int n = (op & 8) ? 4 : 3;
        int per = 1 + ((op & 4) ? 1 : 0) + ((op & 0x10) ? 1 : 0);
        return per * n + ((op & 0x10) ? 0 : 1);
    }
    if (op >= 0x40 && op < 0x60) {
        if (op & 8) return -1;  // polyline: variable
        return (op & 0x10) ? 4 : 3;
    }
    if (op >= 0x60 && op < 0x80) {
        int len = 2 + ((op & 4) ? 1 : 0);
        if (((op >> 3) & 3) == 0) len++;
        return len;
    }
    if (op >= 0x80 && op < 0xa0) return 4;
    if (op >= 0xa0 && op < 0xe0) return 3;
    if (op == 0x02) return 3;
    return 1;
}

static void exec_cmd(const u32 *c, int len) {
    u32 op = c[0] >> 24;
    if (g_capture) {
        GpuDrawState st = {G.texpage, G.off_x, G.off_y, G.clip_x1, G.clip_y1, G.clip_x2, G.clip_y2, G.tex_disable,
                           G.tw_mask_x | (G.tw_mask_y << 5) | (G.tw_off_x << 10) | (G.tw_off_y << 15)};
        capture_cmd(c, G.cmd_addr, len, st);
    }
    if (op >= 0x20 && op < 0x40) { cmd_polygon(c); return; }
    if (op >= 0x40 && op < 0x60) {
        bool gour = op & 0x10;
        u32 c0 = c[0] & 0xffffff, p0 = c[1];
        u32 c1 = gour ? c[2] & 0xffffff : c0;
        u32 p1 = gour ? c[3] : c[2];
        draw_line(sext11(p0) + G.off_x, sext11(p0 >> 16) + G.off_y, c0, sext11(p1) + G.off_x,
                  sext11(p1 >> 16) + G.off_y, c1, gour, op & 2);
        return;
    }
    if (op >= 0x60 && op < 0x80) { cmd_rect(c); return; }
    switch (op) {
    case 0x02: {  // fill rectangle (ignores clip/mask)
        int x = c[1] & 0x3f0, y = (c[1] >> 16) & 0x1ff;
        int w = ((c[2] & 0x3ff) + 15) & ~15, h = (c[2] >> 16) & 0x1ff;
        u32 col = c[0];
        u16 p = (u16)(((col >> 3) & 31) | (((col >> 11) & 31) << 5) | (((col >> 19) & 31) << 10));
        capture_vram_write(x, y, w, h);
        for (int yy = 0; yy < h; yy++)
            for (int xx = 0; xx < w; xx++) vram[((y + yy) & 511) * 1024 + ((x + xx) & 1023)] = p;
        return;
    }
    case 0x1f: G.irq = true; M.irq(IRQ_GPU); return;
    case 0xe1:
        G.texpage = c[0] & 0x3fff;
        G.tex_disable = (c[0] >> 11) & 1;
        return;
    case 0xe2:
        G.tw_mask_x = c[0] & 31; G.tw_mask_y = (c[0] >> 5) & 31;
        G.tw_off_x = (c[0] >> 10) & 31; G.tw_off_y = (c[0] >> 15) & 31;
        return;
    case 0xe3: G.clip_x1 = c[0] & 0x3ff; G.clip_y1 = (c[0] >> 10) & 0x1ff; return;
    case 0xe4: G.clip_x2 = c[0] & 0x3ff; G.clip_y2 = (c[0] >> 10) & 0x1ff; return;
    case 0xe5: G.off_x = sext11(c[0]); G.off_y = sext11(c[0] >> 11); return;
    case 0xe6: G.mask_set = c[0] & 1; G.mask_check = c[0] & 2; return;
    default: break;
    }
    if (op >= 0x80 && op < 0xa0) {  // VRAM to VRAM
        int sx = c[1] & 0x3ff, sy = (c[1] >> 16) & 0x1ff;
        int dx = c[2] & 0x3ff, dy = (c[2] >> 16) & 0x1ff;
        int w = c[3] & 0x3ff, h = (c[3] >> 16) & 0x1ff;
        if (!w) w = 0x400;
        if (!h) h = 0x200;
        capture_vram_write(dx, dy, w, h);
        std::vector<u16> tmp((size_t)w * h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) tmp[y * w + x] = vram[((sy + y) & 511) * 1024 + ((sx + x) & 1023)];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                u16 *d = &vram[((dy + y) & 511) * 1024 + ((dx + x) & 1023)];
                if (G.mask_check && (*d & 0x8000)) continue;
                *d = tmp[y * w + x] | (G.mask_set ? 0x8000 : 0);
            }
        return;
    }
    if (op >= 0xa0 && op < 0xe0) {
        G.tx = c[1] & 0x3ff; G.ty = (c[1] >> 16) & 0x1ff;
        G.tw = c[2] & 0x3ff; G.th = (c[2] >> 16) & 0x1ff;
        if (!G.tw) G.tw = 0x400;
        if (!G.th) G.th = 0x200;
        G.tcx = G.tcy = 0;
        G.mode = (op < 0xc0) ? G.TO_VRAM : G.FROM_VRAM;
        if (G.mode == G.TO_VRAM) capture_vram_write(G.tx, G.ty, G.tw, G.th);
        return;
    }
}

static void vram_put(u16 p) {
    u16 *d = &vram[((G.ty + G.tcy) & 511) * 1024 + ((G.tx + G.tcx) & 1023)];
    if (!(G.mask_check && (*d & 0x8000))) *d = p | (G.mask_set ? 0x8000 : 0);
    if (++G.tcx >= G.tw) {
        G.tcx = 0;
        if (++G.tcy >= G.th) {
            G.mode = G.NONE;
            vram_upload_done(G.tx, G.ty, G.tw, G.th);
        }
    }
}

static u16 vram_get() {
    u16 p = vram[((G.ty + G.tcy) & 511) * 1024 + ((G.tx + G.tcx) & 1023)];
    if (++G.tcx >= G.tw) {
        G.tcx = 0;
        if (++G.tcy >= G.th) G.mode = G.NONE;
    }
    return p;
}

void gpu_write_gp0(u32 v) {
    if (G.mode == G.TO_VRAM) {
        vram_put((u16)v);
        if (G.mode == G.TO_VRAM) vram_put((u16)(v >> 16));
        return;
    }
    if (G.mode == G.POLYLINE) {
        // Words after the first vertex: [color] vertex [color] vertex ... terminator
        if ((v & 0xf000f000) == 0x50005000) { G.mode = G.NONE; return; }
        bool gour = G.pl_cmd & 0x10000000;
        if (gour && G.pl_need_color) {
            G.pl_need_color = false;
            G.cmd[0] = v & 0xffffff;  // pending color of the next vertex
            return;
        }
        u32 c1 = gour ? G.cmd[0] : G.pl_color;
        if (g_capture) capture_line(sext11(G.pl_pos) + G.off_x, sext11(G.pl_pos >> 16) + G.off_y, sext11(v) + G.off_x,
                                   sext11(v >> 16) + G.off_y, G.pl_color, G.off_x, G.off_y);
        draw_line(sext11(G.pl_pos) + G.off_x, sext11(G.pl_pos >> 16) + G.off_y, G.pl_color, sext11(v) + G.off_x,
                  sext11(v >> 16) + G.off_y, c1, gour, G.pl_cmd & 0x02000000);
        G.pl_color = c1;
        G.pl_pos = v;
        G.pl_need_color = gour;
        return;
    }
    if (G.cmd_len == 0) {
        int need = cmd_length(v);
        if (need < 0) {  // polyline: command+color, then the first vertex
            G.pl_cmd = v;
            G.pl_color = v & 0xffffff;
            G.cmd[0] = v;
            G.cmd_len = 1;
            G.cmd_need = 2;
            return;
        }
        G.cmd_need = need;
    }
    G.cmd_addr[G.cmd_len] = g_gp0_addr;
    G.cmd[G.cmd_len++] = v;
    u32 op = G.cmd[0] >> 24;
    if (op >= 0x48 && op < 0x60 && (op & 8)) {
        G.pl_pos = v;
        G.pl_need_color = (G.pl_cmd & 0x10000000) != 0;
        G.mode = G.POLYLINE;
        G.cmd_len = 0;
        return;
    }
    if (G.cmd_len >= G.cmd_need) {
        exec_cmd(G.cmd, G.cmd_len);
        G.cmd_len = 0;
    }
}

void gpu_write_gp1(u32 v) {
    u32 op = (v >> 24) & 0x3f;
    switch (op) {
    case 0x00:
        G.cmd_len = 0; G.mode = G.NONE; G.display_off = true; G.dma_dir = 0;
        G.texpage = 0; G.irq = false;
        break;
    case 0x01: G.cmd_len = 0; G.mode = G.NONE; break;
    case 0x02: G.irq = false; break;
    case 0x03: G.display_off = v & 1; break;
    case 0x04: G.dma_dir = v & 3; break;
    case 0x05: {
        G.disp_x = v & 0x3fe;
        G.disp_y = (v >> 10) & 0x1ff;
        DisplayInfo d = gpu_display();
        capture_flip(d.x, d.y, d.w, d.h);
        break;
    }
    case 0x06: G.h1 = v & 0xfff; G.h2 = (v >> 12) & 0xfff; if (getenv("PSXVR_DISPLOG")) logf("GP1(06) h1 %d h2 %d", G.h1, G.h2); break;
    case 0x07: G.v1 = v & 0x3ff; G.v2 = (v >> 10) & 0x3ff; if (getenv("PSXVR_DISPLOG")) logf("GP1(07) v1 %d v2 %d", G.v1, G.v2); break;
    case 0x08: G.dispmode = v & 0xff; break;
    case 0x10: {
        switch (v & 7) {
        case 2: G.read_latch = (G.tw_mask_x) | (G.tw_mask_y << 5) | (G.tw_off_x << 10) | (G.tw_off_y << 15); break;
        case 3: G.read_latch = G.clip_x1 | (G.clip_y1 << 10); break;
        case 4: G.read_latch = G.clip_x2 | (G.clip_y2 << 10); break;
        case 5: G.read_latch = ((u32)G.off_x & 0x7ff) | (((u32)G.off_y & 0x7ff) << 11); break;
        case 7: G.read_latch = 2; break;
        default: break;
        }
        break;
    }
    default: break;
    }
}

u32 gpu_read() {
    if (G.mode == G.FROM_VRAM) {
        u32 lo = vram_get();
        u32 hi = G.mode == G.FROM_VRAM ? vram_get() : 0;
        return lo | (hi << 16);
    }
    return G.read_latch;
}

u32 gpu_stat() {
    u32 s = G.texpage & 0x7ff;
    s |= (G.mask_set ? 1u : 0) << 11;
    s |= (G.mask_check ? 1u : 0) << 12;
    s |= 1u << 13;  // interlace field
    s |= (G.dispmode & 0x40 ? 1u : 0) << 16;
    s |= (G.dispmode & 3) << 17;
    s |= ((G.dispmode >> 2) & 1) << 19;
    s |= ((G.dispmode >> 3) & 1) << 20;
    s |= ((G.dispmode >> 4) & 1) << 21;
    s |= ((G.dispmode >> 5) & 1) << 22;
    s |= (G.display_off ? 1u : 0) << 23;
    s |= (G.irq ? 1u : 0) << 24;
    s |= 1u << 26;  // ready for command
    s |= 1u << 27;  // ready to send VRAM
    s |= 1u << 28;  // ready for DMA block
    s |= G.dma_dir << 29;
    u32 dreq = 0;
    switch (G.dma_dir) {
    case 1: dreq = 1; break;
    case 2: dreq = 1; break;
    case 3: dreq = 1; break;
    default: break;
    }
    s |= dreq << 25;
    extern int hw_scanline();
    int line = hw_scanline();
    bool interlace = G.dispmode & 0x20;
    G.odd = interlace ? !G.odd : (line & 1);
    s |= (G.odd ? 1u : 0) << 31;
    return s;
}

void gpu_vblank() {}

// Display timing for light guns: first visible video clock (GP1 06), first line (GP1 07), video clocks per pixel.
void gpu_display_timing(int *h1, int *v1, int *dot_div, bool *interlace) {
    static const int div[] = {10, 8, 5, 4};
    *h1 = G.h1;
    *v1 = G.v1;
    *dot_div = (G.dispmode & 0x40) ? 7 : div[G.dispmode & 3];
    *interlace = (G.dispmode & 0x24) == 0x24;
}

DisplayInfo gpu_display() {
    static const int hres[] = {256, 320, 512, 640};
    int w = (G.dispmode & 0x40) ? 368 : hres[G.dispmode & 3];
    int h = ((G.v2 - G.v1) & 0x3ff);
    if (G.dispmode & 0x24) h *= 2;  // interlaced 480
    if (h <= 0 || h > 512) h = 240;
    return {G.disp_x, G.disp_y, w, std::min(h, 512), (G.dispmode & 0x10) != 0, !G.display_off};
}


void st_gpu(StateIO &s) { s.pod(vram); s.pod(G); }

}  // namespace psx
