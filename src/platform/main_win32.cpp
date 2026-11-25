// Windows frontend: window + D3D11 presentation, XAudio2 sound, keyboard/XInput pad.
// Also supports a headless test mode used during development:
//   psxvr.exe --frames N [--shot file.bmp] [--vram file.bmp] [--input "frame:buttons,..."]
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <xaudio2.h>
#include <xinput.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <array>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "../core/psx.h"
#include "../core/capture.h"
#include "../core/state.h"
#include "../vr/scene.h"
#include "../game/game.h"
#include "../vr/vr_hands.h"
namespace vrhands { bool preview_bmp(ID3D11Device *, ID3D11DeviceContext *, const char *, float, float); struct PistolModel; extern const PistolModel *g_preview_pistol; }

using namespace psx;

namespace psx {
bool cdrom_read_file(const std::string &path, std::vector<u8> &out);
bool hw_take_vblank();
extern u64 g_frame_end;
extern std::map<u32, u32> g_pc_hist;
extern bool g_pc_sample;
extern std::map<u32, u32> g_call_hist;
extern bool g_call_log;
static int g_calls_from = -1, g_calls_to = -1;
// --trace-pc addr[,addr...]: print a0..a3 / ra each time the game executes one of these instructions (first 200)
static void trace_pc_hook() {
    static int n = 0;
    if (++n > 20000) return;
    printf("trace pc %08x: a0 %08x a1 %08x a2 %08x a3 %08x ra %08x\n", M.cpu.pc, M.cpu.r[4], M.cpu.r[5], M.cpu.r[6],
           M.cpu.r[7], M.cpu.r[31]);
}  // --calls from,to: log call targets in that frame range
extern bool g_log_bios;
}

// PsyQ PadRead bit layout (pressed = 1)
enum : u16 {
    PAD_L2 = 1 << 0, PAD_R2 = 1 << 1, PAD_L1 = 1 << 2, PAD_R1 = 1 << 3,
    PAD_TRI = 1 << 4, PAD_CIR = 1 << 5, PAD_X = 1 << 6, PAD_SQR = 1 << 7,
    PAD_SEL = 1 << 8, PAD_START = 1 << 11, PAD_UP = 1 << 12, PAD_RIGHT = 1 << 13, PAD_DOWN = 1 << 14,
    PAD_LEFT = 1 << 15,
};

static std::string exe_dir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string s = buf;
    return s.substr(0, s.find_last_of("\\/") + 1);
}

std::string g_boot_name;  // the disc's program (SYSTEM.CNF BOOT): the game module picks its addresses by it
static std::string find_cue() {
    // a .cue image in the working folder, next to the program or up to two folders above it; with more than one, the
    // one of this build's region first (PAL: "Europe", NTSC: "USA" in its name)
#ifdef PSX_PAL
    const char *region = "Europe";
#else
    const char *region = "USA";
#endif
    std::vector<std::string> dirs = {"", exe_dir(), exe_dir() + "..\\", exe_dir() + "..\\..\\"};
    for (auto &d : dirs) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((d + "*.cue").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        std::string first = d + fd.cFileName, pick;
        do {
            if (strstr(fd.cFileName, region)) { pick = d + fd.cFileName; break; }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        return pick.empty() ? first : pick;
    }
    return "";
}

static std::string g_save_state, g_load_state;  // --save-state file@frame, --load-state file (development)
static int g_save_state_frame = -1;
static int g_port_types[2] = {PAD_DUALSHOCK, PAD_NONE};  // --port1 / --port2

static bool boot(const std::string &cue) {
    M.ram = new u8[RAM_SIZE]();
    if (!cdrom_open(cue)) {
        MessageBoxA(nullptr, ("Cannot open disc image: " + cue).c_str(), GAME_TITLE, MB_OK);
        return false;
    }
    std::vector<u8> sys, exe;
    std::string boot_name = "PSX.EXE";  // replaced by SYSTEM.CNF
    if (cdrom_read_file("SYSTEM.CNF", sys)) {
        std::string s(sys.begin(), sys.end());
        size_t p = s.find("cdrom:");
        if (p != std::string::npos) {
            size_t a = s.find_first_not_of("\\", p + 6);
            size_t b = s.find_first_of(";\r\n", a);
            boot_name = s.substr(a, b - a);
        }
    }
#ifdef GAME_DISC
    // (releases) the game module is made for one version of the game (its addresses): any other disc is refused
    if (boot_name.find(GAME_DISC) == std::string::npos) {
        MessageBoxA(nullptr, ("This disc (" + boot_name + ") is not the version this program is made for: Resident Evil "
                              "Survivor, American release (" GAME_DISC ").").c_str(),
                    GAME_TITLE, MB_OK);
        return false;
    }
#endif
    if (!cdrom_read_file(boot_name, exe) || exe.size() < 0x800) return false;
    u32 pc, gp, taddr, tsize, sp, spoff;
    memcpy(&pc, &exe[0x10], 4);
    memcpy(&gp, &exe[0x14], 4);
    memcpy(&taddr, &exe[0x18], 4);
    memcpy(&tsize, &exe[0x1c], 4);
    memcpy(&sp, &exe[0x30], 4);
    memcpy(&spoff, &exe[0x34], 4);
    memcpy(ram_ptr(taddr), &exe[0x800], std::min<size_t>(tsize, exe.size() - 0x800));
    gpu_init();
    spu_init();
    mdec_init();
    cdrom_init();
    pad_init();
    g_pads[0].type = g_port_types[0];
    g_pads[1].type = g_port_types[1];
    bios_init();
    hw_init();
    cpu_reset(pc, gp, sp ? sp + spoff : 0x801ffff0);
    logf("boot %s pc=%08x", boot_name.c_str(), pc);
    g_boot_name = boot_name;
    return true;
}

// ------------------------------------------------------------------ image output
static void display_to_rgba(std::vector<u32> &px, int &w, int &h) {
    DisplayInfo d = gpu_display();
    w = d.w;
    h = d.h;
    px.assign((size_t)w * h, 0xff000000);
    if (!d.enabled) return;
    const u16 *vr = gpu_vram();
    for (int y = 0; y < h; y++) {
        int vy = (d.y + y) & 511;
        if (d.rgb24) {
            const u8 *row = (const u8 *)(vr + vy * 1024);
            for (int x = 0; x < w; x++) {
                size_t o = (size_t)(d.x * 2 + x * 3) % 2048;
                u8 r = row[o], g = row[(o + 1) % 2048], b = row[(o + 2) % 2048];
                px[(size_t)y * w + x] = 0xff000000 | r | (g << 8) | (b << 16);
            }
        } else {
            for (int x = 0; x < w; x++) {
                u16 p = vr[vy * 1024 + ((d.x + x) & 1023)];
                u32 r = (p & 31) << 3, g = ((p >> 5) & 31) << 3, b = ((p >> 10) & 31) << 3;
                px[(size_t)y * w + x] = 0xff000000 | r | (g << 8) | (b << 16);
            }
        }
    }
}

static void save_bmp(const char *path, const std::vector<u32> &px, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int row = w * 3, pad = (4 - row % 4) % 4;
    u32 size = 54 + (row + pad) * h;
    u8 hdr[54] = {'B', 'M'};
    memcpy(hdr + 2, &size, 4);
    u32 off = 54, ihs = 40, planes_bpp = 1 | (24 << 16);
    memcpy(hdr + 10, &off, 4);
    memcpy(hdr + 14, &ihs, 4);
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    memcpy(hdr + 26, &planes_bpp, 4);
    fwrite(hdr, 1, 54, f);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            u32 p = px[(size_t)y * w + x];
            u8 bgr[3] = {(u8)(p >> 16), (u8)(p >> 8), (u8)p};
            fwrite(bgr, 1, 3, f);
        }
        u8 z[3] = {0, 0, 0};
        fwrite(z, 1, pad, f);
    }
    fclose(f);
}

