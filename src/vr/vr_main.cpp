// OpenXR frontend (D3D11) for Resident Evil Survivor. While the player is in control the world is redrawn in true
// stereo 3D from the captured primitives (scene.cpp), with the game's view driven by the headset (game.cpp); menus,
// cut-scenes and movies are shown on a virtual screen. A GunCon is plugged into port 1: the pistol in the player's
// hand aims it (the ray's hit point, projected the way the game drew the frame, is the gun's position on screen), the
// left stick walks towards where the player looks (the gun's own "trigger off screen = walk", with the body turned
// that way), so shooting, damage, enemies and rules stay the original ones.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <atomic>
#include <mutex>
#include <thread>

#include "../core/psx.h"
#include "../core/capture.h"
#include "../core/state.h"
#include "scene.h"
#include "../game/game.h"
#include "vr_hands.h"
#include "vr_glove.h"

using namespace psx;
using game::PAD_CIR;
using game::PAD_START;
using game::PAD_X;

// provided by platform/main_win32.cpp
void frontend_get_frame(std::vector<u32> &px, int &w, int &h);
bool frontend_audio_init();
void frontend_audio_pump();
std::string frontend_snapshot(const std::string &notes);
extern std::string g_boot_name;

namespace {

const char *APP_TITLE = "Resident Evil Survivor VR";

void fail(const char *what, XrResult r = XR_SUCCESS) {
    char buf[512];
    snprintf(buf, sizeof buf, "OpenXR error: %s (XrResult %d)", what, (int)r);
    logf("%s", buf);
    fprintf(stderr, "%s\n", buf);
    MessageBoxA(nullptr, buf, APP_TITLE, MB_OK | MB_ICONERROR);
}

#define XRCHECK(call)                       \
    do {                                    \
        XrResult _r = (call);               \
        if (XR_FAILED(_r)) {                \
            fail(#call, _r);                \
            return 1;                       \
        }                                   \
    } while (0)

// ---------------------------------------------------------------- math helpers (see xmath.h)
Mat4 mul(const Mat4 &a, const Mat4 &b) { return mat_mul(a, b); }
Mat4 identity() { return mat_identity(); }
Mat4 projection(const XrFovf &fov, float n, float f) {
    return mat_frustum(tanf(fov.angleLeft), tanf(fov.angleRight), tanf(fov.angleDown), tanf(fov.angleUp), n, f);
}
Mat4 pose_matrix(const XrPosef &p) {
    return mat_pose(p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w, p.position.x, p.position.y,
                    p.position.z);
}
Mat4 view_from_pose(const XrPosef &p) { return mat_rigid_inverse(pose_matrix(p)); }
float wrap_pi(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

// ---------------------------------------------------------------- D3D11 resources
ID3D11Device *dev;
ID3D11DeviceContext *ctx;
ID3D11VertexShader *vs;
ID3D11PixelShader *ps;
ID3D11Buffer *cbuf;
ID3D11SamplerState *sampler;
ID3D11RasterizerState *raster;
ID3D11BlendState *bs_none, *bs_alpha;
ID3D11DepthStencilState *ds_off;
ID3D11Texture2D *game_tex;
ID3D11ShaderResourceView *game_srv;
// 1 x 1 opaque textures: black (cut-scene bands), red (the aim dot on the virtual screen)
ID3D11ShaderResourceView *solid_srv(uint32_t abgr) {
    static std::map<uint32_t, ID3D11ShaderResourceView *> made;
    ID3D11ShaderResourceView *&srv = made[abgr];
    if (!srv) {
        const uint32_t px = abgr;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM, td.SampleDesc.Count = 1, td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd = {&px, 4, 0};
        ID3D11Texture2D *t = nullptr;
        if (SUCCEEDED(dev->CreateTexture2D(&td, &sd, &t))) dev->CreateShaderResourceView(t, nullptr, &srv), t->Release();
    }
    return srv;
}
ID3D11ShaderResourceView *black_srv() { return solid_srv(0xff000000u); }
int game_w = 0, game_h = 0;
bool srgb = false;

const char *shader_src = R"(
cbuffer C : register(b0) { row_major float4x4 mvp; float4 uvr; };
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID) {
    float2 c[6] = { float2(-1,-1), float2(-1,1), float2(1,1), float2(-1,-1), float2(1,1), float2(1,-1) };
    VSOut o;
    o.pos = mul(mvp, float4(c[id], 0, 1));
    o.uv = uvr.xy + float2(c[id].x * 0.5 + 0.5, 0.5 - c[id].y * 0.5) * uvr.zw;
    return o;
}
float4 ps_main(VSOut i) : SV_Target { return tex.Sample(smp, i.uv); }
)";

bool create_pipeline() {
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(shader_src, strlen(shader_src), nullptr, nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vsb,
                          &err)))
        return false;
    if (FAILED(D3DCompile(shader_src, strlen(shader_src), nullptr, nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &psb,
                          &err)))
        return false;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(Mat4) + 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dev->CreateBuffer(&bd, nullptr, &cbuf);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    dev->CreateSamplerState(&sd, &sampler);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &raster);
    D3D11_BLEND_DESC bld = {};
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bld, &bs_none);
    auto &rt = bld.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    dev->CreateBlendState(&bld, &bs_alpha);
    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dev->CreateDepthStencilState(&dsd, &ds_off);
    return true;
}

// Textured quad (unit square scaled by the model matrix) showing the sub-rectangle (u0,v0)-(u1,v1) of the texture.
void draw_quad_uv(const Mat4 &mvp, ID3D11ShaderResourceView *srv, bool alpha, float u0, float v0, float u1, float v1,
                  ID3D11BlendState *blend = nullptr, const float *factor = nullptr) {
    struct { Mat4 m; float uv[4]; } cb = {mvp, {u0, v0, u1 - u0, v1 - v0}};
    ctx->UpdateSubresource(cbuf, 0, nullptr, &cb, 0, 0);
    ctx->RSSetState(raster);
    ctx->OMSetBlendState(blend ? blend : alpha ? bs_alpha : bs_none, factor, 0xffffffff);
    ctx->OMSetDepthStencilState(ds_off, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &cbuf);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &sampler);
    ctx->Draw(6, 0);
    ctx->OMSetBlendState(bs_none, nullptr, 0xffffffff);
}
void draw_quad(const Mat4 &mvp, ID3D11ShaderResourceView *srv, bool alpha) { draw_quad_uv(mvp, srv, alpha, 0, 0, 1, 1); }

// Picture feedback (the opening: the hero waking up, blurred): the game draws the previous picture over the new one at
// 50 % each drawn frame (25 per second), a trail that fades. VR does the same with each eye's (and the mirror's) own
// previous image, the share kept scaled to the headset's rate so the trail lasts as long as in the original.
struct Ghost {
    ID3D11Texture2D *tex = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    UINT w = 0, h = 0;
    bool valid = false;
};
Ghost ghosts[4];
ID3D11BlendState *bs_const = nullptr;
void ghost_reset() { for (Ghost &g : ghosts) g.valid = false; }
void ghost_apply(int slot, ID3D11RenderTargetView *rtv, float keep) {
    if (slot < 0 || slot >= 4) return;
    Ghost &g = ghosts[slot];
    ID3D11Resource *res = nullptr;
    rtv->GetResource(&res);
    ID3D11Texture2D *tex = nullptr;
    if (!res || FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&tex))) { if (res) res->Release(); return; }
    res->Release();
    D3D11_TEXTURE2D_DESC td;
    tex->GetDesc(&td);
    if (!g.tex || g.w != td.Width || g.h != td.Height) {
        if (g.srv) g.srv->Release(), g.srv = nullptr;
        if (g.tex) g.tex->Release(), g.tex = nullptr;
        D3D11_RENDER_TARGET_VIEW_DESC rd;
        rtv->GetDesc(&rd);
        D3D11_TEXTURE2D_DESC hd = td;
        hd.MipLevels = hd.ArraySize = 1;
        hd.SampleDesc.Count = 1, hd.SampleDesc.Quality = 0;
        hd.Usage = D3D11_USAGE_DEFAULT;
        hd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hd.CPUAccessFlags = hd.MiscFlags = 0;
        if (FAILED(dev->CreateTexture2D(&hd, nullptr, &g.tex))) { tex->Release(); return; }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = rd.Format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(g.tex, &sd, &g.srv))) { g.tex->Release(), g.tex = nullptr; tex->Release(); return; }
        g.w = td.Width, g.h = td.Height, g.valid = false;
    }
    if (g.valid) {
        if (!bs_const) {
            D3D11_BLEND_DESC bd = {};
            auto &rt = bd.RenderTarget[0];
            rt.BlendEnable = TRUE;
            rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
            rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_INV_BLEND_FACTOR;
            rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            dev->CreateBlendState(&bd, &bs_const);
        }
        float f[4] = {keep, keep, keep, keep};
        draw_quad_uv(mat_identity(), g.srv, false, 0, 0, 1, 1, bs_const, f);
        ID3D11ShaderResourceView *none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none);
    }
    ctx->CopyResource(g.tex, tex);
    g.valid = true;
    tex->Release();
}

void upload_game_frame(const std::vector<u32> &px, int w, int h) {
    if (w != game_w || h != game_h) {
        if (game_srv) game_srv->Release();
        if (game_tex) game_tex->Release();
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        dev->CreateTexture2D(&td, nullptr, &game_tex);
        dev->CreateShaderResourceView(game_tex, nullptr, &game_srv);
        game_w = w;
        game_h = h;
    }
    ctx->UpdateSubresource(game_tex, 0, nullptr, px.data(), w * 4, 0);
}

// ---------------------------------------------------------------- input actions
// The GunCon has a trigger and two buttons (A, B). PS VR2 Sense / Touch layout:
//   trigger of the pistol hand = gun trigger        left stick = walk (towards the gaze), right stick x = snap turn
//   A (right, Cross) = gun A    B (right, Circle) = gun B    Y (left, Triangle) = A + B (the game's item / map menu)
//   X (left, Square) = VR options (in a menu)       left trigger: left-hand pistol
//   left stick click = run (until the stick is let go)
//   right stick click = save a snapshot (short press) / recentre (held 0.6 s)
enum Btn { B_A, B_B, B_X, B_Y, B_MENU, B_RUN, B_COUNT };
const char *BTN_NAMES[B_COUNT] = {"gun_a", "gun_b", "vr_options", "item_menu", "menu", "run"};
const char *BTN_LABELS[B_COUNT] = {"GunCon A", "GunCon B", "VR options", "Item / map menu (A+B)", "Item menu",
                                   "Run"};

struct Input {
    XrActionSet set = XR_NULL_HANDLE;
    XrAction move = XR_NULL_HANDLE, turn = XR_NULL_HANDLE, recenter = XR_NULL_HANDLE;
    XrAction btn[B_COUNT] = {};
    XrAction grip_pose = XR_NULL_HANDLE, aim_pose = XR_NULL_HANDLE, trigger = XR_NULL_HANDLE, squeeze = XR_NULL_HANDLE;
    XrAction haptic = XR_NULL_HANDLE;
    XrPath hand_path[2] = {XR_NULL_PATH, XR_NULL_PATH};
    XrSpace grip_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE}, aim_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
};
Input in;

XrPath path(XrInstance inst, const char *s) {
    XrPath p;
    xrStringToPath(inst, s, &p);
    return p;
}

void suggest(XrInstance inst, const char *profile, std::vector<std::pair<XrAction, const char *>> b) {
    std::vector<XrActionSuggestedBinding> v;
    for (auto &x : b) v.push_back({x.first, path(inst, x.second)});
    XrInteractionProfileSuggestedBinding s = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    s.interactionProfile = path(inst, profile);
    s.suggestedBindings = v.data();
    s.countSuggestedBindings = (uint32_t)v.size();
    XrResult r = xrSuggestInteractionProfileBindings(inst, &s);
    if (XR_FAILED(r)) logf("binding suggestion for %s failed (%d)", profile, (int)r);
}

XrAction make_action(XrActionType t, const char *name, const char *label, bool per_hand = false) {
    XrActionCreateInfo ci = {XR_TYPE_ACTION_CREATE_INFO};
    ci.actionType = t;
    if (per_hand) {
        ci.countSubactionPaths = 2;
        ci.subactionPaths = in.hand_path;
    }
    strcpy(ci.actionName, name);
    strcpy(ci.localizedActionName, label);
    XrAction a = XR_NULL_HANDLE;
    xrCreateAction(in.set, &ci, &a);
    return a;
}

