// Resident Evil Survivor (PAL, SLES-02732): game-specific knowledge used by the 3D capture / VR layer.
// Addresses are those of KERNEL.BIN (loaded at 0x80010000 by SLES_027.32) and of the stage overlay in RAM.
#include "game.h"

#include "../core/psx.h"
#include "../core/capture.h"
#include "../core/pgxp.h"
#include "../vr/scene.h"
#include "../vr/vr_hands.h"
// The weapon models made from the game itself (its own models and pictures) are not part of the program: they are
// rebuilt from the player's game while it runs (weapon_harvest.cpp). RESVR_EMBED_WEAPONS (development builds only, the
// headers are made from the developer's own saves and are not published) puts them in the program as a fallback.
#ifdef RESVR_EMBED_WEAPONS
#include "handgun_a_model.h"
#include "handgun_c_model.h"
#include "shotgun_model.h"
#include "magnum_model.h"
#include "custom_magnum_model.h"
#include "handgun_d_model.h"
#include "handgun_b_model.h"
#include "grenade_model.h"
#endif
#include "rocket_model.h"
#include "glock_model.h"

#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <mutex>
#include <cstring>

using namespace psx;

extern std::string g_boot_name;  // main_win32.cpp: the disc's program

namespace game {

bool g_view_log = false;
static unsigned long long g_render_count = 0;

// ---------------------------------------------------------------- controls
// The game reads the pad once per logic frame (0x80071560): 0x80013a4c(port 0) returns the raw buttons, 0x8001188c
// maps them through BUTTON CONFIG to the game's actions (low half = held, high half = pressed this frame) and the
// result is stored at 0x800b4d44 / 0x800b4d48 (0x800b3e74 = previous). With a GunCon the raw gun bits pass through
// (trigger 0x20, A 0x0800, B 0x40) and the gun logic reads them (A/B turn, trigger off screen = walk).
static u32 PC_PAD_MAPPED = 0x800715c0;  // 0x8001188c just returned, v0 = held | pressed << 16
static u16 inject_bits = 0, inject_prev = 0;
static u16 inject_clear = 0;

// The player: coordinate system 0x800a8770 (rotation, then position x / y / z as s32 at +0x18 / +0x1c / +0x20, feet),
// heading 0..4095 at +0x46 (0x800a87b6; a copy at 0x800b3f4c), forward = (-sin h, 0, -cos h). Writing the heading
// before the logic frame turns the player at once (the original turns ~1.9 s per full turn with the D-pad / A-B).
static u32 PLAYER = 0x800a8770, PLAYER_HEADING = 0x800a87b6, PLAYER_HEADING2 = 0x800b3f4c;
static bool body_on = false;
static float body_yaw = 0;
static bool in_control_now = false;
static bool scripted_pov_now = false;
// 1 while the HUD is up and the player has control; 0 in cut-scenes and in the opening (also when the camera is the
// player's eyes there: waking up, blurred, the game turns the view by itself)
static u32 HUD_ON = 0x80062ddc;
static u32 STAGE = 0x80062ef4, ROOM = 0x80062e38;  // the stage (1, 2, 3...) and the room in it
// walking is skipped by the game when WALK_BLOCK >= 4, or WALK_FLAG_A == 0 with WALK_FLAG_B != 0
static u32 WALK_BLOCK = 0x80062e1e, WALK_FLAG_A = 0x80062e30, WALK_FLAG_B = 0x80062e1c;
static u32 WEAPON = 0x800637c6;  // the equipped weapon (see equipped_weapon)
// The eyes camera with the HUD still off is also drawn for a moment when the player gets control back (closing a file
// that was read: ~7 frames looking down, then the HUD): the game's camera there and the head's right after made the
// view jump. It is the game's alone only in the opening (no control since the last movie); otherwise it is the head's.
static std::atomic<bool> control_since_movie{true};
// a movie (FMV) is decoded: the library routine giving the MDEC its command (it reads the register's address from
// 0x80061bcc) runs. The movies are uploaded straight to the screen, so the captured picture cannot tell them.
static u32 PC_MDEC_COMMAND = 0x80038878;
static int mdec_commands = 0;
static void on_mdec_command() {
    control_since_movie = false;
    if (getenv("PSXVR_CTLLOG") && mdec_commands++ % 100 == 0) logf("mdec command %d", mdec_commands);
}
static u64 last_view_cycles = 0;  // the view function runs only while the 3D view is drawn (not in scenes / menus)
// (20 syncs: a long frame - the game busy, or the wide view - must not take the control away for a moment)
static bool control_fresh() { return in_control_now && M.cycles - last_view_cycles < (u64)CYCLES_PER_FRAME * 20; }
static bool control_held = false;  // the player's view, taken once and kept while it stays near the player (on_view_done)

// The game places the player itself when a room is entered (facing away from the door) or a scene ends: then the
// heading differs from the one VR last wrote, or the position jumps. VR must not overwrite that heading: the writes
// stop until the frontend has turned its world so that the player looks that way (take_placement()).
static int last_written = -1;
static s32 last_pos[2] = {0, 0};
static bool have_pos = false, placement = false, back_check = false;
static int game_turning = 0;  // logic frames left in which the game turns the player by itself

struct Headless;
static void headless_check();
static void headless_clear();
static void on_pad_mapped() {
    headless_check();
    s32 px = (s32)read32(PLAYER + 0x18), pz = (s32)read32(PLAYER + 0x20);
    int cur = read16(PLAYER_HEADING) & 0xfff;
    if (have_pos) {
        float dx = (float)(px - last_pos[0]), dz = (float)(pz - last_pos[1]);
        if (dx * dx + dz * dz > 1500.0f * 1500.0f) placement = true;  // moved by the game (a room change)
    }
    last_pos[0] = px, last_pos[1] = pz, have_pos = true;
    bool control = control_fresh();
    if (!control) {
        last_written = -1;  // a scene / menu: the heading the game gives is taken when control comes back
        back_check = true;
    } else if (back_check) {
        back_check = false;
        placement = true;   // control is back after a scene or a room change: align with the game's heading
    } else if (last_written >= 0) {
        int d = std::abs(cur - last_written);
        d = std::min(d, 4096 - d);
        if (d > 100) placement = true;  // turned by the game (~9 degrees or more)
        // Turned a little by the game: a zombie grabbing the player turns him towards it a few degrees a frame, and
        // its attack goes on once he faces it. Written over every frame, the attack never ended: no damage, and
        // the trigger did nothing (the player held in it). The game turns him as it likes for 20 logic frames
        // (~2/3 s) after its last change; then the body follows the gaze again.
        else if (d > 4) game_turning = 20;
    }
    if (placement) last_written = -1;
    if (game_turning > 0) {
        game_turning--;
        last_written = cur;  // (what the game set: its next changes are measured from it)
    } else if (body_on && control && !placement) {
        u16 h = (u16)((s32)lroundf(body_yaw * 4096.0f / 6.2831853f) & 0xfff);
        write16(PLAYER_HEADING, h);
        write16(PLAYER_HEADING2, h);
        last_written = h;
    }
    u32 v = M.cpu.r[2];
    u16 held = (u16)v, pressed = (u16)(v >> 16);
    held &= ~inject_clear;
    pressed &= ~inject_clear;
    held |= inject_bits;
    pressed |= (u16)(inject_bits & ~inject_prev);
    inject_prev = inject_bits;
    M.cpu.r[2] = held | ((u32)pressed << 16);
}

// Walking. 0x800b6230 holds the player's movement requests for the logic frame (bit 0 = walk forward); with a GunCon
// 0x80077708..0x8007776c sets it only when the gun is off the picture and the trigger is held (and the HUD is up and
// 0x80062e1e < 4), so the original cannot walk and shoot at once. VR walks with the stick: right after that code
// (0x80077774, reached by every path) the same bit is set under the same conditions, the trigger stays free to shoot.
static u32 PC_MOVE_REQUESTS = 0x80077774, MOVE_REQUESTS = 0x800b6230;
// Running: the pad code puts the step's speed at 0x800b3ef8 (-80 walk, -150 run: Cross held or the analog stick pushed
// fully, 60 backwards); below -139 the walking code (0x800786f0) takes the running animation / stride table, and when
// the speed crosses that line it restarts the table (0x800b4f94 = 0; 0x800b4fac = last speed). The GunCon code never
// sets it (it stays -80). VR runs by giving the pad's running speed, and puts -80 back when it stops.
static u32 MOVE_SPEED = 0x800b3ef8, MOVE_SPEED_PREV = 0x800b4fac, MOVE_TABLE = 0x800b4f94;
static bool walk_req = false, run_req = false, back_req = false, speed_set = false;
static void set_speed(s32 v) {
    // as the pad code does (0x80077528): compared with the last speed it gave (0x800b4fac), not with 0x800b3ef8, which
    // the game puts back to -80 now and then (0x8007a1f4); restarting the table each time cut the step cycle short and
    // the running footsteps (played at the end of each cycle, 0x800787c4) were never heard
    s32 prev = (s16)read16(MOVE_SPEED_PREV);
    // the step table (bob / stride) is chosen again when the kind of step changes (walk / run / backwards)
    if ((prev < -139) != (v < -139) || (prev > 0) != (v > 0)) write32(MOVE_TABLE, 0);
    write32(MOVE_SPEED, (u32)v);
    write16(MOVE_SPEED_PREV, (u16)(s16)v);
}
static void on_move_requests() {
    s32 want = !walk_req ? -80 : back_req ? 60 : run_req ? -150 : -80;
    if (want == -80) {
        if (speed_set) set_speed(-80), speed_set = false;
    }
    if (!walk_req || read8(HUD_ON) == 0 || (s16)read16(WALK_BLOCK) >= 4) return;
    if (read8(WALK_FLAG_A) == 0 && read8(WALK_FLAG_B) != 0) return;  // the game skips walking then
    if (want != -80) set_speed(want), speed_set = true;
    // walk (bit 0) or step back (bit 1, the pad's Down: speed +60, its own animation) + "moving" (the walking code
    // checks bit 6 first)
    write32(MOVE_REQUESTS, (read32(MOVE_REQUESTS) & ~3u) | (back_req ? 0x42 : 0x41));
}
// Direction of the step: the walking code builds the step's vector from the player's rotation (0x8006f784 called at
// 0x80078784 with a0 = 0x800a87b4, the rotation whose y is the heading). VR keeps the player facing where the head looks
// (shots, doors and everything else see the player facing that way) and walks in the stick's direction: only for that
// call the heading is the walking direction (set at 0x80078778, put back at 0x8007878c).
static u32 PC_STEP_VECTOR = 0x80078778, PC_STEP_VECTOR_DONE = 0x8007878c;
static bool move_dir_on = false, step_swapped = false;
static u16 move_heading = 0, step_saved = 0;
static void on_step_vector() {
    step_swapped = false;
    if (!walk_req || !move_dir_on) return;
    step_saved = read16(PLAYER_HEADING);
    write16(PLAYER_HEADING, (u16)((step_saved & ~0xfff) | move_heading));
    step_swapped = true;
}
static void on_step_vector_done() {
    if (step_swapped) write16(PLAYER_HEADING, step_saved), step_swapped = false;
}
// ---------------------------------------------------------------- weapon in the hand
// 0x800637c6 = the equipped weapon as far as its sound goes (1 Handgun A, 3 Handgun C; changed by the item screen's
// Equip, back and forth; 0x800a8a70, tried first, only follows the last weapon picked up). Handgun C has a polygon
// model in the game (on the shelf, and turning on the "You've obtained" screen): VR holds that one. Handgun A only has a
// picture (its Check screen): VR builds it from that picture with the measures of a Glock 17, which it is
// (handgun_a_model.h). Other weapons: the built-in pistol.
int equipped_weapon() { return read8(WEAPON); }
// A save was loaded while running (VR options): what was kept of the game before is stale. A heading swapped for a step
// must not be put back into the loaded RAM; the player's heading is taken from the game again (a placement).
void on_state_loaded() {
    step_swapped = false;
    speed_set = false;
    have_pos = false, back_check = false;
    placement = true;
    last_written = -1;
    game_turning = 0;
    last_view_cycles = 0;
    in_control_now = false;
    control_held = false;
    control_since_movie = true;  // as with a save loaded at start
    inject_prev = 0;
    headless_clear();  // (the loaded RAM has the heads it had)
}
int player_pos(int axis) { return (int)(s32)read32(PLAYER + 0x18 + 4 * axis); }  // feet x / y / z
int room_key() { return read8(STAGE) << 8 | read8(ROOM); }  // stage, room

// Pictures over the whole screen while the game still draws its 3D world under them (the memories coming back to the
// hero in the street, usa_snap_20261118_181359: pictures of an office, of a room, 320 x 192 between black bars): two
// flat sprites of the movie pages (0x098..0x09f) covering the width and most of the height. The 3D view under them is
// not what the player sees: the frontend shows the game's screen.
bool full_picture(const CapFrame &fr) {
    int width = 0, height = 0;
    for (const CapPrim &p : fr.prims) {
        if (!p.textured || p.nv != 4 || p.v[0].has3d) continue;
        int tp = p.tpage & 0x1ff;
        if (tp < 0x098 || tp > 0x09f) continue;  // (the pages used depend on the buffer drawn: 0x09a / 0x09c, 0x09d / 0x09f)
        int x0 = 1 << 30, x1 = -(1 << 30), y0 = 1 << 30, y1 = -(1 << 30);
        for (int k = 0; k < 4; k++)
            x0 = std::min<int>(x0, p.v[k].sx), x1 = std::max<int>(x1, p.v[k].sx), y0 = std::min<int>(y0, p.v[k].sy),
            y1 = std::max<int>(y1, p.v[k].sy);
        width += x1 - x0;
        height = std::max(height, y1 - y0);
    }
    return width >= fr.disp_w - 8 && height * 10 >= fr.disp_h * 6;
}

// Movies (the opening...) are decoded into VRAM and drawn as two screen-high strips (pages 0x09b / 0x09d, 256 + 64 pixels
// wide); the tips shown before the title (still pictures with text) and the ADJUST GUN background are drawn the same way.
bool movie_frame(const CapFrame &fr, const uint16_t *vram) {
    static uint32_t last_hash = 0;
    static int since_change = 1000;
    uint32_t h = 2166136261u;
    bool any = false;
    for (const CapPrim &p : fr.prims) {
        if (!p.textured || p.nv != 4) continue;
        int tp = p.tpage & 0x1ff;
        if (tp != 0x09b && tp != 0x09d) continue;
        int y0 = 1 << 30, y1 = -(1 << 30);
        for (int k = 0; k < 4; k++) y0 = std::min<int>(y0, p.v[k].sy), y1 = std::max<int>(y1, p.v[k].sy);
        if (y1 - y0 < fr.disp_h - 8) continue;
        any = true;
        int tx = (tp & 0xf) * 64, ty = ((tp >> 4) & 1) * 256;  // 15-bit picture: one VRAM word per texel
        for (int v = 4; v < 240; v += 12)
            for (int u = 2; u < 64; u += 6) h = (h ^ vram[(ty + v) * 1024 + ((tx + u) & 1023)]) * 16777619u;
    }
    if (!any) { since_change = 1000; return false; }
    if (h != last_hash) since_change = 0;
    else if (since_change < 1000) since_change++;
    last_hash = h;
    (void)since_change;  // (whether it moves: kept for diagnosis)
    // a full-screen picture is never a menu the player points at (movies, the tips shown before the title, the ADJUST
    // GUN background, which VR answers by itself): no hands there
    return true;
}
const void *weapon_model(int weapon) {
    // (development) PSXVR_WEAPON_MODEL=a=b: weapon a is held with weapon b's model (to look at a model in another save)
    if (const char *wm = getenv("PSXVR_WEAPON_MODEL")) {
        int from = -1, to = -1;
        if (sscanf(wm, "%d=%d", &from, &to) == 2 && weapon == from && from != to) weapon = to;
    }
    // Rocket Launcher (8, the bonus weapon): no model in the game - an M20A1-like bazooka of solid shapes (VR's own,
    // tools/models/genbazooka.py)
    static const vrhands::PistolModel rocket = {ROCKET_TEX_W, ROCKET_TEX_H, ROCKET_TEX, ROCKET_VERTS, ROCKET_MESH,
                                                ROCKET_MUZZLE_Y, ROCKET_MUZZLE_Z};
    if (weapon == 8) return &rocket;
    // Handgun A (1): the game has no 3D model of it - a Glock 17 of solid shapes made for VR (tools/models/genglock.py)
    static const vrhands::PistolModel glock = {GLOCK_TEX_W, GLOCK_TEX_H, GLOCK_TEX, GLOCK_VERTS, GLOCK_MESH,
                                               GLOCK_MUZZLE_Y, GLOCK_MUZZLE_Z};
    // the game's own models, rebuilt from the player's game (weapon_harvest.cpp; Handgun A: VR's shape coloured with its
    // item picture, read from the disc at start)
    if (const void *h = harvested_model(weapon)) return h;
    if (weapon == 1) return &glock;  // (the disc's picture not found)
#ifdef RESVR_EMBED_WEAPONS
    static const vrhands::PistolModel handgun_c = {HANDGUN_C_TEX_W, HANDGUN_C_TEX_H, HANDGUN_C_TEX, HANDGUN_C_VERTS, HANDGUN_C_MESH};
    static const vrhands::PistolModel handgun_a = {HANDGUN_A_TEX_W, HANDGUN_A_TEX_H, HANDGUN_A_TEX, HANDGUN_A_VERTS, HANDGUN_A_MESH};
    static const vrhands::PistolModel shotgun = {SHOTGUN_TEX_W, SHOTGUN_TEX_H, SHOTGUN_TEX, SHOTGUN_VERTS, SHOTGUN_MESH,
                                                 SHOTGUN_MUZZLE_Y, SHOTGUN_MUZZLE_Z};
    static const vrhands::PistolModel magnum = {MAGNUM_TEX_W, MAGNUM_TEX_H, MAGNUM_TEX, MAGNUM_VERTS, MAGNUM_MESH,
                                                MAGNUM_MUZZLE_Y, MAGNUM_MUZZLE_Z};
    static const vrhands::PistolModel cmagnum = {CMAGNUM_TEX_W, CMAGNUM_TEX_H, CMAGNUM_TEX, CMAGNUM_VERTS, CMAGNUM_MESH,
                                                 CMAGNUM_MUZZLE_Y, CMAGNUM_MUZZLE_Z};
    static const vrhands::PistolModel handgun_d = {HANDGUN_D_TEX_W, HANDGUN_D_TEX_H, HANDGUN_D_TEX, HANDGUN_D_VERTS,
                                                   HANDGUN_D_MESH, HANDGUN_D_MUZZLE_Y, HANDGUN_D_MUZZLE_Z};
    static const vrhands::PistolModel handgun_b = {HANDGUN_B_TEX_W, HANDGUN_B_TEX_H, HANDGUN_B_TEX, HANDGUN_B_VERTS,
                                                   HANDGUN_B_MESH, HANDGUN_B_MUZZLE_Y, HANDGUN_B_MUZZLE_Z};
    static const vrhands::PistolModel grenade = {GRENADE_TEX_W, GRENADE_TEX_H, GRENADE_TEX, GRENADE_VERTS, GRENADE_MESH,
                                                 GRENADE_MUZZLE_Y, GRENADE_MUZZLE_Z};
    switch (weapon) {
    case 1: return &handgun_a;  // (not reached: the Glock above)
    case 2: return &handgun_b;
    case 3: return &handgun_c;
    case 4: return &handgun_d;
    case 5: return &shotgun;
    case 6: return &magnum;
    case 7: return &cmagnum;
    case 9: return &grenade;
    default: break;
    }
#endif
    return nullptr;  // the generic pistol
}

void set_walk(bool on, bool run, bool back) { walk_req = on, run_req = run && !back, back_req = back; }
void set_walk_direction(bool on, float yaw) {
    move_dir_on = on;
    move_heading = (u16)((s32)lroundf(yaw * 4096.0f / 6.2831853f) & 0xfff);
}

void set_actions(unsigned short add, unsigned short remove) {
    inject_bits = add;
    inject_clear = remove;
}

// ---------------------------------------------------------------- view
// 0x80033fc8 (GsSetRefView2-like) computes the world -> screen matrix from the camera coordinate chain (player
// 0x800a8770 -> eye 0x800a87c8, 1600 units above the feet) and copies it to GsWSMATRIX 0x80065b60 (and 0x80065ba0):
// short m[3][3] (4096 = 1), long t[3] at +20. It runs twice per drawn frame (0x800789d8 player update and 0x8008ce6c).
// The game's matrix is pitch (looking ~9 degrees down) x heading: rows = right (-cos h, 0, sin h), down, forward
// (-sin h, 0, -cos h), heading 0..4095 growing to the right. VR replaces it with the head's direction (absolute yaw
// in the game world and pitch, no roll) from the same eye, and stamps the frame with it.
static u32 PC_VIEW_DONE = 0x80034370;  // GsWSMATRIX just written, before the return
static u32 WSMATRIX[2] = {0x80065b60, 0x80065ba0};
static bool view_on = false, view_abs = false;
static float view_yaw = 0, view_pitch = 0;  // radians: yaw absolute (game heading convention) or relative; pitch up
static float game_yaw = 0, game_pitch = 0;  // the game's own camera of the last drawn frame
static float eye_pos[3] = {0, 0, 0};
static bool have_eye = false;

static void read_wsmatrix(float m[3][3], float t[3]) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) m[i][j] = (s16)read16(WSMATRIX[0] + 2 * (3 * i + j)) / 4096.0f;
    for (int i = 0; i < 3; i++) t[i] = (float)(s32)read32(WSMATRIX[0] + 20 + 4 * i);
}

