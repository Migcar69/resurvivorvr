// 3D scene renderer: redraws the game's captured primitives in true 3D with D3D11,
// sampling textures straight from emulated PS1 VRAM.
#pragma once
#include <d3d11.h>

#include <array>
#include <vector>

#include "../core/capture.h"
#include "xmath.h"

namespace scene {

// World units per metre: CALIBRATE for each game on something of known size (Magic Carpet: 146, villager = 256)
constexpr float UNITS_PER_METER = 1000.0f;  // Resident Evil Survivor: 1 unit = 1 mm (eye height 1600, verified on characters)

struct Stats {
    int world_prims = 0, sprite_prims = 0, hud_prims = 0, dropped = 0;
    int anchored = 0, fitted = 0;  // sprites placed at their exact ground point vs. estimated depth
    int hud_objects = 0;           // object-page sprites sent to the 2D overlay
    int decals = 0;                // polygons drawn over a coplanar earlier one (pulled forward in depth)
};

bool init(ID3D11Device *dev, ID3D11DeviceContext *ctx);
// Build GPU buffers for a captured frame and upload VRAM. Returns true if the frame is a 3D scene.
bool update(const psx::CapFrame &frame, const uint16_t *vram);
const Stats &stats();
// Ground points (scene space, meters) of the object sprites placed in the last update (for testing)
const std::vector<std::array<float, 4>> &sprite_anchors();

// Draw the world (scene space = game camera space in meters, VR axes: +x right, +y up, -z forward)
// into the bound render target + depth buffer.
void draw_world(const Mat4 &view_proj, bool srgb_target);
// Render the 2D overlay (HUD) into an internal texture; returns its SRV (premultiplied-free RGBA, alpha=coverage)
ID3D11ShaderResourceView *render_hud(bool srgb_target);
// Flat colour the game filled the screen with behind the world in the last update (false if none)
bool backdrop_color(float rgb[3]);
int hud_width();
// Testing: save the 2D overlay texture to a BMP.
bool hud_bmp(const char *path);
// Nearest world triangle hit by the ray o + t*d (scene space: x right, y up, z backwards, metres, game eye at 0).
bool raycast(const float o[3], const float d[3], float &t);
// Screen position the game's own projection gives that scene-space point (false if behind the eye).
bool project_to_screen(const float p[3], float &sx, float &sy);
// Leave the game's red cross out of the 2D overlay (while the VR laser drives it).
void set_hide_cursor(bool hide);
int hud_height();
// How sprites built around a projected point (derived corners) are rebuilt in 3D:
//  UprightOnGround - Ultima Underworld rules (bottom-centre ground point, upright / floor decals)
//  DepthPlane      - a plane facing the game camera at the anchor's depth: from the game's viewpoint it covers
//                    exactly the original sprite (keeps the view roll), in VR it sits at the object's distance
enum class SpriteMode { UprightOnGround, DepthPlane };
void set_sprite_mode(SpriteMode m);
// Game routines (GTE instruction addresses) whose projected points are the sky backdrop.
void set_sky_pcs(const uint32_t *pcs, int n);
// Game-specific additions to the reconstructed world, called by update() for 3D frames: set hide[i] to drop captured
// primitive i, append primitives of its own (positions in the game's camera space, PS1 units) to extra.
struct ExtraPrim {
    psx::CapPrim p;
    float pos[4][3];
};
using WorldExtraFn = void (*)(const psx::CapFrame &frame, std::vector<uint8_t> &hide, std::vector<ExtraPrim> &extra);
void set_world_extra(WorldExtraFn fn);
// Where the game stands people and things in the frame being built: metres under the camera (<= 0: unknown; call from
// the extra hook).
void set_actor_ground(float metres_below_eye);
// Game-specific corrections of the world's shape: called with each 3D primitive's corners (scene space, metres) before
// it is placed; may move them.
using PointWarpFn = void (*)(const psx::CapFrame &frame, const psx::CapPrim &prim, float pos[4][3]);
void set_point_warp(PointWarpFn fn);
// The game's routines that transform characters (CapVert::src_pc): drawn over the floor where they go into it.
void set_model_pcs(const uint32_t *pcs, int n);
// Distance haze over the world, in meters from the game camera (end <= 0: off); colour = the game's backdrop.
void set_fog(float start_m, float end_m);

// Desktop / test helper: render the current frame from a free camera into a BMP file.
bool preview_bmp(const char *path, int w, int h, const Mat4 &camera_pose, float fov_deg,
                 const Mat4 &model = mat_identity(), void (*extra)(const Mat4 &view_proj) = nullptr);

}  // namespace scene