bool create_actions(XrInstance inst, XrSession sess) {
    XrActionSetCreateInfo si = {XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(si.actionSetName, "survivor");
    strcpy(si.localizedActionSetName, "Survivor");
    if (XR_FAILED(xrCreateActionSet(inst, &si, &in.set))) return false;
    in.move = make_action(XR_ACTION_TYPE_VECTOR2F_INPUT, "move", "Walk");
    in.turn = make_action(XR_ACTION_TYPE_VECTOR2F_INPUT, "turn", "Snap turn");
    in.recenter = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "recenter", "Recenter view");
    for (int i = 0; i < B_COUNT; i++) in.btn[i] = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, BTN_NAMES[i], BTN_LABELS[i]);
    in.hand_path[0] = path(inst, "/user/hand/left");
    in.hand_path[1] = path(inst, "/user/hand/right");
    in.grip_pose = make_action(XR_ACTION_TYPE_POSE_INPUT, "grip_pose", "Hand", true);
    in.aim_pose = make_action(XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Pistol aim", true);
    in.trigger = make_action(XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger", true);
    in.squeeze = make_action(XR_ACTION_TYPE_FLOAT_INPUT, "finger_grip", "Fingers", true);
    in.haptic = make_action(XR_ACTION_TYPE_VIBRATION_OUTPUT, "recoil", "Recoil", true);
    XrAction *b = in.btn;
    static std::vector<std::string> keep;
    keep.reserve(64);
    auto hands = [&](std::vector<std::pair<XrAction, const char *>> v, const char *trig, const char *sq) {
        for (int h = 0; h < 2; h++) {
            std::string side = h ? "/user/hand/right/" : "/user/hand/left/";
            keep.push_back(side + "input/grip/pose");
            v.push_back({in.grip_pose, keep.back().c_str()});
            keep.push_back(side + "input/aim/pose");
            v.push_back({in.aim_pose, keep.back().c_str()});
            keep.push_back(side + "output/haptic");
            v.push_back({in.haptic, keep.back().c_str()});
            if (trig) { keep.push_back(side + "input/" + trig); v.push_back({in.trigger, keep.back().c_str()}); }
            if (sq) { keep.push_back(side + "input/" + sq); v.push_back({in.squeeze, keep.back().c_str()}); }
        }
        return v;
    };
    // Touch layout (SteamVR offers it for the PS VR2 Sense controllers too: Cross = A, Circle = B, Square = X,
    // Triangle = Y, Options / Create = menu)
    suggest(inst, "/interaction_profiles/oculus/touch_controller", hands(
            {{in.move, "/user/hand/left/input/thumbstick"},
             {in.turn, "/user/hand/right/input/thumbstick"},
             {in.recenter, "/user/hand/right/input/thumbstick/click"},
             {b[B_A], "/user/hand/right/input/a/click"},
             {b[B_B], "/user/hand/right/input/b/click"},
             {b[B_X], "/user/hand/left/input/x/click"},
             {b[B_Y], "/user/hand/left/input/y/click"},
             {b[B_MENU], "/user/hand/left/input/menu/click"},
             {b[B_RUN], "/user/hand/left/input/thumbstick/click"}}, "trigger/value", "squeeze/value"));
    suggest(inst, "/interaction_profiles/valve/index_controller", hands(
            {{in.move, "/user/hand/left/input/thumbstick"},
             {in.turn, "/user/hand/right/input/thumbstick"},
             {in.recenter, "/user/hand/right/input/thumbstick/click"},
             {b[B_A], "/user/hand/right/input/a/click"},
             {b[B_B], "/user/hand/right/input/b/click"},
             {b[B_X], "/user/hand/left/input/a/click"},
             {b[B_Y], "/user/hand/left/input/b/click"},
             {b[B_RUN], "/user/hand/left/input/thumbstick/click"}}, "trigger/value", "squeeze/value"));
    suggest(inst, "/interaction_profiles/htc/vive_controller", hands(
            {{in.move, "/user/hand/left/input/trackpad"},
             {in.turn, "/user/hand/right/input/trackpad"},
             {b[B_A], "/user/hand/right/input/trackpad/click"},
             {b[B_B], "/user/hand/right/input/squeeze/click"},
             {b[B_X], "/user/hand/left/input/menu/click"},
             {b[B_Y], "/user/hand/left/input/squeeze/click"},
             {b[B_MENU], "/user/hand/right/input/menu/click"}}, "trigger/value", "squeeze/click"));
    suggest(inst, "/interaction_profiles/khr/simple_controller", hands(
            {{b[B_Y], "/user/hand/left/input/menu/click"},
             {b[B_A], "/user/hand/left/input/select/click"}}, "select/click", nullptr));
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo asi = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
        asi.subactionPath = in.hand_path[h];
        asi.poseInActionSpace.orientation.w = 1;
        asi.action = in.grip_pose;
        xrCreateActionSpace(sess, &asi, &in.grip_space[h]);
        asi.action = in.aim_pose;
        xrCreateActionSpace(sess, &asi, &in.aim_space[h]);
    }
    XrSessionActionSetsAttachInfo at = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    at.countActionSets = 1;
    at.actionSets = &in.set;
    return XR_SUCCEEDED(xrAttachSessionActionSets(sess, &at));
}

float stick_move[2], stick_turn[2];  // x right, y forward
bool recenter_state = false;
bool btn_state[B_COUNT];

bool get_bool(XrSession sess, XrAction a) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    XrActionStateBoolean st = {XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(sess, &gi, &st)) && st.isActive && st.currentState;
}

void get_stick(XrSession sess, XrAction a, float out[2]) {
    out[0] = out[1] = 0;
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    XrActionStateVector2f st = {XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(sess, &gi, &st)) && st.isActive) {
        out[0] = st.currentState.x;
        out[1] = st.currentState.y;
    }
}

float get_float(XrSession sess, XrAction a, XrPath sub) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = sub;
    XrActionStateFloat st = {XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(xrGetActionStateFloat(sess, &gi, &st)) && st.isActive ? st.currentState : 0.0f;
}

void haptic_pulse(XrSession sess, int hand, float amplitude, float seconds) {
    XrHapticActionInfo hi = {XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = in.haptic;
    hi.subactionPath = in.hand_path[hand];
    XrHapticVibration v = {XR_TYPE_HAPTIC_VIBRATION};
    v.amplitude = amplitude;
    v.duration = (XrDuration)(seconds * 1e9);
    v.frequency = XR_FREQUENCY_UNSPECIFIED;
    xrApplyHapticFeedback(sess, &hi, (XrHapticBaseHeader *)&v);
}

// Hands in LOCAL space at the predicted display time
void locate_hands(XrSession sess, XrSpace local, XrTime t, vrhands::HandState hs[2]) {
    for (int h = 0; h < 2; h++) {
        hs[h].valid = false;
        XrSpaceLocation g = {XR_TYPE_SPACE_LOCATION}, a = {XR_TYPE_SPACE_LOCATION};
        const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if (in.grip_space[h] && XR_SUCCEEDED(xrLocateSpace(in.grip_space[h], local, t, &g)) &&
            (g.locationFlags & need) == need) {
            hs[h].valid = true;
            hs[h].grip = pose_matrix(g.pose);
            hs[h].aim = hs[h].grip;
            if (XR_SUCCEEDED(xrLocateSpace(in.aim_space[h], local, t, &a)) && (a.locationFlags & need) == need)
                hs[h].aim = pose_matrix(a.pose);
        }
        hs[h].trigger = get_float(sess, in.trigger, in.hand_path[h]);
        hs[h].squeeze = get_float(sess, in.squeeze, in.hand_path[h]);
    }
}

void poll_actions(XrSession sess) {
    XrActiveActionSet aas = {in.set, XR_NULL_PATH};
    XrActionsSyncInfo sync = {XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &aas;
    stick_move[0] = stick_move[1] = stick_turn[0] = stick_turn[1] = 0;
    recenter_state = false;
    for (bool &b : btn_state) b = false;
    if (XR_FAILED(xrSyncActions(sess, &sync))) return;
    for (int i = 0; i < B_COUNT; i++) btn_state[i] = get_bool(sess, in.btn[i]);
    get_stick(sess, in.move, stick_move);
    get_stick(sess, in.turn, stick_turn);
    recenter_state = get_bool(sess, in.recenter);
}

// Digital decisions from analog values: on past 'on', off under 'off' (the game reads its input on its own logic
// frames, every two video frames: a flickering value would be caught or missed at random).
struct Hyst {
    bool on = false;
    bool step(float v, float hi = 0.35f, float lo = 0.25f) {
        on = on ? v > lo : v > hi;
        return on;
    }
};

// ---------------------------------------------------------------- saves to load
// The snapshots of the disc being played, newest first (a state only runs with the disc it was made with).
static std::vector<std::string> list_saves() {
    bool usa = g_boot_name.find("SLUS") != std::string::npos;
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(usa ? "states\\usa_snap_*.st" : "states\\snap_*.st", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do out.push_back(fd.cFileName);
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(out.begin(), out.end(), std::greater<std::string>());  // (the name holds the date and time)
    return out;
}
// "usa_snap_20261116_115806.st" -> "16/11 11:58:06"
static std::string save_label(const std::string &file) {
    size_t i = file.find("snap_");
    if (i == std::string::npos || file.size() < i + 20) return file;
    std::string d = file.substr(i + 5, 15);  // 20261116_115806
    return d.substr(6, 2) + "/" + d.substr(4, 2) + " " + d.substr(9, 2) + ":" + d.substr(11, 2) + ":" + d.substr(13, 2);
}

// ---------------------------------------------------------------- VR options
// X in a menu (the game's item / map screen, the title...) opens the VR options panel: the stick picks a line, A
// changes it, B or X closes it. Saved to vr_options.ini next to the game.
struct VrOptions {
    int pistol_hand = 1;   // 0 left, 1 right
    bool laser = false;    // the aim ray and its dot (off by default)
    int turn = 1;          // 0 none, 1 snap 30, 2 snap 45, 3 smooth
    bool hud = true;       // the game's HUD (weapon, bullets, condition) on a panel below the view
    int hud_dist = 1;      // 0 near (1.0 m), 1 normal (1.3 m), 2 far (1.8 m)
    bool headshot = false; // the headshot's extra effects (head off, blood, louder sound); off = the original game
    void load() {
        if (FILE *f = fopen("vr_options.ini", "r")) {
            char line[128];
            while (fgets(line, sizeof line, f)) {
                int v = 0;
                if (sscanf(line, "pistol_hand=%d", &v) == 1) pistol_hand = v ? 1 : 0;
                else if (sscanf(line, "laser=%d", &v) == 1) laser = v != 0;
                else if (sscanf(line, "turn=%d", &v) == 1) turn = std::max(0, std::min(3, v));
                else if (sscanf(line, "hud=%d", &v) == 1) hud = v != 0;
                else if (sscanf(line, "hud_dist=%d", &v) == 1) hud_dist = std::max(0, std::min(2, v));
                else if (sscanf(line, "headshot=%d", &v) == 1) headshot = v != 0;
            }
            fclose(f);
        }
    }
    void save() const {
        if (FILE *f = fopen("vr_options.ini", "w")) {
            fprintf(f, "pistol_hand=%d\nlaser=%d\nturn=%d\nhud=%d\nhud_dist=%d\nheadshot=%d\n", pistol_hand, laser ? 1 : 0,
                    turn, hud ? 1 : 0, hud_dist, headshot ? 1 : 0);
            fclose(f);
        }
    }
};

// A panel of text drawn with GDI into a texture (redrawn only when it changes).
struct TextPanel {
    int w = 0, h = 0;
    ID3D11Texture2D *tex = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    std::string shown;
    // lines: '\t' splits a label from its value; a line starting with '>' is highlighted; '#' = title
    void render(const std::vector<std::string> &lines, int pw, int ph) {
        std::string key;
        for (auto &l : lines) key += l + "\n";
        if (srv && key == shown && pw == w && ph == h) return;
        shown = key;
        if (!tex || pw != w || ph != h) {
            if (srv) srv->Release(), srv = nullptr;
            if (tex) tex->Release(), tex = nullptr;
            w = pw, h = ph;
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = w;
            td.Height = h;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex))) return;
            dev->CreateShaderResourceView(tex, nullptr, &srv);
        }
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void *bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(dc, bmp);
        RECT all = {0, 0, w, h};
        HBRUSH bg = CreateSolidBrush(RGB(26, 22, 22));
        FillRect(dc, &all, bg);
        DeleteObject(bg);
        RECT frame = {4, 4, w - 4, h - 4};
        HBRUSH red = CreateSolidBrush(RGB(170, 30, 25));
        FrameRect(dc, &frame, red);
        DeleteObject(red);
        int lh = h / (int)std::max<size_t>(lines.size() + 1, 1);
        HFONT font = CreateFontA(-(lh * 6 / 10), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY,
                                 0, "Segoe UI");
        HGDIOBJ oldf = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        for (size_t i = 0; i < lines.size(); i++) {
            std::string l = lines[i];
            bool hi = !l.empty() && l[0] == '>', title = !l.empty() && l[0] == '#';
            if (hi || title) l = l.substr(1);
            int y = (int)(lh / 2 + i * lh);
            if (hi) {
                RECT bar = {14, y, w - 14, y + lh};
                HBRUSH b = CreateSolidBrush(RGB(90, 30, 28));
                FillRect(dc, &bar, b);
                DeleteObject(b);
            }
            SetTextColor(dc, title ? RGB(230, 60, 45) : hi ? RGB(255, 255, 255) : RGB(200, 195, 190));
            size_t tab = l.find('\t');
            RECT r = {30, y, w - 30, y + lh};
            if (title) {
                DrawTextA(dc, l.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                DrawTextA(dc, l.substr(0, tab).c_str(), -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                if (tab != std::string::npos) {
                    SetTextColor(dc, hi ? RGB(255, 210, 140) : RGB(215, 170, 120));
                    DrawTextA(dc, l.substr(tab + 1).c_str(), -1, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                }
            }
        }
        GdiFlush();
        uint8_t *p = (uint8_t *)bits;
        for (int i = 0; i < w * h; i++) p[i * 4 + 3] = 255;
        ctx->UpdateSubresource(tex, 0, nullptr, bits, w * 4, 0);
        SelectObject(dc, oldf);
        DeleteObject(font);
        SelectObject(dc, old);
        DeleteObject(bmp);
        DeleteDC(dc);
    }
};

// ---------------------------------------------------------------- desktop window
// Shows the game on the monitor while it is played in the headset (for recording videos): 16:9, 80% of the usable
// desktop height, its own view from the head as wide as both eyes together. Closing it ends the game.
// RESVR_NO_WINDOW=1: none.
bool g_mirror_closed = false;
LRESULT CALLBACK mirror_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_CLOSE) { g_mirror_closed = true; return 0; }
    return DefWindowProcA(w, m, wp, lp);
}
struct Mirror {
    HWND wnd = nullptr;
    IDXGISwapChain1 *sc = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    ID3D11DepthStencilView *dsv = nullptr;
    int w = 0, h = 0;
    bool srgb = false;
    bool ready() const { return sc && rtv; }
    bool create(ID3D11Device *d, IDXGIFactory1 *f1, bool srgb_rtv) {
        srgb = srgb_rtv;
        WNDCLASSA wc = {};
        wc.lpfnWndProc = mirror_proc;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = "resvr_mirror";
        RegisterClassA(&wc);
        RECT work;
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
        int uw = work.right - work.left, uh = work.bottom - work.top;
        int ch = uh * 8 / 10, cw = std::min(uw * 9 / 10, ch * 16 / 9);
        ch = cw * 9 / 16;
        DWORD style = WS_OVERLAPPEDWINDOW;
        RECT r = {0, 0, cw, ch};
        AdjustWindowRect(&r, style, FALSE);
        int ww = r.right - r.left, wh = r.bottom - r.top;
        wnd = CreateWindowA("resvr_mirror", APP_TITLE, style, work.left + (uw - ww) / 2, work.top + (uh - wh) / 2, ww, wh,
                            nullptr, nullptr, wc.hInstance, nullptr);
        if (!wnd) return false;
        IDXGIFactory2 *f2 = nullptr;
        if (FAILED(f1->QueryInterface(__uuidof(IDXGIFactory2), (void **)&f2))) return false;
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        HRESULT hr = f2->CreateSwapChainForHwnd(d, wnd, &sd, nullptr, nullptr, &sc);
        f2->Release();
        if (FAILED(hr)) { sc = nullptr; return false; }
        ShowWindow(wnd, SW_SHOWNORMAL);
        fit(d);
        return ready();
    }
    // follow the window's size (it may be resized or maximised)
    void fit(ID3D11Device *d) {
        RECT r;
        GetClientRect(wnd, &r);
        int nw = std::max(1, (int)(r.right - r.left)), nh = std::max(1, (int)(r.bottom - r.top));
        if (rtv && nw == w && nh == h) return;
        if (rtv) rtv->Release(), rtv = nullptr;
        if (dsv) dsv->Release(), dsv = nullptr;
        if (FAILED(sc->ResizeBuffers(0, nw, nh, DXGI_FORMAT_UNKNOWN, 0))) return;
        w = nw, h = nh;
        ID3D11Texture2D *back = nullptr;
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back))) return;
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        d->CreateRenderTargetView(back, &rd, &rtv);
        back->Release();
        D3D11_TEXTURE2D_DESC dd = {};
        dd.Width = w;
        dd.Height = h;
        dd.MipLevels = dd.ArraySize = 1;
        dd.Format = DXGI_FORMAT_D32_FLOAT;
        dd.SampleDesc.Count = 1;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ID3D11Texture2D *dtex = nullptr;
        if (SUCCEEDED(d->CreateTexture2D(&dd, nullptr, &dtex))) {
            d->CreateDepthStencilView(dtex, nullptr, &dsv);
            dtex->Release();
        }
    }
};

