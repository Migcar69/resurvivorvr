// Game-specific knowledge for the 3D capture / VR layer. This is the ONLY place where a new game is described:
// the core (src/core) and the renderer (src/vr) stay generic. Start from the stub in game.cpp and look at
// esempi/magiccarpet/mc_game.cpp for a complete example (sky, ground, sprites, far landscape, view injection).
#pragma once
#include "../core/capture.h"

namespace game {

// Called once at boot when capture is on (VR / --preview3d / --capdump): install code hooks (cpu_add_hook),
// tell the capture where the world is projected (g_world_ofx/ofy), sky routines (scene::set_sky_pcs), etc.
void install_capture_hooks();
extern bool g_view_log;  // --view-log: log the camera arguments of the game's renderer

// Drawn view: yaw added to the game's own heading (radians, positive = right), absolute pitch (radians, positive
// = up), roll levelled. Frames built from now on are stamped with it (CapFrame::cam_yaw/cam_pitch).
void set_view(float yaw_right, float pitch_up, bool level_roll = true);
void clear_view();
// Resident Evil Survivor: absolute view (yaw in the game world, heading convention: 0..2pi growing to the right)
void set_view_abs(float yaw, float pitch_up);
// The player controls the game with the eye camera (false in cut-scenes, menus): the absolute view applies only then;
// a scripted camera is looked around relative to the head yaw it took over with.
bool in_control();
// The opening (the hero waking up, blurred, before the HUD appears): the camera is the player's eyes but the game drives
// it; handled as a scripted camera (not in control), the head looks around it.
bool scripted_pov();
// The frame last given to the scene draws the previous picture over itself at 50 % (the opening's blur; hidden from the
// 3D world, the VR frontend reproduces it as a trail of its own images).
bool picture_feedback();
// The point the VR laser hits in the game world (each frame): a shot remembers it, and the game's shot flash is drawn
// there (place_shot_flashes).
void set_aim_world(const float w[3], bool ok);
// The barrel's direction in the game world (the grenade gun shoots along it)
void set_aim_dir_world(const float d[3], bool ok);
// Where the grenade gun's arc (drawn from the muzzle) meets the room: the grenade is thrown to land there
void set_lob_target(const float w[3], bool ok);
// Grenade gun equipped: where its grenade starts from (game world), its speed (units a logic step, 2 frames; with
// across: the speed across, the speed up / down then follows the barrel's tilt x 1.04) and how far along the barrel's
// line from that point it is put (the arc starts there); false for any other weapon
bool lob_info(float origin[3], float &speed, float &start, bool &across);
// Game world -> scene space of a captured frame (metres: x right, y up, z back), scene_to_world inverted
void world_to_scene(const psx::CapFrame &fr, const float w[3], float sp[3]);
// The headshot's extra effects (the head goes, blood, a louder sound): a VR option, off by default
void set_headshot(bool on);
// Turn the player to this heading (game convention, radians) at each logic frame while on and in control.
void set_body_heading(bool on, float yaw = 0);
float player_heading();
// True once when the game itself placed / turned the player (room entered, scene over): the frontend turns its world
// so the player looks that way; until then the body heading is not written.
bool take_placement();
// The GunCon calibration screen is up (the frontend calibrates by itself: an exact shot at the cross, then A + B).
bool calibration_screen();
// The Y menu shows its Item tab (not File / Map)
bool menu_item_tab();
// Aim: game world point -> pixel of the picture with the camera the game uses now (its shots are tested with it);
// scene space of a captured frame -> game world.
bool project_world(const float w[3], float &px, float &py);
// The GunCon counters that make the game put its sight / shot exactly on pixel px, py of the picture, and the exact
// positions without the game's light-gun smoothing (VR aim has no jitter).
void gun_raw_for(float px, float py, unsigned short &cx, unsigned short &cy);
void set_exact_aim(bool on);
void scene_to_world(const psx::CapFrame &fr, const float sp[3], float w[3]);
constexpr float GUN_CROSS_X = 157, GUN_CROSS_Y = 110;  // gives the ideal offsets -16 / -1 (sight exactly on the gun)

// GunCon aim: ray in scene space (metres, x right, y up, z back, game eye at 0) -> pixel of the game's 320x240 picture
bool gun_pixel(const float o[3], const float d[3], float &px, float &py);
// Game actions (BUTTON CONFIG bits, see PAD_* below) added to / removed from what the pad gives, each logic frame
void set_actions(unsigned short add, unsigned short remove = 0);
// Walk forward (the player's heading) at each logic frame while on, also while shooting (the original GunCon walks only
// with the trigger held off the picture).
void set_walk(bool on, bool run = false, bool back = false);  // run: the pad's running speed; back: the pad's step back
// Direction the step is built from (game heading convention, radians; backwards: the opposite one) while the player
// keeps facing its own heading.
void set_walk_direction(bool on, float yaw = 0);
// The weapon the player holds (0x800637c6: 1 = Handgun A, 3 = Handgun C).
int equipped_weapon();
void on_state_loaded();  // a save was loaded while running: forget what was kept of the game
int player_pos(int axis);  // the player's feet: x, y, z (game units)
int room_key();  // stage << 8 | room (changes when the player goes into another room)
// The captured picture is a full-screen picture (movie, tips, ADJUST GUN), not a menu: no hands / laser then.
bool movie_frame(const psx::CapFrame &fr, const uint16_t *vram);
bool full_picture(const psx::CapFrame &fr);  // a picture over the whole screen hides the 3D view drawn under it
// The game camera's eye (x, z in the game world) as of its last view, false before any.
bool eye_xz(float &x, float &z);
// The game's own model of a weapon for the VR pistol, or nullptr (no model: the built-in pistol).
const void *weapon_model(int weapon);  // a vrhands::PistolModel
// The weapons' own models rebuilt from the player's game (weapon_harvest.cpp): cache file, RAM scan, the model if seen
void harvest_init(const char *path);
void harvest_tick();
const void *harvested_model(int weapon);
// Number of 3D frames the game has drawn (advances only while the 3D view runs; 0 = always the virtual screen).
unsigned long long render_count();
// The game's own view of the last drawn frame: pitch (radians, up positive) and heading (0..2pi, right positive).
float game_pitch_up();
float game_heading();
// Projectiles / attacks launched from now on go towards this direction (yaw relative to the drawn heading).
void set_spell_aim(bool on, float yaw_right = 0, float pitch_up = 0);
// Turn bits that steer the player towards a heading err radians to its right (once per emulated frame).
unsigned short steer_toward(float err);
// Look up / down bits that bring the player's pitch to 'target' radians (positive = up; once per emulated frame).
unsigned short pitch_toward(float target);
// The player's projectiles start from this point: metres from the game camera along the world x / z axes and up.
void set_spell_origin(bool on, float dx_m = 0, float dz_m = 0, float up_m = 0);

// Pad layout (PsyQ PadRead bits)
enum : unsigned short {
    PAD_L2 = 1 << 0, PAD_R2 = 1 << 1, PAD_L1 = 1 << 2, PAD_R1 = 1 << 3,
    PAD_TRI = 1 << 4, PAD_CIR = 1 << 5, PAD_X = 1 << 6, PAD_SQR = 1 << 7,
    PAD_SEL = 1 << 8, PAD_START = 1 << 11, PAD_UP = 1 << 12, PAD_RIGHT = 1 << 13, PAD_DOWN = 1 << 14,
    PAD_LEFT = 1 << 15,
};
// The game's controls, as the VR frontend uses them. FIND THEM by trying each button in game (headless runs with
// --input and --shot, or the position in RAM) and fix these: they are only placeholders.
constexpr unsigned short PAD_FORWARD = PAD_UP;
constexpr unsigned short PAD_BACK = PAD_DOWN;
constexpr unsigned short PAD_STRAFE_L = PAD_L2;
constexpr unsigned short PAD_STRAFE_R = PAD_R2;
constexpr unsigned short PAD_TURN_L = PAD_LEFT;
constexpr unsigned short PAD_TURN_R = PAD_RIGHT;
constexpr unsigned short PAD_LOOK_UP = PAD_R1;
constexpr unsigned short PAD_LOOK_DOWN = PAD_L1;

}  // namespace game