static void save_vram_bmp(const char *path) {
    std::vector<u32> px(1024 * 512);
    const u16 *vr = gpu_vram();
    for (int i = 0; i < 1024 * 512; i++) {
        u16 p = vr[i];
        px[i] = ((p & 31) << 3) | ((((p >> 5) & 31) << 3) << 8) | ((((p >> 10) & 31) << 3) << 16);
    }
    save_bmp(path, px, 1024, 512);
}

// ------------------------------------------------------------------ headless mode
// GunCon aim in input scripts: "g160/120" = the gun points at that pixel of the displayed image, "goff" = off screen
struct GunAim { bool set = false, on = false; float x = 0, y = 0; };
static std::map<int, GunAim> g_script_gun;
static int g_script_port = 0;  // --script-port 2: the scripted buttons / aim go to controller port 2

static std::map<int, u16> parse_input_script(const std::string &s) {
    // "120:start,300:x,301:" -> buttons held from that frame on (empty = release)
    std::map<int, u16> m;
    static const std::map<std::string, u16> names = {
        {"start", PAD_START}, {"select", PAD_SEL}, {"x", PAD_X}, {"o", PAD_CIR}, {"tri", PAD_TRI},
        {"sqr", PAD_SQR}, {"up", PAD_UP}, {"down", PAD_DOWN}, {"left", PAD_LEFT}, {"right", PAD_RIGHT},
        {"l1", PAD_L1}, {"l2", PAD_L2}, {"r1", PAD_R1}, {"r2", PAD_R2}};
    size_t i = 0;
    while (i < s.size()) {
        size_t c = s.find(',', i);
        std::string item = s.substr(i, c == std::string::npos ? std::string::npos : c - i);
        size_t colon = item.find(':');
        if (colon != std::string::npos) {
            int fr = atoi(item.substr(0, colon).c_str());
            u16 b = 0;
            std::string btns = item.substr(colon + 1);
            size_t j = 0;
            while (j < btns.size()) {
                size_t pl = btns.find('+', j);
                std::string nm = btns.substr(j, pl == std::string::npos ? std::string::npos : pl - j);
                auto it = names.find(nm);
                if (it != names.end()) b |= it->second;
                else if (nm == "goff") g_script_gun[fr] = {true, false, 0, 0};
                else if (nm.size() > 1 && nm[0] == 'g') {
                    GunAim g{true, true, 0, 0};
                    if (sscanf(nm.c_str() + 1, "%f/%f", &g.x, &g.y) == 2) g_script_gun[fr] = g;
                }
                if (pl == std::string::npos) break;
                j = pl + 1;
            }
            m[fr] = b;
        }
        if (c == std::string::npos) break;
        i = c + 1;
    }
    return m;
}

namespace psx { void spu_dump_voices(); extern bool g_log_cd; extern const char *g_log_name; }
void spu_dump_voices_pub() { psx::spu_dump_voices(); }
static FILE *g_wav = nullptr;
static int g_rwatch_frame = -1;  // --rwatch lo-hi@frame
static unsigned g_rwatch_range[2];
static const char *g_preview = nullptr;
static float g_mc_view[2];
static bool g_mc_view_on = false;
static float g_view_abs[2];
static float g_gun_dy = 0;
static bool g_gun_exact = false;
static bool g_view_abs_on = false;
namespace psx { extern u32 g_dbg_derived[4]; }
static std::vector<int> g_ramdump_frames;
struct Poke { u32 addr; u16 val; int frame; bool once; };
// --poke addr=value@frame (hex addr; value decimal or 0x-hex), applied every frame from then on, or only on that
// frame with a trailing ! (e.g. links in object lists, which the game itself keeps changing)
static std::vector<Poke> g_pokes;

// Render the last captured frame in 3D from several cameras (development check of the VR renderer)
static void preview_3d(const char *prefix) {
    ID3D11Device *d = nullptr;
    ID3D11DeviceContext *c = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d,
                                 nullptr, &c)))
        return;
    if (!scene::init(d, c)) { printf("scene init failed\n"); return; }
    printf("derived seen %u, not-world %u, frame-mismatch %u (delta %d)\n", psx::g_dbg_derived[0], psx::g_dbg_derived[1], psx::g_dbg_derived[2], (int)psx::g_dbg_derived[3]);
    auto tu0 = std::chrono::steady_clock::now();
    bool is3d = false;
    for (int k = 0; k < 20; k++) is3d = scene::update(g_cap_done, gpu_vram());
    printf("scene update: %.2f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tu0).count() / 20);
    {
        char hn[512];
        snprintf(hn, sizeof hn, "%s_hud.bmp", prefix);
        scene::hud_bmp(hn);
    }
    if (getenv("PSXVR_AIMTEST")) {  // pixel -> ray -> world point -> the game's camera -> pixel (must match)
        float h = (float)(g_gte_h_override ? g_gte_h_override : g_cap_done.gte_h);
        const float px_test[5][2] = {{160, 120}, {60, 60}, {260, 60}, {80, 170}, {250, 160}};
        for (auto &pt : px_test) {
            float d[3] = {(pt[0] - 160) / h, -(pt[1] - 120) / h, -1}, o[3] = {0, 0, 0}, t = 0;
            float l = sqrtf(d[0] * d[0] + d[1] * d[1] + 1);
            for (float &c : d) c /= l;
            if (!scene::raycast(o, d, t)) { printf("aimtest %.0f,%.0f: no hit\n", pt[0], pt[1]); continue; }
            float sp[3] = {d[0] * t, d[1] * t, d[2] * t}, w[3], qx = 0, qy = 0;
            game::scene_to_world(g_cap_done, sp, w);
            bool ok = game::project_world(w, qx, qy);
            printf("aimtest %.0f,%.0f -> %.2f m -> world %.0f %.0f %.0f -> %d %.1f,%.1f\n", pt[0], pt[1], t, w[0], w[1], w[2], (int)ok, qx, qy);
        }
    }
    auto &stt = scene::stats();
    printf("preview: 3d=%d world=%d sprites=%d (anchored %d, fitted %d) hud=%d dropped=%d\n", is3d, stt.world_prims, stt.sprite_prims, stt.anchored, stt.fitted,
           stt.hud_prims, stt.dropped);
    printf("decals: %d\n", stt.decals);
    printf("picture feedback: %d\n", (int)game::picture_feedback());
    struct Cam { const char *name; Mat4 pose; float fov; } cams[] = {
        {"game", mat_identity(), 64.0f},
        {"side", mat_mul(mat_translate(1.2f, 0.4f, 0.5f), mat_rot_y(0.6f)), 90.0f},
        {"low", mat_mul(mat_translate(0.6f, -1.2f, -0.5f), mat_rot_y(0.25f)), 90.0f},
        {"top", mat_mul(mat_translate(0.0f, 6.0f, -4.0f), mat_rot_x(-1.45f)), 90.0f},
        {"wide", mat_identity(), 110.0f},
        {"head", mat_mul(mat_rot_y(-g_cap_done.cam_yaw * (6.2831853f / 65536.0f)), mat_rot_x(g_cap_done.cam_pitch * (6.2831853f / 65536.0f))), 100.0f},
    };
    // development: PSXVR_PREVIEW_ZOOM=x,y,z,yaw_deg,pitch_deg,fov_deg: one more camera ("zoom", metres in the game
    // camera's space) to look closely at something
    if (const char *z = getenv("PSXVR_PREVIEW_ZOOM")) {
        float px = 0, py = 0, pz = 0, yw = 0, pt = 0, fv = 20;
        sscanf(z, "%f,%f,%f,%f,%f,%f", &px, &py, &pz, &yw, &pt, &fv);
        char nm[512];
        snprintf(nm, sizeof nm, "%s_zoom.bmp", prefix);
        float fy = g_cap_done.cam_yaw * (6.2831853f / 65536.0f), fp = g_cap_done.cam_pitch * (6.2831853f / 65536.0f);
        scene::preview_bmp(nm, 640, 480, mat_mul(mat_translate(px, py, pz), mat_mul(mat_rot_y(yw * 0.0174533f), mat_rot_x(pt * 0.0174533f))), fv,
                           mat_mul(mat_rot_y(-fy), mat_rot_x(fp)));
    }
    for (auto &cm : cams) {
        char nm[512];
        snprintf(nm, sizeof nm, "%s_%s.bmp", prefix, cm.name);
        float fy = g_cap_done.cam_yaw * (6.2831853f / 65536.0f), fp = g_cap_done.cam_pitch * (6.2831853f / 65536.0f);
        scene::preview_bmp(nm, 640, 480, cm.pose, cm.fov, mat_mul(mat_rot_y(-fy), mat_rot_x(fp)));
    }
}