struct SwapImage {
    ID3D11Texture2D *tex;
    ID3D11RenderTargetView *rtv;
};
struct EyeSwap {
    XrSwapchain sc;
    int w, h;
    std::vector<SwapImage> imgs;
    ID3D11DepthStencilView *dsv = nullptr;
};

// Ray (origin o, direction d, LOCAL) against the virtual screen (model = rotation / translation, then scale sx, sy
// of the unit square): the hit in the game picture's pixels (w x h), false if it misses.
bool screen_hit(const Mat4 &rigid, float sx, float sy, const float o[3], const float d[3], int w, int h, float &px,
                float &py, float &dist) {
    Mat4 inv = mat_rigid_inverse(rigid);
    float lo[3], ld[3];
    for (int r = 0; r < 3; r++) {
        lo[r] = inv.m[r][0] * o[0] + inv.m[r][1] * o[1] + inv.m[r][2] * o[2] + inv.m[r][3];
        ld[r] = inv.m[r][0] * d[0] + inv.m[r][1] * d[1] + inv.m[r][2] * d[2];
    }
    if (fabsf(ld[2]) < 1e-5f) return false;
    float t = -lo[2] / ld[2];
    if (t <= 0) return false;
    float x = (lo[0] + ld[0] * t) / sx, y = (lo[1] + ld[1] * t) / sy;
    px = (x * 0.5f + 0.5f) * w;
    py = (0.5f - y * 0.5f) * h;
    dist = t;
    return fabsf(x) <= 1.05f && fabsf(y) <= 1.05f;
}

// ---------------------------------------------------------------- emulation thread
// The PS1 runs on its own thread at the PAL 50 Hz cadence: a frame of the game with the 3D capture costs ~6 ms and the
// headset (90 / 120 Hz) cannot wait for it, or frames are dropped and the view judders when the head turns. The headset
// thread hands over its input (EmuIn), takes the results (EmuOut) and the latest captured frame (copied with its VRAM),
// and draws the latest one at its own rate.
struct EmuIn {
    float mx = 0, my = 0;         // left stick
    bool trigger = false;         // the pistol's trigger (with hysteresis)
    bool run = false;             // running (left stick click)
    bool follow = false;          // room scale: walk the player's eye to where the player physically is
    float follow_tx = 0, follow_tz = 0;  // that point in the game world (x, z)
    float gun_px = 160, gun_py = 120;
    bool gun_on = false;
    float aim_world[3] = {0, 0, 0};  // the aimed point in the game world (3D view)
    bool aim_world_ok = false;
    float aim_dir_world[3] = {0, 0, -1};  // the barrel's direction in the game world (the grenade gun shoots along it)
    float lob_target[3] = {0, 0, 0};      // where the grenade gun's arc meets the room (game world)
    bool lob_target_ok = false;
    bool btn[B_COUNT] = {};
    bool vr_menu_open = false;
    int swallow = 0;              // emulated frames without input after the VR options closed (added by the VR thread)
    float view_yaw = 0, view_pitch = 0;  // what the game should draw (game heading, pitch up), head turn lead included
    float gaze_yaw = 0;           // where the head looks now (game heading): the walking reference
    float head_yaw = 0;           // LOCAL, for a placement
    bool is3d = false;            // the last update of the scene was a 3D view
    unsigned placement_ack = 0;   // the last placement the VR thread has taken over
    bool snap = false;            // save a snapshot after the next frame
    std::string snap_notes;
    std::string load_path;        // load this save (states/...st) before the next frame
    bool running = false;
};
struct EmuOut {
    u64 frames = 0, last_render = 0, last_cap = 0;
    unsigned placement_seq = 0;
    float placement_world_yaw = 0;
    unsigned shots = 0;           // shots sent to the game (VR: recoil vibration)
    float player_heading = 0;
    bool in_control = false;
    int weapon = 0;               // equipped weapon (game::equipped_weapon)
    bool lob = false;             // the grenade gun: its grenade's start (game world), speed (a logic step), offset
    float lob_origin[3] = {0, 0, 0}, lob_speed = 0, lob_start = 0;
    bool lob_across = false;      // (grenade rounds: lob_speed is the speed across, up / down follows the tilt)
    bool slow_play = false;       // the game is busy with a long frame while playing: keep the 3D view
    bool screen_frozen = false;   // the game has stopped drawing and the screen still shows its last frame
    unsigned snaps = 0;
    unsigned loads = 0;           // saves loaded (VR: the message)
    bool load_ok = false;
    std::string snap_name;
};
struct EmuShared {
    std::mutex m;
    EmuIn in;
    EmuOut out;
    bool cap_new = false;
    CapFrame cap;
    std::vector<u16> vram;
    bool pix_new = false;
    std::vector<u32> pix;
    int pix_w = 0, pix_h = 0;
    std::atomic<bool> quit{false};
};
EmuShared emu;

