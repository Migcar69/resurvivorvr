// Per-frame capture of the GPU primitives the game draws, annotated with recovered 3D positions.
// The VR renderer redraws the scene from these instead of using the 2D framebuffer.
#pragma once
#include "pgxp.h"

namespace psx {

struct CapVert {
    s16 sx, sy;     // screen position (drawing offset applied, like the rasterizer)
    u8 r, g, b;
    u8 u, v;
    bool has3d;
    bool hud3d;     // tracked, but projected around a non-world centre (HUD model such as the compass)
    bool hud_reproj; // HUD model vertex with a valid camera-space position (x,y,z): redraw with the game's H
    bool derived;   // position computed by the game from a projected point (sprite corner); ax/ay/az = that point
    float ax, ay, az;
    float x, y, z;  // camera space (PS1 convention), valid if has3d
    u32 src_pc;     // GTE instruction that projected it (identifies the game routine)
    s16 gsx, gsy;   // screen position the game's own H gives (valid with hud_reproj)
    u32 addr;       // RAM address of the vertex word (debug)
};

struct CapPrim {
    enum Kind : u8 { TRI, QUAD, LINE, RECT } kind;
    u8 nv;
    bool textured, gouraud, semi, raw;
    u8 blend;        // semi-transparency mode 0..3
    u16 tpage, clut; // tpage: E1-style bits (x base, y base, blend, depth)
    u32 node;        // index of the ordering-table entry (in DMA traversal order) that linked this primitive
    s16 off_x, off_y;
    s16 clip_x1, clip_y1, clip_x2, clip_y2;
    u32 tw;          // texture window (GP0 E2 bits 0..19)
    bool hidden;     // not shown in VR (replaced by VR content)
    // Texels frozen before the game overwrote them in VRAM later in the same frame (e.g. a font that uploads each
    // glyph to the same place right before drawing it): 15-bit colours in the frame's atlas, at (bake_x, bake_y)
    // for texel (umin, vmin) of the primitive's UV bounding box.
    bool baked = false;
    u16 bake_x = 0, bake_y = 0;
    CapVert v[4];
};

// Atlas for frozen texels: 1024x512 (only x 256.. / y 256.. used, so the base minus the UV origin stays >= 0)
constexpr int ATLAS_W = 1024, ATLAS_H = 512, ATLAS_X0 = 256, ATLAS_Y0 = 256;

struct CapFrame {
    std::vector<CapPrim> prims;
    s32 gte_ofx = 0, gte_ofy = 0, gte_h = 1;  // projection used by the game (GTE OFX/OFY in 16.16, H)
    s32 gte_h_game = 1;  // H the game asked for (sprite sizes are computed with it even when H is overridden)
    int disp_x = 0, disp_y = 0, disp_w = 320, disp_h = 240;
    u64 number = 0;
    int y_shift = 0;  // constant y offset the game added after projecting world points (look up/down shear)
    bool y_shift_known = false;  // y_shift given by the game module (else estimated from the vertices)
    s32 cam_yaw = 0, cam_pitch = 0;  // camera offset injected while this frame was built (16-bit angles)
    // game-specific data recorded while this frame was built (e.g. camera and map for a far-terrain layer)
    s32 game_cam[8] = {};
    bool game_cam_valid = false;
    std::vector<u8> game_data;
    std::vector<u16> atlas;          // frozen texels (ATLAS_W x ATLAS_H), empty if nothing was frozen
    int atlas_x = ATLAS_X0, atlas_y = ATLAS_Y0, atlas_row_h = 0;
};

extern bool g_capture;          // enable capture (VR / preview modes)
extern CapFrame g_cap_building;  // being filled
extern CapFrame g_cap_done;      // last complete frame
extern u64 g_cap_frames;
extern s32 g_cam_inject[2];
// Game-specific camera / data recorded when the game starts building a frame. The game builds the next frame
// before the previous one is shown, so it is not written into g_cap_building directly: the capture takes it when
// the frame's first primitive is drawn (the drawing of a frame follows its own building).
extern s32 g_game_cam_pending[8];
extern bool g_game_cam_pending_valid;
extern std::vector<u8> g_game_data_pending;
// The look up/down shift the game applies to the frame being built, when the game module knows it exactly
// (the estimate from the vertices can be a pixel off from frame to frame: far sprites bob up and down).
extern int g_game_yshift_pending;
extern bool g_game_yshift_pending_valid;
extern u32 g_hud_rtps_pc[8];  // game routines whose 3D models belong to the HUD (e.g. compass)
extern u16 g_hud_tpages[4];   // texture pages used only by the HUD (models drawn with them stay 2D)
extern u32 g_hide_rtps_pc[4]; // projections from these routines are not shown in VR
extern u16 g_hide_model_clut; // models from g_model_rtps_pc with this CLUT are not shown in VR
extern u32 g_model_rtps_pc;   // the game's generic 3D model renderer  // latest yaw/pitch offset applied to the game camera (set by the VR hook)

// Called by the GPU for each executed command; addrs = RAM address of each command word (0 if unknown)
struct GpuDrawState {
    u32 texpage;
    int off_x, off_y, clip_x1, clip_y1, clip_x2, clip_y2;
    bool tex_disable;
    u32 tw;
};
void capture_cmd(const u32 *cmd, const u32 *addrs, int len, const GpuDrawState &st);
void capture_flip(int disp_x, int disp_y, int w, int h);
// The GPU is about to overwrite this VRAM rectangle: freeze the texels of primitives of this frame that read it.
void capture_vram_write(int x, int y, int w, int h);
// polyline segment (screen coordinates incl. drawing offset), recorded as a LINE primitive
void capture_line(int x0, int y0, int x1, int y1, u32 color, int off_x, int off_y);  // display start changed: frame complete
extern u32 g_cap_node;  // ordering-table entries seen so far this frame (DMA walker)

}  // namespace psx