static void stamp_frame(float yaw, float pitch);
// The eye of a world -> camera matrix: t = -R eye, so eye = -R^-1 t. The game's R is rounded to 4.12 and not quite a
// rotation: with its transpose instead of its inverse, far from the map's origin (stage 7, 180 m away:
// usa_snap_20261118_195106 / 195124) the eye came out up to 4 cm off, by an amount changing with the view's angle - the
// whole scene shook a little each time the game drew from a new angle (turning the head). Computed in double.
static void eye_from_view(const float m[3][3], const float t[3], float eye[3]) {
    double a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1],
           k = m[2][2];
    double det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
    if (std::fabs(det) < 1e-9) {
        for (int i = 0; i < 3; i++) eye[i] = -(m[0][i] * t[0] + m[1][i] * t[1] + m[2][i] * t[2]);
        return;
    }
    double inv[3][3] = {{(e * k - f * h) / det, (c * h - b * k) / det, (b * f - c * e) / det},
                        {(f * g - d * k) / det, (a * k - c * g) / det, (c * d - a * f) / det},
                        {(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det}};
    for (int i = 0; i < 3; i++) eye[i] = (float)-(inv[i][0] * t[0] + inv[i][1] * t[1] + inv[i][2] * t[2]);
}

static void on_view_done() {
    float m[3][3], t[3];
    read_wsmatrix(m, t);
    // the game's own heading / pitch: right row = (-cos h, 0, sin h), forward row z component = -cos h cos p
    game_yaw = atan2f(m[0][2], -m[0][0]);
    if (game_yaw < 0) game_yaw += 6.2831853f;
    game_pitch = asinf(std::max(-1.0f, std::min(1.0f, -m[2][1])));  // forward y < 0 = looking up
    eye_from_view(m, t, eye_pos);
    have_eye = true;
    // The player is in control when the camera is the player's eyes (1600 units over the feet, with the step's bob);
    // otherwise a scripted camera (cut-scenes, the opening) is drawing: VR leaves it alone.
    {
        float px = (float)(s32)read32(PLAYER + 0x18), py = (float)(s32)read32(PLAYER + 0x1c),
              pz = (float)(s32)read32(PLAYER + 0x20);
        float dx = eye_pos[0] - px, dz = eye_pos[2] - pz, dy = eye_pos[1] - py;
        // Taking control needs the eyes (within 1 m across: in some places the game puts them well ahead of the feet -
        // the rocky tunnel of stage 2 / room 4: 32 cm; stage 7 / room 0x2e: 57 cm). Keeping it only needs the HUD up and
        // the camera near the player (3 m, 0.6..2.6 m over the feet): when Mr X hits him, or the game shakes / pushes
        // its camera, a moment without control froze the player's walking right when he had to run (and hid the
        // hands). Scenes have the HUD off (or are drawn by the scene routines, on_cine_view_done) and end it.
        float across = dx * dx + dz * dz;
        bool eyes = across < 1000.0f * 1000.0f && dy < -1200 && dy > -2000;
        bool near_player = across < 3000.0f * 3000.0f && dy < -600 && dy > -2600;
        bool hud = read8(HUD_ON) != 0;
        bool back = eyes && !hud && control_since_movie;
        if (hud && eyes) control_held = true;
        if (!hud || !near_player) control_held = false;
        in_control_now = control_held || (eyes && back);
        scripted_pov_now = eyes && !hud && !back;
        if (eyes && hud) control_since_movie = true;
        last_view_cycles = M.cycles;
    }
    g_render_count++;
    if (getenv("PSXVR_VIEWCALLS")) logf("viewcall ra %08x frame %llu cap %llu prims %zu", M.cpu.r[31], (unsigned long long)(M.cycles / CYCLES_PER_FRAME), (unsigned long long)g_cap_frames, g_cap_building.prims.size());
    float yaw = game_yaw, pitch = game_pitch;
    // development: PSXVR_VIEW_SPIN=degrees turns the drawn view by that much at each call (checks that every captured
    // frame is placed with the view it was drawn with: --preview3d's "game" camera must look the same as standing still)
    static const char *spin = getenv("PSXVR_VIEW_SPIN");
    if (spin && view_on) view_yaw += (float)atof(spin) * 0.0174533f;
    // Scripted cameras (cut-scenes, the opening's waking up) are the game's alone: VR does not turn them (the head only
    // looks around the 3D picture they draw, placed in front of where the player looked when the scene began).
    if (view_on && (in_control_now || !view_abs)) {
        // While playing, the eye VR draws from is the player's head point (PLAYER + 0x90: the feet's x / z, the eye's
        // height), not the one recovered from the game's own matrix: the game rounds that matrix and its translation
        // differently, and ~200 m from the map's origin (stage 7) the recovered eye wandered by 2-3 cm as the body
        // followed the head (eyelog: the same head angle, the eye 3 cm up and down) - the world shook with it.
        if (in_control_now) {
            s32 hx = (s32)read32(PLAYER + 0x90), hy = (s32)read32(PLAYER + 0x94), hz = (s32)read32(PLAYER + 0x98);
            s32 fx = (s32)read32(PLAYER + 0x18), fy = (s32)read32(PLAYER + 0x1c), fz = (s32)read32(PLAYER + 0x20);
            if (std::abs(hx - fx) < 100 && std::abs(hz - fz) < 100 && hy - fy < -1200 && hy - fy > -2000)
                eye_pos[0] = (float)hx, eye_pos[1] = (float)hy, eye_pos[2] = (float)hz;
        }
        if (!view_abs) yaw = game_yaw + view_yaw, pitch = view_pitch;
        else yaw = view_yaw, pitch = view_pitch;
        // Hysteresis (GUIDA_MOTORE: the head is never still): the game's camera only follows the head once it has moved
        // more than ~0.6 degrees from the view last drawn. Re-drawing from a slightly different angle every frame made
        // the game re-sort and re-clip its polygons each time: surfaces lying on others flickered, and the picture
        // shimmered. The headset still shows the exact head direction (each frame is placed with the view it was drawn
        // with).
        {
            static float drawn_y = -100, drawn_p = 0;
            const float HYST = 0.0105f;  // 0.6 degrees
            float dy = yaw - drawn_y;
            while (dy > 3.14159265f) dy -= 6.2831853f;
            while (dy < -3.14159265f) dy += 6.2831853f;
            static const bool no_hyst = getenv("PSXVR_NOHYST") != nullptr;
            // the view may only change at one of the two calls per drawn frame (latch test: 1 = first after a flip,
            // 2 = the second)
            static const int latch = getenv("PSXVR_LATCH") ? atoi(getenv("PSXVR_LATCH")) : 1;
            static u64 last_cap_seen = ~0ull;
            bool first_after_flip = g_cap_frames != last_cap_seen;
            last_cap_seen = g_cap_frames;
            bool may = latch == 0 || (latch == 1 && first_after_flip) || (latch == 2 && !first_after_flip);
            if (may && (no_hyst || drawn_y < -50 || std::fabs(dy) > HYST || std::fabs(pitch - drawn_p) > HYST)) drawn_y = yaw, drawn_p = pitch;
            yaw = drawn_y, pitch = drawn_p;
        }
        // quantize so the stamp is exact (65536 per turn)
        s32 qy = (s32)lroundf(yaw * 65536.0f / 6.2831853f) & 0xffff;
        s32 qp = (s32)lroundf(pitch * 65536.0f / 6.2831853f);
        yaw = qy * 6.2831853f / 65536.0f;
        pitch = qp * 6.2831853f / 65536.0f;
        float sh = sinf(yaw), ch = cosf(yaw), sp = sinf(pitch), cp = cosf(pitch);
        float right[3] = {-ch, 0, sh}, fwd[3] = {-sh, 0, -ch}, down[3] = {0, 1, 0};
        float r[3][3];
        for (int i = 0; i < 3; i++) {
            float f = fwd[i] * cp - down[i] * sp;   // looking up: forward tilts to world up (-y)
            float d = down[i] * cp + fwd[i] * sp;
            r[0][i] = right[i];
            r[1][i] = d;
            r[2][i] = f;
        }
        // The translation from the rotation as the game gets it (rounded to 4.12), not the exact one: the game draws
        // with the rounded one, so the eye it implies (-R^-1 t) must be the eye. With the exact rotation, ~200 m from the
        // map's origin (stage 7) the rounding moved the drawn world 2-5 cm, differently at each angle: it shook as the
        // head turned (measured: the world from the game's own matrix stays within 1 mm between angles now).
        s16 rq[3][3];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) rq[i][j] = (s16)lroundf(r[i][j] * 4096.0f);
        for (u32 base : WSMATRIX) {
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) write16(base + 2 * (3 * i + j), (u16)rq[i][j]);
            for (int i = 0; i < 3; i++) {
                double v = -((double)rq[i][0] * eye_pos[0] + (double)rq[i][1] * eye_pos[1] + (double)rq[i][2] * eye_pos[2]) / 4096.0;
                write32(base + 20 + 4 * i, (u32)(s32)llround(v));
            }
        }
    }
    stamp_frame(yaw, pitch);
    static const bool ctllog = getenv("PSXVR_CTLLOG") != nullptr;
    if (g_view_log && (ctllog || g_render_count % 10 == 0))
        logf("view: control %d pov %d eye %.0f %.0f %.0f game yaw %.1f pitch %.1f drawn %.1f %.1f", (int)in_control_now, (int)scripted_pov_now, eye_pos[0], eye_pos[1], eye_pos[2],
             game_yaw * 57.2958f, game_pitch * 57.2958f, yaw * 57.2958f, pitch * 57.2958f);
}

