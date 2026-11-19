// 3D scene renderer for captured PS1 primitives (see scene.h).
#include "scene.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

using namespace psx;

namespace scene {

namespace {

struct Vtx {
    float x, y, z;
    float u, v;
    uint8_t r, g, b, a;
    uint32_t attr[4];
    float depth_bias;  // pulls the fragment depth (only) towards the eye: later-drawn coplanar surfaces win
};

struct Batch {
    uint32_t first, count;  // vertices
    bool semi;
    uint8_t blend;
};

ID3D11Device *dev;
ID3D11DeviceContext *ctx;
ID3D11VertexShader *vs;
ID3D11PixelShader *ps;
ID3D11InputLayout *layout;
ID3D11Buffer *cbuf;
ID3D11Buffer *vbuf;
uint32_t vbuf_cap = 0;
ID3D11Texture2D *vram_tex;
ID3D11ShaderResourceView *vram_srv;
ID3D11RasterizerState *raster;
ID3D11DepthStencilState *ds_write, *ds_test, *ds_none;
ID3D11BlendState *bs_opaque, *bs_blend[4];
ID3D11Texture2D *hud_tex;
ID3D11RenderTargetView *hud_rtv;
ID3D11ShaderResourceView *hud_srv_lin, *hud_srv_srgb;
int hud_w = 320, hud_h = 240;

std::vector<Vtx> world_v, hud_v, sky_v;
// Sprite shape stabiliser: the game sizes and places sprites in whole pixels, so a far tree's corners jump by a pixel
// (tens of cm) from frame to frame as the head turns. Each sprite is recognised from frame to frame by its anchor's
// world position and its texture, and its corner offsets (camera-right / up, metres) are averaged over time.
struct SpriteShape {
    float off[4][2];
    uint64_t seen;
};
std::unordered_map<uint64_t, SpriteShape> sprite_shapes;
uint64_t shape_frame = 0;
WorldExtraFn world_extra_fn = nullptr;
std::vector<uint8_t> extra_hide;
std::vector<ExtraPrim> extra_prims;
float backdrop[3];
bool have_backdrop = false;
std::vector<Batch> world_b, hud_b, sky_b;
std::vector<Vtx> skymem_v;
std::vector<Batch> skymem_b;
// Sky memory: the game draws only the part of its sky dome its own camera sees (a gradient grid, 0x8006f140), so
// the wider VR / desktop views showed its edges and nothing overhead. Every sky colour the game draws is kept
// by world direction (2 degree grid, from 12 degrees below the horizon to straight up) and the whole dome is drawn
// from it behind the fresh sky; above the highest part ever seen a column keeps its top colour. Forgotten in menus.
constexpr int SKYM_COLS = 180, SKYM_ROWS = 52;  // vertices: longitude 0..358, latitude -12..90 (2 degrees)
struct SkyMemVert { uint8_t r, g, b, valid; };
SkyMemVert sky_mem[SKYM_ROWS][SKYM_COLS];
int sky_mem_off_frames = 0;
std::vector<const psx::CapPrim *> fresh_sky;
// Game routines whose projected points form the sky backdrop: redrawn as a skybox (direction only, far away)
std::vector<uint32_t> sky_pcs;
constexpr float SKY_DISTANCE_M = 150.0f;
SpriteMode sprite_mode = SpriteMode::UprightOnGround;
bool is_sky(const psx::CapPrim &p) {
    for (int i = 0; i < p.nv; i++)
        if (p.v[i].has3d && std::find(sky_pcs.begin(), sky_pcs.end(), p.v[i].src_pc) != sky_pcs.end()) return true;
    return false;
}
Stats st;
std::vector<std::array<float, 4>> anchors;
float proj_ofx = 160, proj_ofy = 120, proj_h = 256;
// world polygons of the frame being built, in drawing order (decal detection)
struct DecalPlane { float n[3], d, x0, y0, x1, y1, bias; float poly[4][2]; int np; bool model; };  // model: a character's

// Area of the intersection of two convex polygons on screen (Sutherland-Hodgman; both made counter-clockwise).
float poly_area(const float (*q)[2], int n) {
    float a = 0;
    for (int i = 0; i < n; i++) { int j = (i + 1) % n; a += q[i][0] * q[j][1] - q[j][0] * q[i][1]; }
    return a * 0.5f;
}
float overlap_area(const float (*a)[2], int na, const float (*b)[2], int nb) {
    float A[4][2], B[4][2];
    bool fa = poly_area(a, na) < 0, fb = poly_area(b, nb) < 0;
    for (int i = 0; i < na; i++) A[i][0] = a[fa ? na - 1 - i : i][0], A[i][1] = a[fa ? na - 1 - i : i][1];
    for (int i = 0; i < nb; i++) B[i][0] = b[fb ? nb - 1 - i : i][0], B[i][1] = b[fb ? nb - 1 - i : i][1];
    float cur[16][2], nxt[16][2];
    int n = na;
    for (int i = 0; i < na; i++) cur[i][0] = A[i][0], cur[i][1] = A[i][1];
    for (int e = 0; e < nb && n > 0; e++) {
        const float *p0 = B[e], *p1 = B[(e + 1) % nb];
        auto inside = [&](const float *q) { return (p1[0] - p0[0]) * (q[1] - p0[1]) - (p1[1] - p0[1]) * (q[0] - p0[0]) >= 0; };
        int m = 0;
        for (int i = 0; i < n && m < 15; i++) {
            const float *c = cur[i], *d = cur[(i + 1) % n];
            bool ic = inside(c), id = inside(d);
            if (ic) nxt[m][0] = c[0], nxt[m][1] = c[1], m++;
            if (ic != id && m < 16) {
                float dx = d[0] - c[0], dy = d[1] - c[1];
                float den = (p1[0] - p0[0]) * dy - (p1[1] - p0[1]) * dx;
                if (std::fabs(den) > 1e-9f) {
                    float t = ((p1[0] - p0[0]) * (c[1] - p0[1]) - (p1[1] - p0[1]) * (c[0] - p0[0])) / -den;
                    nxt[m][0] = c[0] + dx * t, nxt[m][1] = c[1] + dy * t, m++;
                }
            }
        }
        n = m;
        for (int i = 0; i < n; i++) cur[i][0] = nxt[i][0], cur[i][1] = nxt[i][1];
    }
    return n >= 3 ? std::fabs(poly_area(cur, n)) : 0.0f;
}
std::vector<DecalPlane> decal_planes;  // projection of the last captured world view
bool hide_cursor = false;

struct CB {
    Mat4 mvp;
    uint32_t pass, srgb, fog_on, pad0;
    float fog_color[4];
    float fog_start, fog_end, pad1, pad2;
};
// Distance haze over the world (the original fogs its ground from ~25 m): fades into the game's backdrop colour,
// so the edge of the drawn landscape is never seen.
float fog_start_m = 35.0f, fog_end_m = 100.0f;
bool fog_now = false;

const char *shader_src = R"(
cbuffer C : register(b0) { row_major float4x4 mvp; uint draw_pass; uint srgb; uint fog_on; uint pad0;
                            float4 fog_color; float fog_start; float fog_end; float2 pad1; };
Texture2D<uint> vram : register(t0);
struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR; uint4 attr : TEXCOORD1; float bias : TEXCOORD2; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 col : COLOR; nointerpolation uint4 attr : TEXCOORD1; float fog : TEXCOORD2; };
VSOut vs_main(VSIn i) {
    VSOut o;
    o.pos = mul(mvp, float4(i.pos, 1));
    // depth-only offset towards the eye of i.bias metres (near plane 3 cm: ndc shift = n d / z^2): the visible
    // position is unchanged
    o.pos.z -= i.bias * 0.03 / max(o.pos.w, 0.05);
    o.uv = i.uv; o.col = i.col; o.attr = i.attr;
    float f = fog_on ? saturate((length(i.pos.xz) - fog_start) / (fog_end - fog_start)) : 0;
    o.fog = f * f * (3 - 2 * f);
    return o;
}
uint fetch(int x, int y) { return vram.Load(int3(x & 1023, y & 511, 0)); }
uint fetch_atlas(int x, int y) { return vram.Load(int3(x & 1023, 512 + (y & 511), 0)); }
float4 ps_main(VSOut i) : SV_Target {
    uint a = i.attr.x;
    bool textured = (a >> 22) & 1, raw = (a >> 23) & 1, semi = (a >> 24) & 1;
    float3 rgb = i.col.rgb;
    bool stp = true;
    if (textured) {
        int u = (int)floor(i.uv.x + 0.001), v = (int)floor(i.uv.y + 0.001);
        uint lim = i.attr.w;
        u = clamp(u, (int)(lim & 255), (int)((lim >> 16) & 255));
        v = clamp(v, (int)((lim >> 8) & 255), (int)(lim >> 24));
        uint tw = i.attr.z;
        int mx = (tw & 31) * 8, my = ((tw >> 5) & 31) * 8, ox = ((tw >> 10) & 31) * 8, oy = ((tw >> 15) & 31) * 8;
        u = (u & ~mx) | (ox & mx);
        v = (v & ~my) | (oy & my);
        int tx = a & 1023, ty = (a >> 10) & 511, depth = (a >> 20) & 3;
        int cx = i.attr.y & 1023, cy = (i.attr.y >> 10) & 511;
        uint texel;
        if (depth == 0) {
            uint w = fetch(tx + (u >> 2), ty + v);
            texel = fetch(cx + ((w >> ((u & 3) * 4)) & 15), cy);
        } else if (depth == 1) {
            uint w = fetch(tx + (u >> 1), ty + v);
            texel = fetch(cx + ((w >> ((u & 1) * 8)) & 255), cy);
        } else if ((a >> 19) & 1) {
            texel = fetch_atlas(tx + u, ty + v);
        } else {
            texel = fetch(tx + u, ty + v);
        }
        if (texel == 0) discard;
        stp = (texel >> 15) & 1;
        float3 t = float3(texel & 31, (texel >> 5) & 31, (texel >> 10) & 31) / 31.0;
        rgb = raw ? t : saturate(t * i.col.rgb * (255.0 / 128.0));
    }
    bool is_semi = semi && stp;
    if (draw_pass == 0 && is_semi) discard;
    if (draw_pass == 1 && !is_semi) discard;
    uint blend = (a >> 25) & 3;
    if (is_semi && blend != 0) rgb *= 1 - i.fog;  // additive / subtractive: fade the effect out
    else rgb = lerp(rgb, fog_color.rgb, i.fog);
    if (srgb) rgb = pow(rgb, 2.2);
    return float4(rgb, 1);
}
)";