void emu_thread_main() {
    using clk = std::chrono::steady_clock;
    PadPort &gun = g_pads[0];
    Hyst walk_h;
    u64 frames = 0, last_render_emu = 0, last_render = 0, last_cap = 0, last_cap_emu = 0;
    unsigned placement_seq = 0, shots = 0, snaps = 0, loads = 0;
    bool load_ok = false;
    float placed_world_yaw = 0;
    std::string snap_name;
    u64 last_play_frame = 0;  // the last frame the player was playing in the 3D view
    int last_play_room = -1;
    bool slow_play = false;   // playing, the game busy with a long frame (see the field of view)
    int release = 0, mode = 0, cal_t = -1, swallow = 0;
    bool back_step = false;  // the stick points backwards: the game's step back
    auto t0 = clk::now();
    bool was_running = false;
    while (!emu.quit && !M.quit) {
        EmuIn in;
        {
            std::lock_guard<std::mutex> lk(emu.m);
            in = emu.in;
            swallow += emu.in.swallow;
            emu.in.swallow = 0;
            emu.in.snap = false;
            emu.in.load_path.clear();
        }
        if (!in.running) { was_running = false; std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
        if (!was_running) { t0 = clk::now(); frames = 0; last_render_emu = last_cap_emu = 0; was_running = true; }
        // pacing: frame n starts at t0 + n / 50 s (after a stall, at most 4 frames are caught up)
        double elapsed = std::chrono::duration<double>(clk::now() - t0).count();
        u64 due = (u64)(elapsed * FRAME_HZ);
        if (due > frames + 4) {
            t0 += std::chrono::duration_cast<clk::duration>(std::chrono::duration<double>((double)(due - frames - 4) / FRAME_HZ));
            due = frames + 4;
        }
        if (frames >= due) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
        // a save picked in the VR options: the whole machine is put back (the game's code hooks stay); what VR keeps
        // of the game (headings, steps...) starts again as after a load at start
        if (!in.load_path.empty()) {
            load_ok = psx::state_load(in.load_path.c_str());
            logf("vr: load %s: %s", in.load_path.c_str(), load_ok ? "ok" : "FAILED");
            game::on_state_loaded();
            last_render_emu = last_cap_emu = frames;
            release = 0, swallow = 12, back_step = false;
            loads++;
        }
        bool in3d = frames - last_render_emu < 25 && in.is3d;
        // after a placement, until the VR thread has turned its world, the view and the walking reference come from it
        float view_yaw = in.view_yaw, gaze = in.gaze_yaw;
        if (in.placement_ack != placement_seq) {
            gaze = wrap_pi(in.head_yaw - placed_world_yaw);
            if (gaze < 0) gaze += 6.2831853f;
            view_yaw = gaze;
        }
        game::set_view_abs(view_yaw, in.view_pitch);
        // Walking: the stick's direction relative to the gaze turns the player that way (game heading), and the GunCon
        // walks it as the original does: trigger with the gun off screen. A shot (trigger with the gun on the picture)
        // takes priority; the trigger is let go for a few frames between the two, so the game sees a new press.
        float mag = sqrtf(in.mx * in.mx + in.my * in.my);
        bool control = game::in_control();
        bool walk = in3d && !in.vr_menu_open && walk_h.step(mag) && control;
        bool shoot = in.trigger && !in.vr_menu_open;
        // walking is asked of the game directly (set_walk), so the trigger only shoots and both work at once
        // stick backwards (more than 100 degrees from the gaze): the game's own step back (slower, its animation)
        back_step = walk && mag > 0.2f && fabsf(atan2f(in.mx, in.my)) > 1.745f;
        // room scale: without the stick, the player steps after the headset (walking steps, with the game's collision)
        // decided here with the eye's position now (the headset only knows it from captured frames, a few frames late:
        // stopping from there overshot and the player walked back and forth): step while more than 6 cm away
        bool follow = false;
        float follow_yaw = 0;
        if (!walk && in.follow && in3d && control && !in.vr_menu_open) {
            float ex, ez;
            if (game::eye_xz(ex, ez)) {
                float wx = in.follow_tx - ex, wz = in.follow_tz - ez;
                if (wx * wx + wz * wz > 60.0f * 60.0f) {
                    follow = true;
                    follow_yaw = atan2f(-wx, -wz);  // game heading: forward = (-sin h, -cos h)
                    if (follow_yaw < 0) follow_yaw += 6.2831853f;
                }
            }
        }
        game::set_walk(walk || follow, walk && in.run, back_step);
        int want = shoot ? 1 : 0;
        if (want != 0 && mode != 0 && want != mode && release == 0) release = 3;
        u16 b = 0;
        bool on = in.gun_on || !in3d;  // in menus the gun reads where it points on the virtual screen
        float gx = in.gun_px, gy = in.gun_py;
        game::set_aim_world(in.aim_world, in3d && in.aim_world_ok);
        game::set_aim_dir_world(in.aim_dir_world, in3d && in.aim_world_ok);
        game::set_lob_target(in.lob_target, in3d && in.lob_target_ok);
        if (in3d && in.aim_world_ok) {
            // projected with the camera the game uses for this frame's shot, not the older one the headset shows
            float px, py;
            if (game::project_world(in.aim_world, px, py)) {
                on = px >= 0 && px < 320 && py >= 0 && py < 240;
                gx = std::max(1.0f, std::min(318.0f, px));
                gy = std::max(1.0f, std::min(238.0f, py));
            }
        }
        if (release > 0) {
            release--;
            if (release == 0) mode = 0;
        } else if (want == 1) {
            b |= PAD_CIR;  // GunCon trigger
            on = true;     // a shot always lands on the picture (clamped to its edge)
            if (mode != 1 && in3d) shots++;
            mode = 1;
        } else {
            mode = 0;
        }
        // The player always faces where the head looks (shots are aimed and doors opened that way, as in the original
        // where one walks forwards); the stick only gives the direction of the steps (set_walk_direction).
        if (walk || follow || (in3d && control)) {
            float dir = mag > 0.2f ? atan2f(in.mx, in.my) : 0.0f;  // stick right = to the right of the gaze
            game::set_body_heading(true, wrap_pi(gaze));
            // a step back is built from the direction opposite to the stick's; other steps from the stick's direction
            // (the game has no side steps: only the step's direction is turned, never the player)
            if (follow) game::set_walk_direction(true, follow_yaw);
            else game::set_walk_direction(walk, wrap_pi(gaze + dir + (back_step ? 3.14159265f : 0.0f)));
        } else {
            game::set_body_heading(false);
            game::set_walk_direction(false);
        }
        // The item / map menu opened with Y (GunCon A + B): besides the buttons (A / B = GunCon A / B, as in every
        // menu; the trigger selects), the left stick moves up / down and left / right too (GunCon A = up / left, B =
        // down / right, one press per push, repeated while held). Y goes back / closes it (A + B).
        // It counts as open from that Y until the player is back in the 3D view for half a second.
        static bool y_menu = false, y_prev = false;
        static int y_back = 0, stick_dir = 0, stick_hold = 0;
        bool y_now = in.btn[B_Y] || in.btn[B_MENU];
        // (Y inside the menu is the game's "back": from Equip / Combine / Check to the list, from the list out of the
        // menu; so the menu counts as closed only once the player has been back in the 3D view for half a second)
        if (y_now && !y_prev && in3d && control) y_menu = true, y_back = 0;
        y_prev = y_now;
        if (y_menu && in3d && control) {
            if (++y_back > 30) y_menu = false;
        } else {
            y_back = 0;
        }
        if (y_menu && !(in3d && control)) {
            // up or left = GunCon A, down or right = GunCon B (the rows of the tabs and of Equip / Combine / Check /
            // Cancel go across with the same two buttons)
            float sx = in.mx, sy = in.my;
            int dir = 0;
            if (fabsf(sy) >= fabsf(sx)) dir = sy > 0.6f ? 1 : sy < -0.6f ? -1 : 0;
            else dir = sx < -0.6f ? 1 : sx > 0.6f ? -1 : 0;
            if (dir != stick_dir) stick_dir = dir, stick_hold = 0;
            else if (dir) stick_hold++;
            // a press of 4 frames when pushed, then every 20 frames while held (after 30)
            bool press = dir && (stick_hold < 4 || (stick_hold >= 30 && (stick_hold - 30) % 20 < 4));
            if (press) b |= dir > 0 ? PAD_START : PAD_X;
            if (in.btn[B_A]) b |= PAD_START;  // (the buttons stay the GunCon's A / B: both ways work)
            if (in.btn[B_B]) b |= PAD_X;
        } else if (!(in3d && control)) {
            // GunCon A / B: in menus only (in the 3D view they would turn the player, which VR does itself)
            if (in.btn[B_A]) b |= PAD_START;
            if (in.btn[B_B]) b |= PAD_X;
        }
        if (y_now) b |= PAD_START | PAD_X;  // A + B: the item / map menu (opens / closes it)
        if (in.vr_menu_open || swallow > 0) b = 0, on = in.gun_on;  // the game waits while the VR options are up
        if (swallow > 0) swallow--;
        // GunCon calibration screen: done automatically (the mapping is exact): an exact shot at the centre of the
        // cross, then A + B; the player never sees it for more than a moment
        bool calib = game::calibration_screen();
        if (calib) {
            cal_t = (cal_t + 1) % 150;
            on = true;
            gx = game::GUN_CROSS_X, gy = game::GUN_CROSS_Y;
            b = (cal_t >= 20 && cal_t < 26) ? PAD_CIR : (cal_t >= 60 && cal_t < 66) ? (u16)(PAD_START | PAD_X) : 0;
        } else {
            cal_t = -1;
        }
        // during play the game's shots land off the calibrated sight: aim the gun so that they land where it points
        // during play: the exact counters for the aimed pixel (the game's own conversion inverted) and no light-gun
        // smoothing; the shot lands on the pixel the pistol points at in the frame the trigger is pulled
        bool playing = in3d && control && !calib;
        gun.gun_dy = 0;
        gun.gun_raw = false;
        if (playing) {
            game::gun_raw_for(gx, gy, gun.gun_raw_x, gun.gun_raw_y);
            gun.gun_raw = true;
        }
        g_pad_buttons = 0;
        gun.buttons = b;
        gun.gun_onscreen = on;
        gun.gun_x = gx;
        gun.gun_y = gy;
        cpu_run_frame();
        frames++;
        // The game placed the player (a room entered with the door behind, a scene over): the world is turned so that
        // the player looks the way the game turned it, wherever the head faces now.
        if (game::take_placement()) {
            placed_world_yaw = wrap_pi(in.head_yaw - game::player_heading());
            placement_seq++;
            logf("placement: heading %.0f deg, world turned", game::player_heading() * 57.2958f);
        }
        // The picture on the screen: a sampled checksum of the displayed area, taken when the game renders and each
        // frame after. When the game stops drawing for seconds (usa_snap_20261119_124528: ~5.5 s in a scene, loading
        // from the disc) and the screen still shows its last frame, the headset keeps that 3D frame instead of the
        // virtual screen; anything new on the screen (a movie, a menu) ends it.
        u32 screen_sum = 0;
        {
            DisplayInfo d = gpu_display();
            const u16 *vr = gpu_vram();
            for (int y = 0; y < d.h; y += 3)
                for (int x = 0; x < d.w; x += 5)
                    screen_sum = screen_sum * 31 + vr[((d.y + y) & 511) * 1024 + ((d.x + x) & 1023)];
            screen_sum ^= (u32)d.x * 2654435761u ^ (u32)d.y;
        }
        static u32 render_sum = 0;
        if (game::render_count() != last_render) {
            last_render = game::render_count(), last_render_emu = frames;
            render_sum = screen_sum;
        }
        // (the frame just after a render may still be the flip: compare from then on; a change ends the stillness)
        static bool frozen_ok = false;
        if (frames - last_render_emu <= 2) frozen_ok = true, render_sum = screen_sum;
        else if (screen_sum != render_sum) frozen_ok = false;
        bool screen_frozen = frozen_ok && frames - last_render_emu >= 25;
        // Crash recorder: a save state every 3 s in states/ring/ (the last three kept); when the game runs into data (an
        // illegal instruction, the crashes after the lift), the oldest of them, 6-9 s before, is kept as
        // states/crash_<time>.st so that the crash can be replayed (the player cannot save once it has happened).
        {
            static int ring_slot = 0;
            static bool ring_ok = false, crash_kept = false;
#ifdef PSXVR_DEV
            if (frames % 150 == 0 && psx::g_illegal_count == 0) {
#else
            if (false) {  // (the crash recorder: development only)
#endif
                CreateDirectoryA("states", nullptr);
                CreateDirectoryA("states\\ring", nullptr);
                char nm[64];
                snprintf(nm, sizeof nm, "states/ring/ring_%d.st", ring_slot);
                ring_ok = psx::state_save(nm) || ring_ok;
                ring_slot = (ring_slot + 1) % 3;
            }
            if (psx::g_illegal_count > 0 && !crash_kept && ring_ok) {
                crash_kept = true;
                char from[64], to[96];
                snprintf(from, sizeof from, "states/ring/ring_%d.st", ring_slot);  // the oldest of the three
                SYSTEMTIME t;
                GetLocalTime(&t);
                snprintf(to, sizeof to, "states/crash_%04d%02d%02d_%02d%02d%02d.st", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                         t.wSecond);
                bool ok = CopyFileA(from, to, FALSE) != 0;
                logf("crash: the game ran into data (pc %08x); state from before kept as %s (%d)", M.cpu.pc, to, (int)ok);
            }
        }
        // Field of view by room. The VR asks the game for a wide view (H = 100, ~116 degrees) so that turning the head
        // shows no edges; the game then prepares more of its world each frame, and in a few large outdoor places (the
        // yard with the four dogs) it fell from ~25 to ~16 frames a second (choppy, and a little slow: it only partly
        // makes up for long frames). A faster emulated CPU, as in the MOH port, made the game hang in the shower (its
        // polygon list looped). So: each room starts wide; while playing there, a second under 22 frames a second
        // narrows the view a step (10, at most to H = 130, ~102 degrees: 25 frames a second there again). A new room
        // starts wide again.
        {
            static const u16 wide = g_gte_h_override;
            static int room = -1, window = 0;
            static u64 window_renders = 0;
            // room_h: the room's own narrowing (a second under 22 frames); local: the narrowing where single frames came
            // very late, only around the place it happened (local_at, game units): 2.5 m away the view is wide again
            static int room_h = wide, local = 0;
            static int local_at[2] = {0, 0};
            static bool window_slow = false;
            int r = game::room_key();
            if (r != room) room = r, room_h = wide, local = 0, window = 0, window_renders = game::render_count();
            bool playing_now = in3d && control && !in.vr_menu_open;
            if (playing_now) last_play_frame = frames, last_play_room = r;
            // Looking towards some places with the wide view the game has much more of its world to go through (the
            // corridor at stage 4 / 0x2a's door: ~25 times the polygons, nearly all thrown away) and draws a frame only
            // every 16-25 vertical syncs. Meanwhile it is neither 'in control' nor drawing: that is still playing (2 s at
            // most, same room, no menu) - the view keeps its last 3D frame instead of the virtual screen, and the field
            // of view narrows at once (below).
            slow_play = !playing_now && in.is3d && !in.vr_menu_open && r == last_play_room &&
                        frames - last_play_frame < 2 * (u64)FRAME_HZ && frames - last_render_emu >= 6;
            bool playing = playing_now || slow_play;
            // a frame 8 syncs late while playing: a step narrower right away (each 8 syncs while it lasts), down to
            // H = wide + 100 (~76 degrees) in the worst places
            {
                static u64 last_step = 0;
                if (slow_play && frames - last_render_emu >= 8 && frames - last_step >= 8 && room_h + local < wide + 100) {
                    local += 10;
                    last_step = frames;
                    local_at[0] = game::player_pos(0), local_at[1] = game::player_pos(2);
                    logf("view narrowed to H %d here (room %04x: a frame %d syncs late)", room_h + local, room,
                         (int)(frames - last_render_emu));
                }
                if (local > 0) {
                    float dx = (float)(game::player_pos(0) - local_at[0]), dz = (float)(game::player_pos(2) - local_at[1]);
                    if (dx * dx + dz * dz > 2500.0f * 2500.0f) {
                        local = 0;
                        logf("view wide again (H %d): away from the slow place", room_h);
                    }
                }
                window_slow |= slow_play;
            }
            // The emulated CPU, while playing, does an instruction in 1.5 cycles instead of the core's 2 (the PS1's
            // R3000A is nearer 1): the game keeps its 25 frames a second in more places (the dog yard: 24 instead of 16
            // with the wide view). It cannot go faster than that (it waits for two vertical syncs a frame), and at 1
            // cycle, when its frames came in one sync, it hung (a sprite linked twice into its polygon list). Movies and
            // menus keep the core's timing. PSXVR_CPU_CPI=2 for the old timing.
            {
                static const u32 q8 = getenv("PSXVR_CPU_CPI") ? (u32)(atof(getenv("PSXVR_CPU_CPI")) * 256.0) : 384;
                psx::g_cpu_cpi_q8 = playing ? q8 : 0;
            }
            if (!playing) window = 0, window_renders = game::render_count(), window_slow = false;
            else if (++window >= (int)FRAME_HZ) {
                u64 n = game::render_count() - window_renders;  // drawn frames in the last second
                // (a second with very late frames is the local narrowing's: the whole room is not narrowed for it)
                if (n < 22 && !window_slow && room_h < wide + 30) {
                    room_h += 10;
                    logf("view narrowed to H %d (room %04x: %d frames a second)", room_h, room, (int)n);
                }
                window = 0, window_renders = game::render_count(), window_slow = false;
            }
            g_gte_h_override = (u16)(room_h + local);
        }
        if (in.snap) {
            snap_name = frontend_snapshot(in.snap_notes);
            snaps++;
        }
        // hand over the results, the latest captured frame and the game's picture
        bool new_cap = g_cap_done.number != last_cap;
        std::vector<u32> px;
        int pw = 0, phh = 0;
        frontend_get_frame(px, pw, phh);
        {
            std::lock_guard<std::mutex> lk(emu.m);
            if (new_cap) {
                last_cap = g_cap_done.number;
                last_cap_emu = frames;
                emu.cap = g_cap_done;
                emu.vram.assign(gpu_vram(), gpu_vram() + 1024 * 512);
                emu.cap_new = true;
            }
            emu.pix.swap(px);
            emu.pix_w = pw, emu.pix_h = phh;
            emu.pix_new = true;
            EmuOut &o = emu.out;
            o.frames = frames;
            o.last_render = last_render_emu;
            o.last_cap = last_cap_emu;
            o.placement_seq = placement_seq;
            o.placement_world_yaw = placed_world_yaw;
            o.shots = shots;
            o.player_heading = game::player_heading();
            o.in_control = control;
            o.weapon = game::equipped_weapon();
            o.lob = game::lob_info(o.lob_origin, o.lob_speed, o.lob_start, o.lob_across);
            o.slow_play = slow_play;
            o.screen_frozen = screen_frozen;
            o.snaps = snaps;
            o.snap_name = snap_name;
            o.loads = loads;
            o.load_ok = load_ok;
        }
    }
}

}  // namespace

int vr_main(int, char **) {
    // ---- instance
    const char *exts[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ci.applicationInfo.applicationName, "Resident Evil Survivor VR");
    ci.applicationInfo.applicationVersion = 1;
    strcpy(ci.applicationInfo.engineName, "psxvr");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = exts;
    XrInstance inst;
    XRCHECK(xrCreateInstance(&ci, &inst));
    {
        XrInstanceProperties ip = {XR_TYPE_INSTANCE_PROPERTIES};
        xrGetInstanceProperties(inst, &ip);
        logf("OpenXR runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
             XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
    }
    XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys;
    XRCHECK(xrGetSystem(inst, &sgi, &sys));

    // ---- D3D11 device on the headset's adapter
    PFN_xrGetD3D11GraphicsRequirementsKHR getReq = nullptr;
    XRCHECK(xrGetInstanceProcAddr(inst, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction *)&getReq));
    XrGraphicsRequirementsD3D11KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XRCHECK(getReq(inst, sys, &req));
    IDXGIFactory1 *factory;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory);
    IDXGIAdapter1 *adapter = nullptr, *a;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        if (memcmp(&d.AdapterLuid, &req.adapterLuid, sizeof(LUID)) == 0) { adapter = a; break; }
        a->Release();
    }
    D3D_FEATURE_LEVEL fl = std::max(req.minFeatureLevel, D3D_FEATURE_LEVEL_11_0);
    if (FAILED(D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                 &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        fail("D3D11CreateDevice");
        return 1;
    }

    // ---- session
    XrGraphicsBindingD3D11KHR gb = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    gb.device = dev;
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = sys;
    XrSession sess;
    XRCHECK(xrCreateSession(inst, &sci, &sess));

    XrReferenceSpaceCreateInfo rs = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1;
    XrSpace space;
    XRCHECK(xrCreateReferenceSpace(sess, &rs, &space));

    // ---- swapchains
    uint32_t nfmt;
    xrEnumerateSwapchainFormats(sess, 0, &nfmt, nullptr);
    std::vector<int64_t> fmts(nfmt);
    xrEnumerateSwapchainFormats(sess, nfmt, &nfmt, fmts.data());
    int64_t fmt = fmts.empty() ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : fmts[0];
    for (int64_t f : fmts)
        if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) { fmt = f; break; }
    srgb = fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

    uint32_t nviews;
    xrEnumerateViewConfigurationViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nviews, nullptr);
    std::vector<XrViewConfigurationView> vcv(nviews, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, nviews, &nviews, vcv.data());
    std::vector<EyeSwap> eyes(nviews);
    for (uint32_t e = 0; e < nviews; e++) {
        XrSwapchainCreateInfo sc = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sc.format = fmt;
        sc.sampleCount = 1;
        sc.width = vcv[e].recommendedImageRectWidth;
        sc.height = vcv[e].recommendedImageRectHeight;
        sc.faceCount = sc.arraySize = sc.mipCount = 1;
        XRCHECK(xrCreateSwapchain(sess, &sc, &eyes[e].sc));
        eyes[e].w = sc.width;
        eyes[e].h = sc.height;
        uint32_t n;
        xrEnumerateSwapchainImages(eyes[e].sc, 0, &n, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(eyes[e].sc, n, &n, (XrSwapchainImageBaseHeader *)imgs.data());
        for (auto &im : imgs) {
            D3D11_RENDER_TARGET_VIEW_DESC rd = {};
            rd.Format = (DXGI_FORMAT)fmt;
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            ID3D11RenderTargetView *rtv;
            dev->CreateRenderTargetView(im.texture, &rd, &rtv);
            eyes[e].imgs.push_back({im.texture, rtv});
        }
        D3D11_TEXTURE2D_DESC dd = {};
        dd.Width = eyes[e].w;
        dd.Height = eyes[e].h;
        dd.MipLevels = dd.ArraySize = 1;
        dd.Format = DXGI_FORMAT_D32_FLOAT;
        dd.SampleDesc.Count = 1;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ID3D11Texture2D *dtex;
        dev->CreateTexture2D(&dd, nullptr, &dtex);
        dev->CreateDepthStencilView(dtex, nullptr, &eyes[e].dsv);
    }
    logf("VR: %u views %dx%d, format %lld", nviews, eyes[0].w, eyes[0].h, (long long)fmt);
    Mirror mirror;
    if (!getenv("RESVR_NO_WINDOW")) {
        if (mirror.create(dev, factory, srgb)) logf("desktop window: %dx%d", mirror.w, mirror.h);
        else logf("desktop window: not available");
    }

    if (!create_pipeline()) { fail("shader compile"); return 1; }
    if (!vrhands::init(dev, ctx)) { fail("hands shader compile"); return 1; }
    VrOptions opt;
    opt.load();
    game::set_headshot(opt.headshot);
    TextPanel menu_panel, hint_panel;
    int menu_sel = 0;
    bool vr_menu_open = false;
    int menu_swallow = 0;  // emulated frames the game still gets no input after the panel closed
    Mat4 menu_pose = identity(), hint_pose = identity();
    auto hint_until = std::chrono::steady_clock::now();
    bool run_on = false;            // left stick click while walking: run until the stick is let go
    int held_weapon = 0;            // the equipped weapon (from the emulation thread)
    bool emu_in_control = false;    // the player controls the game (from the emulation thread, last exchange)
    bool following = false, follow_blocked = false, have_prev_eye = false;  // room scale (see below)
    float follow_block_yaw = 0, follow_dir_local = 0, prev_eye[2] = {0, 0}, follow_moved = 0;
    int follow_frames = 0;
    std::chrono::steady_clock::time_point follow_until{};
    bool snap_request = false;      // right stick click / F5 / options panel: save a snapshot after the emulated frames
    std::string snap_name;
    std::string load_request;       // options panel: load this save
    std::vector<std::string> load_list;  // saves of this disc, newest first
    int load_pick = 0;
    unsigned loads_seen = 0;
    std::string load_msg;
    auto snap_until = std::chrono::steady_clock::now();
    TextPanel snap_panel;
    std::string snap_text;
    Mat4 snap_pose = identity();
    bool show_hint = false;
    if (!scene::init(dev, ctx)) { fail("scene shader compile"); return 1; }
    // True 3D: track vertices, capture primitives, widen the game's field of view, drive its view.
    g_pgxp = g_capture = true;
    if (!g_gte_h_override) g_gte_h_override = 100;  // ~116 deg horizontal, ~100 deg vertical (320 x 240)
    XrReferenceSpaceCreateInfo vsi = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    vsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    vsi.poseInReferenceSpace.orientation.w = 1;
    XrSpace view_space;
    XRCHECK(xrCreateReferenceSpace(sess, &vsi, &view_space));
    u64 last_cap = 0, last_cap_emu = 0, last_render = 0, last_render_emu = 0;
    float head_yaw = 0, head_pitch = 0;
    // LOCAL yaw of the game's world: LOCAL heading = game heading + world_yaw. Turning round for real turns the view
    // (the game draws what the head faces); the snap turn moves world_yaw.
    float world_yaw = 0;
    bool world_init = false;
    float screen_yaw = 0;              // the virtual screen / menus face this LOCAL yaw (set by recentring)
    float pos_ref[3] = {0, 0, 0};      // LOCAL position of the game's eye (set by recentring)
    Mat4 head_mat = identity();
    bool is3d = false, prev_recenter = false;
    vrhands::HandState hands[2];
    float frame_pitch = 0, frame_abs_yaw = 0;
    bool cine_on = false;           // a cut-scene in 3D: the game's camera, placed in front of where the player looked
    float cine_yaw = 0;             // LOCAL yaw the scene's camera faces (the head's when the scene began)
    float cine_head_pitch = 0;      // the head's pitch when the scene began
    // A scene's camera may look steeply down or up (the lift going down its shaft, seen from 58 degrees above): placed
    // with its own pitch it was far below a player looking ahead, who saw only black. Its pitch is kept within 25
    // degrees of where the player looked when the scene began (steeper cameras: the scene is tilted that much less).
    auto place_pitch = [&](float pitch) {
        if (!cine_on) return pitch;
        const float LIM = 25.0f * 0.0174533f;
        return cine_head_pitch + std::max(-LIM, std::min(LIM, pitch - cine_head_pitch));
    };
    size_t screen_prims = 0;        // primitives of the last captured picture (a movie has only one or two)
    bool screen_movie = false;      // the last captured picture is a movie playing (game::movie_frame)
    bool screen_picture = false;    // a picture covers the screen over the 3D view (game::full_picture)
    bool picture_feedback = false;  // the frame shown blends the previous picture over itself (the game's blur)
    int disp_w = 320, disp_h = 240;
    CapFrame aim_frame;          // stamp (eye, heading, pitch) of the frame the world is drawn from
    float aim_world[3] = {0, 0, 0};
    bool aim_world_ok = false;
    float aim_dir_world[3] = {0, 0, -1};
    bool lob_on = false;               // the grenade gun is held: its aim is the grenade's arc (from the emulation)
    float lob_origin[3] = {0, 0, 0}, lob_speed = 0, lob_start = 0;
    bool lob_across = false;
    float lob_target[3] = {0, 0, 0};   // where the arc meets the room (game world): the grenade is thrown there
    bool lob_target_ok = false;
    bool frame_abs_valid = false;
    ID3D11ShaderResourceView *hud_srv = nullptr;
    if (!create_actions(inst, sess)) logf("VR: action setup failed, controller input disabled");
    bool have_audio = frontend_audio_init();
    game::set_exact_aim(true);
    std::thread emu_thread(emu_thread_main);
    // the GunCon in port 1 (set at boot by main for --vr)
    float gun_px = 160, gun_py = 120;  // where the pistol points on the game's picture
    bool gun_on = false;               // ... and whether that is on the picture
    float aim_dist = 4;                // metres to what the ray hits (drawn ray length)
    Hyst trig_h, turn_l, turn_r;
    bool prev_btn[B_COUNT] = {};

    // ---- main loop
    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();
    auto prev_loop = clk::now();
    u64 emu_frames = 0;
    bool eo_slow_play = false;  // (EmuOut::slow_play of the last hand-over)
    bool eo_screen_frozen = false;  // (EmuOut::screen_frozen)
    bool running = false, quit = false;
    while (!quit && !M.quit) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        if (g_mirror_closed) break;  // the desktop window was closed: end the game
        XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(inst, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto *sc = (XrEventDataSessionStateChanged *)&ev;
                logf("VR: session state %d", (int)sc->state);
                if (sc->state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    xrBeginSession(sess, &bi);
                    running = true;
                    emu_frames = 0;
                } else if (sc->state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(sess);
                    running = false;
                    std::lock_guard<std::mutex> lk(emu.m);
                    emu.in.running = false;  // the game pauses while the headset is off
                } else if (sc->state == XR_SESSION_STATE_EXITING || sc->state == XR_SESSION_STATE_LOSS_PENDING) {
                    quit = true;
                }
            } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                quit = true;
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (!running) {
            Sleep(10);
            continue;
        }

        XrFrameState fs = {XR_TYPE_FRAME_STATE};
        if (XR_FAILED(xrWaitFrame(sess, nullptr, &fs))) break;
        xrBeginFrame(sess, nullptr);
        float dt = std::min(0.1f, std::chrono::duration<float>(clk::now() - prev_loop).count());
        prev_loop = clk::now();
        {
            // timing log (psxvr.log every 5 s): headset frame interval, slow frames, emulated frames, captured frames
            static int n = 0, slow = 0;
            static float sum = 0, worst = 0;
            static auto since = clk::now();
            static u64 emu0 = 0, cap0 = 0;
            n++, sum += dt, worst = std::max(worst, dt);
            if (dt > 0.016f) slow++;
            float span = std::chrono::duration<float>(clk::now() - since).count();
            if (span >= 5) {
                logf("timing: headset %.1f fps (worst %.1f ms, %d frames over 16 ms), game %.1f fps, 3D frames %.1f/s",
                     n / span, worst * 1000, slow, (emu_frames - emu0) / span, (last_cap - cap0) / span);
                n = slow = 0, sum = worst = 0, since = clk::now(), emu0 = emu_frames, cap0 = last_cap;
            }
        }

        // Head pose in LOCAL space
        {
            XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
            if (XR_SUCCEEDED(xrLocateSpace(view_space, space, fs.predictedDisplayTime, &loc)) &&
                (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
                const XrQuaternionf &q = loc.pose.orientation;
                // forward = R * (0,0,-1)
                float fx = -2 * (q.x * q.z + q.y * q.w), fy = -2 * (q.y * q.z - q.x * q.w);
                float fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
                head_yaw = atan2f(fx, -fz);
                head_pitch = asinf(std::max(-1.0f, std::min(1.0f, fy)));
                head_mat = pose_matrix(loc.pose);
            }
        }
        poll_actions(sess);
        bool pressed[B_COUNT];
        for (int i = 0; i < B_COUNT; i++) pressed[i] = btn_state[i] && !prev_btn[i], prev_btn[i] = btn_state[i];
        locate_hands(sess, space, fs.predictedDisplayTime, hands);
#ifdef PSXVR_DEV
        {
            static bool f5_was = false;
            bool f5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
            if (f5 && !f5_was) snap_request = true;
            f5_was = f5;
        }
#endif
        const int ph = opt.pistol_hand;
        // the weapon in the hand: the game's own model when it has one (Handgun C), else the built-in pistol
        vrhands::set_pistol_model((const vrhands::PistolModel *)game::weapon_model(held_weapon));
        for (int h = 0; h < 2; h++) {
            hands[h].pistol = h == ph;
            hands[h].kick = 0;  // no visual recoil (it moved the aim right after a shot)
            hands[h].ray_dot = false;
            hands[h].ray_len = 4;
            hands[h].support = false;
        }
        // The shotgun with both hands: the other hand's grip (squeeze) on the pump - 41 cm behind the muzzle, 2 cm
        // under the barrel's line (measured in the headset: usa_snap_20261121_143126) - holds it there; the barrel
        // then points from the weapon hand through the other hand. It starts within 15 cm of the pump and lasts while
        // the grip is held.
        {
            static bool two_hands = false;
            const vrhands::PistolModel *pm = (const vrhands::PistolModel *)game::weapon_model(held_weapon);
            const int oh = 1 - ph;
            bool can = held_weapon == 5 && pm && hands[ph].valid && hands[oh].valid;
            if (!can || hands[oh].squeeze < 0.35f) two_hands = false;
            if (can) {
                float anchor[3] = {0, pm->muzzle_y - 0.020f, pm->muzzle_z + 0.413f};
                if (!two_hands && hands[oh].squeeze > 0.6f) {
                    Mat4 pf = vrhands::pistol_muzzle(hands[ph]);  // (one hand: where the pump is now)
                    float p[3], dd = 0;
                    for (int r = 0; r < 3; r++) {
                        p[r] = pf.m[r][1] * (anchor[1] - pm->muzzle_y) + pf.m[r][2] * (anchor[2] - pm->muzzle_z) + pf.m[r][3];
                        float e = p[r] - hands[oh].grip.m[r][3];
                        dd += e * e;
                    }
                    two_hands = dd < 0.15f * 0.15f;
                }
                if (two_hands) {
                    hands[ph].support = true;
                    for (int r = 0; r < 3; r++)
                        hands[ph].support_pos[r] = hands[oh].grip.m[r][3], hands[ph].support_anchor[r] = anchor[r];
                }
            }
        }
        // (a long frame while playing keeps the last 3D frame: no flash of the virtual screen)
        bool in_3d_now = is3d && ((emu_frames - last_render_emu < 25 && emu_frames - last_cap_emu < 20) || eo_slow_play ||
                                  eo_screen_frozen);

        // ---- VR options panel (X in a menu screen): the stick picks, A changes, B / X closes
        {
            static int stick_dir = 0, stick_xdir = 0;
            float sy = fabsf(stick_move[1]) > fabsf(stick_turn[1]) ? stick_move[1] : stick_turn[1];
            int dir = sy > 0.6f ? 1 : sy < -0.6f ? -1 : (fabsf(sy) < 0.3f ? 0 : stick_dir);
            int step = dir != stick_dir ? dir : 0;
            stick_dir = dir;
            float sx = fabsf(stick_move[0]) > fabsf(stick_turn[0]) ? stick_move[0] : stick_turn[0];
            int xdir = sx > 0.6f ? 1 : sx < -0.6f ? -1 : (fabsf(sx) < 0.3f ? 0 : stick_xdir);
            int xstep = xdir != stick_xdir ? xdir : 0;
            stick_xdir = xdir;
            auto ahead = [&](float dist, float down) {
                float hx = head_mat.m[0][3], hy = head_mat.m[1][3], hz = head_mat.m[2][3];
                return mul(mat_translate(hx + sinf(head_yaw) * dist, hy - down, hz - cosf(head_yaw) * dist), mat_rot_y(-head_yaw));
            };
            static bool prev_menu_screen = false;
            bool menu_screen = !in_3d_now;
            prev_menu_screen = menu_screen;
            // the hint only for a few seconds after the player opens the game's menu (Y), not on every screen
            show_hint = false;  // no "X: VR options" reminder (the player knows the key)
            (void)hint_until;
            if (!vr_menu_open && menu_screen && pressed[B_X]) {
                vr_menu_open = true;
                menu_sel = 0;
                load_list = list_saves();
                load_pick = 0;
                menu_pose = ahead(1.0f, 0.12f);
            } else if (vr_menu_open) {
                if (pressed[B_X] || pressed[B_B]) vr_menu_open = false, menu_swallow = 12;
#ifdef PSXVR_DEV
                const int N = 8, LOAD_LINE = 7;  // (development: also "save a state for analysis")
#else
                const int N = 7, LOAD_LINE = 6;  // (saves: the right stick's click saves, this line loads)
#endif
                if (step) menu_sel = (menu_sel - step + N) % N;  // stick up = previous line
                int change = pressed[B_A] ? 1 : xstep;
                if (change) {
                    if (menu_sel == 0) opt.pistol_hand ^= 1;
                    if (menu_sel == 1) opt.laser = !opt.laser;
                    if (menu_sel == 2) opt.turn = pressed[B_A] ? (opt.turn + 1) % 4 : std::max(0, std::min(3, opt.turn + change));
                    if (menu_sel == 3) opt.hud = !opt.hud;
                    if (menu_sel == 4) opt.hud_dist = pressed[B_A] ? (opt.hud_dist + 1) % 3 : std::max(0, std::min(2, opt.hud_dist + change));
                    if (menu_sel == 5) opt.headshot = !opt.headshot, game::set_headshot(opt.headshot);
#ifdef PSXVR_DEV
                    if (menu_sel == 6 && pressed[B_A]) snap_request = true;
#endif
                    if (menu_sel == LOAD_LINE && !load_list.empty()) {
                        if (pressed[B_A]) {
                            load_request = "states/" + load_list[load_pick];
                            vr_menu_open = false, menu_swallow = 12;
                        } else {
                            // stick right = older, left = newer
                            load_pick = std::max(0, std::min((int)load_list.size() - 1, load_pick + change));
                        }
                    }
                    opt.save();
                }
            }
            if (vr_menu_open) {
                static const char *turn_names[4] = {"<  NONE  >", "<  SNAP 30\xB0  >", "<  SNAP 45\xB0  >", "<  SMOOTH  >"};
                static const char *dist_names[3] = {"<  NEAR  >", "<  NORMAL  >", "<  FAR  >"};
                std::vector<std::string> lines = {
                    "#VR OPTIONS",
                    std::string(menu_sel == 0 ? ">" : "") + "Gun hand\t" + (opt.pistol_hand ? "RIGHT" : "LEFT"),
                    std::string(menu_sel == 1 ? ">" : "") + "Aiming laser\t" + (opt.laser ? "ON" : "OFF"),
                    std::string(menu_sel == 2 ? ">" : "") + "Right stick turning\t" + turn_names[opt.turn],
                    std::string(menu_sel == 3 ? ">" : "") + "Game HUD\t" + (opt.hud ? "ON" : "OFF"),
                    std::string(menu_sel == 4 ? ">" : "") + "HUD distance\t" + dist_names[opt.hud_dist],
                    std::string(menu_sel == 5 ? ">" : "") + "Headshot visual feedback\t" + (opt.headshot ? "ON" : "OFF"),
                };
#ifdef PSXVR_DEV
                lines.push_back(std::string(menu_sel == 6 ? ">" : "") + "Save state for analysis\t" +
                                (snap_name.empty() ? "A" : snap_name));
                lines.push_back(std::string(menu_sel == 7 ? ">" : "") + "Load state\t" +
                                (load_list.empty() ? std::string("NONE")
                                                   : "<  " + save_label(load_list[load_pick]) + "  >  " +
                                                         std::to_string(load_pick + 1) + "/" + std::to_string(load_list.size())));
                lines.push_back("Stick: select / adjust     A: change / load     B / X: close");
#else
                lines.push_back(std::string(menu_sel == 6 ? ">" : "") + "Load save\t" +
                                (load_list.empty() ? std::string("NONE")
                                                   : "<  " + save_label(load_list[load_pick]) + "  >  " +
                                                         std::to_string(load_pick + 1) + "/" + std::to_string(load_list.size())));
                lines.push_back("Stick: select / adjust     A: change / load     B / X: close");
                lines.push_back("Right stick click: save");
#endif
                menu_panel.render(lines, 1024, 640);
            } else if (show_hint) {
                hint_panel.render({"X: VR options"}, 512, 96);
            }
        }

        // ---- orientation of the game world in LOCAL, snap turn, recentre
        float body = emu.out.player_heading;  // (read without the lock: only for the first alignment)
        if (!world_init) {
            world_yaw = head_yaw - body;  // start looking where the player faces
            screen_yaw = head_yaw;
            for (int c = 0; c < 3; c++) pos_ref[c] = head_mat.m[c][3];
            world_init = true;
        }
        // right stick click: a short press saves a snapshot, held 0.6 s it recentres (virtual screen, eye position)
        {
            static float rc_held = 0;
            static bool rc_done = false;
            if (recenter_state) {
                rc_held += dt;
                if (!rc_done && rc_held >= 0.6f) {
                    screen_yaw = head_yaw;
                    for (int c = 0; c < 3; c++) pos_ref[c] = head_mat.m[c][3];
                    rc_done = true;
                    haptic_pulse(sess, 1, 0.3f, 0.05f);
                }
            } else {
                if (prev_recenter && !rc_done) snap_request = true;  // a short click: save (load it from the VR options)
                rc_held = 0, rc_done = false;
            }
        }
        prev_recenter = recenter_state;
        // left stick click: run, until the stick is let go
        {
            float m = sqrtf(stick_move[0] * stick_move[0] + stick_move[1] * stick_move[1]);
            if (pressed[B_RUN] && m > 0.3f) run_on = true;
            if (m < 0.2f) run_on = false;
        }
        if (in_3d_now && !vr_menu_open) {
            float tx = stick_turn[0];
            if (opt.turn == 1 || opt.turn == 2) {
                float step = opt.turn == 1 ? 0.5235988f : 0.7853982f;
                static bool was_r = false, was_l = false;
                bool r = turn_r.step(tx, 0.6f, 0.3f), l = turn_l.step(-tx, 0.6f, 0.3f);
                // turning right: the world turns left round the player (its LOCAL yaw decreases)
                if (r && !was_r) world_yaw -= step;
                if (l && !was_l) world_yaw += step;
                was_r = r, was_l = l;
            } else if (opt.turn == 3 && fabsf(tx) > 0.2f) {
                world_yaw -= (fabsf(tx) - 0.2f) / 0.8f * (tx > 0 ? 1.0f : -1.0f) * 2.0f * dt;  // up to ~115 deg/s
            }
            world_yaw = wrap_pi(world_yaw);
        }
        // The game draws what the head faces: game heading = LOCAL yaw - world_yaw, the head's pitch, level horizon
        float view_game_yaw = wrap_pi(head_yaw - world_yaw);
        float view_lead_yaw = 0, view_lead_pitch = 0;
        if (view_game_yaw < 0) view_game_yaw += 6.2831853f;
        // The game draws 25 frames a second from where the head looks when a frame starts; turning, the edge of what
        // it drew trailed the gaze in steps. It is asked to draw where the head will look ~70 ms later (head turn rate,
        // smoothed); each frame is still placed exactly with the view it was drawn with.
        static float prev_hy = 0, prev_hp = 0, rate_y = 0, rate_p = 0;
        static bool have_prev = false;
        if (have_prev && dt > 0.0005f) {
            float ry = wrap_pi(head_yaw - prev_hy) / dt, rp = (head_pitch - prev_hp) / dt;
            float k = std::min(1.0f, dt * 12.0f);
            rate_y += (ry - rate_y) * k;
            rate_p += (rp - rate_p) * k;
        }
        prev_hy = head_yaw, prev_hp = head_pitch, have_prev = true;
        const float LEAD = 0.0f;  // no prediction: the game draws where the head looks (hysteresis in game.cpp)
        float lead_y = std::max(-0.35f, std::min(0.35f, rate_y * LEAD)), lead_p = std::max(-0.3f, std::min(0.3f, rate_p * LEAD));
        // (handed to the emulation thread below)
        view_lead_yaw = wrap_pi(view_game_yaw + lead_y);
        if (view_lead_yaw < 0) view_lead_yaw += 6.2831853f;
        view_lead_pitch = std::max(-1.4f, std::min(1.4f, head_pitch + lead_p));

        // ---- the pistol: where it points on the game's picture
        Mat4 scene_to_local = identity();
        {
            float cam_local_yaw = cine_on ? cine_yaw : frame_abs_valid ? frame_abs_yaw + world_yaw : head_yaw;
            scene_to_local = mul(mat_translate(pos_ref[0], pos_ref[1], pos_ref[2]),
                                 mul(mat_rot_y(-cam_local_yaw), mat_rot_x(place_pitch(frame_pitch))));
        }
        // virtual screen for menus / movies / cut-scenes: 2.4 m x 1.8 m (4:3), 2.2 m ahead of the recentred forward
        Mat4 screen_rigid = mul(mat_translate(pos_ref[0], pos_ref[1], pos_ref[2]),
                                mul(mat_rot_y(-screen_yaw), mat_translate(0, 0, -2.2f)));
        const float SCR_W = 1.2f, SCR_H = 0.9f;
        Mat4 screen_model = mul(screen_rigid, mat_scale(SCR_W, SCR_H, 1));
        hands[0].arc_n = hands[1].arc_n = 0;  // (the grenade gun's arc: set again below while it is aimed in 3D)
        if (hands[ph].valid) {
            Mat4 mz = vrhands::pistol_muzzle(hands[ph]);
            float o[3] = {mz.m[0][3], mz.m[1][3], mz.m[2][3]}, d[3] = {-mz.m[0][2], -mz.m[1][2], -mz.m[2][2]};
            int pw = disp_w > 0 ? disp_w : 320, phh = disp_h > 0 ? disp_h : 240;
            if (in_3d_now) {
                // into the captured frame's camera space (inverse of scene_to_local)
                Mat4 inv = mat_rigid_inverse(scene_to_local);
                float so[3], sd[3];
                for (int r = 0; r < 3; r++) {
                    so[r] = inv.m[r][0] * o[0] + inv.m[r][1] * o[1] + inv.m[r][2] * o[2] + inv.m[r][3];
                    sd[r] = inv.m[r][0] * d[0] + inv.m[r][1] * d[1] + inv.m[r][2] * d[2];
                }
                float t = 0;
                aim_dist = scene::raycast(so, sd, t) && t < 60 ? t : 30;
                // the aimed point in the game world: the emulation projects it with the camera the game uses at the
                // shot (the frame shown here is older, and the game's camera may have moved since)
                {
                    float sp[3] = {so[0] + sd[0] * aim_dist, so[1] + sd[1] * aim_dist, so[2] + sd[2] * aim_dist};
                    game::scene_to_world(aim_frame, sp, aim_world);
                    aim_world_ok = frame_abs_valid;
                    float w0[3], w1[3], s1[3] = {so[0] + sd[0], so[1] + sd[1], so[2] + sd[2]};
                    game::scene_to_world(aim_frame, so, w0);
                    game::scene_to_world(aim_frame, s1, w1);
                    float l = 0;
                    for (int i = 0; i < 3; i++) aim_dir_world[i] = w1[i] - w0[i], l += aim_dir_world[i] * aim_dir_world[i];
                    l = sqrtf(l);
                    for (int i = 0; i < 3; i++) aim_dir_world[i] /= std::max(1e-6f, l);
                    // The grenade gun: the grenade flies along the barrel and falls (game::lob_info: from the head
                    // point, 14 units a logic step more downwards each step, measured); the laser is drawn as that
                    // arc, starting at the muzzle (the offset between the muzzle and where the grenade really starts
                    // fades out over its first 1.5 m, so the arc ends where the grenade lands), ending where it meets
                    // the room.
                    hands[ph].arc_n = 0;
                    lob_target_ok = false;
                    if (lob_on && frame_abs_valid && lob_speed > 0) {
                        // from the muzzle along the barrel, falling straight down (in the barrel's upright plane); the
                        // game is then told where it ends and throws its grenade there from its own starting point
                        float p[3], v[3];
                        for (int i = 0; i < 3; i++) p[i] = w0[i], v[i] = aim_dir_world[i] * lob_speed;
                        if (lob_across) {  // grenade rounds: the speed across is fixed, up / down follows the tilt
                            const float *u = aim_dir_world;
                            float h = std::max(0.26f, sqrtf(u[0] * u[0] + u[2] * u[2]));  // (at most ~75 degrees)
                            v[0] = u[0] / h * lob_speed, v[2] = u[2] / h * lob_speed, v[1] = u[1] / h * lob_speed * 1.04f;
                        }
                        Mat4 &L = scene_to_local;
                        auto to_local = [&](const float w[3], float out[3]) {
                            float s[3];
                            game::world_to_scene(aim_frame, w, s);
                            for (int r = 0; r < 3; r++)
                                out[r] = L.m[r][0] * s[0] + L.m[r][1] * s[1] + L.m[r][2] * s[2] + L.m[r][3];
                        };
                        float prev_s[3];
                        game::world_to_scene(aim_frame, w0, prev_s);
                        to_local(w0, hands[ph].arc[0]);
                        int n = 1;
                        bool hit = false;
                        for (int step = 0; step < 60 && n < 64 && !hit; step++) {
                            v[1] += 14.0f;
                            for (int i = 0; i < 3; i++) p[i] += v[i];
                            float qs[3];
                            game::world_to_scene(aim_frame, p, qs);
                            float dd[3] = {qs[0] - prev_s[0], qs[1] - prev_s[1], qs[2] - prev_s[2]};
                            float len = sqrtf(dd[0] * dd[0] + dd[1] * dd[1] + dd[2] * dd[2]);
                            if (len < 1e-5f) continue;
                            for (int i = 0; i < 3; i++) dd[i] /= len;
                            float th = 0;
                            if (scene::raycast(prev_s, dd, th) && th < len) {
                                for (int i = 0; i < 3; i++) qs[i] = prev_s[i] + dd[i] * th;
                                hit = true;
                            }
                            for (int r = 0; r < 3; r++)
                                hands[ph].arc[n][r] = L.m[r][0] * qs[0] + L.m[r][1] * qs[1] + L.m[r][2] * qs[2] + L.m[r][3];
                            n++;
                            memcpy(prev_s, qs, sizeof prev_s);
                        }
                        hands[ph].arc_n = n;
                        lob_target_ok = hit;
                        if (hit) game::scene_to_world(aim_frame, prev_s, lob_target);
                    }
                }
                float px, py;
                if (game::gun_pixel(so, sd, px, py)) {
                    // outside the picture the gun would read "off screen" and the trigger walk: keep it on the edge
                    gun_on = px >= 0 && px < pw && py >= 0 && py < phh;
                    gun_px = std::max(1.0f, std::min(pw - 2.0f, px));
                    gun_py = std::max(1.0f, std::min(phh - 2.0f, py));
                } else {
                    gun_on = false;
                }
                hands[ph].ray_dot = true;
                hands[ph].ray_len = aim_dist;
            } else {
                float px, py, dist;
                gun_on = screen_hit(screen_rigid, SCR_W, SCR_H, o, d, game_w > 0 ? game_w : 320, game_h > 0 ? game_h : 240,
                                    px, py, dist);
                if (gun_on) {
                    gun_px = std::max(1.0f, std::min((game_w > 0 ? game_w : 320) - 2.0f, px));
                    gun_py = std::max(1.0f, std::min((game_h > 0 ? game_h : 240) - 2.0f, py));
                    hands[ph].ray_len = dist;
                    hands[ph].ray_dot = true;
                }
            }
        }
        bool trigger_held = trig_h.step(hands[ph].valid ? hands[ph].trigger : 0.0f, 0.55f, 0.35f);

        // ---- hand the input to the emulation thread, take its results
        static unsigned shots_seen = 0, snaps_seen = 0, placement_taken = 0;
        EmuOut eo;
        bool got_cap = false, got_pix = false;
        static CapFrame cap;          // the frame the scene is built from (this thread's copy)
        static std::vector<u16> cap_vram;
        static std::vector<u32> pix;
        static int pix_w = 0, pix_h = 0;
        // ---- room scale: the player's eye follows the headset. When the head is more than 10 cm (horizontally) from
        // the game's eye and the stick is not used, the game is asked to step that way (its walking, so walls, doors,
        // pick-ups and enemies see the player where the player really is); each step the eye takes moves the anchor
        // of the world by as much (pos_ref, below), so the world stays still in the room. Against a wall (the eye does
        // not move) it stops trying until the head goes another way.
        bool follow_now = false;
        float follow_tx = 0, follow_tz = 0;
        {
            float dx = head_mat.m[0][3] - pos_ref[0], dz = head_mat.m[2][3] - pos_ref[2];
            float d = sqrtf(dx * dx + dz * dz);
            float sm = sqrtf(stick_move[0] * stick_move[0] + stick_move[1] * stick_move[1]);
            bool can = in_3d_now && emu_in_control && !cine_on && !vr_menu_open && sm < 0.2f;
            float lyaw = atan2f(dx, -dz);  // LOCAL yaw of the way to the head
            // blocked: stays so while the head keeps pushing that way (the drag keeps it at the wall); free again when it
            // turns away from the obstacle (more than ~45 degrees) or the stick is used
            if (follow_blocked && ((d > 0.06f && fabsf(wrap_pi(lyaw - follow_block_yaw)) > 0.8f) || !can)) follow_blocked = false;
            following = can && !follow_blocked && (following ? d > 0.07f : d > 0.10f);
            // only when the player cannot get there (a wall, a crate... the game's collision stops the steps) the world
            // is pulled along with the head, so one never walks into them; otherwise the player follows at any distance
            // (only the part of the offset that goes into the obstacle: moving along a wall the player follows again)
            if (follow_blocked && in_3d_now && emu_in_control && !cine_on) {
                float bx = sinf(follow_block_yaw), bz = -cosf(follow_block_yaw);
                float into = dx * bx + dz * bz;
                if (into > 0) {
                    float k = std::min(1.0f, dt * 12.0f) * into;
                    pos_ref[0] += bx * k, pos_ref[2] += bz * k;
                }
                dx = head_mat.m[0][3] - pos_ref[0], dz = head_mat.m[2][3] - pos_ref[2];
                d = sqrtf(dx * dx + dz * dz);
                lyaw = atan2f(dx, -dz);
            }
            if (following && have_prev_eye) {
                // where the head is, in the game world: the eye of the last captured frame (pos_ref) + the head's offset
                // (LOCAL yaw l -> game heading l - world_yaw; game forward (-sin g, -cos g), 1 unit = 1 mm). It stays
                // put while the player walks: each step moves the eye and pos_ref together.
                float g = lyaw - world_yaw;
                follow_now = true;
                follow_tx = prev_eye[0] - sinf(g) * d * 1000.0f;
                follow_tz = prev_eye[1] - cosf(g) * d * 1000.0f;
                follow_dir_local = lyaw;
                follow_until = clk::now() + std::chrono::milliseconds(300);
            }
        }
        {
            std::lock_guard<std::mutex> lk(emu.m);
            EmuIn &ei = emu.in;
            ei.mx = stick_move[0], ei.my = stick_move[1];
            ei.follow = follow_now, ei.follow_tx = follow_tx, ei.follow_tz = follow_tz;
            ei.trigger = trigger_held;
            ei.run = run_on;
            ei.gun_px = gun_px, ei.gun_py = gun_py, ei.gun_on = gun_on;
            memcpy(ei.aim_world, aim_world, sizeof aim_world);
            memcpy(ei.aim_dir_world, aim_dir_world, sizeof aim_dir_world);
            memcpy(ei.lob_target, lob_target, sizeof lob_target);
            ei.lob_target_ok = lob_target_ok && in_3d_now;
            ei.aim_world_ok = aim_world_ok && in_3d_now;
            for (int k = 0; k < B_COUNT; k++) ei.btn[k] = btn_state[k];
            ei.vr_menu_open = vr_menu_open;
            ei.swallow += menu_swallow;
            menu_swallow = 0;
            ei.view_yaw = view_lead_yaw, ei.view_pitch = view_lead_pitch;
            ei.gaze_yaw = view_game_yaw;
            ei.head_yaw = head_yaw;
            ei.is3d = is3d;
            ei.running = true;
            eo = emu.out;
            held_weapon = eo.weapon;
            lob_on = eo.lob;
            memcpy(lob_origin, eo.lob_origin, sizeof lob_origin);
            lob_speed = eo.lob_speed, lob_start = eo.lob_start, lob_across = eo.lob_across;
            emu_in_control = eo.in_control;
            if (eo.placement_seq != placement_taken) {
                world_yaw = eo.placement_world_yaw;  // the game placed the player: look the way it turned it
                placement_taken = eo.placement_seq;
                // and stand where the player was put (room scale: no steps to catch up with an old offset)
                pos_ref[0] = head_mat.m[0][3], pos_ref[2] = head_mat.m[2][3];
                following = follow_blocked = false;
            }
            ei.placement_ack = placement_taken;
            if (!load_request.empty()) {
                ei.load_path = load_request;
                load_msg = save_label(load_request.substr(7));
                load_request.clear();
            }
            if (snap_request) {
                snap_request = false;
                char notes[2048];
                Mat4 mz = hands[ph].valid ? vrhands::pistol_muzzle(hands[ph]) : identity();
                snprintf(notes, sizeof notes,
                         "head_yaw_deg %.2f\nhead_pitch_deg %.2f\nworld_yaw_deg %.2f\nview_game_yaw_deg %.2f\n"
                         "player_heading_deg %.2f\nin_control %d\nin_3d %d\ngun_px %.1f\ngun_py %.1f\ngun_on %d\naim_dist_m %.2f\n"
                         "trigger %.2f\nmuzzle_local %.3f %.3f %.3f dir %.3f %.3f %.3f\nhead_local %.3f %.3f %.3f\n"
                         "pos_ref %.3f %.3f %.3f\nframe_yaw_deg %.2f frame_pitch_deg %.2f\noptions hand %d laser %d turn %d hud %d\n",
                         head_yaw * 57.2958f, head_pitch * 57.2958f, world_yaw * 57.2958f, view_game_yaw * 57.2958f,
                         eo.player_heading * 57.2958f, (int)eo.in_control, (int)in_3d_now, gun_px, gun_py, (int)gun_on,
                         aim_dist, hands[ph].trigger, mz.m[0][3], mz.m[1][3], mz.m[2][3], -mz.m[0][2], -mz.m[1][2], -mz.m[2][2],
                         head_mat.m[0][3], head_mat.m[1][3], head_mat.m[2][3], pos_ref[0], pos_ref[1], pos_ref[2],
                         frame_abs_yaw * 57.2958f, frame_pitch * 57.2958f, opt.pistol_hand, (int)opt.laser, opt.turn, (int)opt.hud);
                // (both hands: the other hand's grip in the weapon's own axes, from the muzzle: x right, y up, z back
                // along the barrel - where a second hand should hold a long gun)
                {
                    std::string extra;
                    char line[256];
                    for (int h = 0; h < 2; h++) {
                        const Mat4 &g = hands[h].grip;
                        snprintf(line, sizeof line, "hand_%s valid %d grip %.3f %.3f %.3f fwd %.3f %.3f %.3f up %.3f %.3f %.3f\n",
                                 h ? "right" : "left", (int)hands[h].valid, g.m[0][3], g.m[1][3], g.m[2][3], -g.m[0][2],
                                 -g.m[1][2], -g.m[2][2], g.m[0][1], g.m[1][1], g.m[2][1]);
                        extra += line;
                    }
                    if (hands[ph].valid && hands[1 - ph].valid) {
                        Mat4 inv = mat_rigid_inverse(mz);
                        const Mat4 &g = hands[1 - ph].grip;
                        float p[3], f[3];
                        for (int r = 0; r < 3; r++) {
                            p[r] = inv.m[r][0] * g.m[0][3] + inv.m[r][1] * g.m[1][3] + inv.m[r][2] * g.m[2][3] + inv.m[r][3];
                            f[r] = -(inv.m[r][0] * g.m[0][2] + inv.m[r][1] * g.m[1][2] + inv.m[r][2] * g.m[2][2]);
                        }
                        snprintf(line, sizeof line, "offhand_from_muzzle %.3f %.3f %.3f fwd %.3f %.3f %.3f weapon %d\n", p[0], p[1],
                                 p[2], f[0], f[1], f[2], held_weapon);
                        extra += line;
                    }
                    strncat(notes, extra.c_str(), sizeof notes - strlen(notes) - 1);
                }
                ei.snap = true;
                ei.snap_notes = notes;
            }
            if (emu.cap_new) { std::swap(cap, emu.cap); cap_vram.swap(emu.vram); emu.cap_new = false; got_cap = true; }
            if (emu.pix_new) { pix.swap(emu.pix); pix_w = emu.pix_w, pix_h = emu.pix_h; emu.pix_new = false; got_pix = true; }
        }
        emu_frames = eo.frames;
        last_render_emu = eo.last_render;
        eo_slow_play = eo.slow_play;
        eo_screen_frozen = eo.screen_frozen;
        last_cap_emu = eo.last_cap;
        if (eo.shots != shots_seen) {
            haptic_pulse(sess, ph, 0.9f, 0.06f);
            shots_seen = eo.shots;
        }
        if (eo.snaps != snaps_seen) {
            snaps_seen = eo.snaps;
            snap_name = eo.snap_name;
            snap_text = "State saved: " + snap_name;
            snap_until = clk::now() + std::chrono::seconds(3);
            haptic_pulse(sess, 0, 0.5f, 0.1f);
        }
        if (eo.loads != loads_seen) {
            loads_seen = eo.loads;
            snap_text = (eo.load_ok ? "State loaded: " : "Loading failed: ") + load_msg;
            snap_until = clk::now() + std::chrono::seconds(3);
            haptic_pulse(sess, 0, 0.5f, 0.1f);
        }
        if (clk::now() < snap_until) {
            snap_panel.render({snap_text}, 1024, 128);
            float hx = head_mat.m[0][3], hy = head_mat.m[1][3], hz = head_mat.m[2][3];
            snap_pose = mul(mat_translate(hx + sinf(head_yaw) * 1.1f, hy + 0.25f, hz - cosf(head_yaw) * 1.1f), mat_rot_y(-head_yaw));
        }
        if (got_pix && pix_w > 0) upload_game_frame(pix, pix_w, pix_h);
        if (have_audio) frontend_audio_pump();
        if (got_cap && !cap_vram.empty()) {
            last_cap = cap.number;
            is3d = scene::update(cap, cap_vram.data());
            // room scale: the eye moved by a following step -> the world's anchor moves with it (game world -> LOCAL:
            // game heading g = LOCAL yaw - world_yaw; the game's x is mirrored: forward (-sin g, -cos g))
            if (cap.game_cam_valid) {
                float ex = (float)cap.game_cam[0], ez = (float)cap.game_cam[4];
                if (have_prev_eye) {
                    float wx = ex - prev_eye[0], wz = ez - prev_eye[1], L = sqrtf(wx * wx + wz * wz);
                    if (clk::now() < follow_until && L > 0.5f && L < 400.0f) {
                        float l = atan2f(-wx, -wz) + world_yaw;
                        pos_ref[0] += sinf(l) * L / 1000.0f;
                        pos_ref[2] += -cosf(l) * L / 1000.0f;
                        // progress towards the head (sliding along a wall does not count)
                        follow_moved += std::max(0.0f, L * cosf(l - follow_dir_local));
                    }
                }
                prev_eye[0] = ex, prev_eye[1] = ez, have_prev_eye = true;
                // blocked: following for a while without the eye moving
                if (following) {
                    // a fifth of a second of steps that do not take the eye anywhere: something is in the way
                    if (++follow_frames >= 5) {
                        float hx = head_mat.m[0][3] - pos_ref[0], hz = head_mat.m[2][3] - pos_ref[2];
                        if (follow_moved < 15.0f && hx * hx + hz * hz > 0.10f * 0.10f)
                            follow_blocked = true, follow_block_yaw = follow_dir_local;
                        follow_frames = 0, follow_moved = 0;
                    }
                } else {
                    follow_frames = 0, follow_moved = 0;
                }
            } else {
                have_prev_eye = false;
            }
            // (development) PSXVR_EYELOG: each captured frame - its eye and angles, the world's anchor and the head
            if (getenv("PSXVR_EYELOG"))
                logf("eyelog emu %llu eye %d %d %d yaw16 %d pitch16 %d world_yaw %.4f pos_ref %.4f %.4f %.4f head %.4f %.4f %.4f "
                     "hyaw %.3f following %d ctl %d",
                     (unsigned long long)emu_frames, cap.game_cam[0], cap.game_cam[1], cap.game_cam[4], cap.game_cam[5],
                     cap.cam_pitch, world_yaw, pos_ref[0], pos_ref[1], pos_ref[2], head_mat.m[0][3], head_mat.m[1][3],
                     head_mat.m[2][3], head_yaw * 57.2958f, (int)following, (int)emu_in_control);
            picture_feedback = is3d && game::picture_feedback();
            screen_prims = cap.prims.size();
            screen_movie = game::movie_frame(cap, cap_vram.data());
            screen_picture = game::full_picture(cap);
            frame_abs_valid = cap.game_cam_valid;
            memcpy(aim_frame.game_cam, cap.game_cam, sizeof cap.game_cam);
            aim_frame.cam_pitch = cap.cam_pitch;
            frame_abs_yaw = cap.game_cam[5] * (6.2831853f / 65536.0f);  // heading it was drawn with
            frame_pitch = cap.cam_pitch * (6.2831853f / 65536.0f);
            disp_w = cap.disp_w, disp_h = cap.disp_h;
            hud_srv = scene::render_hud(srgb);
        }
        // Menus, cut-scenes and movies are not the player's 3D view: show the game's screen instead, placed in front of
        // where the player looks when it appears.
        bool show3d = is3d && ((emu_frames - last_cap_emu < 20 && emu_frames - last_render_emu < 25) || eo_screen_frozen) &&
                      frame_abs_valid && !screen_picture;
        // Cut-scenes (the player not in control) in 3D: the game's camera alone (the head does not turn it), each frame
        // placed with the camera facing where the player looked when the scene began (and the camera's own pitch, within
        // 25 degrees of the head's then: place_pitch)
        {
            bool cine = show3d && !eo.in_control;
            if (cine && !cine_on) cine_yaw = head_yaw, cine_head_pitch = head_pitch;
            cine_on = cine;
        }
        // the game's picture feedback (blurred waking up): the previous image kept at 0.5 per drawn frame (25 Hz)
        bool blur_on = show3d && picture_feedback;
        if (!blur_on) ghost_reset();
        float ghost_keep = powf(0.5f, std::max(0.004f, dt) * 25.0f);
        {
            static bool prev_show3d = true;
            if (prev_show3d && !show3d) {
                screen_yaw = head_yaw;
                screen_rigid = mul(mat_translate(pos_ref[0], pos_ref[1], pos_ref[2]),
                                   mul(mat_rot_y(-screen_yaw), mat_translate(0, 0, -2.2f)));
                screen_model = mul(screen_rigid, mat_scale(SCR_W, SCR_H, 1));
            }
            prev_show3d = show3d;
        }

        std::vector<XrCompositionLayerProjectionView> pviews(nviews, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
        XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        bool rendered = false;
        if (fs.shouldRender) {
            XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
            li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            li.displayTime = fs.predictedDisplayTime;
            li.space = space;
            XrViewState vstate = {XR_TYPE_VIEW_STATE};
            std::vector<XrView> views(nviews, {XR_TYPE_VIEW});
            uint32_t nv;
            xrLocateViews(sess, &li, &vstate, nviews, &nv, views.data());
            if ((vstate.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) && game_srv) {
                // the world as the frame was drawn (recomputed: the frame may have changed since the aim above)
                float cam_local_yaw = cine_on ? cine_yaw : frame_abs_valid ? frame_abs_yaw + world_yaw : head_yaw;
                Mat4 s2l = mul(mat_translate(pos_ref[0], pos_ref[1], pos_ref[2]),
                               mul(mat_rot_y(-cam_local_yaw), mat_rot_x(place_pitch(frame_pitch))));
                // HUD: the game's 2D overlay on a panel in front of the camera that drew the frame, ~52 degrees wide,
                // a little below the horizon (its weapon / bullets / condition bar sits at the bottom of the picture)
                const float hud_dists[3] = {1.0f, 1.3f, 1.8f};
                const float hud_dist = hud_dists[opt.hud_dist], hud_half_w = hud_dist * 0.49f, hud_down = 0.10f;
                // the HUD follows the head smoothly (with the frame camera it moved in 25 Hz steps when turning)
                static float hud_y = 0, hud_p = 0;
                static bool hud_init = false;
                if (!hud_init) hud_y = head_yaw, hud_p = head_pitch, hud_init = true;
                {
                    float k = std::min(1.0f, dt * 8.0f);
                    hud_y = wrap_pi(hud_y + wrap_pi(head_yaw - hud_y) * k);
                    hud_p += (head_pitch - hud_p) * k;
                }
                Mat4 hud_rot = mul(mat_rot_y(-hud_y), mat_rot_x(hud_p));
                Mat4 hud_model = mul(mat_translate(head_mat.m[0][3], head_mat.m[1][3], head_mat.m[2][3]),
                                     mul(hud_rot, mul(mat_rot_x(-hud_down), mul(mat_translate(0, 0, -hud_dist),
                                                      mat_scale(hud_half_w, hud_half_w * 0.75f, 1)))));
                auto draw_view = [&](ID3D11RenderTargetView *rtv, ID3D11DepthStencilView *dsv, int w, int h,
                                     const Mat4 &view_proj, int slot) {
                    float clear[4] = {0.0f, 0.0f, 0.0f, 1};
                    ctx->ClearRenderTargetView(rtv, clear);
                    ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
                    ctx->OMSetRenderTargets(1, &rtv, dsv);
                    D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
                    ctx->RSSetViewports(1, &vp);
                    if (show3d) {
                        scene::draw_world(mul(view_proj, s2l), srgb);
                        if (blur_on) ghost_apply(slot, rtv, ghost_keep);
                        // (the desktop window, slot 3, draws the HUD itself: flat, at the headset's proportions)
                        if (hud_srv && opt.hud && slot != 3) draw_quad(mul(view_proj, hud_model), hud_srv, true);
                        // cut-scene bands (test): black above and below the gaze, fixed to the head, a window of about
                        // +-30 degrees between them, as a film's letterbox: the player sees at once it is a scene
                        if (cine_on && black_srv()) {
                            Mat4 hm = head_mat;
                            const float dist = 0.5f, edge = dist * 0.58f, h = 1.2f;  // window +-30 degrees
                            for (int b = -1; b <= 1; b += 2) {
                                Mat4 m = mul(hm, mul(mat_translate(0, b * (edge + h * 0.5f), -dist), mat_scale(2.5f, h * 0.5f, 1)));
                                draw_quad(mul(view_proj, m), black_srv(), false);
                            }
                        }
                    } else {
                        draw_quad(mul(view_proj, screen_model), game_srv, false);
                    }
                    if (vr_menu_open && menu_panel.srv)
                        draw_quad(mul(view_proj, mul(menu_pose, mat_scale(0.4f, 0.25f, 1))), menu_panel.srv, false);
                    else if (show_hint && !show3d && hint_panel.srv)
                        draw_quad(mul(view_proj, mul(hint_pose, mat_scale(0.16f, 0.03f, 1))), hint_panel.srv, false);
                    if (clk::now() < snap_until && snap_panel.srv)
                        draw_quad(mul(view_proj, mul(snap_pose, mat_scale(0.3f, 0.04f, 1))), snap_panel.srv, false);
                    // hands and laser: in play (the laser if chosen) and on the virtual screen for menus (a 2D picture
                    // with something to point at); never in cut-scenes, whether shown in 3D or on the screen (a 3D
                    // picture the frontend could not place), nor in movies (one or two primitives)
                    // hands, pistol and laser only while playing; on the virtual screen only a small red dot where the
                    // pistol points (the title's NEW GAME and other light-gun menus are chosen by shooting at them), not
                    // in movies / full-screen pictures
                    if (show3d && !cine_on) vrhands::draw(view_proj, hands, opt.laser ? ph : -1, srgb);
                    else if (!show3d && !is3d && screen_prims > 6 && !screen_movie && gun_on && game_w > 0) {
                        float x = gun_px / game_w * 2 - 1, y = 1 - gun_py / game_h * 2, r = 0.012f;
                        Mat4 m = mul(screen_rigid, mul(mat_translate(x * SCR_W, y * SCR_H, 0.003f), mat_scale(r, r, 1)));
                        draw_quad(mul(view_proj, m), solid_srv(0xff2020ffu), false);
                    }
                };
                for (uint32_t e = 0; e < nviews; e++) {
                    uint32_t idx;
                    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                    xrAcquireSwapchainImage(eyes[e].sc, &ai, &idx);
                    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                    wi.timeout = XR_INFINITE_DURATION;
                    xrWaitSwapchainImage(eyes[e].sc, &wi);
                    draw_view(eyes[e].imgs[idx].rtv, eyes[e].dsv, eyes[e].w, eyes[e].h,
                              mul(projection(views[e].fov, 0.03f, 400.0f), view_from_pose(views[e].pose)), (int)e);
                    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    xrReleaseSwapchainImage(eyes[e].sc, &ri);
                    pviews[e].pose = views[e].pose;
                    pviews[e].fov = views[e].fov;
                    pviews[e].subImage.swapchain = eyes[e].sc;
                    pviews[e].subImage.imageRect = {{0, 0}, {eyes[e].w, eyes[e].h}};
                }
                layer.space = space;
                layer.viewCount = nviews;
                layer.views = pviews.data();
                rendered = true;
                // The desktop window (for videos): its own view from the head, as wide as both eyes see together,
                // as tall as the window's 16:9 shape leaves; menus and movies fill it with the game's screen.
                if (mirror.ready() && !IsIconic(mirror.wnd)) {
                    mirror.fit(dev);
                    if (show3d) {
                        float l = 0, r = 0, mid = 0;
                        for (uint32_t e = 0; e < nviews; e++) {
                            l = std::min(l, tanf(views[e].fov.angleLeft));
                            r = std::max(r, tanf(views[e].fov.angleRight));
                            mid += (tanf(views[e].fov.angleUp) + tanf(views[e].fov.angleDown)) / (2.0f * nviews);
                        }
                        if (r - l < 0.1f) l = -1, r = 1;
                        const float eyes_w = r - l;  // (the width both eyes see: the HUD's share of it below)
                        // a little narrower than the eyes together (x 0.8 of the width's tangents): at the edges the
                        // headset's lenses hide what the game never drew (black), the window showed it
                        // (RESVR_WINDOW_FOV=k: another factor)
                        {
                            static const float k = getenv("RESVR_WINDOW_FOV") ? (float)atof(getenv("RESVR_WINDOW_FOV")) : 0.8f;
                            float c = (l + r) / 2, hw = (r - l) / 2 * std::max(0.3f, std::min(1.0f, k));
                            l = c - hw, r = c + hw;
                        }
                        float half = (r - l) / 2 * mirror.h / (float)mirror.w;
                        XrFovf fov = {atanf(l), atanf(r), atanf(mid + half), atanf(mid - half)};
                        draw_view(mirror.rtv, mirror.dsv, mirror.w, mirror.h,
                                  mul(projection(fov, 0.03f, 400.0f), mat_rigid_inverse(head_mat)), 3);
                        // The HUD: in the headset a panel low in the view, 0.98 of its distance wide (~2 x 26 degrees)
                        // out of the whole width both eyes see; the window's 16:9 view is much shorter, so the same
                        // panel in 3D filled its lower part. Here it is drawn flat, as wide a share of the window as of
                        // the eyes' view, at the bottom in the middle (HUD option on).
                        if (hud_srv && opt.hud) {
                            float aspect = mirror.w / (float)mirror.h;
                            float hw = 0.98f / std::max(0.5f, eyes_w);  // half width in the window (clip units)
                            float hh = hw * 0.75f * aspect;
                            draw_quad(mul(mat_translate(0, -0.97f + hh, 0), mat_scale(hw, hh, 1)), hud_srv, true);
                        }
                    } else {
                        float black[4] = {0, 0, 0, 1};
                        ctx->ClearRenderTargetView(mirror.rtv, black);
                        ctx->OMSetRenderTargets(1, &mirror.rtv, nullptr);
                        D3D11_VIEWPORT vp = {0, 0, (float)mirror.w, (float)mirror.h, 0, 1};
                        ctx->RSSetViewports(1, &vp);
                        float sx = (4.0f / 3.0f) * mirror.h / mirror.w, sy = 1;  // the game's 4:3 picture
                        if (sx > 1) sy = 1 / sx, sx = 1;
                        draw_quad(mat_scale(sx, sy, 1), game_srv, false);
                    }
                    // the VR options panel and the save / load message, which the headset shows floating in the room:
                    // here flat over the picture (the panel in the middle, 60% of the window's width; the message low)
                    {
                        float aspect = mirror.w / (float)mirror.h;
                        if (vr_menu_open && menu_panel.srv) {
                            float pw = 0.6f, ph = pw * (640.0f / 1024.0f) * aspect;
                            if (ph > 0.9f) pw *= 0.9f / ph, ph = 0.9f;
                            draw_quad(mat_scale(pw, ph, 1), menu_panel.srv, false);
                        }
                        if (clk::now() < snap_until && snap_panel.srv) {
                            float pw = 0.45f, ph = pw * (128.0f / 1024.0f) * aspect;
                            draw_quad(mul(mat_translate(0, -0.8f, 0), mat_scale(pw, ph, 1)), snap_panel.srv, false);
                        }
                    }
                    mirror.sc->Present(0, 0);
                }
            }
        }
        const XrCompositionLayerBaseHeader *layers[1] = {(XrCompositionLayerBaseHeader *)&layer};
        XrFrameEndInfo fe = {XR_TYPE_FRAME_END_INFO};
        fe.displayTime = fs.predictedDisplayTime;
        fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fe.layerCount = rendered ? 1 : 0;
        fe.layers = layers;
        xrEndFrame(sess, &fe);
    }
    emu.quit = true;
    if (emu_thread.joinable()) emu_thread.join();
    xrDestroySession(sess);
    xrDestroyInstance(inst);
    return 0;
}