static const char *g_capdump = nullptr;

// development: --model-dump tpage,clut,file writes the captured primitives with that texture page / CLUT (camera space
// points, UVs, colours) and their texture decoded from VRAM (RGBA, 0 = transparent), to turn a game model into a VR one
static const char *g_model_dump = nullptr;
static void dump_model(const char *spec) {
    unsigned tp = 0, cl = 0;
    char path[400] = {0};
    if (sscanf(spec, "%x,%x,%399s", &tp, &cl, path) != 3) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    const CapFrame &fr = g_cap_done;
    const u16 *vr = gpu_vram();
    int umin = 255, vmin = 255, umax = 0, vmax = 0, n = 0;
    for (const CapPrim &p : fr.prims)
        if ((p.tpage & 0x1ff) == tp && p.clut == cl)
            for (int k = 0; k < p.nv; k++)
                umin = std::min<int>(umin, p.v[k].u), umax = std::max<int>(umax, p.v[k].u),
                vmin = std::min<int>(vmin, p.v[k].v), vmax = std::max<int>(vmax, p.v[k].v);
    int tx = (tp & 0xf) * 64, ty = ((tp >> 4) & 1) * 256, depth = (tp >> 7) & 3;
    int cx = (cl & 0x3f) * 16, cy = (cl >> 6) & 0x1ff;
    int w = umax - umin + 1, h = vmax - vmin + 1;
    fprintf(f, "tex %d %d %d %d\n", umin, vmin, w, h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int u = umin + x, v = vmin + y;
            u16 c;
            if (depth == 0) {
                u16 wd = vr[(ty + v) * 1024 + tx + u / 4];
                c = vr[cy * 1024 + cx + ((wd >> ((u & 3) * 4)) & 0xf)];
            } else if (depth == 1) {
                u16 wd = vr[(ty + v) * 1024 + tx + u / 2];
                c = vr[cy * 1024 + cx + ((wd >> ((u & 1) * 8)) & 0xff)];
            } else {
                c = vr[(ty + v) * 1024 + tx + u];
            }
            unsigned r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3, a = c ? 255 : 0;
            fprintf(f, "%02x%02x%02x%02x", r, g, b, a);
        }
        fprintf(f, "\n");
    }
    for (const CapPrim &p : fr.prims) {
        if ((p.tpage & 0x1ff) != tp || p.clut != cl) continue;
        fprintf(f, "prim %d", p.nv);
        for (int k = 0; k < p.nv; k++) {
            const CapVert &v = p.v[k];
            fprintf(f, " %d %.1f %.1f %.1f %d %d %d %d %d", (int)v.has3d, v.x, v.y, v.z, v.u, v.v, v.r, v.g, v.b);
        }
        fprintf(f, "\n");
        n++;
    }
    fclose(f);
    printf("model dump: %d primitives, texture %dx%d\n", n, w, h);
}

static void dump_capture(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    const CapFrame &fr = g_cap_done;
    fprintf(f, "frame %llu y_shift %d prims %zu ofx %f ofy %f h %d disp %d,%d %dx%d eyez %d\n", (unsigned long long)fr.number,
            fr.y_shift, fr.prims.size(), fr.gte_ofx / 65536.0, fr.gte_ofy / 65536.0, fr.gte_h, fr.disp_x, fr.disp_y, fr.disp_w,
            fr.disp_h, fr.game_cam[4]);
    fprintf(f, "game_cam %d %d %d %d valid %d yaw16 %d pitch16 %d feet_y %d eye_off %08x\n", fr.game_cam[0], fr.game_cam[1],
            fr.game_cam[2], fr.game_cam[3], fr.game_cam_valid, fr.game_cam[5], fr.cam_pitch, fr.game_cam[6],
            (unsigned)fr.game_cam[7]);
    if (!fr.game_data.empty()) {
        std::string bp = std::string(path) + ".map";
        if (FILE *b = fopen(bp.c_str(), "wb")) { fwrite(fr.game_data.data(), 1, fr.game_data.size(), b); fclose(b); }
    }
    for (size_t i = 0; i < fr.prims.size(); i++) {
        const CapPrim &p = fr.prims[i];
        fprintf(f, "%4zu k%d node %08x tex%d g%d semi%d/%d tp %03x clut %04x tw %05x off %d,%d uv", i, p.kind, p.node,
                p.textured, p.gouraud, p.semi, p.blend, p.tpage, p.clut, p.tw, p.off_x, p.off_y);
        for (int k = 0; k < p.nv; k++) fprintf(f, " %d,%d", p.v[k].u, p.v[k].v);
        fprintf(f, " |");
        for (int k = 0; k < p.nv; k++) {
            const CapVert &v = p.v[k];
            if (v.has3d) fprintf(f, " (%d,%d)[%.0f %.0f %.0f @%x c%d]", v.sx, v.sy, v.x, v.y, v.z, v.src_pc, v.r);
            else if (v.derived) fprintf(f, " (%d,%d)<%.0f %.0f %.0f @%x>", v.sx, v.sy, v.ax, v.ay, v.az, v.src_pc);
            else if (v.hud_reproj) fprintf(f, " (%d,%d)H[%.0f %.0f %.0f @%x]", v.sx, v.sy, v.x, v.y, v.z, v.src_pc);
            else fprintf(f, " (%d,%d){%x}", v.sx, v.sy, v.addr);
        }
        fprintf(f, "\n");
    }
    fclose(f);
}