bool make_shaders() {
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(shader_src, strlen(shader_src), "scene", nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vsb,
                          &err))) {
        if (err) logf("scene VS: %s", (const char *)err->GetBufferPointer());
        return false;
    }
    if (FAILED(D3DCompile(shader_src, strlen(shader_src), "scene", nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &psb,
                          &err))) {
        if (err) logf("scene PS: %s", (const char *)err->GetBufferPointer());
        return false;
    }
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
    D3D11_INPUT_ELEMENT_DESC el[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_UINT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev->CreateInputLayout(el, 5, vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout);
    return true;
}

void make_states() {
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &raster);

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    dev->CreateDepthStencilState(&dd, &ds_write);
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dev->CreateDepthStencilState(&dd, &ds_test);
    dd.DepthEnable = FALSE;
    dev->CreateDepthStencilState(&dd, &ds_none);

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bd, &bs_opaque);
    auto &rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    // 0: B/2 + F/2
    rt.SrcBlend = D3D11_BLEND_BLEND_FACTOR; rt.DestBlend = D3D11_BLEND_INV_BLEND_FACTOR; rt.BlendOp = D3D11_BLEND_OP_ADD;
    dev->CreateBlendState(&bd, &bs_blend[0]);
    // 1: B + F
    rt.SrcBlend = D3D11_BLEND_ONE; rt.DestBlend = D3D11_BLEND_ONE; rt.BlendOp = D3D11_BLEND_OP_ADD;
    dev->CreateBlendState(&bd, &bs_blend[1]);
    // 2: B - F
    rt.BlendOp = D3D11_BLEND_OP_REV_SUBTRACT;
    dev->CreateBlendState(&bd, &bs_blend[2]);
    // 3: B + F/4
    rt.SrcBlend = D3D11_BLEND_BLEND_FACTOR; rt.DestBlend = D3D11_BLEND_ONE; rt.BlendOp = D3D11_BLEND_OP_ADD;
    dev->CreateBlendState(&bd, &bs_blend[3]);
}

void make_hud_target(int w, int h) {
    if (hud_tex && w == hud_w && h == hud_h) return;
    if (hud_rtv) hud_rtv->Release();
    if (hud_srv_lin) hud_srv_lin->Release();
    if (hud_srv_srgb) hud_srv_srgb->Release();
    if (hud_tex) hud_tex->Release();
    hud_w = w;
    hud_h = h;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    dev->CreateTexture2D(&td, nullptr, &hud_tex);
    D3D11_RENDER_TARGET_VIEW_DESC rv = {};
    rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    dev->CreateRenderTargetView(hud_tex, &rv, &hud_rtv);
    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dev->CreateShaderResourceView(hud_tex, &sv, &hud_srv_lin);
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    dev->CreateShaderResourceView(hud_tex, &sv, &hud_srv_srgb);
}

// ---------------------------------------------------------------- frame building
struct Proj {
    float ofx, ofy, h;
};

void prim_attrs(const CapPrim &p, uint32_t attr[4]) {
    int umin = 255, vmin = 255, umax = 0, vmax = 0;
    for (int i = 0; i < p.nv; i++) {
        umin = std::min<int>(umin, p.v[i].u); umax = std::max<int>(umax, p.v[i].u);
        vmin = std::min<int>(vmin, p.v[i].v); vmax = std::max<int>(vmax, p.v[i].v);
    }
    if (p.kind == CapPrim::RECT) {  // rect corners are one past the last texel
        umax = std::max(umin, umax - 1);
        vmax = std::max(vmin, vmax - 1);
    }
    uint32_t tx = (p.tpage & 0xf) * 64, ty = ((p.tpage >> 4) & 1) * 256, depth = (p.tpage >> 7) & 3, atlas = 0;
    if (depth == 3) depth = 2;
    if (p.baked) {  // frozen texels: 15-bit colours in the atlas (rows 512..), texel (umin, vmin) at (bake_x, bake_y)
        tx = p.bake_x - umin;
        ty = p.bake_y - vmin;
        depth = 2;
        atlas = 1;
    }
    attr[0] = tx | (ty << 10) | (atlas << 19) | (depth << 20) | ((p.textured ? 1u : 0) << 22) | ((p.raw ? 1u : 0) << 23) |
              ((p.semi ? 1u : 0) << 24) | ((uint32_t)p.blend << 25);
    attr[1] = ((p.clut & 0x3f) * 16) | (((p.clut >> 6) & 0x1ff) << 10);
    attr[2] = p.baked ? 0 : p.tw;  // the frozen texels already went through the texture window
    attr[3] = (uint32_t)umin | ((uint32_t)vmin << 8) | ((uint32_t)umax << 16) | ((uint32_t)vmax << 24);
}

