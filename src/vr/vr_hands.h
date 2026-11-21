// VR hands: the hands follow the controllers (fingers close with grip and trigger), one may hold the pistol, and a
// faint ray from the aiming hand shows where it points.
#pragma once
#include <d3d11.h>

#include "xmath.h"

namespace vrhands {

struct HandState {
    bool valid = false;
    Mat4 grip;            // controller grip pose (LOCAL)
    Mat4 aim;             // controller aim pose (LOCAL): the spell ray runs along its -Z
    float trigger = 0;    // 0..1 index finger
    float squeeze = 0;    // 0..1 other fingers
    float open = 0;       // 0..1 hand flung open (a spell just left it)
    bool point = false;   // index straight out (touching the spell panel)
    bool pistol = false;  // the hand holds the pistol (fingers round its grip, index on the trigger)
    float kick = 0;       // 0..1 recoil of the pistol (a shot just left it)
    float ray_len = 4;    // metres of aim ray drawn; with dot, a bright dot where it ends (what it hits)
    // A long gun held with both hands: the weapon turns about the fist so that its support point (support_anchor, in
    // the weapon's frame: metres, -Z barrel, +Y up, origin = fist) lies towards the other hand (support_pos, LOCAL)
    bool support = false;
    float support_pos[3] = {0, 0, 0};
    float support_anchor[3] = {0, 0, 0};
    bool ray_dot = false;
    int arc_n = 0;        // >= 2: the aim ray is this curve instead (LOCAL points; the grenade gun's arc)
    float arc[64][3] = {};
};
// The pistol held by a hand: barrel along the controller's aim -Z, grip through the fist. Muzzle pose (LOCAL, -Z out).
Mat4 pistol_muzzle(const HandState &h);

// A textured pistol model (a game's own weapon) used instead of the built-in one: nverts vertices of triangles, each
// x y z, nx ny nz, u v (metres, in the pistol frame: -Z barrel, +Y up, origin at the fist), texture tw x th RGBA (alpha 0
// = transparent). nullptr = the built-in pistol. The pointer must stay valid.
struct PistolModel {
    int tw, th;
    const unsigned char *rgba;
    int nverts;
    const float *mesh;
    float muzzle_y = 0.066f, muzzle_z = -0.152f;  // where the barrel ends (the aim ray starts there)
};
void set_pistol_model(const PistolModel *m);

bool init(ID3D11Device *dev, ID3D11DeviceContext *ctx);
// Draw both hands into the bound target (depth tested); ray_hand = hand showing the aim ray (-1 = none).
void draw(const Mat4 &view_proj, const HandState hands[2], int ray_hand, bool srgb);

}  // namespace vrhands