static int run_headless(int frames, const char *shot, const char *vram_shot, const std::string &input,
                        int shot_every) {
    auto script = parse_input_script(input);
    auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames && !M.quit; f++) {
        static u16 script_pad = 0;
        auto it = script.find(f);
        if (it != script.end()) {
            script_pad = it->second;
            if (g_script_port == 1) g_pads[1].buttons = script_pad;
            else g_pad_buttons = script_pad;
        }
        auto gi = g_script_gun.find(f);
        if (gi != g_script_gun.end()) {
            PadPort &gp = g_pads[g_script_port];
            gp.gun_onscreen = gi->second.on;
            gp.gun_x = gi->second.x;
            gp.gun_y = gi->second.y;
            gp.gun_raw = false;
        }
        for (auto &pk : g_pokes)
            if (pk.once ? f == pk.frame : f >= pk.frame) write16(pk.addr, pk.val);
        if (g_rwatch_frame >= 0 && f == g_rwatch_frame) { extern u32 g_rwatch_pub(u32, u32); g_rwatch_pub(g_rwatch_range[0], g_rwatch_range[1]); }
        g_call_log = f >= g_calls_from && f < g_calls_to;
        // development: head steering test. PSXVR_STEER_TEST=frame,head_deg[,x|tri]: from that frame on the head looks
        // head_deg right of the carpet's initial heading, the forward button is held and the carpet is turned towards
        // the head like the VR frontend does; the heading and position are printed.
        static const char *steer = getenv("PSXVR_STEER_TEST");
        if (steer) {
            static int sf = -1;
            static float target = 0, base = 0;
            static u16 fwd = PAD_TRI;
            if (sf < 0) {
                char b[8] = {};
                sscanf(steer, "%d,%f,%7s", &sf, &target, b);
                target *= 0.0174533f;
                if (!strcmp(b, "x")) fwd = PAD_X;
            }
            if (f == sf) base = game::game_heading();
            if (f >= sf) {
                float carpet = game::game_heading(), rel = base + target - carpet;
                while (rel > 3.14159265f) rel -= 6.2831853f;
                while (rel < -3.14159265f) rel += 6.2831853f;
                game::set_view(rel, 0);
                u16 pad = script_pad | fwd;
                pad |= game::steer_toward(rel);
                g_pad_buttons = pad;
                if ((f - sf) % 10 == 0) {
                    u32 base_ptr = read32(0x80098a60);
                    u32 cam = base_ptr + read16(base_ptr + 10168) * 14 + 10742;
                    printf("steer f %d carpet %.1f deg rel %.1f pos %d %d\n", f, carpet * 57.2958f, rel * 57.2958f,
                           (s16)read16(cam), (s16)read16(cam + 2));
                }
            }
        }
        // development: PSXVR_BODY_TEST=degrees keeps the player's heading there like the VR frontend does, and realigns
        // (prints) when the game itself places / turns the player
        // development: PSXVR_AUTOCAL=1 does the VR frontend's automatic GunCon calibration (exact shot at the cross,
        // then A + B) whenever the ADJUST GUN screen is up
        static const bool autocal = getenv("PSXVR_AUTOCAL") != nullptr;
        if (autocal) {
            static int cal_t = -1;
            if (game::calibration_screen()) {
                cal_t = (cal_t + 1) % 150;
                g_pads[0].gun_onscreen = true;
                g_pads[0].gun_x = game::GUN_CROSS_X;
                g_pads[0].gun_y = game::GUN_CROSS_Y;
                g_pads[0].buttons = (cal_t >= 20 && cal_t < 26) ? PAD_CIR : (cal_t >= 60 && cal_t < 66) ? (u16)(PAD_START | PAD_X) : 0;
                if (cal_t == 0) printf("frame %d: calibration screen, calibrating\n", f);
            } else if (cal_t >= 0) {
                cal_t = -1;
                g_pads[0].buttons = 0;
                printf("frame %d: calibration done, offsets %d %d\n", f, (s16)read16(0x80080710), (s16)read16(0x80080712));
            }
        }
        // development: PSXVR_VIEW_RAMP=deg_per_frame turns the absolute view by that much each emulated frame, as a head
        // turning (checks that each captured frame is stamped with the view it was drawn with)
        static const char *ramp = getenv("PSXVR_VIEW_RAMP");
        // PSXVR_VIEW_JITTER=deg: a head that is never quite still (small irregular yaw / pitch wobble)
        static const char *jit = getenv("PSXVR_VIEW_JITTER");
        if (jit && g_view_abs_on) {
            float j = (float)atof(jit) * 0.0174533f;
            game::set_view_abs(g_view_abs[0] + j * (sinf(f * 0.9f) + 0.5f * sinf(f * 2.3f)) / 1.5f,
                               g_view_abs[1] + j * (sinf(f * 1.3f + 1) + 0.5f * sinf(f * 3.1f)) / 1.5f);
        }
        if (ramp && g_view_abs_on) game::set_view_abs(g_view_abs[0] + f * (float)atof(ramp) * 0.0174533f, g_view_abs[1]);
        // development: PSXVR_BODY_RAMP: the body turns with the ramped view, as in VR (the body faces the gaze)
        if (ramp && g_view_abs_on && getenv("PSXVR_BODY_RAMP")) {
            float y = fmodf(g_view_abs[0] + f * (float)atof(ramp) * 0.0174533f, 6.2831853f);
            game::set_body_heading(true, y < 0 ? y + 6.2831853f : y);
        }
        if (g_gun_exact) {
            static bool once = (game::set_exact_aim(true), true);
            (void)once;
            PadPort &gp = g_pads[g_script_port];
            if (gp.type == PAD_GUNCON && !game::calibration_screen()) {
                game::gun_raw_for(gp.gun_x, gp.gun_y, gp.gun_raw_x, gp.gun_raw_y);
                gp.gun_raw = true;
            }
        }
        static const char *body_test = getenv("PSXVR_BODY_TEST");
        static float body_deg = body_test ? (float)atof(body_test) : 0;
        if (body_test) game::set_body_heading(true, body_deg * 0.0174533f);
        // development: PSXVR_INJECT=hex (held action bits added after the game's button mapping), PSXVR_POSLOG prints the
        // player's position each frame
        static const char *inj = getenv("PSXVR_INJECT");
        if (inj) game::set_actions((unsigned short)strtoul(inj, nullptr, 16));
        // development: PSXVR_FOLLOW=dx,dz (game units from the eye at the start) walks the player there as the VR room
        // scale does (step while more than 60 units away, decided with the eye's position now)
        if (const char *fo = getenv("PSXVR_FOLLOW")) {
            static float tx = 0, tz = 0;
            static bool init = false;
            float ex, ez;
            if (game::eye_xz(ex, ez)) {
                if (!init) { float ddx = 0, ddz = 0; sscanf(fo, "%f,%f", &ddx, &ddz); tx = ex + ddx, tz = ez + ddz; init = true; }
                float wx = tx - ex, wz = tz - ez;
                bool on = wx * wx + wz * wz > 60.0f * 60.0f;
                game::set_walk(on);
                float y = atan2f(-wx, -wz);
                game::set_walk_direction(on, y < 0 ? y + 6.2831853f : y);
            }
        }
        // development: PSXVR_SHOWLOG prints each new captured frame: primitives with depth (>= 30 = a 3D view), all, the
        // game's control flag and render count (what the VR frontend decides 3D / virtual screen from)
        if (getenv("PSXVR_SHOWLOG")) {
            static u64 seen = 0;
            if (g_cap_frames != seen) {
                seen = g_cap_frames;
                int n3 = 0;
                for (const CapPrim &p : g_cap_done.prims) {
                    int k = 0;
                    for (int i = 0; i < p.nv; i++) k += p.v[i].has3d;
                    n3 += k == p.nv && p.kind != CapPrim::LINE;
                }
                printf("show f %d 3d %d prims %zu ctl %d render %llu valid %d picture %d\n", f, n3, g_cap_done.prims.size(),
                       (int)game::in_control(), (unsigned long long)game::render_count(), (int)g_cap_done.game_cam_valid,
                       (int)game::full_picture(g_cap_done));
            }
        }
        // development: PSXVR_CPU_FAST_AT=frame[,cycles]: from that frame on the CPU runs with that many cycles per
        // instruction (1 = twice as fast), as the VR frontend switches it while playing
        if (const char *fa = getenv("PSXVR_CPU_FAST_AT")) {
            int at = 0, cyc = 1;
            sscanf(fa, "%d,%d", &at, &cyc);
            if (f == at) g_cpu_instr_cycles = (u32)std::max(1, cyc);
        }
        if (getenv("PSXVR_MOVIELOG")) printf("movie %d\n", (int)game::movie_frame(g_cap_done, gpu_vram()));
        if (getenv("PSXVR_WALK") && *getenv("PSXVR_WALK")) game::set_walk(true, atoi(getenv("PSXVR_WALK")) == 2, atoi(getenv("PSXVR_WALK")) == 3);
        if (getenv("PSXVR_MOVE_DIR")) game::set_walk_direction(true, (float)atof(getenv("PSXVR_MOVE_DIR")) * 0.0174533f);
        if (getenv("PSXVR_POSLOG"))
            printf("pos %d %d %d\n", game::player_pos(0), game::player_pos(1), game::player_pos(2));
        cpu_run_frame();
        if (getenv("PSXVR_FRAMELOG")) logf("frame %d", f);
        if (const char *ta = getenv("PSXVR_TRACE_AT")) g_trace = f + 1 >= atoi(ta);  // development: trace from that frame  // development: the last frame begun, if the emulator hangs
        {  // a crash (the game ran into data): the frame it happened in
            static bool told = false;
            if (g_illegal_count && !told) told = true, logf("crash: illegal instruction in frame %d (pc %08x)", f, M.cpu.pc);
        }
        if (body_test && game::take_placement()) {
            printf("frame %d: placement, game heading %.0f deg (was keeping %.0f)\n", f, game::player_heading() * 57.2958f, body_deg);
            body_deg = game::player_heading() * 57.2958f;
        }
        if (body_test && f % 50 == 0) printf("frame %d: heading %.0f control %d\n", f, game::player_heading() * 57.2958f, (int)game::in_control());
        static const int snap_at = getenv("PSXVR_SNAP_AT") ? atoi(getenv("PSXVR_SNAP_AT")) : -1;  // test of the VR snapshot
        // test of the VR options' "load state": PSXVR_LOAD_AT=frame:path loads that save while running
        static const char *load_at = getenv("PSXVR_LOAD_AT");
        if (load_at && f == atoi(load_at)) {
            const char *path = strchr(load_at, ':') + 1;
            bool ok = state_load(path);
            game::on_state_loaded();
            printf("load at %d %s: %d\n", f, path, (int)ok);
        }
        if (f == snap_at) { extern std::string frontend_snapshot(const std::string &); frontend_snapshot("headless test"); }
        if (f == g_save_state_frame) printf("save state %s: %d\n", g_save_state.c_str(), (int)state_save(g_save_state.c_str()));
        if (f + 1 == g_calls_to) {
            FILE *cf = fopen("calls.txt", "w");
            for (auto &c : g_call_hist) fprintf(cf, "%08x %u\n", c.first, c.second);
            fclose(cf);
        }
        for (int rf : g_ramdump_frames)
            if (rf == f) {
                char nm[256];
                snprintf(nm, sizeof nm, "ram_%05d.bin", f);
                FILE *rfp = fopen(nm, "wb");
                if (rfp) { fwrite(M.ram, 1, RAM_SIZE, rfp); fclose(rfp); }
            }
        s16 tmp[2048 * 2];
        int got;
        while ((got = spu_fetch(tmp, 2048)) > 0) {
            if (g_wav) fwrite(tmp, 4, got, g_wav);
            if (got < 2048) break;
        }
        if (shot_every > 0 && f % shot_every == 0 && shot) {
            std::vector<u32> px;
            int w, h;
            display_to_rgba(px, w, h);
            char nm[512];
            snprintf(nm, sizeof nm, "%s_%05d.bmp", shot, f);
            save_bmp(nm, px, w, h);
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    printf("ran %d frames in %.2fs (%.1f fps), pc=%08x\n", frames, secs, frames / secs, M.cpu.pc);
    if (shot && shot_every <= 0) {
        std::vector<u32> px;
        int w, h;
        display_to_rgba(px, w, h);
        save_bmp(shot, px, w, h);
    }
    if (vram_shot) save_vram_bmp(vram_shot);
    if (const char *gp = getenv("PSXVR_GRID_PREVIEW")) { extern void save_grid_preview(const char *); save_grid_preview(gp); }
    if (const char *raw = getenv("PSXVR_VRAM_RAW")) {  // development: the VRAM as it is (1024 x 512 16-bit words)
        if (FILE *f = fopen(raw, "wb")) { fwrite(gpu_vram(), 2, 1024 * 512, f); fclose(f); }
    }
    if (g_capdump) dump_capture(g_capdump);
    if (g_model_dump) dump_model(g_model_dump);
    if (g_preview) preview_3d(g_preview);
    extern void spu_dump_voices_pub();
    spu_dump_voices_pub();
    if (g_pc_sample) {
        std::vector<std::pair<u32, u32>> v(g_pc_hist.begin(), g_pc_hist.end());
        std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second > b.second; });
        for (size_t i = 0; i < v.size() && i < 25; i++) printf("  %08x %u\n", v[i].first, v[i].second);
    }
    return 0;
}