const char *emit_tag = "";
// extra depth pull of each corner of the primitive being emitted (metres; added to depth_bias)
static float emit_vbias[4] = {0, 0, 0, 0};
void emit_prim(std::vector<Vtx> &out, std::vector<Batch> &batches, const CapPrim &p, const float pos[4][3],
               float depth_bias = 0) {
    static const bool check = getenv("PSXVR_SCENECHK") != nullptr;
    if (check) {  // development: report tall or long primitives (metres, scene space)
        float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
        for (int i = 0; i < p.nv; i++)
            for (int c = 0; c < 3; c++) { lo[c] = std::min(lo[c], pos[i][c]); hi[c] = std::max(hi[c], pos[i][c]); }
        float dy = hi[1] - lo[1], dh = std::max(hi[0] - lo[0], hi[2] - lo[2]);
        if (&out == &world_v && strcmp(emit_tag, "extra") && (dy > 4.5f || dh > 30))
            printf("SCENECHK %s tp %03x clut %04x node %x nv %d dy %.1f dh %.1f p0 %.1f %.1f %.1f p1 %.1f %.1f %.1f p2 %.1f %.1f %.1f\n",
                   emit_tag, p.tpage & 0x1ff, p.clut, p.node, p.nv, dy, dh, pos[0][0], pos[0][1], pos[0][2], pos[1][0],
                   pos[1][1], pos[1][2], pos[2][0], pos[2][1], pos[2][2]);
    }
    uint32_t attr[4];
    prim_attrs(p, attr);
    static const int tri_idx[6] = {0, 1, 2, 1, 2, 3};
    int n = (p.nv == 4) ? 6 : 3;
    uint32_t first = (uint32_t)out.size();
    for (int k = 0; k < n; k++) {
        int i = tri_idx[k];
        const CapVert &cv = p.v[i];
        Vtx v;
        v.x = pos[i][0]; v.y = pos[i][1]; v.z = pos[i][2];
        v.u = cv.u; v.v = cv.v;
        v.r = cv.r; v.g = cv.g; v.b = cv.b; v.a = 255;
        if (!p.gouraud) { v.r = p.v[0].r; v.g = p.v[0].g; v.b = p.v[0].b; }
        memcpy(v.attr, attr, sizeof attr);
        v.depth_bias = depth_bias + emit_vbias[i];
        out.push_back(v);
    }
    bool semi = p.semi;
    if (!batches.empty() && batches.back().semi == semi && (!semi || batches.back().blend == p.blend) &&
        batches.back().first + batches.back().count == first) {
        batches.back().count += n;
    } else {
        batches.push_back({first, (uint32_t)n, semi, p.blend});
    }
}

}  // namespace

bool init(ID3D11Device *d, ID3D11DeviceContext *c) {
    dev = d;
    ctx = c;
    if (!make_shaders()) return false;
    make_states();
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(CB);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dev->CreateBuffer(&bd, nullptr, &cbuf);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 1024;
    td.Height = 1024;  // rows 0..511: PS1 VRAM, rows 512..1023: the frame's atlas of frozen texels
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16_UINT;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    dev->CreateTexture2D(&td, nullptr, &vram_tex);
    dev->CreateShaderResourceView(vram_tex, nullptr, &vram_srv);
    make_hud_target(320, 240);
    return true;
}

const Stats &stats() { return st; }
const std::vector<std::array<float, 4>> &sprite_anchors() { return anchors; }
bool backdrop_color(float rgb[3]) {
    if (!have_backdrop) return false;
    for (int i = 0; i < 3; i++) rgb[i] = backdrop[i];
    return true;
}
void set_world_extra(WorldExtraFn fn) { world_extra_fn = fn; }
static float actor_ground = 0;
void set_actor_ground(float m) { actor_ground = m; }
static PointWarpFn point_warp = nullptr;
void set_point_warp(PointWarpFn fn) { point_warp = fn; }
static std::vector<uint32_t> model_pcs;
void set_model_pcs(const uint32_t *pcs, int n) { model_pcs.assign(pcs, pcs + n); }
void set_fog(float start_m, float end_m) { fog_start_m = start_m; fog_end_m = end_m; }
int hud_width() { return hud_w; }
int hud_height() { return hud_h; }

// world direction (game axes: x east, y up, z the map's z) of sky memory vertex (row j, column i)
static void sky_mem_dir(int j, int i, float d[3]) {
    float lat = (-12.0f + 2.0f * j) * 0.0174533f, lon = 2.0f * i * 0.0174533f;
    d[0] = cosf(lat) * sinf(lon);
    d[1] = sinf(lat);
    d[2] = cosf(lat) * cosf(lon);
}

static void update_sky_memory(const CapFrame &fr, const Proj &pj, bool is3d) {
    static const bool off = getenv("PSXVR_NO_SKYMEM") != nullptr;
    if (off) return;
    if (!is3d || !fr.game_cam_valid) {
        if (++sky_mem_off_frames > 25) memset(sky_mem, 0, sizeof sky_mem);  // menus, level change: start again
        return;
    }
    sky_mem_off_frames = 0;
    // scene space (the frame's camera, level: x right, y up, z back) <-> world, by the heading it was drawn with
    float yaw = fr.game_cam[2] * (6.2831853f / 2048.0f), c = cosf(yaw), sn = sinf(yaw);
    auto to_scene = [&](const float w[3], float s[3]) {
        s[0] = c * w[0] + sn * w[2];
        s[1] = w[1];
        s[2] = -sn * w[0] + c * w[2];
    };
    // 1. learn: every memory vertex inside a fresh sky polygon takes its colour (interpolated as the GPU does)
    if (!fresh_sky.empty()) {
        struct Tri { float x[3], y[3]; const psx::CapVert *v[3]; float x0, x1, y0, y1; };
        std::vector<Tri> tris;
        for (const psx::CapPrim *p : fresh_sky) {
            static const int idx[2][3] = {{0, 1, 2}, {1, 3, 2}};
            for (int t = 0; t < (p->nv == 4 ? 2 : 1); t++) {
                Tri tr;
                for (int k = 0; k < 3; k++) {
                    const psx::CapVert &v = p->v[idx[t][k]];
                    tr.x[k] = (float)(v.sx - p->off_x);
                    tr.y[k] = (float)(v.sy - p->off_y);
                    tr.v[k] = &v;
                }
                tr.x0 = std::min({tr.x[0], tr.x[1], tr.x[2]}); tr.x1 = std::max({tr.x[0], tr.x[1], tr.x[2]});
                tr.y0 = std::min({tr.y[0], tr.y[1], tr.y[2]}); tr.y1 = std::max({tr.y[0], tr.y[1], tr.y[2]});
                tris.push_back(tr);
            }
        }
        for (int j = 0; j < SKYM_ROWS; j++)
            for (int i = 0; i < SKYM_COLS; i++) {
                float w[3], s[3];
                sky_mem_dir(j, i, w);
                to_scene(w, s);
                if (s[2] > -0.05f) continue;  // behind the camera
                float px = pj.ofx + s[0] / -s[2] * pj.h, py = pj.ofy + fr.y_shift - s[1] / -s[2] * pj.h;
                for (const Tri &t : tris) {
                    if (px < t.x0 || px > t.x1 || py < t.y0 || py > t.y1) continue;
                    float d = (t.x[1] - t.x[0]) * (t.y[2] - t.y[0]) - (t.x[2] - t.x[0]) * (t.y[1] - t.y[0]);
                    if (fabsf(d) < 1e-6f) continue;
                    float b1 = ((px - t.x[0]) * (t.y[2] - t.y[0]) - (t.x[2] - t.x[0]) * (py - t.y[0])) / d;
                    float b2 = ((t.x[1] - t.x[0]) * (py - t.y[0]) - (px - t.x[0]) * (t.y[1] - t.y[0])) / d;
                    float b0 = 1 - b1 - b2;
                    if (b0 < -0.001f || b1 < -0.001f || b2 < -0.001f) continue;
                    SkyMemVert &m = sky_mem[j][i];
                    m.r = (uint8_t)std::clamp(b0 * t.v[0]->r + b1 * t.v[1]->r + b2 * t.v[2]->r + 0.5f, 0.0f, 255.0f);
                    m.g = (uint8_t)std::clamp(b0 * t.v[0]->g + b1 * t.v[1]->g + b2 * t.v[2]->g + 0.5f, 0.0f, 255.0f);
                    m.b = (uint8_t)std::clamp(b0 * t.v[0]->b + b1 * t.v[1]->b + b2 * t.v[2]->b + 0.5f, 0.0f, 255.0f);
                    m.valid = 1;
                    break;
                }
            }
    }
    // 2. draw: the remembered dome. Gaps along a row (directions never seen) blend between their known
    // neighbours; above the highest known colour of a column the sky fades smoothly into the mean colour of the
    // top ring (the game never looks straight up), so the zenith has no seam or rays.
    static SkyMemVert shown[SKYM_ROWS][SKYM_COLS];
    for (int j = 0; j < SKYM_ROWS; j++) {
        int first = -1;
        for (int i = 0; i < SKYM_COLS; i++)
            if (sky_mem[j][i].valid) { first = i; break; }
        for (int i = 0; i < SKYM_COLS; i++) shown[j][i] = sky_mem[j][i];
        if (first < 0) continue;
        // walk the ring from one known vertex to the next, filling what lies between
        for (int a = first, n = 0; n < SKYM_COLS; n++) {
            int b = (a + 1) % SKYM_COLS, gap = 1;
            while (!sky_mem[j][b].valid) b = (b + 1) % SKYM_COLS, gap++;
            const SkyMemVert &ca = sky_mem[j][a], &cb = sky_mem[j][b];
            for (int k = 1; k < gap; k++) {
                float t = (float)k / gap;
                SkyMemVert &m = shown[j][(a + k) % SKYM_COLS];
                m.r = (uint8_t)(ca.r + (cb.r - ca.r) * t);
                m.g = (uint8_t)(ca.g + (cb.g - ca.g) * t);
                m.b = (uint8_t)(ca.b + (cb.b - ca.b) * t);
                m.valid = 1;
            }
            n += gap - 1;
            a = b;
            if (a == first) break;
        }
    }
    int top_row[SKYM_COLS];
    float zr = 0, zg = 0, zb = 0;
    int nz = 0;
    for (int i = 0; i < SKYM_COLS; i++) {
        top_row[i] = -1;
        for (int j = 0; j < SKYM_ROWS; j++)
            if (shown[j][i].valid) top_row[i] = j;
        if (top_row[i] >= 0) {
            const SkyMemVert &t = shown[top_row[i]][i];
            zr += t.r, zg += t.g, zb += t.b, nz++;
        }
    }
    if (nz) {
        zr /= nz, zg /= nz, zb /= nz;
        for (int i = 0; i < SKYM_COLS; i++) {
            int t0 = top_row[i];
            if (t0 < 0) continue;
            const SkyMemVert top = shown[t0][i];
            for (int j = t0 + 1; j < SKYM_ROWS; j++) {
                float t = (float)(j - t0) / (SKYM_ROWS - 1 - t0);
                t = t * t * (3 - 2 * t);
                SkyMemVert &m = shown[j][i];
                m.r = (uint8_t)(top.r + (zr - top.r) * t);
                m.g = (uint8_t)(top.g + (zg - top.g) * t);
                m.b = (uint8_t)(top.b + (zb - top.b) * t);
                m.valid = 1;
            }
        }
    }
    psx::CapPrim q = {};
    q.kind = psx::CapPrim::QUAD;
    q.nv = 4;
    q.gouraud = true;
    for (int j = 0; j + 1 < SKYM_ROWS; j++)
        for (int i = 0; i < SKYM_COLS; i++) {
            int i2 = (i + 1) % SKYM_COLS;
            const SkyMemVert *cv[4] = {&shown[j + 1][i], &shown[j + 1][i2], &shown[j][i], &shown[j][i2]};
            if (!cv[0]->valid || !cv[1]->valid || !cv[2]->valid || !cv[3]->valid) continue;
            float pos[4][3];
            const int rows[4] = {j + 1, j + 1, j, j}, cols[4] = {i, i2, i, i2};
            for (int k = 0; k < 4; k++) {
                float w[3], s[3];
                sky_mem_dir(rows[k], cols[k], w);
                to_scene(w, s);
                for (int a = 0; a < 3; a++) pos[k][a] = s[a] * SKY_DISTANCE_M;
                q.v[k].r = cv[k]->r; q.v[k].g = cv[k]->g; q.v[k].b = cv[k]->b;
            }
            emit_prim(skymem_v, skymem_b, q, pos, 0);
        }
}