// The frame being drawn: camera position and direction (heading, pitch) in the game world.
static void stamp_frame(float yaw, float pitch) {
    // frame stamp: yaw relative to the game's own heading (as the template's tools expect), pitch absolute
    {
        float rel = yaw - game_yaw;
        while (rel > 3.14159265f) rel -= 6.2831853f;
        while (rel < -3.14159265f) rel += 6.2831853f;
        g_cam_inject[0] = (s32)lroundf(rel * 65536.0f / 6.2831853f);
        g_cam_inject[1] = (s32)lroundf(pitch * 65536.0f / 6.2831853f);
    }
    if (g_capture) {
        s32 *cam = g_game_cam_pending;
        cam[0] = (s32)eye_pos[0];
        cam[1] = (s32)eye_pos[1];
        cam[2] = (s32)lroundf(yaw * 2048.0f / 6.2831853f) & 0x7ff;  // heading as drawn, 2048 per turn (frontend)
        cam[3] = g_cam_inject[1];
        cam[4] = (s32)eye_pos[2];
        cam[5] = (s32)lroundf(yaw * 65536.0f / 6.2831853f) & 0xffff;  // heading as drawn, 65536 per turn
        cam[6] = (s32)read32(PLAYER + 0x1c);  // the player's feet (height): the ground people stand on
        g_game_cam_pending_valid = true;
    }
}

// Cut-scenes (finding someone, a door being opened between rooms...) set their camera with another routine, which copies
// the world -> screen matrix to GsWSMATRIX at 0x800306f0..0x80030734 (the view function above does not run then):
// stamp the frame with it, so the scene is shown in 3D as well.
static u32 PC_CINE_VIEW_DONE = 0x80030738;
// a third camera routine, same copy to GsWSMATRIX at 0x80030d60..0x80030da4 (the cable car arriving behind the fence)
static u32 PC_CINE_VIEW_DONE2 = 0x80030da8;

// The sound tick (0x80019858, called at each vertical sync: the libsnd sequencer, 0x80019930 -> 0x80019da8, and the
// voices' update, 0x800198a4 -> 0x80020bf0) counts in syncs: the game is made for 50 a second. Built for 60 Hz
// (build60: ~30 frames a second instead of 25) sequences ran a fifth fast and their notes / voices were cut a sixth
// early (one keyed off after 0.65 s instead of 0.78 s): every sixth tick is skipped there (the function returns at
// once), so sound keeps the 50 Hz pace.
static u32 PC_SOUND_TICK = 0x80019858;
static void on_sound_tick() {
    static int n = 0;
    if (++n < 6) return;
    n = 0;
    M.cpu.pc = M.cpu.r[31];  // straight back to the caller (the tick's function has no result)
}
static void on_cine_view_done() {
    // The same routines also draw some things during play (the room after the lift: ~1.5 times a frame): then they
    // are not a scene's camera (the HUD is up; in scenes it is off). Taken as one, they turned the scene bands on and
    // off while playing.
    if (read8(HUD_ON)) return;
    float m[3][3], t[3];
    read_wsmatrix(m, t);
    game_yaw = atan2f(m[0][2], -m[0][0]);
    if (game_yaw < 0) game_yaw += 6.2831853f;
    game_pitch = asinf(std::max(-1.0f, std::min(1.0f, -m[2][1])));
    eye_from_view(m, t, eye_pos);
    have_eye = true;
    in_control_now = false;
    control_held = false;
    scripted_pov_now = false;
    g_render_count++;
    stamp_frame(game_yaw, game_pitch);
}