// ------------------------------------------------------------------ D3D11 presenter
static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static IDXGISwapChain *swap;
static ID3D11RenderTargetView *rtv;
static ID3D11Texture2D *frame_tex;
static ID3D11ShaderResourceView *frame_srv;
static ID3D11VertexShader *vs;
static ID3D11PixelShader *ps;
static ID3D11SamplerState *sampler;
static int tex_w = 0, tex_h = 0;
static HWND hwnd;

static const char *shader_src = R"(
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
float4 ps_main(VSOut i) : SV_Target { return tex.Sample(smp, i.uv); }
)";

static bool d3d_init(HWND h) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = h;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                             D3D11_SDK_VERSION, &sd, &swap, &dev, nullptr, &ctx)))
        return false;
    ID3D11Texture2D *bb;
    swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
    dev->CreateRenderTargetView(bb, nullptr, &rtv);
    bb->Release();
    ID3DBlob *vsb, *psb, *err;
    if (FAILED(D3DCompile(shader_src, strlen(shader_src), nullptr, nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vsb,
                          &err)))
        return false;
    if (FAILED(D3DCompile(shader_src, strlen(shader_src), nullptr, nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &psb,
                          &err)))
        return false;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
    D3D11_SAMPLER_DESC smp = {};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    dev->CreateSamplerState(&smp, &sampler);
    return true;
}