bool update(const CapFrame &fr, const uint16_t *vram) {
    D3D11_BOX top = {0, 0, 0, 1024, 512, 1};
    ctx->UpdateSubresource(vram_tex, 0, &top, vram, 1024 * 2, 0);
    if (!fr.atlas.empty()) {
        D3D11_BOX bottom = {0, 512, 0, 1024, 1024, 1};
        ctx->UpdateSubresource(vram_tex, 0, &bottom, fr.atlas.data(), psx::ATLAS_W * 2, 0);
    }
    world_v.clear(); world_b.clear();
    sky_v.clear(); sky_b.clear();
    skymem_v.clear(); skymem_b.clear();
    fresh_sky.clear();
    hud_v.clear(); hud_b.clear();
    st = Stats();
    have_backdrop = false;
    anchors.clear();
    if (++shape_frame % 64 == 0)  // forget sprites not seen for a while
        for (auto it = sprite_shapes.begin(); it != sprite_shapes.end();)
            it = it->second.seen + 60 < shape_frame ? sprite_shapes.erase(it) : std::next(it);
    Proj pj = {fr.gte_ofx / 65536.0f, fr.gte_ofy / 65536.0f, (float)std::max(1, fr.gte_h)};
    proj_ofx = pj.ofx; proj_ofy = pj.ofy; proj_h = pj.h;

    // Fit depth as a function of ordering-table slot from the precisely tracked primitives.
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int n = 0;
    uint32_t max_node = 0;
    for (const CapPrim &p : fr.prims) {
        max_node = std::max(max_node, p.node);
        int k = 0;
        float z = 0;
        for (int i = 0; i < p.nv; i++)
            if (p.v[i].has3d) { z += p.v[i].z; k++; }
        if (k == p.nv && p.kind != CapPrim::LINE) {
            z /= k;
            sx += p.node; sy += z; sxx += (double)p.node * p.node; sxy += (double)p.node * z;
            n++;
        }
    }
    double slope = 0, icpt = 0;
    if (n >= 2 && n * sxx - sx * sx != 0) {
        slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        icpt = (sy - slope * sx) / n;
    }
    // hysteresis: a 3D view stays one while a few primitives keep depth (door / stair scenes draw only the door and its
    // frame for some frames: 28-29 primitives); one frame on the virtual screen flashed the 2D picture
    static bool was3d = false;
    // and a scene of few polygons all in 3D is one too (the descent down a rope: only the rope, 13-16 primitives, in the
    // dark), menus and pictures being 2D
    bool is3d = n >= (was3d ? 8 : 30) || (n >= 8 && n * 10 >= (int)fr.prims.size() * 9);
    was3d = is3d;

    extra_hide.assign(fr.prims.size(), 0);
    extra_prims.clear();
    if (is3d && world_extra_fn) world_extra_fn(fr, extra_hide, extra_prims);

    std::map<uint32_t, int> slot_rank;
    decal_planes.clear();
    const float inv = 1.0f / UNITS_PER_METER;
    for (size_t pi = 0; pi < fr.prims.size(); pi++) {
        const CapPrim &p = fr.prims[pi];
        if (p.kind == CapPrim::LINE || p.hidden || extra_hide[pi]) { st.dropped++; continue; }
        {
            static const char *only = getenv("PSXVR_ONLY");  // development: "tpage,clut[;tpage,clut...]" (hex): draw only those
            if (only) {
                bool keep = false;
                for (const char *q = only; q && *q; q = strchr(q, ';') ? strchr(q, ';') + 1 : nullptr) {
                    unsigned otp = 0, ocl = 0;
                    if (sscanf(q, "%x,%x", &otp, &ocl) == 2 && (p.tpage & 0x1ff) == otp && p.clut == ocl) keep = true;
                }
                if (!keep) continue;
            }
        }
        // The GPU rejects primitives spanning >= 1024x512 pixels (e.g. sprites the game mis-projects when they
        // are behind the camera); they never appear on a real PS1, so drop them here too.
        {
            int x0 = 1 << 30, x1 = -(1 << 30), y0 = 1 << 30, y1 = -(1 << 30);
            for (int i = 0; i < p.nv; i++) {
                x0 = std::min<int>(x0, p.v[i].sx); x1 = std::max<int>(x1, p.v[i].sx);
                y0 = std::min<int>(y0, p.v[i].sy); y1 = std::max<int>(y1, p.v[i].sy);
            }
            if (x1 - x0 >= 1024 || y1 - y0 >= 512) { st.dropped++; continue; }
            // Backdrop: in flight the game first fills the whole screen with a flat colour (sky colour, behind
            // the dome). Not part of the overlay: keep its colour for the empty space around the world instead.
            // Only a flat (not shaded) untextured rectangle exactly covering the display: big sky dome polygons
            // close to the camera also span the screen, but they are shaded. Its corners may carry a stale 3D tag
            // (the packet reuses memory that held projected points): ignore tags here, or it became a giant panel.
            if (is3d && !p.textured && !p.semi && !p.gouraud && x0 - p.off_x <= 0 && y0 - p.off_y <= 0 &&
                x1 - p.off_x >= fr.disp_w && y1 - p.off_y >= fr.disp_h) {
                backdrop[0] = p.v[0].r / 255.0f; backdrop[1] = p.v[0].g / 255.0f; backdrop[2] = p.v[0].b / 255.0f;
                have_backdrop = true;
                st.dropped++;
                continue;
            }
        }
        bool hud_model = false;
        int anchor = -1;
        for (int i = 0; i < p.nv; i++) {
            hud_model |= p.v[i].hud3d;
            if (p.v[i].derived && anchor < 0) anchor = i;
        }
        // an object sprite with a world ground point is a world object, whatever its other corners say
        if (anchor >= 0 && (p.tpage & 0x1f) == 0x1c) hud_model = false;
        // Object sprites: the game projects the object's ground point and builds the quad around it on screen
        // (anchor = bottom centre). Rebuild it in 3D at that exact point, upright in the world.
        if (sprite_mode == SpriteMode::DepthPlane && is3d && !hud_model && anchor >= 0 && p.nv == 4 && !p.v[0].has3d &&
            p.textured) {
            const CapVert &a = p.v[anchor];
            if (a.az < 8) { st.dropped++; continue; }
            // The game places the sprite around its anchor's screen position (real projection) but sizes it with
            // its own H: undo that around the anchor, then lift every corner to the anchor's depth.
            float hg = (float)std::max(1, fr.gte_h_game);
            // the anchor's screen point as the GTE gave it to the game (whole pixels): the corners were built from
            // it, so measuring them from the exact projection would add a sub-pixel error that changes as the head
            // turns (sprites wobbling by tens of cm at a distance)
            float asx = floorf(pj.ofx + a.ax * pj.h / a.az), asy = floorf(pj.ofy + a.ay * pj.h / a.az) + fr.y_shift;
            // a sprite right in front of the eyes (a spell just cast from the hand) would fill the view at its
            // world size: shrink it within 2.5 m, down to a quarter
            float dist = sqrtf(a.ax * a.ax + a.ay * a.ay + a.az * a.az) * inv;
            float near_k = std::max(0.25f, std::min(1.0f, dist / 2.5f));
            float off[4][2];
            for (int i = 0; i < 4; i++) {
                off[i][0] = (p.v[i].sx - p.off_x - asx) / hg * near_k * a.az * inv;
                off[i][1] = (p.v[i].sy - p.off_y - asy) / hg * near_k * a.az * inv;
            }
            if (fr.game_cam_valid) {
                // anchor in the world (game units: camera position + its heading), 1/8 m cells, plus the texture
                float yaw = fr.game_cam[2] * (6.2831853f / 2048.0f), c = cosf(yaw), sn = sinf(yaw);
                float wx = fr.game_cam[0] + c * a.ax + sn * a.az, wz = fr.game_cam[1] + sn * a.ax - c * a.az;
                float wy = (float)fr.game_cam[3] - a.ay;
                int umin = 255, vmin = 255;
                for (int i = 0; i < 4; i++) { umin = std::min<int>(umin, p.v[i].u); vmin = std::min<int>(vmin, p.v[i].v); }
                uint64_t key = ((uint64_t)((int)floorf(wx / 18) & 0xfff) << 52) | ((uint64_t)((int)floorf(wz / 18) & 0xfff) << 40) |
                               ((uint64_t)((int)floorf(wy / 18) & 0xfff) << 28) | ((uint64_t)(p.tpage & 0x1ff) << 19) |
                               ((uint64_t)(umin >> 3) << 14) | ((uint64_t)(vmin >> 3) << 9) | (uint64_t)(p.clut & 0x1ff);
                auto it = sprite_shapes.find(key);
                bool similar = it != sprite_shapes.end() && it->second.seen + 30 > shape_frame;
                if (similar)
                    for (int i = 0; i < 4 && similar; i++)
                        for (int j = 0; j < 2; j++)
                            if (fabsf(off[i][j] - it->second.off[i][j]) > 0.08f + 0.15f * fabsf(it->second.off[i][j])) similar = false;
                if (similar) {
                    for (int i = 0; i < 4; i++)
                        for (int j = 0; j < 2; j++) {
                            float &o = it->second.off[i][j];
                            o += (off[i][j] - o) * 0.15f;
                            off[i][j] = o;
                        }
                    it->second.seen = shape_frame;
                } else {
                    SpriteShape sh;
                    memcpy(sh.off, off, sizeof off);
                    sh.seen = shape_frame;
                    sprite_shapes[key] = sh;
                }
            }
            float pos[4][3];
            for (int i = 0; i < 4; i++) {
                pos[i][0] = a.ax * inv + off[i][0];
                pos[i][1] = -a.ay * inv - off[i][1];
                pos[i][2] = -a.az * inv;
            }
            emit_tag = "sprite";
            emit_prim(world_v, world_b, p, pos, 5e-5f);
            static const bool spr_dump = getenv("PSXVR_SPRDUMP") != nullptr;
            if (spr_dump)
                printf("spr %.4f %.4f %.4f | %.4f %.4f %.4f %.4f %.4f %.4f\n", a.ax * inv, -a.ay * inv, -a.az * inv, pos[0][0],
                       pos[0][1], pos[0][2], pos[3][0], pos[3][1], pos[3][2]);
            st.sprite_prims++;
            st.anchored++;
            anchors.push_back({a.ax * inv, -a.ay * inv, -a.az * inv, 0});
            continue;
        }
        if (is3d && !hud_model && anchor >= 0 && p.nv == 4 && !p.v[0].has3d) {
            int x0 = 1 << 30, x1 = -(1 << 30), y0 = 1 << 30, y1 = -(1 << 30);
            for (int i = 0; i < 4; i++) {
                x0 = std::min<int>(x0, p.v[i].sx); x1 = std::max<int>(x1, p.v[i].sx);
                y0 = std::min<int>(y0, p.v[i].sy); y1 = std::max<int>(y1, p.v[i].sy);
            }
            const CapVert &a = p.v[anchor];
            float wpx = (float)(x1 - x0), hpx = (float)(y1 - y0);
            if (wpx <= 0 || hpx <= 0 || a.az < 8) { st.dropped++; continue; }
            float hg = (float)std::max(1, fr.gte_h_game);
            // The game sizes sprites by the object's true 3D distance (sqrt(x^2+y^2+z^2), see 0x8004dda0), which
            // does not depend on where the camera looks; using view depth would make them grow/shrink with the head.
            float dist = sqrtf(a.ax * a.ax + a.ay * a.ay + a.az * a.az);
            float w = wpx * dist / hg * inv, h = hpx * dist / hg * inv;
            // the game's ground point sits 6 units under the floor polygon; lift it onto the floor
            float base[3] = {a.ax * inv, -(a.ay - 6) * inv, -a.az * inv};
            // World up as seen from the camera that drew the frame (scene space = that camera's space).
            float pitch = fr.cam_pitch * (6.2831853f / 65536.0f);
            float wup[3] = {0, cosf(pitch), -sinf(pitch)};
            float cxs = (x0 + x1) * 0.5f;
            float pos[4][3];
            // Low objects (bones, blood, coins...) are drawn 'lying' in the middle of their sprite: lay them on
            // the floor as decals whose outline, seen from the game camera, is exactly the original sprite.
            // The top corners are where the rays from the camera through the sprite's top corners hit the floor.
            bool flat = h < 0.25f;
            float up_cam[3] = {0, 1, 0};  // the original sprite plane (faces the game camera)
            for (int i = 0; i < 4 && flat; i++) {
                float fx = (p.v[i].sx - cxs) / wpx, fy = (y1 - p.v[i].sy) / hpx;
                float q[3];
                for (int c = 0; c < 3; c++) q[c] = base[c] + (c == 0 ? fx * w : 0) + up_cam[c] * fy * h;
                if (fy < 0.01f) { memcpy(pos[i], q, sizeof q); continue; }
                float nd = wup[0] * q[0] + wup[1] * q[1] + wup[2] * q[2];
                float nb = wup[0] * base[0] + wup[1] * base[1] + wup[2] * base[2];
                float t = nd < -1e-4f ? nb / nd : -1;  // ray from the camera (origin) through q, onto the floor plane
                float bd = std::sqrt(base[0] * base[0] + base[1] * base[1] + base[2] * base[2]);
                if (t <= 0 || t * std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]) > bd + 1.5f) { flat = false; break; }
                for (int c = 0; c < 3; c++) pos[i][c] = q[c] * t;
            }
            if (!flat) {
                // taller things stand upright in the world
                for (int i = 0; i < 4; i++) {
                    float fx = (p.v[i].sx - cxs) / wpx, fy = (y1 - p.v[i].sy) / hpx;
                    for (int c = 0; c < 3; c++) pos[i][c] = base[c] + (c == 0 ? fx * w : 0) + wup[c] * fy * h;
                }
            }
            emit_prim(world_v, world_b, p, pos, 5e-5f);  // in front of the floor it lies on
            st.sprite_prims++;
            st.anchored++;
            anchors.push_back({base[0], base[1], base[2], w});
            continue;
        }
        int k = 0;
        float zsum = 0;
        for (int i = 0; i < p.nv; i++)
            if (p.v[i].has3d) { zsum += p.v[i].z; k++; }
        bool world = false;
        float zfill = 0;
        if (hud_model) {
            // compass etc.: stays in the 2D overlay
        } else if (is3d && k > 0) {
            world = true;
            zfill = zsum / k;
            st.world_prims++;
        } else if (is3d && p.node > 0 && p.node < max_node && (p.tpage & 0x1f) == 0x1c) {
            // object sprite page without an exact anchor: estimate the depth from its ordering-table slot
            // (everything else untracked is 2D overlay: flasks, power gem, messages...)
            world = true;
            zfill = (float)(slope * p.node + icpt);
            st.sprite_prims++;
            st.fitted++;
        }
        if (world) {
            if (zfill < 8) { st.dropped++; continue; }
            float pos[4][3];
            bool bad = false;
            int rank = slot_rank[p.node]++;
            // Sprites (no tracked vertex): the game positions them with the real projection but sizes them
            // with its own H, so rescale the corners around the centre to undo a widened field of view.
            float cx = 0, cy = 0, size_k = 1;
            if (k == 0) {
                for (int i = 0; i < p.nv; i++) { cx += p.v[i].sx; cy += p.v[i].sy; }
                cx /= p.nv;
                cy /= p.nv;
                size_k = pj.h / (float)std::max(1, fr.gte_h_game);
            }
            float dbias = 0.0004f * std::min(rank, 25);  // metres: coplanar surfaces in one OT slot, later ones in front
            for (int i = 0; i < p.nv; i++) {
                const CapVert &v = p.v[i];
                float x, y, z;
                if (v.has3d) { x = v.x; y = v.y; z = v.z; }
                else {
                    float sx = cx + (v.sx - cx) * size_k, sy = cy + (v.sy - cy) * size_k;
                    if (k > 0) { sx = v.sx; sy = v.sy; }
                    // screen y includes the look up / down shift the game adds to every projected point: remove it
                    // (else a vertex without its own 3D point lands metres too high or low: spikes in the ground)
                    float px = sx - p.off_x - pj.ofx, py = sy - p.off_y - pj.ofy - fr.y_shift;
                    if (std::abs(px) > 1100 || std::abs(py) > 1100) bad = true;
                    z = zfill;
                    x = px * z / pj.h;
                    y = py * z / pj.h;
                }
                pos[i][0] = x * inv;
                pos[i][1] = -y * inv;
                pos[i][2] = -z * inv;
            }
            if (bad) { st.dropped++; continue; }
            if (point_warp && k == p.nv) point_warp(fr, p, pos);  // the game's corrections of its rooms
            if (is_sky(p)) {
                // The game mirrors the dome on screen after projecting it, so its 3D points are not where the
                // player sees the sky: take each corner's direction from where it was drawn (game projection,
                // look up/down shear removed) and push it far away.
                for (int i = 0; i < p.nv; i++) {
                    float dx = (p.v[i].sx - p.off_x - pj.ofx) / pj.h;
                    float dy = (p.v[i].sy - p.off_y - pj.ofy - fr.y_shift) / pj.h;
                    float l = std::sqrt(dx * dx + dy * dy + 1.0f);
                    pos[i][0] = dx * SKY_DISTANCE_M / l;
                    pos[i][1] = -dy * SKY_DISTANCE_M / l;
                    pos[i][2] = -SKY_DISTANCE_M / l;
                }
                emit_prim(sky_v, sky_b, p, pos, 0);
                if (!p.textured && p.nv >= 3) fresh_sky.push_back(&p);
                continue;
            }
            emit_tag = k > 0 ? "world3d" : "fitted";
            // Decals: the game paints a poster on a wall, an item or a blood pool on the floor after the surface under
            // it, on (nearly) the same plane; the PS1 has no depth buffer, so the later one simply covers it. Here
            // they fought over the depth (flickering, vanishing as the head moved): a polygon drawn after another,
            // overlapping it on screen, parallel and from 2.5 cm in front of its plane to 15 cm behind it, is pulled
            // towards the eye in depth just in front of that one (+8 mm).
            {
                DecalPlane me;
                me.model = false;
                for (int i = 0; i < p.nv && !me.model; i++)
                    me.model = std::find(model_pcs.begin(), model_pcs.end(), p.v[i].src_pc) != model_pcs.end();
                float e1[3], e2[3];
                for (int c = 0; c < 3; c++) e1[c] = pos[1][c] - pos[0][c], e2[c] = pos[2][c] - pos[0][c];
                me.n[0] = e1[1] * e2[2] - e1[2] * e2[1];
                me.n[1] = e1[2] * e2[0] - e1[0] * e2[2];
                me.n[2] = e1[0] * e2[1] - e1[1] * e2[0];
                float l = std::sqrt(me.n[0] * me.n[0] + me.n[1] * me.n[1] + me.n[2] * me.n[2]);
                me.x0 = me.y0 = 1e9f, me.x1 = me.y1 = -1e9f;
                for (int i = 0; i < p.nv; i++) {
                    me.x0 = std::min(me.x0, (float)p.v[i].sx); me.x1 = std::max(me.x1, (float)p.v[i].sx);
                    me.y0 = std::min(me.y0, (float)p.v[i].sy); me.y1 = std::max(me.y1, (float)p.v[i].sy);
                }
                me.bias = 0;
                bool decal = false;
                // outline on screen (a PS1 quad's corners are 0, 1, 3, 2 around)
                static const int ord4[4] = {0, 1, 3, 2};
                me.np = p.nv == 4 ? 4 : 3;
                for (int i = 0; i < me.np; i++) {
                    const CapVert &cv = p.v[p.nv == 4 ? ord4[i] : i];
                    me.poly[i][0] = (float)cv.sx, me.poly[i][1] = (float)cv.sy;
                }
                if (l > 1e-8f) {
                    for (float &c : me.n) c /= l;
                    me.d = -(me.n[0] * pos[0][0] + me.n[1] * pos[0][1] + me.n[2] * pos[0][2]);
                    for (const DecalPlane &o : decal_planes) {
                        if (o.x1 <= me.x0 || me.x1 <= o.x0 || o.y1 <= me.y0 || me.y1 <= o.y0) continue;
                        // nothing is painted on a character: a sign drawn after two children standing a few cm in front
                        // of it was taken for a decal on their faces and covered them
                        if (o.model) continue;
                        float dn = o.n[0] * me.n[0] + o.n[1] * me.n[1] + o.n[2] * me.n[2];
                        if (std::fabs(dn) < 0.97f) continue;
                        // signed distances of my corners from its plane, positive towards the eye (scene origin): the
                        // later polygon may sit on it or even a little behind it (notes pinned on a board whose face is
                        // in front of the wall they are modelled on), the PS1 shows it on top either way
                        float side = o.d >= 0 ? 1.0f : -1.0f, dmin = 1e9f, dmax = -1e9f;
                        for (int i = 0; i < p.nv; i++) {
                            float dd = side * (o.n[0] * pos[i][0] + o.n[1] * pos[i][1] + o.n[2] * pos[i][2] + o.d);
                            dmin = std::min(dmin, dd), dmax = std::max(dmax, dd);
                        }
                        // it must really cover it on screen (the outlines overlap by more than a pixel or two):
                        // neighbouring floor tiles only touch, and their bounding boxes overlapping chained the pulls
                        // up and made the items on the floor come and go
                        // o.bias is its whole pull (its own and its place in the ordering-table slot): a magazine 18 mm
                        // under a floor tile that was itself pulled 1 cm for its slot stayed hidden under it
                        // The pull is along the line of sight while the gap is measured across the plane: seen at a slant
                        // (a floor a few metres ahead, ~20 degrees) 18 mm across are 5 cm along the sight line, so the
                        // gap is divided by the cosine between the line of sight and the plane's normal.
                        if (dmax < 0.025f && dmin > -0.15f && overlap_area(me.poly, me.np, o.poly, o.np) > 2.0f) {
                            float c[3] = {0, 0, 0};
                            for (int i = 0; i < p.nv; i++)
                                for (int a = 0; a < 3; a++) c[a] += pos[i][a] / p.nv;
                            float cl = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
                            float cosv = cl > 1e-6f ? std::fabs(o.n[0] * c[0] + o.n[1] * c[1] + o.n[2] * c[2]) / cl : 1.0f;
                            cosv = std::max(cosv, 0.12f);
                            me.bias = std::max(me.bias, o.bias + (std::max(0.0f, -dmin) + 0.008f) / cosv);
                            decal = true;
                        }
                    }
                    me.bias = std::min(me.bias, 0.35f);
                    static const char *ddbg = getenv("PSXVR_DECALDBG");  // development: "tpage,clut" (hex)
                    if (ddbg) {
                        unsigned dtp = 0, dcl = 0;
                        if (sscanf(ddbg, "%x,%x", &dtp, &dcl) == 2 && (p.tpage & 0x1ff) == dtp && p.clut == dcl)
                            printf("decal prim %zu bias %.4f rank %d k %d pos0 %.3f %.3f %.3f\n", pi, me.bias, rank, k, pos[0][0], pos[0][1], pos[0][2]);
                    }
                    if (decal) st.decals++;
                    me.bias = std::max(me.bias, dbias);  // the total pull (slot order or decal, the larger)
                    decal_planes.push_back(me);
                }
                dbias = std::max(dbias, me.bias);
            }
            {
                static const float biasx = getenv("PSXVR_BIASX") ? (float)atof(getenv("PSXVR_BIASX")) : 1.0f;  // development
                dbias *= biasx;
            }
            // Characters into the floor: the PS1 draws the floor before them, so bodies lying up to 15 cm into it (and
            // feet a little under it) still show whole. A point of a character (set_model_pcs) under the game's ground
            // is pulled in depth (only) to the floor's surface along the line of sight, plus 1 cm: nothing else changes
            // (the room's own geometry under the floor, a manhole's shaft, stays hidden).
            bool model = false;
            for (int i = 0; i < p.nv && !model; i++)
                model = std::find(model_pcs.begin(), model_pcs.end(), p.v[i].src_pc) != model_pcs.end();
            static const bool no_sink = getenv("PSXVR_NOSINK") != nullptr;  // development
            if (model && actor_ground > 0 && !no_sink) {
                float pitch = fr.cam_pitch * (6.2831853f / 65536.0f);
                float wup[3] = {0, cosf(pitch), -sinf(pitch)};
                for (int i = 0; i < p.nv; i++) {
                    float h = wup[0] * pos[i][0] + wup[1] * pos[i][1] + wup[2] * pos[i][2];  // over the eye (negative)
                    float sink = -actor_ground - h;
                    if (sink <= 0 || h >= -0.3f) continue;
                    float d = std::sqrt(pos[i][0] * pos[i][0] + pos[i][1] * pos[i][1] + pos[i][2] * pos[i][2]);
                    emit_vbias[i] = std::min((sink + 0.01f) * d / -h, 1.0f);  // the gap along the sight line
                }
            }
            emit_prim(world_v, world_b, p, pos, dbias);
            for (float &b : emit_vbias) b = 0;
        } else {
            // the game's red cross (23x23, page 0x08d / CLUT 0x7900): replaced by the VR laser when that drives it
            if (hide_cursor && p.nv == 4 && (p.tpage & 0x1ff) == 0x08d && p.clut == 0x7900 &&
                std::abs(p.v[1].sx - p.v[0].sx) == 23 && std::abs(p.v[2].sy - p.v[0].sy) == 23)
                continue;
            float pos[4][3];
            for (int i = 0; i < p.nv; i++) {
                // screen position = from the origin of the drawing area (games that project around 0,0 and centre
                // the picture with the drawing offset, like RE Survivor, have offset = area origin + half screen)
                float sx = (float)(p.v[i].sx - p.clip_x1), sy = (float)(p.v[i].sy - p.clip_y1);
                if (p.v[i].hud_reproj) {
                    // HUD models (weapon arm, fist, compass) are projected around the world centre, so the widened
                    // field of view shrinks them: draw them where the game's own H puts them, as the original did
                    sx = p.v[i].gsx + (float)(p.off_x - p.clip_x1);
                    sy = p.v[i].gsy + (float)(p.off_y - p.clip_y1);
                }
                pos[i][0] = sx / (float)fr.disp_w * 2 - 1;
                pos[i][1] = 1 - sy / (float)fr.disp_h * 2;
                pos[i][2] = 0.5f;
            }
            emit_prim(hud_v, hud_b, p, pos);
            st.hud_prims++;
            if ((p.tpage & 0x1f) == 0x1c) {
                st.hud_objects++;
                if (getenv("PSXVR_HUDWHY"))
                    printf("HUD obj: node %x max %x is3d %d k %d anchor-derived %d%d%d%d has3d %d%d%d%d hud3d %d sx %d,%d %d,%d\n", p.node, max_node, is3d, k,
                           p.v[0].derived, p.v[1].derived, p.v[2].derived, p.v[3].derived, p.v[0].has3d, p.v[1].has3d,
                           p.v[2].has3d, p.v[3].has3d, p.v[0].hud3d, p.v[0].sx, p.v[0].sy, p.v[3].sx, p.v[3].sy);
            }
        }
    }
    for (const ExtraPrim &e : extra_prims) {
        float pos[4][3];
        for (int i = 0; i < e.p.nv; i++) {
            pos[i][0] = e.pos[i][0] * inv;
            pos[i][1] = -e.pos[i][1] * inv;
            pos[i][2] = -e.pos[i][2] * inv;
        }
        emit_tag = "extra";
        emit_prim(world_v, world_b, e.p, pos);
        st.world_prims++;
    }
    update_sky_memory(fr, pj, is3d);
    make_hud_target(std::max(64, fr.disp_w), std::max(64, fr.disp_h));
    return is3d;
}