void set_view(float yaw_right, float pitch_up, bool) {
    view_on = true;
    view_abs = false;
    view_yaw = yaw_right;
    view_pitch = pitch_up;
}
void set_view_abs(float yaw, float pitch_up) {
    view_on = true;
    view_abs = true;
    view_yaw = yaw;
    view_pitch = pitch_up;
}
void clear_view() { view_on = false; }
bool eye_xz(float &x, float &z) {
    if (!have_eye) return false;
    x = eye_pos[0], z = eye_pos[2];
    return true;
}
bool in_control() { return control_fresh(); }
bool scripted_pov() { return scripted_pov_now && M.cycles - last_view_cycles < (u64)CYCLES_PER_FRAME * 20; }
void set_body_heading(bool on, float yaw) {
    body_on = on;
    body_yaw = yaw;
}
// The point the game's camera sees at a world point, with the world -> screen matrix the game is using now (the last one
// written to GsWSMATRIX: its shots are tested with it), in pixels of the 320 x 240 picture.
bool project_world(const float w[3], float &px, float &py) {
    float m[3][3], t[3];
    read_wsmatrix(m, t);
    float c[3];
    for (int i = 0; i < 3; i++) c[i] = m[i][0] * w[0] + m[i][1] * w[1] + m[i][2] * w[2] + t[i];
    if (c[2] < 10) return false;
    float h = (float)(g_gte_h_override ? g_gte_h_override : (u16)M.gte.ctrl[26]);
    px = 160 + c[0] * h / c[2];
    py = 120 + c[1] * h / c[2];
    return true;
}

// Scene space of a captured frame (metres, x right, y up, z back, from the eye it was drawn from) -> game world.
void scene_to_world(const CapFrame &fr, const float sp[3], float w[3]) {
    float h = fr.game_cam[5] * 6.2831853f / 65536.0f, p = fr.cam_pitch * 6.2831853f / 65536.0f;
    float sh = sinf(h), ch = cosf(h), sn = sinf(p), cp = cosf(p);
    float right[3] = {-ch, 0, sh}, fwd[3] = {-sh, 0, -ch}, down[3] = {0, 1, 0}, d2[3], f2[3];
    for (int i = 0; i < 3; i++) f2[i] = fwd[i] * cp - down[i] * sn, d2[i] = down[i] * cp + fwd[i] * sn;
    float c[3] = {sp[0] * 1000.0f, -sp[1] * 1000.0f, -sp[2] * 1000.0f};  // camera space, PS1 convention
    float eye[3] = {(float)fr.game_cam[0], (float)fr.game_cam[1], (float)fr.game_cam[4]};
    for (int i = 0; i < 3; i++) w[i] = eye[i] + right[i] * c[0] + d2[i] * c[1] + f2[i] * c[2];
}

bool take_placement() {
    if (!placement) return false;
    placement = false;
    return true;
}
float player_heading() { return (read16(PLAYER_HEADING) & 0xfff) * 6.2831853f / 4096.0f; }

// GunCon aim from a ray in scene space (the last captured frame's camera: x right, y up, z back, metres): the point
// it hits in the reconstructed world, projected the way the game drew that frame, in pixels of the 320 x 240 picture
// (the game projects around 0,0 and adds the drawing offset 160,120).
bool gun_pixel(const float o[3], const float d[3], float &px, float &py) {
    float t = 0, p[3];
    if (!scene::raycast(o, d, t) || t > 60) t = 30;
    for (int i = 0; i < 3; i++) p[i] = o[i] + d[i] * t;
    float sx, sy;
    if (!scene::project_to_screen(p, sx, sy)) return false;
    px = sx + 160;
    py = sy + 120;
    return true;
}

// Letterbox bars (320 x 24 at the top and bottom, flat), full-screen fades, and the start-of-play blend of the
// picture over itself (the frame buffer used as a texture, in a 256 and a 64 pixel wide piece, as tall as the screen):
// on the HUD panel they made a dark floating "window" in VR. 2D pieces as tall as the screen or flat bars as wide as
// it are left out of the VR view (the real HUD has neither).
static bool feedback_now = false;
static void hide_overlays(const CapFrame &fr, std::vector<uint8_t> &hide) {
    feedback_now = false;
    for (size_t i = 0; i < fr.prims.size() && i < hide.size(); i++) {
        const CapPrim &p = fr.prims[i];
        if (p.nv != 4) continue;
        int x0 = 1 << 30, x1 = -(1 << 30), y0 = 1 << 30, y1 = -(1 << 30);
        bool any3d = false;
        for (int k = 0; k < p.nv; k++) {
            x0 = std::min<int>(x0, p.v[k].sx), x1 = std::max<int>(x1, p.v[k].sx);
            y0 = std::min<int>(y0, p.v[k].sy), y1 = std::max<int>(y1, p.v[k].sy);
            any3d |= p.v[k].has3d;
        }
        if (any3d) continue;
        if (y1 - y0 >= fr.disp_h - 4 || (!p.textured && x1 - x0 >= fr.disp_w - 2)) {
            hide[i] = 1;
            // the picture drawn over itself at 50 % (the opening's blur): the VR frontend makes its own trail
            if (p.textured && p.semi && p.blend == 0 && y1 - y0 >= fr.disp_h - 4) feedback_now = true;
        }
    }
}
bool picture_feedback() { return feedback_now; }

// Shot flashes (spark on a wall, flash on an enemy, page 0x019 / CLUT 0x7b58): the game draws them as a small square
// facing the camera at a fixed depth (~2000 units, 2 m: the "glass" of the screen) on the line of the shot, last in the
// ordering table, so on the flat picture they sit on whatever was hit. In VR that is a sprite floating 2 m away. Each
// one is moved along its own line of sight onto the first surface of the captured world behind it (just in front of it)
// and scaled with the distance, so it keeps the size it had on the screen.
// The laser's point at the last shot (emulation thread writes, the VR thread reads while building the scene)
static std::mutex aim_m;
static float aim_now[3] = {0, 0, 0}, shot_pt[3] = {0, 0, 0};
static bool aim_now_ok = false, shot_pt_ok = false;
static u64 shot_cycles = 0;
void set_aim_world(const float w[3], bool ok) {
    std::lock_guard<std::mutex> lk(aim_m);
    memcpy(aim_now, w, sizeof aim_now);
    aim_now_ok = ok;
}
static void remember_shot() {
    std::lock_guard<std::mutex> lk(aim_m);
    memcpy(shot_pt, aim_now, sizeof shot_pt);
    shot_pt_ok = aim_now_ok;
    shot_cycles = M.cycles;
}
// game world -> camera space of a captured frame (PS1 units, x right, y down, z forward): scene_to_world inverted
static void world_to_frame(const CapFrame &fr, const float w[3], float c[3]) {
    float h = fr.game_cam[5] * 6.2831853f / 65536.0f, p = fr.cam_pitch * 6.2831853f / 65536.0f;
    float sh = sinf(h), ch = cosf(h), sn = sinf(p), cp = cosf(p);
    float right[3] = {-ch, 0, sh}, fwd[3] = {-sh, 0, -ch}, down[3] = {0, 1, 0}, d2[3], f2[3];
    for (int i = 0; i < 3; i++) f2[i] = fwd[i] * cp - down[i] * sn, d2[i] = down[i] * cp + fwd[i] * sn;
    float r[3] = {w[0] - (float)fr.game_cam[0], w[1] - (float)fr.game_cam[1], w[2] - (float)fr.game_cam[4]};
    c[0] = right[0] * r[0] + right[1] * r[1] + right[2] * r[2];
    c[1] = d2[0] * r[0] + d2[1] * r[1] + d2[2] * r[2];
    c[2] = f2[0] * r[0] + f2[1] * r[1] + f2[2] * r[2];
}
void world_to_scene(const CapFrame &fr, const float w[3], float sp[3]) {
    float c[3];
    world_to_frame(fr, w, c);
    sp[0] = c[0] / 1000.0f, sp[1] = -c[1] / 1000.0f, sp[2] = -c[2] / 1000.0f;
}

static bool shot_flash(const CapPrim &p) {
    if (p.kind == CapPrim::LINE || !p.textured || (p.tpage & 0x1ff) != 0x019 || p.clut != 0x7b58) return false;
    for (int i = 0; i < p.nv; i++)
        if (!p.v[i].has3d || p.v[i].z < 1500 || p.v[i].z > 2600) return false;
    return true;
}