static void d3d_resize() {
    if (!swap) return;
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    rtv->Release();
    swap->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    ID3D11Texture2D *bb;
    swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
    dev->CreateRenderTargetView(bb, nullptr, &rtv);
    bb->Release();
}

static void d3d_present_frame() {
    std::vector<u32> px;
    int w, h;
    display_to_rgba(px, w, h);
    if (w != tex_w || h != tex_h) {
        if (frame_srv) frame_srv->Release();
        if (frame_tex) frame_tex->Release();
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        dev->CreateTexture2D(&td, nullptr, &frame_tex);
        dev->CreateShaderResourceView(frame_tex, nullptr, &frame_srv);
        tex_w = w;
        tex_h = h;
    }
    ctx->UpdateSubresource(frame_tex, 0, nullptr, px.data(), w * 4, 0);
    RECT rc;
    GetClientRect(hwnd, &rc);
    float cw = (float)(rc.right - rc.left), ch = (float)(rc.bottom - rc.top);
    // 4:3 letterbox
    float vw = cw, vh = cw * 3.0f / 4.0f;
    if (vh > ch) { vh = ch; vw = ch * 4.0f / 3.0f; }
    D3D11_VIEWPORT vp = {(cw - vw) / 2, (ch - vh) / 2, vw, vh, 0, 1};
    float black[4] = {0, 0, 0, 1};
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->ClearRenderTargetView(rtv, black);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &frame_srv);
    ctx->PSSetSamplers(0, 1, &sampler);
    ctx->Draw(3, 0);
    swap->Present(0, 0);
}

// ------------------------------------------------------------------ audio
// Audio output device: --audio "<name part>" picks a device; in VR mode we prefer the headset.
std::string g_audio_filter;
bool g_audio_prefer_vr = false;

static std::wstring pick_audio_device() {
    IMMDeviceEnumerator *en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                (void **)&en)))
        return L"";
    auto name_of = [](IMMDevice *d) {
        std::wstring name;
        IPropertyStore *ps;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.pwszVal) name = v.pwszVal;
            PropVariantClear(&v);
            ps->Release();
        }
        return name;
    };
    IMMDevice *def = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def))) {
        logf("audio: default device: %ls", name_of(def).c_str());
        def->Release();
    }
    std::wstring chosen, chosen_name;
    IMMDeviceCollection *col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
        UINT n = 0;
        col->GetCount(&n);
        static const wchar_t *vr_words[] = {L"Index", L"Rift", L"Oculus", L"Quest", L"Meta", L"Steam Streaming",
                                            L"Virtual Desktop", L"Pico", L"Vive", L"HMD", L"Reverb", L"Bigscreen",
                                            L"Pimax", L"VR"};
        std::wstring filt(g_audio_filter.begin(), g_audio_filter.end());
        for (UINT i = 0; i < n; i++) {
            IMMDevice *d;
            if (FAILED(col->Item(i, &d))) continue;
            LPWSTR id = nullptr;
            d->GetId(&id);
            std::wstring name = name_of(d);
            logf("audio: device %u: %ls", i, name.c_str());
            bool match = false;
            if (!filt.empty()) match = name.find(filt) != std::wstring::npos;
            else if (g_audio_prefer_vr)
                for (auto w : vr_words) match |= name.find(w) != std::wstring::npos;
            if (match && chosen.empty() && id) { chosen = id; chosen_name = name; }
            if (id) CoTaskMemFree(id);
            d->Release();
        }
        col->Release();
    }
    en->Release();
    if (!chosen.empty()) logf("audio: using %ls", chosen_name.c_str());
    else logf("audio: using the Windows default device");
    return chosen;
}

struct AudioOut : IXAudio2VoiceCallback {
    IXAudio2 *xa = nullptr;
    IXAudio2MasteringVoice *master = nullptr;
    IXAudio2SourceVoice *voice = nullptr;
    std::vector<std::vector<s16>> bufs;
    int next = 0;
    bool init() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HRESULT hr = XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(hr)) { logf("audio: XAudio2Create failed %08x", (unsigned)hr); return false; }
        std::wstring dev = pick_audio_device();
        hr = xa->CreateMasteringVoice(&master, 0, 0, 0, dev.empty() ? nullptr : dev.c_str());
        if (FAILED(hr) && !dev.empty()) hr = xa->CreateMasteringVoice(&master);
        if (FAILED(hr)) { logf("audio: CreateMasteringVoice failed %08x", (unsigned)hr); return false; }
        WAVEFORMATEX wf = {};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = 2;
        wf.nSamplesPerSec = 44100;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = 4;
        wf.nAvgBytesPerSec = 44100 * 4;
        if (FAILED(xa->CreateSourceVoice(&voice, &wf, 0, 2.0f, this))) return false;
        bufs.resize(8);
        voice->Start();
        return true;
    }
    void pump() {
        XAUDIO2_VOICE_STATE st;
        voice->GetState(&st);
        while (st.BuffersQueued < 4) {
            auto &b = bufs[next];
            b.resize(735 * 2);
            spu_fetch(b.data(), 735);
            XAUDIO2_BUFFER xb = {};
            xb.AudioBytes = (UINT32)(b.size() * 2);
            xb.pAudioData = (const BYTE *)b.data();
            voice->SubmitSourceBuffer(&xb);
            next = (next + 1) % (int)bufs.size();
            st.BuffersQueued++;
        }
    }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void *) override {}
    void STDMETHODCALLTYPE OnBufferEnd(void *) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void *) override {}
    void STDMETHODCALLTYPE OnVoiceError(void *, HRESULT) override {}
};