bool raycast(const float o[3], const float d[3], float &t_hit) {
    // Moller-Trumbore over the world triangles (scene space, metres)
    float best = 1e30f;
    for (size_t i = 0; i + 2 < world_v.size(); i += 3) {
        const Vtx &a = world_v[i], &b = world_v[i + 1], &c = world_v[i + 2];
        float e1[3] = {b.x - a.x, b.y - a.y, b.z - a.z}, e2[3] = {c.x - a.x, c.y - a.y, c.z - a.z};
        float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
        float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
        if (std::fabs(det) < 1e-9f) continue;
        float inv_det = 1.0f / det;
        float s[3] = {o[0] - a.x, o[1] - a.y, o[2] - a.z};
        float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv_det;
        if (u < 0 || u > 1) continue;
        float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
        float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv_det;
        if (v < 0 || u + v > 1) continue;
        float t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv_det;
        if (t > 0.01f && t < best) best = t;
    }
    if (best > 1e29f) return false;
    t_hit = best;
    return true;
}

bool project_to_screen(const float p[3], float &sx, float &sy) {
    // scene space (x right, y up, z backwards, metres) -> PS1 camera space -> the game's screen
    float x = p[0] * UNITS_PER_METER, y = -p[1] * UNITS_PER_METER, z = -p[2] * UNITS_PER_METER;
    if (z < 1) return false;
    sx = proj_ofx + x * proj_h / z;
    sy = proj_ofy + y * proj_h / z;
    return true;
}

