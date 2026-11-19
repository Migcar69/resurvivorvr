// Primitive capture + PGXP storage.
#include "capture.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

namespace psx {

bool g_pgxp = false;
Vert3 g_vring[VRING];
u32 g_vcount = 0;
u32 g_vframe = 0;
u32 g_dbg_derived[4];
Shadow g_shadow_ram[RAM_SIZE / 4];
Shadow g_shadow_spad[256];
HalfShadow g_shadow_half_ram[RAM_SIZE / 2];
HalfShadow g_shadow_half_spad[512];
Shadow g_reg_tag[32];
u32 g_sxy_vid[3];
u32 g_gte_in_vid = 0;

bool g_capture = false;
CapFrame g_cap_building, g_cap_done;
u64 g_cap_frames = 0;
s32 g_cam_inject[2] = {0, 0};
s32 g_game_cam_pending[8];
bool g_game_cam_pending_valid = false;
std::vector<u8> g_game_data_pending;
int g_game_yshift_pending = 0;
bool g_game_yshift_pending_valid = false;
u32 g_hud_rtps_pc[8] = {};
u16 g_hud_tpages[4] = {};
u32 g_hide_rtps_pc[4] = {};
u16 g_hide_model_clut = 0;
u32 g_model_rtps_pc = 0;
u32 g_cap_node = 0;

static inline s16 sext11(u32 v) { return (s16)(((s32)(v << 21)) >> 21); }

static void set_vertex(CapVert &cv, u32 pos, u32 addr, const GpuDrawState &st) {
    cv.sx = (s16)(sext11(pos) + st.off_x);
    cv.sy = (s16)(sext11(pos >> 16) + st.off_y);
    cv.has3d = false;
    cv.hud3d = false;
    cv.addr = addr;
    cv.derived = false;
    cv.hud_reproj = false;
    if (addr && g_pgxp) {
        u32 vid = shadow_vid(addr, pos);
        if (vid_valid(vid)) {
            const Vert3 &p = vid_get(vid);
            cv.src_pc = p.pc;
            cv.gsx = p.gsx;
            cv.gsy = p.gsy;
            bool hud_src = false;
            for (u32 hp : g_hud_rtps_pc) hud_src |= hp && hp == p.pc;
            if (vid & VID_DERIVED) {
                // Packet memory is reused: corners may still carry tags left by older frames (even from HUD models).
                // Only derivations from points projected for this frame (or the previous flip) count.
                if (g_vframe - p.frame > 1) return;
                if (hud_src) {
                    cv.hud3d = true;  // e.g. the left-handed fist model, built from projected points
                    return;
                }
                g_dbg_derived[0]++;
                if (p.kind != 1) g_dbg_derived[1]++;
                else if (g_vframe - p.frame > 1) { g_dbg_derived[2]++; g_dbg_derived[3] = g_vframe - p.frame; }
                // The game projects frame N while frame N-1 is still being displayed, so the anchor carries the
                // previous flip's stamp; stale tags left in reused (double-buffered) packets are 2+ frames old.
                if (p.kind == 1 && g_vframe - p.frame <= 1) {
                    cv.derived = true;
                    cv.ax = p.x; cv.ay = p.y; cv.az = p.z;
                }
                return;
            }
            if (p.kind != 1 || hud_src) {
                cv.hud3d = true;
                cv.hud_reproj = p.kind == 1;  // world-centred projection: can be redrawn with the game's H
                cv.x = p.x; cv.y = p.y; cv.z = p.z;
                return;
            }
            cv.has3d = true;
            cv.x = p.x;
            cv.y = p.y;
            cv.z = p.z;
        }
    }
}

void capture_cmd(const u32 *c, const u32 *addrs, int len, const GpuDrawState &st) {
    if (!g_capture) return;
    u32 op = c[0] >> 24;
    CapPrim p = {};
    p.node = g_cap_node;
    p.off_x = (s16)st.off_x;
    p.off_y = (s16)st.off_y;
    p.clip_x1 = (s16)st.clip_x1; p.clip_y1 = (s16)st.clip_y1;
    p.clip_x2 = (s16)st.clip_x2; p.clip_y2 = (s16)st.clip_y2;
    p.tpage = (u16)(st.texpage & 0x9ff);
    p.blend = (u8)((st.texpage >> 5) & 3);
    p.tw = st.tw;
    if (op >= 0x20 && op < 0x40) {
        p.gouraud = op & 0x10;
        p.kind = (op & 0x08) ? CapPrim::QUAD : CapPrim::TRI;
        p.nv = (op & 0x08) ? 4 : 3;
        p.textured = (op & 0x04) && !st.tex_disable;
        p.semi = op & 0x02;
        p.raw = op & 0x01;
        int idx = 0;
        u32 color = c[idx++] & 0xffffff;
        for (int i = 0; i < p.nv; i++) {
            if (i > 0 && p.gouraud) color = c[idx++] & 0xffffff;
            CapVert &v = p.v[i];
            set_vertex(v, c[idx], addrs[idx], st);
            idx++;
            v.r = (u8)color; v.g = (u8)(color >> 8); v.b = (u8)(color >> 16);
            if (op & 0x04) {
                u32 t = c[idx++];
                v.u = (u8)t;
                v.v = (u8)(t >> 8);
                if (i == 0) p.clut = (u16)(t >> 16);
                if (i == 1) {
                    p.tpage = (u16)((t >> 16) & 0x9ff);
                    p.blend = (u8)((t >> 21) & 3);
                }
            }
        }
    } else if (op >= 0x60 && op < 0x80) {
        p.kind = CapPrim::RECT;
        p.nv = 4;
        p.textured = (op & 0x04) && !st.tex_disable;
        p.semi = op & 0x02;
        p.raw = op & 0x01;
        int idx = 1;
        CapVert base;
        set_vertex(base, c[idx], addrs[idx], st);
        idx++;
        u8 u0 = 0, v0 = 0;
        if (op & 0x04) {
            u0 = (u8)c[idx];
            v0 = (u8)(c[idx] >> 8);
            p.clut = (u16)(c[idx] >> 16);
            idx++;
        }
        int w, h;
        switch ((op >> 3) & 3) {
        case 0: w = c[idx] & 0x3ff; h = (c[idx] >> 16) & 0x1ff; break;
        case 1: w = h = 1; break;
        case 2: w = h = 8; break;
        default: w = h = 16; break;
        }
        u32 col = c[0] & 0xffffff;
        for (int i = 0; i < 4; i++) {
            CapVert &v = p.v[i];
            v = base;
            int dx = (i & 1) ? w : 0, dy = (i & 2) ? h : 0;
            v.sx = (s16)(base.sx + dx);
            v.sy = (s16)(base.sy + dy);
            v.has3d = false;
            v.hud3d = base.hud3d;
            v.derived = false;
            v.u = (u8)(u0 + dx);
            v.v = (u8)(v0 + dy);
            v.r = (u8)col; v.g = (u8)(col >> 8); v.b = (u8)(col >> 16);
        }
        p.v[0].has3d = base.has3d;
    } else if (op >= 0x40 && op < 0x60 && !(op & 0x08)) {
        p.kind = CapPrim::LINE;
        p.nv = 2;
        p.gouraud = op & 0x10;
        p.semi = op & 0x02;
        u32 c0 = c[0] & 0xffffff, c1 = p.gouraud ? c[2] & 0xffffff : c0;
        int i1 = p.gouraud ? 3 : 2;
        set_vertex(p.v[0], c[1], addrs[1], st);
        set_vertex(p.v[1], c[i1], addrs[i1], st);
        p.v[0].r = (u8)c0; p.v[0].g = (u8)(c0 >> 8); p.v[0].b = (u8)(c0 >> 16);
        p.v[1].r = (u8)c1; p.v[1].g = (u8)(c1 >> 8); p.v[1].b = (u8)(c1 >> 16);
    } else {
        return;
    }
    for (int i = 0; i < p.nv; i++) {
        u32 pc = p.v[i].src_pc;  // set for tracked, HUD and derived vertices
        if (!pc) continue;
        for (u32 hp : g_hide_rtps_pc) p.hidden |= hp && hp == pc;
        if (g_hide_model_clut && p.clut == g_hide_model_clut && pc == g_model_rtps_pc) p.hidden = true;
    }
    // 3D models drawn with HUD texture pages (weapon arm, compass...) belong to the 2D overlay
    // (also untextured ones: the compass needle is flat shaded, drawn while the HUD page is current)
    {
        for (u16 tp : g_hud_tpages)
            if (tp && (p.tpage & 0x1ff) == tp) {
                for (int i = 0; i < p.nv; i++) {
                    p.v[i].hud_reproj = p.v[i].has3d;
                    p.v[i].has3d = false;
                    p.v[i].hud3d = true;
                    p.v[i].derived = false;
                }
            }
    }
    if (g_cap_building.prims.empty()) {
        g_cap_building.cam_yaw = g_cam_inject[0];
        g_cap_building.cam_pitch = g_cam_inject[1];
        if (g_game_cam_pending_valid) {
            memcpy(g_cap_building.game_cam, g_game_cam_pending, sizeof g_game_cam_pending);
            g_cap_building.game_cam_valid = true;
            g_cap_building.game_data.swap(g_game_data_pending);
            g_game_cam_pending_valid = false;
        }
        if (g_game_yshift_pending_valid) {
            g_cap_building.y_shift = g_game_yshift_pending;
            g_cap_building.y_shift_known = true;
            g_game_yshift_pending_valid = false;
        }
    }
    g_cap_building.prims.push_back(p);
}

void capture_line(int x0, int y0, int x1, int y1, u32 color, int off_x, int off_y) {
    CapPrim p = {};
    p.kind = CapPrim::LINE;
    p.nv = 2;
    p.node = g_cap_node;
    p.off_x = (s16)off_x;
    p.off_y = (s16)off_y;
    p.v[0].sx = (s16)x0; p.v[0].sy = (s16)y0;
    p.v[1].sx = (s16)x1; p.v[1].sy = (s16)y1;
    for (auto &v : p.v) { v.r = (u8)color; v.g = (u8)(color >> 8); v.b = (u8)(color >> 16); }
    g_cap_building.prims.push_back(p);
}

// Software engines often move the projected y by a constant after the GTE (y-shearing to look up/down,
// horizon offset). Such vertices reach the GPU tagged as derived although they are exact world points:
// x equals the game's own projection and y is off by the same amount for the whole frame. Find that shift
// and promote those vertices to real 3D ones (sprite corners differ from their anchor in x, so they stay).
static void promote_shifted_vertices(CapFrame &f, float ofx, float ofy, float h) {
    // where the frame's real projection puts a derived vertex's anchor point
    auto proj = [&](const CapVert &v, float &px, float &py) {
        if (v.az < 1) return false;
        px = ofx + v.ax * h / v.az;
        py = ofy + v.ay * h / v.az;
        return true;
    };
    std::map<int, int> votes;
    for (auto &p : f.prims)
        for (int k = 0; k < p.nv; k++) {
            const CapVert &v = p.v[k];
            float px, py;
            if (v.derived && proj(v, px, py) && std::abs(v.sx - p.off_x - px) <= 1.5f)
                votes[(int)std::lround(v.sy - p.off_y - py)]++;
        }
    int best = 0, shift = 0;
    for (auto &kv : votes)
        if (kv.second > best) best = kv.second, shift = kv.first;
    if (f.y_shift_known) {
        shift = f.y_shift;  // the game told us
        best = votes.count(shift) ? votes[shift] : 0;
    }
    f.y_shift = shift;
    if (best < 16) return;  // no consistent shift this frame
    for (auto &p : f.prims) {
        // A sprite's corners all derive from the same anchor (the object's projected point); in a tiny far sprite a
        // corner can land exactly on the anchor's shifted position, but it is still a sprite corner, not a 3D point
        // (promoting it turned small sprites into stray polygons). Only primitives whose derived vertices each have
        // their own point (e.g. shifted ground) are promoted.
        bool sprite = false;
        for (int a = 0; a < p.nv && !sprite; a++)
            for (int b = a + 1; b < p.nv && !sprite; b++)
                sprite = p.v[a].derived && p.v[b].derived && p.v[a].ax == p.v[b].ax && p.v[a].ay == p.v[b].ay &&
                         p.v[a].az == p.v[b].az;
        if (sprite) continue;
        for (int k = 0; k < p.nv; k++) {
            CapVert &v = p.v[k];
            float px, py;
            if (!v.derived || !proj(v, px, py) || std::abs(v.sx - p.off_x - px) > 1.5f ||
                std::abs(v.sy - p.off_y - py - shift) > 1.5f)
                continue;
            v.derived = false;
            v.has3d = true;
            v.x = v.ax; v.y = v.ay; v.z = v.az;
        }
    }
}

// ---------------------------------------------------------------- frozen texels
static void prim_uv_box(const CapPrim &p, int &umin, int &vmin, int &umax, int &vmax) {
    umin = vmin = 255;
    umax = vmax = 0;
    for (int i = 0; i < p.nv; i++) {
        umin = std::min<int>(umin, p.v[i].u); umax = std::max<int>(umax, p.v[i].u);
        vmin = std::min<int>(vmin, p.v[i].v); vmax = std::max<int>(vmax, p.v[i].v);
    }
    if (p.kind == CapPrim::RECT) {  // rect corners are one past the last texel
        umax = std::max(umin, umax - 1);
        vmax = std::max(vmin, vmax - 1);
    }
}

static bool rects_meet(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

// Does the primitive read VRAM inside (x, y, w, h)? (texture page area and CLUT row)
static bool prim_reads(const CapPrim &p, int x, int y, int w, int h) {
    int umin, vmin, umax, vmax;
    prim_uv_box(p, umin, vmin, umax, vmax);
    if (p.tw) umin = vmin = 0, umax = vmax = 255;  // texture window: be conservative
    int tx = (p.tpage & 0xf) * 64, ty = ((p.tpage >> 4) & 1) * 256, depth = (p.tpage >> 7) & 3;
    int div = depth == 0 ? 4 : depth == 1 ? 2 : 1;
    if (rects_meet(x, y, w, h, tx + umin / div, ty + vmin, umax / div - umin / div + 1, vmax - vmin + 1)) return true;
    if (depth < 2) {
        int cx = (p.clut & 0x3f) * 16, cy = (p.clut >> 6) & 0x1ff;
        if (rects_meet(x, y, w, h, cx, cy, depth == 0 ? 16 : 256, 1)) return true;
    }
    return false;
}

static u16 texel_at(const u16 *vram, const CapPrim &p, int u, int v) {
    u32 tw = p.tw;
    int mx = (tw & 31) * 8, my = ((tw >> 5) & 31) * 8, ox = ((tw >> 10) & 31) * 8, oy = ((tw >> 15) & 31) * 8;
    u = (u & ~mx) | (ox & mx);
    v = (v & ~my) | (oy & my);
    int tx = (p.tpage & 0xf) * 64, ty = ((p.tpage >> 4) & 1) * 256, depth = (p.tpage >> 7) & 3;
    int cx = (p.clut & 0x3f) * 16, cy = (p.clut >> 6) & 0x1ff;
    if (depth == 0) {
        u16 w = vram[((ty + v) & 511) * 1024 + ((tx + u / 4) & 1023)];
        return vram[cy * 1024 + ((cx + ((w >> ((u & 3) * 4)) & 15)) & 1023)];
    }
    if (depth == 1) {
        u16 w = vram[((ty + v) & 511) * 1024 + ((tx + u / 2) & 1023)];
        return vram[cy * 1024 + ((cx + ((w >> ((u & 1) * 8)) & 255)) & 1023)];
    }
    return vram[((ty + v) & 511) * 1024 + ((tx + u) & 1023)];
}

static void bake(CapFrame &f, CapPrim &p) {
    int umin, vmin, umax, vmax;
    prim_uv_box(p, umin, vmin, umax, vmax);
    int w = umax - umin + 1, h = vmax - vmin + 1;
    if (f.atlas_x + w > ATLAS_W) {  // next shelf
        f.atlas_x = ATLAS_X0;
        f.atlas_y += f.atlas_row_h;
        f.atlas_row_h = 0;
    }
    if (f.atlas_y + h > ATLAS_H) return;  // atlas full: leave it reading VRAM
    if (f.atlas.empty()) f.atlas.assign((size_t)ATLAS_W * ATLAS_H, 0);
    const u16 *vram = gpu_vram();
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            f.atlas[(size_t)(f.atlas_y + y) * ATLAS_W + f.atlas_x + x] = texel_at(vram, p, umin + x, vmin + y);
    p.baked = true;
    p.bake_x = (u16)f.atlas_x;
    p.bake_y = (u16)f.atlas_y;
    f.atlas_x += w;
    f.atlas_row_h = std::max(f.atlas_row_h, h);
}

void capture_vram_write(int x, int y, int w, int h) {
    if (!g_capture) return;
    for (CapPrim &p : g_cap_building.prims)
        if (p.textured && !p.baked && prim_reads(p, x, y, w, h)) bake(g_cap_building, p);
}

void capture_flip(int dx, int dy, int w, int h) {
    if (!g_capture) return;
    if (g_cap_building.prims.empty()) return;  // game re-set the same buffer without drawing
    promote_shifted_vertices(g_cap_building, M.gte.ctrl[24] / 65536.0f, M.gte.ctrl[25] / 65536.0f,
                             (float)(g_gte_h_override ? g_gte_h_override : (u16)M.gte.ctrl[26]));
    g_cap_building.disp_x = dx;
    g_cap_building.disp_y = dy;
    g_cap_building.disp_w = w;
    g_cap_building.disp_h = h;
    g_cap_building.gte_ofx = M.gte.ctrl[24];
    g_cap_building.gte_ofy = M.gte.ctrl[25];
    g_cap_building.gte_h = g_gte_h_override ? g_gte_h_override : (u16)M.gte.ctrl[26];
    g_cap_building.gte_h_game = g_gte_h_game ? g_gte_h_game : g_cap_building.gte_h;
    g_cap_building.number = ++g_cap_frames;
    g_vframe = (u32)g_cap_frames;
    std::swap(g_cap_done, g_cap_building);
    g_cap_node = 0;
    g_cap_building.prims.clear();
    g_cap_building.game_cam_valid = false;
    g_cap_building.y_shift_known = false;
    g_cap_building.atlas.clear();
    g_cap_building.atlas_x = ATLAS_X0;
    g_cap_building.atlas_y = ATLAS_Y0;
    g_cap_building.atlas_row_h = 0;
}

}  // namespace psx