static bool ray_tri(const float d[3], const CapVert &a, const CapVert &b, const CapVert &c, float &t) {
    float e1[3] = {b.x - a.x, b.y - a.y, b.z - a.z}, e2[3] = {c.x - a.x, c.y - a.y, c.z - a.z};
    float pv[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
    float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
    if (std::fabs(det) < 1e-6f) return false;
    float inv = 1.0f / det, s[3] = {-a.x, -a.y, -a.z};  // ray from the camera (origin)
    float u = (s[0] * pv[0] + s[1] * pv[1] + s[2] * pv[2]) * inv;
    if (u < 0 || u > 1) return false;
    float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
    float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
    if (v < 0 || u + v > 1) return false;
    t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
    return t > 0;
}

// distance (units) along the unit direction d from the camera to the nearest captured 3D surface, or -1
static float first_hit(const CapFrame &fr, const std::vector<uint8_t> &hide, const float d[3]) {
    float best = -1;
    for (size_t i = 0; i < fr.prims.size(); i++) {
        const CapPrim &p = fr.prims[i];
        if (p.kind == CapPrim::LINE || p.hidden || hide[i] || p.nv < 3 || shot_flash(p)) continue;
        bool all = true;
        for (int k = 0; k < p.nv && all; k++) all = p.v[k].has3d && !p.v[k].hud3d;
        if (!all) continue;
        float t;
        if (ray_tri(d, p.v[0], p.v[1], p.v[2], t) && t > 100 && (best < 0 || t < best)) best = t;
        if (p.nv == 4 && ray_tri(d, p.v[1], p.v[3], p.v[2], t) && t > 100 && (best < 0 || t < best)) best = t;
    }
    return best;
}

static void place_shot_flashes(const CapFrame &fr, std::vector<uint8_t> &hide, std::vector<scene::ExtraPrim> &extra) {
    for (size_t i = 0; i < fr.prims.size() && i < hide.size(); i++) {
        const CapPrim &p = fr.prims[i];
        if (hide[i] || !shot_flash(p)) continue;
        float c[3] = {0, 0, 0};
        for (int k = 0; k < p.nv; k++) c[0] += p.v[k].x, c[1] += p.v[k].y, c[2] += p.v[k].z;
        float len = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        if (len < 1) continue;
        float d[3] = {c[0] / len, c[1] / len, c[2] / len};
        len /= p.nv;
        // centre: the point the laser hit when the trigger was pulled (where the player saw the dot), if this flash
        // belongs to that shot (on its line within ~6 degrees, shot less than 1.5 s ago); else the first surface on
        // the flash's own line of sight
        float centre[3], dist = -1;
        {
            std::lock_guard<std::mutex> lk(aim_m);
            if (shot_pt_ok && fr.game_cam_valid && M.cycles - shot_cycles < (u64)CYCLES_PER_FRAME * 75) {
                float q[3];
                world_to_frame(fr, shot_pt, q);
                float ql = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
                if (ql > 150 && (q[0] * d[0] + q[1] * d[1] + q[2] * d[2]) / ql > 0.9945f)
                    for (int c = 0; c < 3; c++) centre[c] = q[c] / ql, dist = ql;
            }
        }
        if (dist < 0) {
            float t = first_hit(fr, hide, d);
            dist = t < 0 ? 20000 : t;  // nothing behind it (open sky): far away
            memcpy(centre, d, sizeof centre);
        }
        // just in front of the surface (5 cm), so it is not cut by it; scaled to keep its size on the screen up to 4 m
        // away, then its size in the world: the game hardly shrinks it for far targets (Mr X 20 m away: 60 cm wide
        // when kept at its screen size), a real flash gets smaller with the distance
        float at = std::max(150.0f, dist - 50.0f), k = at / len * std::min(1.0f, 4000.0f / at);
        float pc[3] = {c[0] / p.nv, c[1] / p.nv, c[2] / p.nv};
        scene::ExtraPrim e;
        e.p = p;
        for (int v = 0; v < p.nv; v++) {
            e.pos[v][0] = centre[0] * at + (p.v[v].x - pc[0]) * k;
            e.pos[v][1] = centre[1] * at + (p.v[v].y - pc[1]) * k;
            e.pos[v][2] = centre[2] * at + (p.v[v].z - pc[2]) * k;
        }
        float t = dist;
        extra.push_back(e);
        hide[i] = 1;
        if (getenv("PSXVR_FLASHLOG")) logf("shot flash prim %zu at %.0f -> hit %.0f (x%.2f)", i, len, t, k);
    }
}

static u32 CHARACTER_PCS[] = {0x8003e600, 0x8003eec0, 0x8003ef38};  // routines transforming zombies, bodies...

// The shower (stage 1, room 0x20) has its floor modelled 20 cm over the ground the zombies walk on and things lie on, in
// its two ends (x >= 3000 and x <= -4000, the middle is low): the PS1 draws the floor first and them over it, VR showed
// them in it to the knees. VR lowers that floor instead: in those ends every point of the room up to 20 cm over the
// ground comes down onto it and the walls up to the ceiling (3 m) stretch evenly to meet it: a function of the place
// alone, and even along the walls, so it is the same however the game cuts its polygons from frame to frame. Characters, and things lying low (all corners under
// 15 cm: the shotgun), are not moved.
static bool shower_now = false;

// Lickers on the ceiling and the walls (usa_snap_20261119_064036, stage 5 corridor, ceiling 6 m up): the game draws one
// crawling up there upside down (its body against the ceiling, the legs pointing up) but on the wrong side of it: from
// ~10 cm under the ceiling up to 1-1.8 m above it; one climbing a wall likewise has its body against the wall and its
// legs up to 1.4 m inside it. The PS1 has no depth buffer, so they show over the surface anyway; in VR the ceiling or the
// wall hid them. Each frame the room's flat surfaces (level polygons over the eyes: ceilings; upright ones facing along
// x or z: walls) and the characters (their polygons grouped by boxes less than 25 cm apart) are collected; a character
// over a ceiling (its lowest point less than 30 cm under it, up to 2.5 m over it), or off the floor (40 cm up or more)
// and touching a wall (its nearest point less than 30 cm from it either way) and through it by 30 cm..2.5 m, is moved
// back by how far it sticks through: legs on the surface, body hanging from it. For ceilings the highest one over the character
// is used (a balcony's floor seen from below has the ceiling over it: someone standing on it is left alone). (A first
// version mirrored the points under the ceiling: that turned the licker the right way up, as if it walked on a
// floor.) PSXVR_NOCEILINGFIX=1 turns it off.
struct Surface { int ax; float plane, lo[3], hi[3]; int side; };  // side: +1 = behind it is where ax grows
static std::vector<Surface> surfaces;
struct Moved { float lo[3], hi[3], d[3]; };  // a character's box (world) and how far it is moved
static std::vector<Moved> moved;
static bool is_character(const psx::CapPrim &p) {
    for (int k = 0; k < p.nv; k++)
        for (u32 pc : CHARACTER_PCS)
            if (p.v[k].src_pc == pc) return true;
    return false;
}
// camera space of the frame (PS1 units: x right, y down, z forward) -> game world
static void cam_to_world(const CapFrame &fr, const float c[3], float w[3]) {
    float sp[3] = {c[0] / 1000.0f, -c[1] / 1000.0f, -c[2] / 1000.0f};
    scene_to_world(fr, sp, w);
}
static bool prim_world(const CapFrame &fr, const psx::CapPrim &p, float w[4][3]) {
    for (int k = 0; k < p.nv; k++) {
        if (!p.v[k].has3d) return false;
        float c[3] = {p.v[k].x, p.v[k].y, p.v[k].z};
        cam_to_world(fr, c, w[k]);
    }
    return true;
}
static void collect_ceilings(const CapFrame &fr) {
    surfaces.clear();
    moved.clear();
    if (!fr.game_cam_valid || getenv("PSXVR_NOCEILINGFIX")) return;
    // only in the lickers' corridor (stage 2, room 0x12): elsewhere characters stand behind railings, under walkways...
    // and were moved where they should not be (usa_snap_20261119_110054, stage 4 room 0x1e: the cleaners)
    if (read8(STAGE) != 2 || read8(ROOM) != 0x12) return;
    const float eye[3] = {(float)fr.game_cam[0], (float)fr.game_cam[1], (float)fr.game_cam[4]};
    const float ground = (float)fr.game_cam[6];  // the floor under the player (y grows downwards)
    std::vector<Moved> chars;
    for (const auto &p : fr.prims) {
        if (p.nv < 3) continue;
        float w[4][3];
        if (!prim_world(fr, p, w)) continue;
        Moved b;
        for (int i = 0; i < 3; i++) {
            b.lo[i] = b.hi[i] = w[0][i], b.d[i] = 0;
            for (int k = 1; k < p.nv; k++) b.lo[i] = std::min(b.lo[i], w[k][i]), b.hi[i] = std::max(b.hi[i], w[k][i]);
        }
        if (is_character(p)) {
            chars.push_back(b);
            continue;
        }
        for (int ax = 0; ax < 3; ax++) {
            // flat across ax (3 cm), at least 30 cm both other ways
            int o1 = (ax + 1) % 3, o2 = (ax + 2) % 3;
            if (b.hi[ax] - b.lo[ax] > 30 || b.hi[o1] - b.lo[o1] < 300 || b.hi[o2] - b.lo[o2] < 300) continue;
            Surface sf = {ax, (b.lo[ax] + b.hi[ax]) * 0.5f, {b.lo[0], b.lo[1], b.lo[2]}, {b.hi[0], b.hi[1], b.hi[2]}, 0};
            if (ax == 1) {
                if (b.hi[1] > eye[1] - 300) continue;  // a ceiling: at least 30 cm over the eyes (y grows downwards)
                sf.side = -1;
            } else {
                sf.side = eye[ax] < sf.plane ? 1 : -1;
            }
            surfaces.push_back(sf);
        }
    }
    if (surfaces.empty()) return;
    // the characters: polygons whose boxes are less than 25 cm apart are one character
    std::vector<Moved> groups;
    for (const auto &b : chars) groups.push_back(b);
    for (bool merged = true; merged;) {
        merged = false;
        for (size_t a = 0; a < groups.size() && !merged; a++)
            for (size_t c = a + 1; c < groups.size(); c++) {
                bool touch = true;
                for (int i = 0; i < 3; i++)
                    touch = touch && groups[a].lo[i] <= groups[c].hi[i] + 250 && groups[a].hi[i] >= groups[c].lo[i] - 250;
                if (!touch) continue;
                for (int i = 0; i < 3; i++)
                    groups[a].lo[i] = std::min(groups[a].lo[i], groups[c].lo[i]),
                    groups[a].hi[i] = std::max(groups[a].hi[i], groups[c].hi[i]);
                groups.erase(groups.begin() + c);
                merged = true;
                break;
            }
    }
    for (auto &G : groups) {
        float c[3];
        for (int i = 0; i < 3; i++) c[i] = (G.lo[i] + G.hi[i]) * 0.5f;
        bool any = false;
        for (int ax = 0; ax < 3; ax++) {
            // the surface across ax over the character's middle: for ceilings the highest, for walls the one it touches
            int o1 = (ax + 1) % 3, o2 = (ax + 2) % 3;
            const float M = 600;
            const Surface *best = nullptr;
            float best_deep = 0;
            for (const auto &sf : surfaces) {
                if (sf.ax != ax) continue;
                if (c[o1] < sf.lo[o1] - M || c[o1] > sf.hi[o1] + M || c[o2] < sf.lo[o2] - M || c[o2] > sf.hi[o2] + M)
                    continue;
                if (ax == 1 && best && sf.plane >= best->plane) continue;  // (a ceiling: the highest wins)
                float a = sf.side * (G.lo[ax] - sf.plane), b = sf.side * (G.hi[ax] - sf.plane);
                float deep = std::max(a, b), nr = std::min(a, b);
                // (a ceiling: the whole of it may be over the ceiling, up to 2.5 m; a wall: it must touch it)
                if (ax == 1) { best = &sf; best_deep = (nr > -300 && deep > 300 && deep < 2500) ? deep : 0; continue; }
                if (nr > -300 && nr < 300 && deep > 300 && deep < 2500 && deep > best_deep) best = &sf, best_deep = deep;
            }
            // walls: only for someone off the floor (a licker climbing it, its lowest point 40 cm up or more); someone
            // standing behind a wall is in the next room (usa_snap_20261119_103420: zombies waiting behind a closed
            // door were pulled through it into the corridor)
            if (ax != 1 && G.hi[1] > ground - 400) continue;
            if (best && best_deep > 0) G.d[ax] = -best->side * best_deep, any = true;
        }
        if (getenv("PSXVR_CEILLOG"))
            logf("character box x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f moved %.0f %.0f %.0f (ground %.0f)", G.lo[0], G.hi[0],
                 G.lo[1], G.hi[1], G.lo[2], G.hi[2], G.d[0], G.d[1], G.d[2], ground);
        if (any) moved.push_back(G);
    }
}
static bool hang_under_ceiling(const CapFrame &fr, const psx::CapPrim &p, float pos[4][3]) {
    if (!is_character(p)) return false;
    if (moved.empty()) return true;
    float w[4][3], m[3] = {0, 0, 0};
    for (int k = 0; k < p.nv; k++) {
        scene_to_world(fr, pos[k], w[k]);
        for (int i = 0; i < 3; i++) m[i] += w[k][i] / p.nv;
    }
    for (const auto &G : moved) {
        bool in = true;
        for (int i = 0; i < 3; i++) in = in && m[i] >= G.lo[i] - 50 && m[i] <= G.hi[i] + 50;
        if (!in) continue;
        for (int k = 0; k < p.nv; k++) {
            for (int i = 0; i < 3; i++) w[k][i] += G.d[i];
            float c[3];
            world_to_frame(fr, w[k], c);
            pos[k][0] = c[0] / 1000.0f, pos[k][1] = -c[1] / 1000.0f, pos[k][2] = -c[2] / 1000.0f;
        }
        break;
    }
    return true;
}

// The shotgun on the shower's floor (American disc, stage 1 room 0x0b, usa_snap_20261119_131554): the floor there is
// modelled 20 cm over the ground the game puts things on (as in the European shower), so the shotgun (11 cm over the
// ground to 6 cm under it) is all under the floor; the PS1 draws it after the floor, VR hid it. Its polygons (texture
// page 0x017, CLUT 0x78db, in that room only) are drawn 27 cm higher: just over the floor.
static bool american_disc = false;
static bool shower_shotgun_now = false;
static void raise_shower_shotgun(const CapFrame &fr, const psx::CapPrim &p, float pos[4][3]) {
    if (!shower_shotgun_now || (p.tpage & 0x1ff) != 0x017 || p.clut != 0x78db) return;
    for (int k = 0; k < p.nv; k++) {
        float w[3], c[3];
        scene_to_world(fr, pos[k], w);
        w[1] -= 270.0f;  // (y grows downwards)
        world_to_frame(fr, w, c);
        pos[k][0] = c[0] / 1000.0f, pos[k][1] = -c[1] / 1000.0f, pos[k][2] = -c[2] / 1000.0f;
    }
}

static void point_warp(const CapFrame &fr, const psx::CapPrim &p, float pos[4][3]) {
    if (hang_under_ceiling(fr, p, pos)) return;  // (a character: nothing else to do)
    raise_shower_shotgun(fr, p, pos);
    if (!shower_now || !fr.game_cam_valid) return;
    if (is_character(p)) return;
    const float ground = (float)fr.game_cam[6], RAISED = 200, CEILING = 3000;
    float w[4][3], h[4];
    bool low = true, any = false;
    for (int k = 0; k < p.nv; k++) {
        scene_to_world(fr, pos[k], w[k]);
        h[k] = ground - w[k][1];  // over the ground (mm)
        low = low && h[k] < 150;
        any = any || ((w[k][0] >= 2970 || w[k][0] <= -3970) && std::fabs(w[k][2]) <= 3030 && h[k] < CEILING);
    }
    if (low || !any) return;
    for (int k = 0; k < p.nv; k++) {
        if (!(w[k][0] >= 2970 || w[k][0] <= -3970) || std::fabs(w[k][2]) > 3030 || h[k] >= CEILING) continue;
        float nh = h[k] <= RAISED ? 0.0f : (h[k] - RAISED) * CEILING / (CEILING - RAISED);
        w[k][1] = ground - nh;
        float c[3];
        world_to_frame(fr, w[k], c);
        pos[k][0] = c[0] / 1000.0f, pos[k][1] = -c[1] / 1000.0f, pos[k][2] = -c[2] / 1000.0f;
    }
}
static void world_extra(const CapFrame &fr, std::vector<uint8_t> &hide, std::vector<scene::ExtraPrim> &extra) {
    // the ground people and things stand on: the player's feet, under the camera that drew the frame
    scene::set_actor_ground(fr.game_cam_valid ? (fr.game_cam[6] - fr.game_cam[1]) / scene::UNITS_PER_METER : 0.0f);
    // (the European disc's shower; on the American disc room 0x20 of stage 1 is another room, its shower is 0x0b)
    shower_now = !american_disc && read8(STAGE) == 1 && read8(ROOM) == 0x20;
    shower_shotgun_now = american_disc && fr.game_cam_valid && read8(STAGE) == 1 && read8(ROOM) == 0x0b &&
                         !getenv("PSXVR_NOSHOTGUNFIX");
    collect_ceilings(fr);
    hide_overlays(fr, hide);
    place_shot_flashes(fr, hide, extra);
}

// GunCon calibration ("ADJUST GUN", at every boot with a GunCon): 0x80077dd4 runs on each logic frame while the screen
// is up. A shot is accepted within 32 pixels of the reference; the offsets (x / y halfwords at 0x80080710, copy at
// 0x800825d4) are reference - gun. The VR mapping is exact, so the frontend makes the ideal shot itself (the centre of
// the cross, 157,110 of the picture: offsets -16 / -1, the sight then lands exactly where the gun points) and closes it.
static u32 PC_CALIBRATION = 0x80077dd4;  // in its per-frame loop (also with the gun off screen)
static u64 calib_seen = 0;
static bool calib_ever = false;
static void on_calibration() { calib_seen = M.cycles, calib_ever = true; }
bool calibration_screen() { return calib_ever && M.cycles - calib_seen < (u64)CYCLES_PER_FRAME * 10; }
// The item / map menu (Y): 0x800a8822 (American disc) is 1 while its tab is Item (the item list, Equip / Combine /
// Check), 0 on File (the documents: "press A / B" to turn them) and Map. Not found on the European disc (always Item).
// (Unused for now: the VR controls of the Y menu are the same on every tab, as the player asked.)
static u32 MENU_ITEM_TAB = 0;
bool menu_item_tab() { return MENU_ITEM_TAB == 0 || read8(MENU_ITEM_TAB) == 1; }

// ---------------------------------------------------------------- GunCon aim (exact)
// The game turns the gun's counters into a picture position in 0x80011150 (every frame: the sight, gp+308 / gp+312)
// and 0x800113a4 (at a shot: also 0x80062dcc / 0x80062dd0):
//   x = (cx * 320 * 0x057619f1 >> 35) - 64 - *(s16 *)0x80062e08      (cx: 8 MHz clocks from HSYNC)
//   y = cy - 16 - *(s16 *)0x80062e0a                                  (cy: scanline)
// and then smooths it against light-gun jitter: a position within 20 pixels of the previous one only moves a third of
// the way each time. With the VR aim (no jitter) that made a shot fired right after a quick move land behind it. The
// frontend therefore sends the exact counters for the pixel it wants (gun_raw_for) and, with exact aim on, the smoothed
// positions are replaced by the exact ones right after each of the two routines (the filter only; rules unchanged).
static u32 GUN_RAW = 0x80062dc8, GUN_OFF_X = 0x80062e08, GUN_OFF_Y = 0x80062e0a;
static u32 GUN_SHOT_X = 0x80062dcc, GUN_SHOT_Y = 0x80062dd0;
static u32 PC_GUN_FRAME_DONE = 0x80011254, PC_GUN_SHOT_DONE = 0x8001150c;
static bool exact_aim = false;

static int gun_game_x(u32 cx) {
    u64 v = (u64)(cx * 320u) * 0x057619f1ull;
    return (int)((u32)(v >> 32) >> 3) - 64 - (s16)read16(GUN_OFF_X);
}

void gun_raw_for(float px, float py, unsigned short &cx, unsigned short &cy) {
    int want = (int)lroundf(px);
    int offx = (s16)read16(GUN_OFF_X);
    int c = (int)lroundf((want + 64 + offx) / 0.8534355f);
    for (int k = c - 3; k <= c + 3; k++)
        if (k > 0 && gun_game_x((u32)k) == want) { c = k; break; }
    cx = (unsigned short)std::max(1, std::min(0xffff, c));
    cy = (unsigned short)std::max(1, std::min(0x3ff, (int)lroundf(py) + 16 + (s16)read16(GUN_OFF_Y)));
}

static void exact_positions(bool shot) {
    if (!exact_aim || g_pads[0].type != PAD_GUNCON) return;
    u32 v = read32(GUN_RAW);
    u32 cx = ((v >> 8) & 0xff00) | (v >> 24), cy = ((v & 0xff) << 8) | ((v >> 8) & 0xff);
    // the controller buffer the routines read is a frame or two old (polled at vblank) while the trigger is already
    // there: use where the gun points now
    const PadPort &gun = g_pads[0];
    if (gun.gun_raw && gun.gun_onscreen) cx = gun.gun_raw_x, cy = gun.gun_raw_y;
    int x = gun_game_x(cx), y = (int)cy - 16 - (s16)read16(GUN_OFF_Y);
    u32 gp = M.cpu.r[28];
    write32(gp + 308, (u32)x);
    write32(gp + 312, (u32)y);
    if (shot) write32(GUN_SHOT_X, (u32)x), write32(GUN_SHOT_Y, (u32)y), remember_shot();
    if (getenv("PSXVR_GUNLOG")) logf("gun %s frame %llu raw %u %u -> %d %d", shot ? "SHOT " : "frame", (unsigned long long)(M.cycles / CYCLES_PER_FRAME), cx, cy, x, y);
}
static void on_gun_frame_done() {
    exact_positions(false);
    harvest_tick();  // (the weapons' models, from the game: weapon_harvest.cpp)
}
static void on_gun_shot_done() { exact_positions(true); }
void set_exact_aim(bool on) { exact_aim = on; }

// ---------------------------------------------------------------- headshot (experimental)
// Rocket Launcher: 0x8009f280 EU / 0x8009f0d0 USA fires the rocket. Its aim is a point 500 units deep on the line of
// the GunCon's pixel, 0x8009b71c(x - 160, y - 60, 500, out at sp+24) - with 60, not 120, as the picture's middle row,
// so on the game's own camera the rocket flies ~29 degrees under the sight (measured: always on the floor 1.7-3.4 m
// ahead). Then it flies from the head point (PLAYER+0x90, 28 lower) towards that point at 4 times the distance a frame.
// VR: right after that call the point is turned onto the laser's line, at the same distance (same speed).
static u32 PC_ROCKET_AIM = 0x8009f2d8;
// The grenade gun's shot (0x8009bbf0 USA) aims the same way (same call, same mistake): same correction.
static u32 PC_GRENADE_AIM = 0x8009be0c;
static u32 EQUIPPED_SLOT = 0x800a8730;  // pointer to the equipped weapon's inventory slot (item, rounds, ammo type)
static void aim_projectile(bool lobbed);
static void on_rocket_aim() { aim_projectile(false); }
// Grenade rounds fly ~398 units a logic step (2 frames) whatever the point's distance (flame / acid rounds: twice the
// distance to the point), and fall: 14 units more a step downwards (measured; the step's speed grows by 14, then it
// moves). Their direction across is the laser's; the angle
// up is chosen so that the arc passes through the laser's point (the lower of the two arcs; out of reach, ~10 m on
// flat ground: the longest one).
static void on_grenade_aim() { aim_projectile(true); }
// The grenade gun in VR shoots like a real one: the grenade leaves along the barrel and falls, and the laser is drawn as
// that arc (vr_main). The game's point is put on the barrel's line, 600 units from where the grenade starts (the head
// point, 28 lower). Grenade rounds are a fan of 5 grenades (measured): turned -7, +7, -14, +14 and 0 degrees, flying
// 375, 375, 562, 562 and 750 units a logic step across whatever the barrel's tilt, and up / down as the tilt (x ~1.04);
// the group's middle lands where one at ~1.35 x 375 = 505 across would (measured: within ~5 cm across and ~10 % along
// at 2-7 m), so that is the arc. Flame / acid rounds are one, at twice that distance a step (1200) along the barrel. lob_info gives
// the VR thread the same numbers.
static const float LOB_START = 600.0f;
static float aim_dir_now[3] = {0, 0, -1};
static bool aim_dir_now_ok = false;
void set_aim_dir_world(const float d[3], bool ok) {
    std::lock_guard<std::mutex> lk(aim_m);
    memcpy(aim_dir_now, d, sizeof aim_dir_now);
    aim_dir_now_ok = ok;
}
static bool grenade_rounds() {
    u32 slot = read32(EQUIPPED_SLOT);
    return (slot >> 24) == 0x80 && (s16)read16(slot + 4) == 15;
}
bool lob_info(float origin[3], float &speed, float &start, bool &across) {
    if (equipped_weapon() != 9 || getenv("PSXVR_NOROCKETAIM")) return false;
    for (int i = 0; i < 3; i++) origin[i] = (float)(s32)read32(PLAYER + 0x90 + 4 * i);
    origin[1] += 28;
    across = grenade_rounds();
    speed = across ? 505.0f : 2.0f * LOB_START;
    start = LOB_START;
    return true;
}
static float lob_target_now[3] = {0, 0, 0};
static bool lob_target_now_ok = false;
void set_lob_target(const float w[3], bool ok) {
    std::lock_guard<std::mutex> lk(aim_m);
    memcpy(lob_target_now, w, sizeof lob_target_now);
    lob_target_now_ok = ok;
}
// The tilt (up / down over across) that takes the grenade from the head point (+28) to the point t: the same flight as
// the arc's (grenade rounds: 505 across a step, up / down = tilt x 505 x 1.04; flame / acid: 1200 along the line),
// starting 600 along the line; the lower of the two arcs (out of reach: the longest).
static bool lob_tilt_to(const float o[3], const float t[3], bool rounds, float &tilt) {
    float dx = t[0] - o[0], dz = t[2] - o[2], dh = sqrtf(dx * dx + dz * dz);
    if (dh < 100) return false;
    float best = -1e30f, best_tilt = 0;
    for (float a = 1.3f; a > -1.3f; a -= 0.002f) {  // radians, positive = down
        float tl = tanf(a), c = cosf(a), s = sinf(a);
        float x = LOB_START * c, y = o[1] + LOB_START * s;  // start (across from o, height)
        float vh = rounds ? 505.0f : 1200.0f * c, vy = rounds ? 505.0f * 1.04f * tl : 1200.0f * s;
        bool reached = false;
        for (int n = 0; n < 400 && x < dh; n++) {
            vy += 14.0f;
            float nx = x + vh, ny = y + vy;
            if (nx >= dh) { y = y + (ny - y) * (dh - x) / vh; x = dh; reached = true; break; }
            x = nx, y = ny;
        }
        if (!reached) continue;
        if (y <= t[1]) { tilt = a; return true; }  // (y grows downwards: the first angle reaching it from below)
        if (-y > best) best = -y, best_tilt = a;
    }
    tilt = best_tilt;
    return true;
}
static bool lob_along_barrel() {
    float u[3];
    {
        float t[3];
        bool ok;
        {
            std::lock_guard<std::mutex> lk(aim_m);
            ok = lob_target_now_ok && !getenv("PSXVR_GRENADE_DIR");
            memcpy(t, lob_target_now, sizeof t);
        }
        if (const char *e = getenv("PSXVR_GRENADE_AT")) ok = sscanf(e, "%f,%f,%f", &t[0], &t[1], &t[2]) == 3;  // (tests)
        if (ok) {
            float o[3], a;
            for (int i = 0; i < 3; i++) o[i] = (float)(s32)read32(PLAYER + 0x90 + 4 * i);
            o[1] += 28;
            if (lob_tilt_to(o, t, grenade_rounds(), a)) {
                float dx = t[0] - o[0], dz = t[2] - o[2], dh = sqrtf(dx * dx + dz * dz);
                u32 sp = M.cpu.r[29];
                float w[3] = {o[0] + dx / dh * cosf(a) * LOB_START, o[1] + sinf(a) * LOB_START,
                              o[2] + dz / dh * cosf(a) * LOB_START};
                for (int i = 0; i < 3; i++) write32(sp + 24 + 4 * i, (u32)(s32)lroundf(w[i]));
                if (getenv("PSXVR_GRENADE_AT")) logf("grenade to %.0f %.0f %.0f: tilt %.1f", t[0], t[1], t[2], a * 57.3f);
                return true;
            }
        }
    }
    if (const char *e = getenv("PSXVR_GRENADE_DIR")) {  // (testing: the barrel's direction in the world)
        sscanf(e, "%f,%f,%f", &u[0], &u[1], &u[2]);
    } else {
        std::lock_guard<std::mutex> lk(aim_m);
        if (!aim_dir_now_ok) return false;
        memcpy(u, aim_dir_now, sizeof u);
    }
    float l = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (l < 1e-6f) return false;
    for (int i = 0; i < 3; i++) u[i] /= l;
    u32 sp = M.cpu.r[29];
    float o[3];
    for (int i = 0; i < 3; i++) o[i] = (float)(s32)read32(PLAYER + 0x90 + 4 * i);
    o[1] += 28;
    for (int i = 0; i < 3; i++) write32(sp + 24 + 4 * i, (u32)(s32)lroundf(o[i] + u[i] * LOB_START));
    return true;
}
static void aim_projectile(bool lobbed) {
    float t[3];
    if (getenv("PSXVR_NOROCKETAIM")) return;
    if (lobbed && !getenv("PSXVR_ROCKET_TEST") && lob_along_barrel()) return;
    if (const char *e = getenv("PSXVR_ROCKET_TEST")) {  // (testing: a world point as the laser's)
        sscanf(e, "%f,%f,%f", &t[0], &t[1], &t[2]);
    } else {
        std::lock_guard<std::mutex> lk(aim_m);
        if (!aim_now_ok) return;
        memcpy(t, aim_now, sizeof t);
    }
    u32 sp = M.cpu.r[29];
    float o[3], g[3], d[3];
    for (int i = 0; i < 3; i++) o[i] = (float)(s32)read32(PLAYER + 0x90 + 4 * i);
    o[1] += 28;
    for (int i = 0; i < 3; i++) g[i] = (float)(s32)read32(sp + 24 + 4 * i) - o[i], d[i] = t[i] - o[i];
    float lg = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]), ld = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (lg < 1 || ld < 1) return;
    float u[3] = {d[0] / ld, d[1] / ld, d[2] / ld};
    float dh = sqrtf(d[0] * d[0] + d[2] * d[2]);
    if (lobbed && dh > 1) {
        // grenade rounds (ammo 15) fly at ~398 a step, turned by the game; flame / acid rounds (13 / 14) at twice
        // the distance to the aimed point a step, straight
        u32 slot = read32(EQUIPPED_SLOT);
        bool rounds = (slot >> 24) == 0x80 && (s16)read16(slot + 4) == 15;
        const float G = 14.0f, spd = rounds ? 398.0f : 2.0f * lg;
        float best = 1e30f, best_a = 0, low = 0;
        bool found = false;
        // from steeply down to steeply up: the first angle whose arc reaches the point's height is the lower arc
        for (float a = 1.5f; a > -1.5f; a -= 0.002f) {  // radians, positive = down
            float vh = spd * cosf(a), vy = spd * sinf(a);
            float n = dh / vh;                          // steps to get there
            float y = vy * n + G * n * (n + 1) * 0.5f;  // how far down by then
            if (y <= d[1]) { low = a; found = true; break; }
            if (y < best) best = y, best_a = a;
        }
        float a = found ? low : best_a;
        if (getenv("PSXVR_ROCKET_TEST")) logf("grenade aim: dist %.0f drop %.0f speed %.0f angle %.1f %s", dh, d[1], spd, a * 57.3f, found ? "low arc" : "out of reach");
        // the game turns the grenade's flight by 80 / 4096 of a turn (~7 degrees: atan2(x, z) comes out smaller):
        // the point is turned the other way first
        const float k = rounds ? 80.0f * 6.2831853f / 4096.0f : 0.0f, ck = cosf(k), sk = sinf(k);
        float hx = d[0] / dh, hz = d[2] / dh;
        u[0] = (hx * ck + hz * sk) * cosf(a), u[2] = (hz * ck - hx * sk) * cosf(a), u[1] = sinf(a);
    }
    for (int i = 0; i < 3; i++) write32(sp + 24 + 4 * i, (u32)(s32)lroundf(o[i] + u[i] * lg));
    if (getenv("PSXVR_ROCKET_TEST"))
        logf("projectile aim: origin %.0f %.0f %.0f point %d %d %d", o[0], o[1], o[2], (s32)read32(sp + 24),
             (s32)read32(sp + 28), (s32)read32(sp + 32));
}