// ------------------------------------------------------------------ input
static u16 poll_input() {
    u16 b = 0;
    auto key = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    if (GetForegroundWindow() == hwnd) {
        // Resident Evil Survivor, BUTTON CONFIG A-TYPE: arrows / WASD move and turn, Space or J attack / OK (Cross),
        // Shift or K aim (R1 target), L1 quick search = Q, Esc or I cancel (Square), Enter = Start, Backspace / Tab =
        // Select (options)
        if (key(VK_UP) || key('W')) b |= PAD_UP;
        if (key(VK_DOWN) || key('S')) b |= PAD_DOWN;
        if (key(VK_LEFT) || key('A')) b |= PAD_LEFT;
        if (key(VK_RIGHT) || key('D')) b |= PAD_RIGHT;
        if (key(VK_RETURN)) b |= PAD_START;
        if (key(VK_BACK) || key(VK_TAB)) b |= PAD_SEL;
        if (key(VK_SPACE) || key('J')) b |= PAD_X;
        if (key(VK_SHIFT) || key('K')) b |= PAD_R1;
        if (key(VK_ESCAPE) || key('I')) b |= PAD_SQR;
        if (key('L')) b |= PAD_CIR;
        if (key('O')) b |= PAD_TRI;
        if (key('Q')) b |= PAD_L1;
        if (key('E')) b |= PAD_R1;
        if (key('1')) b |= PAD_L2;
        if (key('3')) b |= PAD_R2;
    }
    XINPUT_STATE xs;
    if (XInputGetState(0, &xs) == ERROR_SUCCESS) {
        WORD w = xs.Gamepad.wButtons;
        if (w & XINPUT_GAMEPAD_DPAD_UP || xs.Gamepad.sThumbLY > 16000) b |= PAD_UP;
        if (w & XINPUT_GAMEPAD_DPAD_DOWN || xs.Gamepad.sThumbLY < -16000) b |= PAD_DOWN;
        if (w & XINPUT_GAMEPAD_DPAD_LEFT || xs.Gamepad.sThumbLX < -16000) b |= PAD_LEFT;
        if (w & XINPUT_GAMEPAD_DPAD_RIGHT || xs.Gamepad.sThumbLX > 16000) b |= PAD_RIGHT;
        if (w & XINPUT_GAMEPAD_START) b |= PAD_START;
        if (w & XINPUT_GAMEPAD_BACK) b |= PAD_SEL;
        if (w & XINPUT_GAMEPAD_A) b |= PAD_X;
        if (w & XINPUT_GAMEPAD_B) b |= PAD_CIR;
        if (w & XINPUT_GAMEPAD_X) b |= PAD_SQR;
        if (w & XINPUT_GAMEPAD_Y) b |= PAD_TRI;
        if (w & XINPUT_GAMEPAD_LEFT_SHOULDER) b |= PAD_L1;
        if (w & XINPUT_GAMEPAD_RIGHT_SHOULDER) b |= PAD_R1;
        if (xs.Gamepad.bLeftTrigger > 100) b |= PAD_L2;
        if (xs.Gamepad.bRightTrigger > 100) b |= PAD_R2;
    }
    return b;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_SIZE: d3d_resize(); return 0;
    default: return DefWindowProcA(h, msg, wp, lp);
    }
}

// ------------------------------------------------------------------ services for the VR frontend
static AudioOut g_audio;
void frontend_get_frame(std::vector<u32> &px, int &w, int &h) { display_to_rgba(px, w, h); }
bool frontend_audio_init() { return g_audio.init(); }
void frontend_audio_pump() { g_audio.pump(); }
u16 frontend_poll_xinput() { return poll_input(); }  // keyboard part is inactive without a focused window

