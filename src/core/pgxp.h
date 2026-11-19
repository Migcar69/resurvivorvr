// Precise vertex tracking (PGXP-style): every screen coordinate the GTE produces gets a vertex id
// that points to its camera-space position. The id follows the value through CPU registers and
// guest memory, so when a primitive reaches the GPU we can recover the 3D position of its vertices.
#pragma once
#include "psx.h"

namespace psx {

struct Vert3 {
    float x, y, z;  // camera space (PS1 convention: +x right, +y down, +z forward), game units
    u32 kind;       // 1 = world view projection, 2 = other projection centre (HUD 3D models)
    u32 pc;         // address of the RTPS/RTPT instruction that produced it
    u32 frame;      // capture frame number when it was projected
    s16 gsx, gsy;   // screen position with the game's own H (differs from the drawn one when H is overridden)
};

extern bool g_pgxp;  // enabled in VR / 3D preview mode

constexpr u32 VRING = 1u << 20;
extern Vert3 g_vring[VRING];
extern u32 g_vcount;  // total vertices emitted (vid = index + 1)
extern u32 g_vframe;  // current capture frame number (stamped on new vertices)

// A vertex id with this bit set was derived from a projected vertex by partial (16/8-bit) rewrites,
// e.g. sprite corners computed from an object's projected position.
constexpr u32 VID_DERIVED = 0x80000000u;

inline bool vid_valid(u32 vid) {
    vid &= ~VID_DERIVED;
    return vid && vid <= g_vcount && g_vcount - vid < VRING - 1;
}
inline const Vert3 &vid_get(u32 vid) { return g_vring[((vid & ~VID_DERIVED) - 1) & (VRING - 1)]; }
inline u32 vid_new(float x, float y, float z, u32 kind, s16 gsx = 0, s16 gsy = 0) {
    g_vring[g_vcount & (VRING - 1)] = {x, y, z, kind, M.cpu.pc, g_vframe, gsx, gsy};
    return ++g_vcount;
}

// Shadow of RAM/scratchpad words: value that was stored + its vertex id
struct Shadow {
    u32 val, vid;
};
extern Shadow g_shadow_ram[RAM_SIZE / 4];
extern Shadow g_shadow_spad[256];
extern Shadow g_reg_tag[32];
extern u32 g_sxy_vid[3];
extern u32 g_gte_in_vid;  // tag of the value being written to a GTE data register

inline Shadow *shadow_at(u32 addr) {
    u32 p = addr & 0x1fffffff;
    if (p < 0x00800000) return &g_shadow_ram[(p & (RAM_SIZE - 1)) >> 2];
    if ((p & 0xfffffc00) == 0x1f800000) return &g_shadow_spad[(p & 0x3ff) >> 2];
    return nullptr;
}

inline u32 shadow_vid(u32 addr, u32 cur_val) {
    Shadow *s = shadow_at(addr);
    return (s && s->val == cur_val) ? s->vid : 0;
}

inline void pgxp_store(u32 addr, u32 val, u32 vid) {
    if (Shadow *s = shadow_at(addr)) { s->val = val; s->vid = vid; }
}

// Shadow of RAM/scratchpad halfwords: games that copy screen coordinates 16 bits at a time
// (x and y separately) rebuild a tagged word once both halves carry the same vertex.
struct HalfShadow {
    u16 val;
    u32 vid;
};
extern HalfShadow g_shadow_half_ram[RAM_SIZE / 2];
extern HalfShadow g_shadow_half_spad[512];
inline HalfShadow *half_shadow_at(u32 addr) {
    u32 p = addr & 0x1fffffff;
    if (p < 0x00800000) return &g_shadow_half_ram[(p & (RAM_SIZE - 1)) >> 1];
    if ((p & 0xfffffc00) == 0x1f800000) return &g_shadow_half_spad[(p & 0x3ff) >> 1];
    return nullptr;
}

inline u32 reg_vid(int r, u32 val) { return (g_reg_tag[r].val == val) ? g_reg_tag[r].vid : 0; }

}  // namespace psx