void set_hide_cursor(bool hide) { hide_cursor = hide; }

namespace {

void upload(const std::vector<Vtx> &v) {
    if (v.size() > vbuf_cap) {
        if (vbuf) vbuf->Release();
        vbuf_cap = (uint32_t)v.size() * 2;
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = vbuf_cap * sizeof(Vtx);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        dev->CreateBuffer(&bd, nullptr, &vbuf);
    }
    if (v.empty()) return;
    D3D11_MAPPED_SUBRESOURCE m;
    ctx->Map(vbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    memcpy(m.pData, v.data(), v.size() * sizeof(Vtx));
    ctx->Unmap(vbuf, 0);
}

void set_common(const Mat4 &mvp, uint32_t pass, bool srgb) {
    CB cb = {mvp, pass, srgb ? 1u : 0u, fog_now ? 1u : 0u, 0, {0.55f, 0.58f, 0.78f, 1}, fog_start_m, fog_end_m, 0, 0};
    if (have_backdrop) for (int i = 0; i < 3; i++) cb.fog_color[i] = backdrop[i];
    ctx->UpdateSubresource(cbuf, 0, nullptr, &cb, 0, 0);
    UINT stride = sizeof(Vtx), offset = 0;
    ctx->IASetVertexBuffers(0, 1, &vbuf, &stride, &offset);
    ctx->IASetInputLayout(layout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &cbuf);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &cbuf);
    ctx->PSSetShaderResources(0, 1, &vram_srv);
    ctx->RSSetState(raster);
}

void set_blend(int mode) {
    float f = mode == 3 ? 0.25f : 0.5f;
    float bf[4] = {f, f, f, f};
    ctx->OMSetBlendState(mode < 0 ? bs_opaque : bs_blend[mode], bf, 0xffffffff);
}

void draw_batches(const Mat4 &mvp, const std::vector<Batch> &batches, bool srgb, bool depth) {
    // opaque pixels of everything, then semi-transparent pixels in submission order
    set_common(mvp, 0, srgb);
    set_blend(-1);
    ctx->OMSetDepthStencilState(depth ? ds_write : ds_none, 0);
    if (depth) {
        uint32_t total = 0;
        for (auto &b : batches) total = std::max(total, b.first + b.count);
        ctx->Draw(total, 0);
        set_common(mvp, 1, srgb);
        ctx->OMSetDepthStencilState(ds_test, 0);
        for (auto &b : batches) {
            if (!b.semi) continue;
            set_blend(b.blend);
            ctx->Draw(b.count, b.first);
        }
    } else {
        for (auto &b : batches) {
            set_common(mvp, 0, srgb);
            set_blend(-1);
            ctx->Draw(b.count, b.first);
            if (b.semi) {
                set_common(mvp, 1, srgb);
                set_blend(b.blend);
                ctx->Draw(b.count, b.first);
            }
        }
    }
    set_blend(-1);
}

}  // namespace