// A shot is queued on an enemy by 0x800710d8 (a0 = the enemy, a2 = the hit box): enemies are 0x8c4 bytes, with their hit
// boxes at +908 (92 bytes each: +14 the kind, +48 the bone's GsCOORDINATE2) and their model at +0x34 as pairs (bone,
// mesh); a mesh is 0xffffffff, a pointer, the number of its polygons, then the polygons. Each enemy has its own copy of
// its model. Only zombies have a hit box of kind 0 (a small one low in the head): it kills at once (their code sets the
// health to -1 and plays a sound, 0x8008080c bank 2 / 14), with the ordinary death. VR adds what the game does not show:
// the head goes (its mesh draws no polygon). The count is put back when the enemy's slot holds a living one again.
static u32 PC_HIT_QUEUE = 0x800710d8;
// The game's hit effects: 0x80099218(position object: x / y / z as s32 at +0x18, owner, 0, 0; kind on the stack) spawns
// one at the object (a bone's GsCOORDINATE2 for the zombies' own limbs coming off, 0x80101f88); kind 31 is a spurt of
// blood. The headshot spawns a few at the head's bone.
static u32 PC_HIT_EFFECT = 0x80099218;
// The position is read from +0x18 / +0x1c / +0x20 of the object (a temporary one below the stack holds it).
static void hit_effect(const s32 w[3], int kind, u32 owner) {
    Cpu &c = M.cpu;
    u32 a2 = c.r[6], a3 = c.r[7];
    u32 obj = c.r[29] - 0xc0;  // (cpu_call runs the function 0x100 below the stack: this stays clear of its frames)
    for (int i = 0; i < 3; i++) write32(obj + 0x18 + 4 * i, (u32)w[i]);
    c.r[6] = 0, c.r[7] = 0;
    write32(c.r[29] - 0x100 + 16, (u32)kind);  // its fifth argument, on the stack
    cpu_call(PC_HIT_EFFECT, obj, owner);
    c.r[6] = a2, c.r[7] = a3;
}
// A point given in a bone's own space, in the world: the bone's matrix workm (GsCOORDINATE2 +0x24, 3 x 3 s16 in 4.12)
// and translation (+0x38; the translation at +0x18 is relative to the parent bone).
static void bone_to_world(u32 bone, const s32 l[3], s32 w[3]) {
    for (int i = 0; i < 3; i++) {
        s64 v = 0;
        for (int j = 0; j < 3; j++) v += (s64)(s16)read16(bone + 0x24 + 2 * (3 * i + j)) * l[j];
        w[i] = (s32)(v >> 12) + (s32)read32(bone + 0x38 + 4 * i);
    }
}
// The sound of the critical shot (the zombies' 0x8008080c: bank 2, sound 14, its volume from the distance) is played
// twice for a headshot, the second at full volume: it starts with the first (same tick), so it is louder.
static u32 PC_POS_SOUND = 0x8008080c, PC_PLAY_SOUND = 0x8008038c;
static int headshot_sound = 0;  // logic frames in which the next critical sound is a headshot's
static void on_pos_sound() {
    Cpu &c = M.cpu;
    if (headshot_sound <= 0 || c.r[5] != 2 || c.r[6] != 14) return;
    headshot_sound = 0;
    static const int copies = getenv("PSXVR_HS_SOUND") ? atoi(getenv("PSXVR_HS_SOUND")) : 3;
    u32 a2 = c.r[6], a3 = c.r[7];
    for (int i = 0; i < copies; i++) {
        c.r[6] = 127, c.r[7] = 127;
        cpu_call(PC_PLAY_SOUND, 2, 14);
    }
    c.r[6] = a2, c.r[7] = a3;
}
// off by default (the original game); the VR options turn it on (headless tests: PSXVR_HEADSHOT=1)
static volatile bool headshot_on = getenv("PSXVR_HEADSHOT") != nullptr;
static void headshot_spurt(u32 bone, const s32 centre[3], int i, int spread, int kind, u32 owner) {
    static const s32 around[5][3] = {{0, 0, 0}, {1, -1, 0}, {-1, -1, 0}, {0, -1, 1}, {0, -1, -1}};
    s32 l[3], w[3];
    for (int j = 0; j < 3; j++) l[j] = centre[j] + around[i % 5][j] * spread;
    bone_to_world(bone, l, w);
    // the game's spurt (kind 31) appears ~20 cm to the right of and ~25 cm above the point it is given, as the camera
    // sees it: started that much the other way (along the camera's right / down axes, the rows of the world -> camera
    // matrix), it bursts around the head. PSXVR_HS_OFF=left,down (world units) to try others.
    static float off_left = 220, off_down = 250;
    static const bool off_read = [] {
        if (const char *o = getenv("PSXVR_HS_OFF")) sscanf(o, "%f,%f", &off_left, &off_down);
        return true;
    }();
    (void)off_read;
    {
        float m[3][3], t[3];
        read_wsmatrix(m, t);
        for (int j = 0; j < 3; j++) {
            float lr = std::sqrt(m[0][0] * m[0][0] + m[0][1] * m[0][1] + m[0][2] * m[0][2]);
            float ld = std::sqrt(m[1][0] * m[1][0] + m[1][1] * m[1][1] + m[1][2] * m[1][2]);
            if (lr < 1e-6f || ld < 1e-6f) break;
            w[j] += (s32)lroundf(-off_left * m[0][j] / lr + off_down * m[1][j] / ld);
        }
    }
    static const bool no_owner = getenv("PSXVR_HS_NOOWNER") != nullptr;
    hit_effect(w, kind, no_owner ? 0 : owner);
}
struct Gush { u32 bone; s32 centre[3]; int t; u32 enemy; };
static int last_hs_room = -1;
static std::vector<Gush> gushes;
// once a logic frame after a headshot: a spurt every other frame from the head's place, for 16 logic frames
static void gush_step() {
    static const int every = getenv("PSXVR_HS_EVERY") ? atoi(getenv("PSXVR_HS_EVERY")) : 2;
    static const int length = getenv("PSXVR_HS_LENGTH") ? atoi(getenv("PSXVR_HS_LENGTH")) : 16;
    for (size_t i = 0; i < gushes.size();) {
        Gush &g = gushes[i];
        g.t++;
        if (g.t % std::max(1, every) == 0) headshot_spurt(g.bone, g.centre, g.t / std::max(1, every), 45, 31, g.enemy);
        if (g.t >= length) gushes.erase(gushes.begin() + i);
        else i++;
    }
}
struct Headless { u32 enemy, mesh, count; int room; };
static std::vector<Headless> headless;
static void on_hit_queue() {
    if (!headshot_on) return;
    u32 e = M.cpu.r[4], part = M.cpu.r[6] & 0xff;
    if ((e >> 24) != 0x80 || part > 15) return;
    u32 box = e + 908 + 92 * part;
    if ((s16)read16(box + 14) != 0) return;  // not the critical spot
    u32 bone = read32(box + 48);
    for (int k = 0; k < 24; k++) {
        u32 c = read32(e + 0x34 + 8 * k), mesh = read32(e + 0x38 + 8 * k);
        if (c == 0) break;
        if (c != bone || read32(mesh) != 0xffffffffu) continue;
        u32 n = read32(mesh + 8);
        if (n == 0 || n > 2000) return;
        headless.push_back({e, mesh, n, room_key()});
        write32(mesh + 8, 0);
        logf("headshot: enemy %08x head (mesh %08x, %u polygons) gone", e, mesh, n);
        // blood where the head was: the centre of its hit box (in the bone's space), and around it to make it wider
        static const int spurts = getenv("PSXVR_HS_BLOOD") ? atoi(getenv("PSXVR_HS_BLOOD")) : 5;
        static const int kind = getenv("PSXVR_HS_KIND") ? atoi(getenv("PSXVR_HS_KIND")) : 31;
        static const int spread = getenv("PSXVR_HS_SPREAD") ? atoi(getenv("PSXVR_HS_SPREAD")) : 90;
        s32 centre[3] = {0, -88, 0};
        for (int b = 0; b < 16; b++) {  // the head's own box (kind 1) on the same bone
            u32 hb = e + 908 + 92 * b;
            if ((s16)read16(hb + 6) < 0) break;
            if ((s16)read16(hb + 14) == 1 && read32(hb + 48) == bone) {
                for (int i = 0; i < 3; i++) centre[i] = ((s16)read16(hb + 2 * i) + (s16)read16(hb + 8 + 2 * i)) / 2;
                break;
            }
        }
        for (int i = 0; i < spurts; i++) headshot_spurt(bone, centre, i, spread, kind, e);
        gushes.push_back({bone, {centre[0], centre[1], centre[2]}, 0, e});
        last_hs_room = room_key();
        headshot_sound = 30;
        return;
    }
}
// once a logic frame: a slot that holds a living enemy again gets its head back; another room's data is not touched
static void headless_check() {
    if (headshot_sound > 0) headshot_sound--;
    if (headshot_on && room_key() == last_hs_room) gush_step();
    else gushes.clear();
    for (size_t i = 0; i < headless.size();) {
        Headless &h = headless[i];
        bool drop = h.room != room_key() || read32(h.mesh) != 0xffffffffu || read32(h.mesh + 8) != 0;
        if (!drop && (s16)read16(h.enemy + 798) > 0) {
            write32(h.mesh + 8, h.count);
            drop = true;
        }
        if (drop) headless.erase(headless.begin() + i);
        else i++;
    }
}
void set_headshot(bool on) { headshot_on = on; }