// Snapshot for analysis (VR: left stick click / F5; desktop: F5): the whole machine state (loadable with --load-state),
// the 3D capture of the last frame, a screenshot and the caller's notes, as states/<name>.*  Returns the name.
std::string frontend_snapshot(const std::string &notes) {
    CreateDirectoryA("states", nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char name[64];
    // the American disc's states apart (usa_snap_...): a state only runs with the disc it was made with
    bool usa = g_boot_name.find("SLUS") != std::string::npos;
    snprintf(name, sizeof name, "states/%ssnap_%04d%02d%02d_%02d%02d%02d", usa ? "usa_" : "", t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute, t.wSecond);
    std::string base = name;
    bool ok = state_save((base + ".st").c_str());
#ifdef PSXVR_DEV
    if (g_capture) dump_capture((base + "_cap.txt").c_str());  // (development: the frame's 3D capture, for analysis)
#endif
    std::vector<u32> px;
    int w, h;
    display_to_rgba(px, w, h);
    save_bmp((base + ".bmp").c_str(), px, w, h);
    if (FILE *f = fopen((base + ".txt").c_str(), "w")) {
        fprintf(f, "state %s\nport1 %d\ngte_h_override %d\n%s\n", ok ? "ok" : "FAILED", g_pads[0].type, g_gte_h_override,
                notes.c_str());
        fclose(f);
    }
    logf("snapshot %s (%s)", base.c_str(), ok ? "ok" : "failed");
    return base.substr(7);
}

int vr_main(int argc, char **argv);  // vr/vr_main.cpp

int main(int argc, char **argv) {
    int frames = -1, shot_every = 0;
    const char *shot = nullptr, *vram_shot = nullptr;
    std::string input, cue;
    bool vr = false, port1_given = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) {
            frames = atoi(argv[++i]);
            psx::g_log_name = "psxvr_test.log";  // test runs keep the game's own log
        }
        else if (a == "--shot" && i + 1 < argc) shot = argv[++i];
        else if (a == "--shot-every" && i + 1 < argc) shot_every = atoi(argv[++i]);
        else if (a == "--vram" && i + 1 < argc) vram_shot = argv[++i];
        else if (a == "--rawaudio" && i + 1 < argc) g_wav = fopen(argv[++i], "wb");
        else if (a == "--logcd") psx::g_log_cd = true;
        else if (a == "--logspu") { extern bool g_log_spu_pub(); g_log_spu_pub(); }
        else if (a == "--input" && i + 1 < argc) input = argv[++i];
        else if (a == "--input-file" && i + 1 < argc) {  // the input script from a file (long scripts exceed the command line)
            if (FILE *inf = fopen(argv[++i], "rb")) {
                char buf[4096];
                size_t got;
                while ((got = fread(buf, 1, sizeof buf, inf)) > 0) input.append(buf, got);
                fclose(inf);
                while (!input.empty() && (unsigned char)input.back() < 32) input.pop_back();
            }
        }
        else if (a == "--cue" && i + 1 < argc) cue = argv[++i];
        else if (a == "--trace") g_trace = true;
        else if (a == "--cpu-cycles" && i + 1 < argc) g_cpu_instr_cycles = (u32)std::max(1, atoi(argv[++i]));  // overclock: 1
        else if (a == "--cpu-cpi" && i + 1 < argc) g_cpu_cpi_q8 = (u32)std::max(64.0, atof(argv[++i]) * 256.0);  // e.g. 1.5
        else if (a == "--pcs") g_pc_sample = true;
        else if (a == "--trace-pc" && i + 1 < argc) {
            for (char *t = strtok(argv[++i], ","); t; t = strtok(nullptr, ",")) cpu_add_hook((u32)strtoul(t, nullptr, 16), trace_pc_hook);
        }
        else if (a == "--calls" && i + 1 < argc) sscanf(argv[++i], "%d,%d", &g_calls_from, &g_calls_to);
        else if (a == "--rwatch" && i + 1 < argc) {
            sscanf(argv[++i], "%x-%x@%d", &g_rwatch_range[0], &g_rwatch_range[1], &g_rwatch_frame);
            g_rwatch_range[0] &= 0x1fffffff;
            g_rwatch_range[1] &= 0x1fffffff;
        }
        else if (a == "--watch" && i + 1 < argc) {
            extern u32 g_watch_lo_pub(u32, u32);
            unsigned lo, hi;
            if (sscanf(argv[++i], "%x-%x", &lo, &hi) == 2) g_watch_lo_pub(lo & 0x1fffffff, hi & 0x1fffffff);
        }
        else if (a == "--poke" && i + 1 < argc) {
            unsigned ad, val;
            int fr = 0;
            const char *arg = argv[++i];
            if (sscanf(arg, "%x=%i@%d", &ad, (int *)&val, &fr) >= 2) g_pokes.push_back({ad, (u16)val, fr, strchr(arg, '!') != nullptr});
        }
        else if (a == "--ramdump" && i + 1 < argc) {
            for (char *t = strtok(argv[++i], ","); t; t = strtok(nullptr, ",")) g_ramdump_frames.push_back(atoi(t));
        }
        else if (a == "--gte-h" && i + 1 < argc) g_gte_h_override = (u16)atoi(argv[++i]);
        else if (a == "--view-log") game::g_view_log = true;
        else if (a == "--aim" && i + 1 < argc) {  // yaw_deg,pitch_deg: spells aimed there (testing)
            float y = 0, p = 0;
            sscanf(argv[++i], "%f,%f", &y, &p);
            game::set_spell_aim(true, y * 0.0174533f, p * 0.0174533f);
        }
        else if (a == "--gun-exact") g_gun_exact = true;  // GunCon: exact counters + no smoothing (as the VR frontend)
        else if (a == "--gun-dy" && i + 1 < argc) g_gun_dy = (float)atof(argv[++i]);  // GunCon rows added (VR play: -28)
        else if (a == "--view-abs" && i + 1 < argc) {  // yaw_deg,pitch_deg in the game world (as a VR snapshot's .txt)
            float y = 0, p = 0;
            sscanf(argv[++i], "%f,%f", &y, &p);
            g_view_abs[0] = y * 0.0174533f; g_view_abs[1] = p * 0.0174533f; g_view_abs_on = true;
        }
        else if (a == "--view" && i + 1 < argc) {  // yaw_deg,pitch_deg: fixed head orientation (testing)
            float y = 0, p = 0;
            sscanf(argv[++i], "%f,%f", &y, &p);
            g_mc_view[0] = y * 0.0174533f; g_mc_view[1] = p * 0.0174533f; g_mc_view_on = true;
        }
        else if (a == "--hands-preview" && i + 1 < argc) {  // file.bmp[,trigger,squeeze]: draw the VR hands
            char fn[512] = {};
            float tr = 0, sq = 0;
            sscanf(argv[++i], "%511[^,],%f,%f", fn, &tr, &sq);
            ID3D11Device *d;
            ID3D11DeviceContext *c;
            if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d,
                                            nullptr, &c))) {
                if (getenv("HANDS_WEAPON"))
                    vrhands::g_preview_pistol = (const vrhands::PistolModel *)game::weapon_model(atoi(getenv("HANDS_WEAPON")));
                printf("hands preview: %d\n", (int)vrhands::preview_bmp(d, c, fn, tr, sq));
            }
            return 0;
        }
        else if (a == "--preview3d" && i + 1 < argc) g_preview = argv[++i], g_pgxp = g_capture = true;
        else if (a == "--capdump" && i + 1 < argc) g_capdump = argv[++i], g_pgxp = g_capture = true;
        else if (a == "--model-dump" && i + 1 < argc) g_model_dump = argv[++i], g_pgxp = g_capture = true;
        else if (a == "--logbios") g_log_bios = true;
        else if ((a == "--port1" || a == "--port2") && i + 1 < argc) {  // none | digital | dualshock | guncon
            std::string t = argv[++i];
            int type = t == "none" ? PAD_NONE : t == "digital" ? PAD_DIGITAL : t == "guncon" ? PAD_GUNCON : PAD_DUALSHOCK;
            g_port_types[a == "--port1" ? 0 : 1] = type;
            if (a == "--port1") port1_given = true;
        }
        else if (a == "--save-state" && i + 1 < argc) {
            std::string v = argv[++i];
            size_t at = v.rfind('@');
            g_save_state = v.substr(0, at);
            g_save_state_frame = at == std::string::npos ? 0 : atoi(v.c_str() + at + 1);
        }
        else if (a == "--load-state" && i + 1 < argc) g_load_state = argv[++i];
        else if (a == "--script-port" && i + 1 < argc) g_script_port = atoi(argv[++i]) == 2 ? 1 : 0;
        else if (a == "--vr") vr = true;
        else if (a == "--audio" && i + 1 < argc) g_audio_filter = argv[++i];
    }
#ifdef RESVR_RELEASE
    vr = true;  // (releases: VR only)
#endif
    // VR: a GunCon in port 1 (the pistol in the player's hand aims it), unless --port1 says otherwise
    if (vr && !port1_given) g_port_types[0] = PAD_GUNCON;
    if (cue.empty()) cue = find_cue();
    if (!boot(cue)) {
        fprintf(stderr, "boot failed (cue: %s)\n", cue.c_str());
        return 1;
    }
    if (!g_load_state.empty() && !state_load(g_load_state.c_str())) {
        fprintf(stderr, "cannot load state %s\n", g_load_state.c_str());
        return 1;
    }
    game::install_capture_hooks();
    if (g_mc_view_on) game::set_view(g_mc_view[0], g_mc_view[1]);
    if (g_view_abs_on) game::set_view_abs(g_view_abs[0], g_view_abs[1]);
    g_pads[0].gun_dy = g_gun_dy;
    if (frames >= 0) return run_headless(frames, shot, vram_shot, input, shot_every);
    if (vr) return vr_main(argc, argv);

    WNDCLASSA wc = {};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "PSXVR";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    RECT r = {0, 0, 960, 720};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowA("PSXVR", GAME_TITLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                         CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                         wc.hInstance, nullptr);
    if (!d3d_init(hwnd)) {
        MessageBoxA(hwnd, "Direct3D 11 init failed", GAME_TITLE, MB_OK);
        return 1;
    }
    AudioOut audio;
    bool have_audio = audio.init();

    using clk = std::chrono::steady_clock;
    const auto frame_dur = std::chrono::nanoseconds(1000000000 / FRAME_HZ);
    auto next = clk::now();
    MSG msg;
    while (!M.quit) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) M.quit = true;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        g_pad_buttons = poll_input();
        {
            static bool f5_was = false;
            bool f5 = GetForegroundWindow() == hwnd && (GetAsyncKeyState(VK_F5) & 0x8000);
            if (f5 && !f5_was) frontend_snapshot("desktop");
            f5_was = f5;
        }
        cpu_run_frame();
        if (have_audio) audio.pump();
        d3d_present_frame();
        next += frame_dur;
        auto now = clk::now();
        if (next > now) std::this_thread::sleep_until(next);
        else if (now - next > std::chrono::milliseconds(100)) next = now;
    }
    return 0;
}