void set_sprite_mode(SpriteMode m) { sprite_mode = m; }

void set_sky_pcs(const uint32_t *pcs, int n) { sky_pcs.assign(pcs, pcs + n); }

void draw_world(const Mat4 &view_proj, bool srgb) {
    if (!skymem_v.empty()) {  // the remembered whole dome, under the part the game drew this time
        upload(skymem_v);
        draw_batches(view_proj, skymem_b, srgb, false);
    }
    if (!sky_v.empty()) {  // backdrop first, without depth: everything in the world draws over it
        upload(sky_v);
        draw_batches(view_proj, sky_b, srgb, false);
    }
    if (world_v.empty()) return;
    upload(world_v);
    fog_now = fog_end_m > 0;
    draw_batches(view_proj, world_b, srgb, true);
    fog_now = false;
}

ID3D11ShaderResourceView *render_hud(bool srgb_target) {
    float clear[4] = {0, 0, 0, 0};
    ctx->ClearRenderTargetView(hud_rtv, clear);
    if (!hud_v.empty()) {
        upload(hud_v);
        ctx->OMSetRenderTargets(1, &hud_rtv, nullptr);
        D3D11_VIEWPORT vp = {0, 0, (float)hud_w, (float)hud_h, 0, 1};
        ctx->RSSetViewports(1, &vp);
        draw_batches(mat_identity(), hud_b, false, false);
    }
    return srgb_target ? hud_srv_srgb : hud_srv_lin;
}