static void headless_clear() { headless.clear(); gushes.clear(); }

// The American disc (SLUS_010.87): the same program with its code and data moved (found with tools/addrmap.py
// from RAM dumps of both discs in the first alley: every one matched exactly).
static void use_american_addresses() {
    american_disc = true;
    PC_PAD_MAPPED = 0x800715e4;
    PLAYER = 0x800a85c0;
    PLAYER_HEADING = 0x800a8606;
    PLAYER_HEADING2 = 0x800b3d9c;
    HUD_ON = 0x80062dec;
    STAGE = 0x80062f04;
    ROOM = 0x80062e48;
    WALK_BLOCK = 0x80062e2e;
    WALK_FLAG_A = 0x80062e40;
    WALK_FLAG_B = 0x80062e2c;
    WEAPON = 0x800637d6;
    PC_MDEC_COMMAND = 0x80038888;
    PC_MOVE_REQUESTS = 0x8007777c;
    MOVE_REQUESTS = 0x800b6080;
    MOVE_SPEED = 0x800b3d48;
    MOVE_SPEED_PREV = 0x800b4dfc;
    MOVE_TABLE = 0x800b4de4;
    PC_STEP_VECTOR = 0x80078780;
    PC_STEP_VECTOR_DONE = 0x80078794;
    PC_VIEW_DONE = 0x80034380;
    PC_CINE_VIEW_DONE = 0x80030748;
    PC_CINE_VIEW_DONE2 = 0x80030db8;
    PC_SOUND_TICK = 0x80019868;
    PC_CALIBRATION = 0x80077c5c;  // (in the boot overlay: found from dumps taken at the calibration screen)
    GUN_RAW = 0x80062dd8;
    GUN_OFF_X = 0x80062e18;
    GUN_OFF_Y = 0x80062e1a;
    GUN_SHOT_X = 0x80062ddc;
    GUN_SHOT_Y = 0x80062de0;
    PC_GUN_FRAME_DONE = 0x80011264;
    PC_GUN_SHOT_DONE = 0x8001151c;
    PC_HIT_QUEUE = 0x800710fc;
    PC_ROCKET_AIM = 0x8009f128;
    PC_GRENADE_AIM = 0x8009bc5c;
    MENU_ITEM_TAB = 0x800a8822;
    EQUIPPED_SLOT = 0x800a8580;
    PC_HIT_EFFECT = 0x80099068;
    WSMATRIX[0] = 0x80065b70, WSMATRIX[1] = 0x80065bb0;
    CHARACTER_PCS[0] = 0x8003e610, CHARACTER_PCS[1] = 0x8003eed0, CHARACTER_PCS[2] = 0x8003ef48;
}
void install_capture_hooks() {
    // The addresses below are the European disc's (SLES_027.32). Another disc (the American SLUS_010.87) has its code
    // elsewhere: nothing is hooked there until it has its own table (the game runs as on a PS1).
    if (::g_boot_name.find("SLUS_010.87") != std::string::npos) {
        // The American disc refuses the GunCon: after reading the controllers (0x80010f48..) it changes the GunCon's
        // type (0x200000) into a pad's (0x8000) at 0x80010f70, the only difference of its main program from the
        // European one besides moved addresses. That store is skipped: the GunCon works as on the European disc.
        cpu_add_hook(0x80010f70, [] { M.cpu.pc = 0x80010f74; });
        // The demo (attract mode, "Press Any Button") ends on Start of a pad, but its check ignores a GunCon on purpose
        // (0x800716d0: the controller's type; 0x200000 = GunCon -> no check). Here, with a GunCon, any of its buttons
        // pressed (the raw buttons at 0x8006bd82, active low: trigger, A, B) ends it the same way (0x800716f4 on).
        cpu_add_hook(0x800716dc, [] {
            if (M.cpu.r[2] != 0x200000) return;
            u16 raw = read16(0x8006bd82);
            M.cpu.pc = (raw & 0x6008) != 0x6008 ? 0x800716f4 : 0x80071724;  // (bit 3 A, 13 trigger, 14 B)
        });
        use_american_addresses();
        logf("game module: American disc (GunCon allowed again)");
    } else if (::g_boot_name.find("SLES_027.32") == std::string::npos) {
        logf("game module: %s is not the European disc, no VR hooks", ::g_boot_name.c_str());
        return;
    }
    scene::set_world_extra(world_extra);
    // characters (zombies, bodies): 0x8003e600 / 0x8003eec0 / 0x8003ef38, never used for the room
    scene::set_model_pcs(CHARACTER_PCS, 3);
    scene::set_point_warp(point_warp);
    // RE Survivor projects around 0,0 (GTE OFX/OFY = 0) and centres the picture with the GPU drawing offset
    g_world_ofx = 0;
    g_world_ofy = 0;
    scene::set_fog(0, 0);  // the game darkens with distance by itself (vertex colours): no extra VR haze
    cpu_add_hook(PC_PAD_MAPPED, on_pad_mapped);
    cpu_add_hook(PC_CALIBRATION, on_calibration);
    cpu_add_hook(PC_GUN_FRAME_DONE, on_gun_frame_done);
    cpu_add_hook(PC_GUN_SHOT_DONE, on_gun_shot_done);
    cpu_add_hook(PC_HIT_QUEUE, on_hit_queue);
    cpu_add_hook(PC_ROCKET_AIM, on_rocket_aim);
    cpu_add_hook(PC_GRENADE_AIM, on_grenade_aim);
    cpu_add_hook(PC_POS_SOUND, on_pos_sound);
    cpu_add_hook(PC_VIEW_DONE, on_view_done);
    cpu_add_hook(PC_CINE_VIEW_DONE, on_cine_view_done);
    cpu_add_hook(PC_CINE_VIEW_DONE2, on_cine_view_done);
    // (the European disc at 60 Hz only: the American one is made for 60)
    if (FRAME_HZ == 60 && ::g_boot_name.find("SLES") != std::string::npos) cpu_add_hook(PC_SOUND_TICK, on_sound_tick);
    cpu_add_hook(PC_MDEC_COMMAND, on_mdec_command);
    cpu_add_hook(PC_MOVE_REQUESTS, on_move_requests);
    cpu_add_hook(PC_STEP_VECTOR, on_step_vector);
    cpu_add_hook(PC_STEP_VECTOR_DONE, on_step_vector_done);
    if (const char *e = getenv("PSXVR_INJECT")) inject_bits = (u16)strtoul(e, nullptr, 16);
}

unsigned long long render_count() { return g_render_count; }
float game_pitch_up() { return game_pitch; }
float game_heading() { return game_yaw; }
void set_spell_aim(bool, float, float) {}
unsigned short steer_toward(float) { return 0; }
unsigned short pitch_toward(float) { return 0; }
void set_spell_origin(bool, float, float, float) {}

}  // namespace game