bool hud_bmp(const char *path) {
    render_hud(false);
    D3D11_TEXTURE2D_DESC td;
    hud_tex->GetDesc(&td);
    td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *st;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &st))) return false;
    ctx->CopyResource(st, hud_tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) return false;
    FILE *f = fopen(path, "wb");
    int w = td.Width, h = td.Height, row = w * 3, pad = (4 - row % 4) % 4;
    uint32_t size = 54 + (row + pad) * h, off = 54, ihs = 40, pb = 1 | (24 << 16);
    uint8_t hdr[54] = {'B', 'M'};
    memcpy(hdr + 2, &size, 4); memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &ihs, 4);
    memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4); memcpy(hdr + 26, &pb, 4);
    fwrite(hdr, 1, 54, f);
    for (int y = h - 1; y >= 0; y--) {
        const uint8_t *src = (const uint8_t *)m.pData + y * m.RowPitch;
        for (int x = 0; x < w; x++) {
            // alpha 0 = transparent: show as dark grey
            uint8_t a = src[x * 4 + 3];
            uint8_t bgr[3] = {a ? src[x * 4 + 2] : (uint8_t)40, a ? src[x * 4 + 1] : (uint8_t)40, a ? src[x * 4] : (uint8_t)40};
            fwrite(bgr, 1, 3, f);
        }
        uint8_t z[3] = {0, 0, 0};
        fwrite(z, 1, pad, f);
    }
    fclose(f);
    ctx->Unmap(st, 0);
    st->Release();
    return true;
}

bool preview_bmp(const char *path, int w, int h, const Mat4 &camera_pose, float fov_deg, const Mat4 &model,
                 void (*extra)(const Mat4 &view_proj)) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *rt, *depth, *staging;
    dev->CreateTexture2D(&td, nullptr, &rt);
    td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    dev->CreateTexture2D(&td, nullptr, &staging);
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.CPUAccessFlags = 0;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    dev->CreateTexture2D(&td, nullptr, &depth);
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    dev->CreateRenderTargetView(rt, nullptr, &rtv);
    dev->CreateDepthStencilView(depth, nullptr, &dsv);
    float clear[4] = {0.15f, 0.0f, 0.15f, 1};
    if (have_backdrop) clear[0] = backdrop[0], clear[1] = backdrop[1], clear[2] = backdrop[2];
    ctx->ClearRenderTargetView(rtv, clear);
    ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    ctx->OMSetRenderTargets(1, &rtv, dsv);
    D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
    ctx->RSSetViewports(1, &vp);
    float t = tanf(fov_deg * 3.14159265f / 360.0f);
    float a = (float)h / w;
    // near plane as in the headset (3 cm): the depth-only decal offsets are worked out for it
    Mat4 vpm = mat_mul(mat_frustum(-t, t, -t * a, t * a, 0.03f, 200.0f), mat_rigid_inverse(camera_pose));
    draw_world(mat_mul(vpm, model), false);
    if (extra) extra(vpm);
    ctx->CopyResource(staging, rt);
    D3D11_MAPPED_SUBRESOURCE m;
    bool ok = false;
    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
        FILE *f = fopen(path, "wb");
        if (f) {
            int row = w * 3, pad = (4 - row % 4) % 4;
            uint32_t size = 54 + (row + pad) * h, off = 54, ihs = 40, pb = 1 | (24 << 16);
            uint8_t hdr[54] = {'B', 'M'};
            memcpy(hdr + 2, &size, 4); memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &ihs, 4);
            memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4); memcpy(hdr + 26, &pb, 4);
            fwrite(hdr, 1, 54, f);
            for (int y = h - 1; y >= 0; y--) {
                const uint8_t *src = (const uint8_t *)m.pData + y * m.RowPitch;
                for (int x = 0; x < w; x++) {
                    uint8_t bgr[3] = {src[x * 4 + 2], src[x * 4 + 1], src[x * 4]};
                    fwrite(bgr, 1, 3, f);
                }
                uint8_t z[3] = {0, 0, 0};
                fwrite(z, 1, pad, f);
            }
            fclose(f);
            ok = true;
        }
        ctx->Unmap(staging, 0);
    }
    rtv->Release(); dsv->Release(); rt->Release(); depth->Release(); staging->Release();
    return ok;
}

}  // namespace scene
