// VR hands (see vr_hands.h): smooth procedural hands (rounded palm, capsule phalanges, nails, robe sleeve) built
// around the controller grip, lit like the outdoors, plus an additive aim ray.
#include "vr_hands.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../core/psx.h"
#include "vr_glove.h"

namespace vrhands {

namespace {

struct V {
    float p[3], n[3];
    uint8_t c[4];
};

struct CB {
    Mat4 vp;
    uint32_t srgb, additive, pad[2];
};

ID3D11Device *dev;
ID3D11DeviceContext *ctx;
ID3D11VertexShader *vs;
ID3D11PixelShader *ps;
ID3D11InputLayout *layout;
ID3D11Buffer *cbuf, *vbuf;
UINT vbuf_cap = 0;
ID3D11RasterizerState *raster;
ID3D11DepthStencilState *ds_write, *ds_test;
ID3D11BlendState *bs_opaque, *bs_add;

const char *src = R"(
cbuffer C : register(b0) { row_major float4x4 vp; uint srgb; uint additive; uint2 pad; };
struct VI { float3 p : POSITION; float3 n : NORMAL; float4 c : COLOR; };
struct VO { float4 p : SV_Position; float3 n : NORMAL; float4 c : COLOR; };
VO vs_main(VI i) { VO o; o.p = mul(vp, float4(i.p, 1)); o.n = i.n; o.c = i.c; return o; }
float4 ps_main(VO i) : SV_Target {
    float3 c = i.c.rgb;
    if (!additive) {
        // outdoor light: warm sun from above, blue sky fill, a little warm light scattered through the skin
        float3 n = normalize(i.n);
        float3 sun = normalize(float3(0.35, 0.85, 0.4));
        float d = dot(n, sun);
        float wrap = saturate((d + 0.35) / 1.35);                     // soft terminator
        float sky = 0.5 + 0.5 * n.y;
        float3 light = (float3(0.85, 0.88, 1.0) * wrap * 0.75 + lerp(float3(0.22, 0.2, 0.2), float3(0.30, 0.33, 0.42), sky)) * 0.7;
        float3 sss = float3(0.25, 0.06, 0.03) * saturate(1 - abs(d)) * i.c.a;  // skin only (alpha = skin amount)
        c = c * light + c * sss;
    }
    if (srgb) c = pow(saturate(c), 2.2);
    return float4(c * (additive ? i.c.a : 1), 1);
}
)";

// a box of size s centred on the origin of m (m: local -> LOCAL space); used for the aim ray
void box(std::vector<V> &out, const Mat4 &m, float sx, float sy, float sz, uint32_t rgb, uint8_t alpha = 255) {
    static const float N[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const float U[6][3] = {{0, 1, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}};
    uint8_t c[4] = {(uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb, alpha};
    const float h[3] = {sx * 0.5f, sy * 0.5f, sz * 0.5f};
    for (int f = 0; f < 6; f++) {
        const float *n = N[f], *u = U[f];
        float w[3] = {n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0]};
        V q[4];
        const float cs[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (int k = 0; k < 4; k++) {
            float lp[3];
            for (int i = 0; i < 3; i++) lp[i] = (n[i] + u[i] * cs[k][0] + w[i] * cs[k][1]) * h[i];
            for (int i = 0; i < 3; i++) {
                q[k].p[i] = m.m[i][0] * lp[0] + m.m[i][1] * lp[1] + m.m[i][2] * lp[2] + m.m[i][3];
                q[k].n[i] = m.m[i][0] * n[0] + m.m[i][1] * n[1] + m.m[i][2] * n[2];
            }
            memcpy(q[k].c, c, 4);
        }
        const int idx[6] = {0, 1, 2, 0, 2, 3};
        for (int i : idx) out.push_back(q[i]);
    }
}

Mat4 T(float x, float y, float z) { return mat_translate(x, y, z); }
Mat4 M(const Mat4 &a, const Mat4 &b) { return mat_mul(a, b); }

// ---------------------------------------------------------------- smooth hand
struct Vec {
    float x, y, z;
};
Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec operator*(Vec a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec norm(Vec a) {
    float l = sqrtf(dot(a, a));
    return l > 1e-9f ? a * (1 / l) : Vec{0, 1, 0};
}
// rotate v about the unit axis k by angle a (Rodrigues)
Vec rot(Vec v, Vec k, float a) {
    float c = cosf(a), s = sinf(a);
    return v * c + cross(k, v) * s + k * (dot(k, v) * (1 - c));
}

struct Col {
    float r, g, b, skin;  // skin: how much light scatters through it (0 cloth, 1 skin)
};
const Col SKIN = {0.86f, 0.62f, 0.50f, 1}, SKIN_PALM = {0.90f, 0.66f, 0.56f, 1}, NAIL = {0.93f, 0.76f, 0.70f, 0.4f},
          SLEEVE = {0.24f, 0.17f, 0.50f, 0}, SLEEVE_IN = {0.17f, 0.12f, 0.36f, 0}, CUFF = {0.80f, 0.63f, 0.25f, 0};

// Mesh builder in the hand's own frame (metres): X towards the thumb along the knuckles, Y along the fingers,
// Z out of the palm. 'xf' places it in LOCAL space (it may mirror: the left hand).
struct HandMesh {
    std::vector<V> &out;
    Mat4 xf;
    void vert(Vec p, Vec n, const Col &c) {
        V v;
        v.p[0] = xf.m[0][0] * p.x + xf.m[0][1] * p.y + xf.m[0][2] * p.z + xf.m[0][3];
        v.p[1] = xf.m[1][0] * p.x + xf.m[1][1] * p.y + xf.m[1][2] * p.z + xf.m[1][3];
        v.p[2] = xf.m[2][0] * p.x + xf.m[2][1] * p.y + xf.m[2][2] * p.z + xf.m[2][3];
        v.n[0] = xf.m[0][0] * n.x + xf.m[0][1] * n.y + xf.m[0][2] * n.z;
        v.n[1] = xf.m[1][0] * n.x + xf.m[1][1] * n.y + xf.m[1][2] * n.z;
        v.n[2] = xf.m[2][0] * n.x + xf.m[2][1] * n.y + xf.m[2][2] * n.z;
        v.c[0] = (uint8_t)(c.r * 255); v.c[1] = (uint8_t)(c.g * 255); v.c[2] = (uint8_t)(c.b * 255);
        v.c[3] = (uint8_t)(c.skin * 255);
        out.push_back(v);
    }
    // a grid of points (rows x cols, cols wrapping around) as triangles
    void grid(const std::vector<Vec> &p, const std::vector<Vec> &n, int rows, int cols, const Col &c) {
        for (int r = 0; r + 1 < rows; r++)
            for (int k = 0; k < cols; k++) {
                int k1 = (k + 1) % cols;
                int a = r * cols + k, b = r * cols + k1, d = (r + 1) * cols + k, e = (r + 1) * cols + k1;
                vert(p[a], n[a], c); vert(p[d], n[d], c); vert(p[b], n[b], c);
                vert(p[b], n[b], c); vert(p[d], n[d], c); vert(p[e], n[e], c);
            }
    }
    // tapered capsule from a (radius ra) to b (radius rb); 'up' fixes the seam; flat < 1 squashes it along 'up'
    void capsule(Vec a, Vec b, float ra, float rb, const Col &c, Vec up = {0, 0, 1}, float flat = 1.0f) {
        const int S = 12, CAP = 4;
        Vec ax = norm(b - a), u = norm(cross(ax, cross(up, ax))), w = cross(ax, u);
        std::vector<Vec> P, N;
        int rows = 0;
        auto ring = [&](Vec ctr, float r, float axial_n) {  // axial_n: normal tilt along the axis (-1..1)
            for (int k = 0; k < S; k++) {
                float t = 6.2831853f * k / S, cu = cosf(t), sw = sinf(t);
                Vec radial = u * (cu * flat) + w * sw;
                float rad = sqrtf(std::max(0.0f, 1 - axial_n * axial_n));
                P.push_back(ctr + radial * (r * rad));
                N.push_back(norm(u * (cu / std::max(0.3f, flat)) * rad + w * (sw * rad) + ax * axial_n));
            }
            rows++;
        };
        for (int i = CAP; i >= 1; i--) {  // cap at a
            float t = 1.5707963f * i / CAP;
            ring(a - ax * (ra * sinf(t)), ra, -sinf(t));
        }
        for (int i = 0; i <= 3; i++) {    // body
            float f = i / 3.0f;
            ring(a + (b - a) * f, ra + (rb - ra) * f, 0);
        }
        for (int i = 1; i <= CAP; i++) {  // cap at b
            float t = 1.5707963f * i / CAP;
            ring(b + ax * (rb * sinf(t)), rb, sinf(t));
        }
        grid(P, N, rows, S, c);
    }
    // rounded box (superellipsoid) centred at ctr with half sizes h, exponent e (0.1 = boxy, 1 = ellipsoid)
    void blob(Vec ctr, Vec h, float e, const Col &c) {
        const int R = 12, S = 20;
        auto sp = [](float v, float p) { return (v < 0 ? -1.0f : 1.0f) * powf(fabsf(v), p); };
        std::vector<Vec> P, N;
        for (int i = 0; i <= R; i++) {
            float lat = -1.5707963f + 3.1415927f * i / R;
            for (int k = 0; k < S; k++) {
                float lon = 6.2831853f * k / S;
                float cl = cosf(lat), sl = sinf(lat), co = cosf(lon), so = sinf(lon);
                P.push_back(ctr + Vec{h.x * sp(cl, e) * sp(co, e), h.y * sp(cl, e) * sp(so, e), h.z * sp(sl, e)});
                N.push_back(norm(Vec{sp(cl, 2 - e) * sp(co, 2 - e) / h.x, sp(cl, 2 - e) * sp(so, 2 - e) / h.y,
                                     sp(sl, 2 - e) / h.z}));
            }
        }
        grid(P, N, R + 1, S, c);
    }
};

// One finger: three phalanges from the knuckle 'base', straight along 'dir', each joint flexing by flex[i] towards
// the palm (about 'hinge'). A nail sits on the back of the last one.
void finger(HandMesh &m, Vec base, Vec dir, Vec hinge, const float len[3], float r, const float flex[3]) {
    Vec p = base, d = norm(dir), back = norm(cross(d, hinge));  // back of the finger (away from the palm)
    for (int i = 0; i < 3; i++) {
        d = norm(rot(d, hinge, flex[i]));
        back = norm(cross(d, hinge));
        float r0 = r * (1.0f - 0.1f * i), r1 = r * (0.92f - 0.1f * i);
        Vec q = p + d * len[i];
        m.capsule(p, q, r0, r1, SKIN, back, 0.88f);
        if (i == 2) m.capsule(p + d * (len[i] * 0.45f) + back * (r1 * 0.75f), q + back * (r1 * 0.55f) - d * (r1 * 0.2f),
                              r1 * 0.55f, r1 * 0.5f, NAIL, back, 0.35f);
        p = q;
    }
}

// The hand gripping the controller. OpenXR grip space: -Z along the handle from the little finger to the thumb,
// +X to the right for both hands (into the right palm, out of the left one), +Y = up. So in grip space the right
// hand lies on the +X side of the handle with its palm facing -X; the fingers run from the knuckles round the
// handle; the wrist is towards +Y. side = +1 right, -1 left (mirror image).
void build_hand(std::vector<V> &out, const HandState &h, float side) {
    // hand frame -> grip: X_h = -Z_g (towards the thumb), Y_h = -Y_g (along the fingers), Z_h = -X_g (out of the palm,
    // towards the handle); wrist (origin) placed so the handle runs through the curled fingers
    Mat4 f = mat_identity();
    f.m[0][0] = 0;  f.m[0][1] = 0;  f.m[0][2] = -side;
    f.m[1][0] = 0;  f.m[1][1] = -1; f.m[1][2] = 0;
    f.m[2][0] = -1; f.m[2][1] = 0;  f.m[2][2] = 0;
    f.m[0][3] = side * 0.036f; f.m[1][3] = 0.085f; f.m[2][3] = 0.004f;
    HandMesh m{out, mat_mul(h.grip, f)};
    const Vec X = {1, 0, 0}, Y = {0, 1, 0};

    // palm, slightly thicker at the heel, and the heel of the thumb
    m.blob({-0.002f, 0.048f, 0.0f}, {0.041f, 0.047f, 0.0135f}, 0.32f, SKIN_PALM);
    m.blob({0.022f, 0.026f, 0.007f}, {0.017f, 0.024f, 0.012f}, 0.6f, SKIN_PALM);
    // wrist and forearm in the robe's sleeve
    m.capsule({0, 0.012f, 0}, {0, -0.05f, 0}, 0.024f, 0.026f, SKIN, {0, 0, 1}, 0.62f);
    m.capsule({0, -0.02f, 0}, {0, -0.16f, 0}, 0.034f, 0.042f, SLEEVE, {0, 0, 1}, 0.8f);
    m.capsule({0, -0.022f, 0}, {0, -0.03f, 0}, 0.037f, 0.037f, CUFF, {0, 0, 1}, 0.8f);
    m.capsule({0, -0.012f, 0}, {0, -0.025f, 0}, 0.031f, 0.033f, SLEEVE_IN, {0, 0, 1}, 0.8f);

    // fingers: index on the trigger (straight when lifted: pointing), the others round the handle with the grip
    struct F { float x, y, r, len[3]; };
    const F fs[4] = {{0.027f, 0.093f, 0.0094f, {0.042f, 0.025f, 0.020f}},
                     {0.008f, 0.097f, 0.0098f, {0.046f, 0.029f, 0.021f}},
                     {-0.011f, 0.094f, 0.0092f, {0.043f, 0.027f, 0.020f}},
                     {-0.028f, 0.086f, 0.0080f, {0.034f, 0.020f, 0.018f}}};
    for (int i = 0; i < 4; i++) {
        float c = i == 0 ? h.trigger : h.squeeze;
        float rest = i == 0 ? 0.1f : 0.35f;  // relaxed hand: slightly bent
        float flex[3] = {rest + c * 1.25f, rest * 1.2f + c * 1.45f, rest * 0.8f + c * 0.95f};
        Vec dir = norm(Vec{(fs[i].x) * 0.6f, 1, 0});  // fingers fan out a little
        m.blob({fs[i].x, fs[i].y - 0.004f, -0.002f}, {fs[i].r * 1.05f, fs[i].r * 1.1f, fs[i].r * 0.95f}, 0.9f, SKIN);
        finger(m, {fs[i].x, fs[i].y, -0.001f}, dir, X, fs[i].len, fs[i].r, flex);
    }
    // thumb: from the heel of the hand along the top of the handle, bending in with the grip
    {
        float c = 0.3f + 0.4f * h.squeeze;
        Vec base = {0.03f, 0.03f, 0.008f};
        Vec dir = norm(Vec{0.75f, 0.55f, 0.38f});
        Vec hinge = norm(cross(dir, Vec{0.2f, -0.3f, 1}));
        float len[3] = {0.03f, 0.026f, 0.022f}, flex[3] = {0.15f, 0.2f + c * 0.4f, 0.25f + c * 0.5f};
        finger(m, base, dir, hinge, len, 0.0118f, flex);
    }
    (void)Y;
}

// ---------------------------------------------------------------- pistol
// Built in a frame with the aim pose's orientation at the grip's position (the fist): the grip runs down from there,
// tilted back, the slide sits on top of the fist. Gunmetal, a little lighter slide; kick = recoil (up and back).
Mat4 pistol_frame(const HandState &h) {
    Mat4 f = h.aim;
    for (int r = 0; r < 3; r++) f.m[r][3] = h.grip.m[r][3];
    float k = std::max(0.0f, std::min(1.0f, h.kick));
    // recoil pivots about the top of the grip: muzzle up, the gun pushed back
    return M(f, M(T(0, 0.05f, 0.02f * k), M(mat_rot_x(0.22f * k), T(0, -0.05f, 0))));
}

void build_pistol(std::vector<V> &out, const HandState &h) {
    Mat4 f = pistol_frame(h);
    const uint32_t metal = 0x2b2d31, slide = 0x3c3f45, dark = 0x18191b;
    box(out, M(f, M(T(0, 0.004f, 0.008f), mat_rot_x(0.28f))), 0.029f, 0.115f, 0.046f, metal);   // grip
    box(out, M(f, T(0, 0.066f, -0.052f)), 0.030f, 0.033f, 0.195f, slide);                        // slide
    box(out, M(f, T(0, 0.044f, -0.062f)), 0.026f, 0.016f, 0.13f, metal);                         // frame / dust cover
    box(out, M(f, T(0, 0.066f, -0.1505f)), 0.012f, 0.012f, 0.002f, dark);                        // muzzle bore
    box(out, M(f, T(0, 0.0215f, -0.083f)), 0.008f, 0.004f, 0.05f, metal);                        // trigger guard (bottom)
    box(out, M(f, T(0, 0.031f, -0.107f)), 0.008f, 0.024f, 0.004f, metal);                        // trigger guard (front)
    box(out, M(f, M(T(0, 0.028f, -0.072f), mat_rot_x(-0.3f * h.trigger))), 0.006f, 0.02f, 0.006f, dark);  // trigger
    box(out, M(f, T(0, 0.086f, -0.145f)), 0.004f, 0.007f, 0.006f, dark);                         // front sight
    box(out, M(f, T(0, 0.086f, 0.038f)), 0.022f, 0.007f, 0.006f, dark);                          // rear sight
}

void build_ray(std::vector<V> &out, const Mat4 &aim, float len = 4.0f, bool dot = false) {
    // a thin glowing beam along the aim -Z, fading out along its length (drawn additively), a dot where it ends
    const int steps = 8;
    len = std::max(0.1f, std::min(60.0f, len));
    for (int i = 0; i < steps; i++) {
        float z0 = 0.02f + len * i / steps, l = len / steps;
        uint8_t a = (uint8_t)(110 * (1.0f - 0.8f * (float)i / steps));
        box(out, M(aim, T(0, 0, -(z0 + l * 0.5f))), 0.0025f, 0.0025f, l, 0xff4030, a);
    }
    if (dot) {
        float r = 0.012f + 0.004f * len;  // stays visible far away
        box(out, M(aim, T(0, 0, -len)), r, r, r, 0xff3020, 255);
    }
}

// The grenade gun's aim: the same thin beam bent along the grenade's arc (LOCAL points), a dot where it lands
void build_arc(std::vector<V> &out, const HandState &h) {
    float total = 0;
    for (int i = 1; i < h.arc_n; i++) {
        float d[3] = {h.arc[i][0] - h.arc[i - 1][0], h.arc[i][1] - h.arc[i - 1][1], h.arc[i][2] - h.arc[i - 1][2]};
        total += sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    }
    float run = 0;
    for (int i = 1; i < h.arc_n; i++) {
        const float *a = h.arc[i - 1], *b = h.arc[i];
        float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l < 1e-4f) continue;
        // a frame with -Z along the segment
        float z[3] = {-d[0] / l, -d[1] / l, -d[2] / l}, up[3] = {0, 1, 0};
        if (fabsf(z[1]) > 0.95f) up[0] = 1, up[1] = 0;
        float x[3] = {up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0]};
        float xl = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
        for (int k = 0; k < 3; k++) x[k] /= xl;
        float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
        Mat4 m = mat_identity();
        for (int k = 0; k < 3; k++)
            m.m[k][0] = x[k], m.m[k][1] = y[k], m.m[k][2] = z[k], m.m[k][3] = (a[k] + b[k]) * 0.5f;
        uint8_t al = (uint8_t)(110 * (1.0f - 0.6f * run / std::max(0.01f, total)));
        box(out, m, 0.0025f, 0.0025f, l, 0xff4030, al);
        run += l;
    }
    if (h.ray_dot && h.arc_n >= 2) {
        const float *e = h.arc[h.arc_n - 1];
        float r = 0.012f + 0.004f * total * 0.5f;
        Mat4 m = mat_identity();
        m.m[0][3] = e[0], m.m[1][3] = e[1], m.m[2][3] = e[2];
        box(out, m, r, r, r, 0xff3020, 255);
    }
}

void draw_vertices(const std::vector<V> &v, const Mat4 &vp, bool srgb, bool additive) {
    if (v.empty()) return;
    UINT bytes = (UINT)(v.size() * sizeof(V));
    if (bytes > vbuf_cap) {
        if (vbuf) vbuf->Release();
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = vbuf_cap = std::max<UINT>(bytes, 64 * 1024);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        dev->CreateBuffer(&bd, nullptr, &vbuf);
    }
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(ctx->Map(vbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
    memcpy(ms.pData, v.data(), bytes);
    ctx->Unmap(vbuf, 0);
    CB cb = {vp, srgb ? 1u : 0u, additive ? 1u : 0u, {0, 0}};
    ctx->UpdateSubresource(cbuf, 0, nullptr, &cb, 0, 0);
    UINT stride = sizeof(V), off = 0;
    ctx->IASetVertexBuffers(0, 1, &vbuf, &stride, &off);
    ctx->IASetInputLayout(layout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &cbuf);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &cbuf);
    ctx->RSSetState(raster);
    ctx->OMSetBlendState(additive ? bs_add : bs_opaque, nullptr, 0xffffffff);
    ctx->OMSetDepthStencilState(additive ? ds_test : ds_write, 0);
    ctx->Draw((UINT)v.size(), 0);
    ctx->OMSetBlendState(bs_opaque, nullptr, 0xffffffff);
}

// ---------------------------------------------------------------- textured pistol (a game's own model)
struct TV {
    float p[3], n[3], uv[2];
};
const char *tsrc = R"(
cbuffer C : register(b0) { row_major float4x4 vp; uint srgb; uint additive; uint2 pad; };
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct VI { float3 p : POSITION; float3 n : NORMAL; float2 uv : TEXCOORD; };
struct VO { float4 p : SV_Position; float3 n : NORMAL; float2 uv : TEXCOORD; };
VO vs_main(VI i) { VO o; o.p = mul(vp, float4(i.p, 1)); o.n = i.n; o.uv = i.uv; return o; }
float4 ps_main(VO i) : SV_Target {
    float4 t = tex.Sample(smp, i.uv);
    if (t.a < 0.5) discard;
    float3 n = normalize(i.n);
    float3 sun = normalize(float3(0.35, 0.85, 0.4));
    float wrap = saturate((dot(n, sun) + 0.35) / 1.35);
    float sky = 0.5 + 0.5 * n.y;
    float3 light = (float3(0.85, 0.88, 1.0) * wrap * 0.75 + lerp(float3(0.22, 0.2, 0.2), float3(0.30, 0.33, 0.42), sky)) * 1.1;
    float3 c = t.rgb * light;
    if (srgb) c = pow(saturate(c), 2.2);
    return float4(c, 1);
}
)";
ID3D11VertexShader *tvs;
ID3D11PixelShader *tps;
ID3D11InputLayout *tlayout;
ID3D11Buffer *tvbuf;
UINT tvbuf_cap = 0;
ID3D11SamplerState *tsamp;
const PistolModel *pistol_model = nullptr, *pistol_model_tex = nullptr;
ID3D11ShaderResourceView *pistol_srv = nullptr;

bool init_textured() {
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(tsrc, strlen(tsrc), "pistol", nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(tsrc, strlen(tsrc), "pistol", nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &psb, &err))) {
        if (err) psx::logf("pistol shader: %s", (const char *)err->GetBufferPointer());
        return false;
    }
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &tvs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &tps);
    D3D11_INPUT_ELEMENT_DESC el[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev->CreateInputLayout(el, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), &tlayout);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;  // PS1 texels, as the game shows them
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    dev->CreateSamplerState(&sd, &tsamp);
    return true;
}

void draw_textured_pistol(const HandState &h, const Mat4 &vp, bool srgb) {
    const PistolModel *m = pistol_model;
    if (!m || !tvs) return;
    if (pistol_model_tex != m) {
        if (pistol_srv) pistol_srv->Release(), pistol_srv = nullptr;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = m->tw, td.Height = m->th, td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM, td.SampleDesc.Count = 1, td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd = {m->rgba, (UINT)m->tw * 4, 0};
        ID3D11Texture2D *t = nullptr;
        if (SUCCEEDED(dev->CreateTexture2D(&td, &sd, &t))) dev->CreateShaderResourceView(t, nullptr, &pistol_srv), t->Release();
        pistol_model_tex = m;
    }
    if (!pistol_srv) return;
    Mat4 f = pistol_frame(h);
    std::vector<TV> v((size_t)m->nverts);
    for (int i = 0; i < m->nverts; i++) {
        const float *s = m->mesh + i * 8;
        for (int r = 0; r < 3; r++) {
            v[i].p[r] = f.m[r][0] * s[0] + f.m[r][1] * s[1] + f.m[r][2] * s[2] + f.m[r][3];
            v[i].n[r] = f.m[r][0] * s[3] + f.m[r][1] * s[4] + f.m[r][2] * s[5];
        }
        v[i].uv[0] = s[6], v[i].uv[1] = s[7];
    }
    UINT bytes = (UINT)(v.size() * sizeof(TV));
    if (bytes > tvbuf_cap) {
        if (tvbuf) tvbuf->Release();
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = tvbuf_cap = std::max<UINT>(bytes, 32 * 1024);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        dev->CreateBuffer(&bd, nullptr, &tvbuf);
    }
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(ctx->Map(tvbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
    memcpy(ms.pData, v.data(), bytes);
    ctx->Unmap(tvbuf, 0);
    CB cb = {vp, srgb ? 1u : 0u, 0u, {0, 0}};
    ctx->UpdateSubresource(cbuf, 0, nullptr, &cb, 0, 0);
    UINT stride = sizeof(TV), off = 0;
    ctx->IASetVertexBuffers(0, 1, &tvbuf, &stride, &off);
    ctx->IASetInputLayout(tlayout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(tvs, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &cbuf);
    ctx->PSSetShader(tps, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &cbuf);
    ctx->PSSetShaderResources(0, 1, &pistol_srv);
    ctx->PSSetSamplers(0, 1, &tsamp);
    ctx->RSSetState(raster);
    ctx->OMSetBlendState(bs_opaque, nullptr, 0xffffffff);
    ctx->OMSetDepthStencilState(ds_write, 0);
    ctx->Draw((UINT)v.size(), 0);
    ID3D11ShaderResourceView *none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
}

}  // namespace

void set_pistol_model(const PistolModel *m) { pistol_model = m; }
const PistolModel *g_preview_pistol = nullptr;  // development (--hands-preview with HANDS_WEAPON)

Mat4 pistol_muzzle(const HandState &h) {
    return pistol_model ? M(pistol_frame(h), T(0, pistol_model->muzzle_y, pistol_model->muzzle_z))
                        : M(pistol_frame(h), T(0, 0.066f, -0.152f));
}

bool init(ID3D11Device *d, ID3D11DeviceContext *c) {
    dev = d;
    ctx = c;
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(src, strlen(src), "hands", nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(src, strlen(src), "hands", nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &psb, &err))) {
        if (err) psx::logf("hands shader: %s", (const char *)err->GetBufferPointer());
        return false;
    }
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
    D3D11_INPUT_ELEMENT_DESC el[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev->CreateInputLayout(el, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout);
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(CB);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dev->CreateBuffer(&bd, nullptr, &cbuf);
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
    D3D11_BLEND_DESC bld = {};
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bld, &bs_opaque);
    auto &rt = bld.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = rt.DestBlend = D3D11_BLEND_ONE;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = rt.DestBlendAlpha = D3D11_BLEND_ONE;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    dev->CreateBlendState(&bld, &bs_add);
    init_textured();
    vrglove::init(d, c);  // realistic gloves when SteamVR's models are there
    return true;
}

void draw(const Mat4 &view_proj, const HandState hands[2], int ray_hand, bool srgb) {
    // the mesh is rebuilt only when the hands moved (draw runs once per eye)
    static std::vector<V> solid, glow;
    static HandState last[2];
    static int last_ray = -2;
    static const PistolModel *last_model = nullptr;
    if (last_ray != ray_hand || last_model != pistol_model || memcmp(last, hands, sizeof last) != 0) {
        last_model = pistol_model;
        solid.clear();
        glow.clear();
        for (int h = 0; h < 2; h++)
            if (hands[h].valid && !vrglove::ready()) build_hand(solid, hands[h], h == 0 ? -1.0f : 1.0f);
        for (int h = 0; h < 2; h++)
            if (hands[h].valid && hands[h].pistol && !pistol_model) build_pistol(solid, hands[h]);
        for (int h = 0; h < 2; h++)  // ray_hand 0 / 1: that hand, 2: both; from the muzzle when it holds the pistol
            if ((ray_hand == h || ray_hand == 2) && hands[h].valid) {
                if (hands[h].arc_n >= 2) build_arc(glow, hands[h]);
                else build_ray(glow, hands[h].pistol ? pistol_muzzle(hands[h]) : hands[h].aim, hands[h].ray_len, hands[h].ray_dot);
            }
        memcpy(last, hands, sizeof last);
        last_ray = ray_hand;
    }
    vrglove::draw(view_proj, hands, srgb);
    draw_vertices(solid, view_proj, srgb, false);
    for (int h = 0; h < 2; h++)
        if (pistol_model && hands[h].valid && hands[h].pistol) draw_textured_pistol(hands[h], view_proj, srgb);
    draw_vertices(glow, view_proj, srgb, true);
}

}  // namespace vrhands

namespace vrhands {

// Development: render both hands, as if holding the controllers in front of the viewer, into a BMP.
bool preview_bmp(ID3D11Device *d, ID3D11DeviceContext *c, const char *path, float trigger, float squeeze) {
    if (!init(d, c)) return false;
    const int W = 640, H = 480;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W; td.Height = H; td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *rt, *dt, *st;
    d->CreateTexture2D(&td, nullptr, &rt);
    td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d->CreateTexture2D(&td, nullptr, &st);
    td.Format = DXGI_FORMAT_D32_FLOAT; td.Usage = D3D11_USAGE_DEFAULT; td.CPUAccessFlags = 0;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    d->CreateTexture2D(&td, nullptr, &dt);
    ID3D11RenderTargetView *rtv; ID3D11DepthStencilView *dsv;
    d->CreateRenderTargetView(rt, nullptr, &rtv);
    d->CreateDepthStencilView(dt, nullptr, &dsv);
    float clear[4] = {0.5f, 0.6f, 0.8f, 1};
    c->ClearRenderTargetView(rtv, clear);
    c->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1, 0);
    c->OMSetRenderTargets(1, &rtv, dsv);
    D3D11_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
    c->RSSetViewports(1, &vp);
    // controllers held at chest height, 35 cm ahead, handles tilted forward by 40 degrees
    HandState hs[2];
    for (int h = 0; h < 2; h++) {
        hs[h].valid = true;
        hs[h].grip = mat_mul(mat_translate(h ? 0.13f : -0.13f, -0.1f, -0.32f), mat_mul(mat_rot_y(h ? -0.25f : 0.25f), mat_rot_x(0.85f)));  // handle tilted up, as held
        if (const char *e = getenv("HANDS_TURN")) {  // look at them from another side, turned around their centre
            float t = (float)atof(e);
            hs[h].grip = mat_mul(mat_translate(0, -0.1f, -0.32f), mat_mul(mat_rot_y(t), mat_mul(mat_translate(0, 0.1f, 0.32f), hs[h].grip)));
        }
        hs[h].aim = mat_mul(hs[h].grip, mat_rot_x(-0.85f));
        hs[h].trigger = trigger;
        hs[h].squeeze = squeeze;
    }
    Mat4 proj = mat_frustum(-0.6f, 0.6f, -0.45f, 0.45f, 0.03f, 50.0f);
    if (getenv("HANDS_EYE")) {
        // as seen from the eyes: hands at chest height 35 cm ahead, the head looking down 35 degrees
        for (int h = 0; h < 2; h++) {
            hs[h].grip = mat_mul(mat_translate(h ? 0.17f : -0.17f, -0.35f, -0.35f), mat_mul(mat_rot_y(h ? -0.2f : 0.2f), mat_rot_x(0.75f)));
            hs[h].aim = mat_mul(hs[h].grip, mat_rot_x(-0.85f));
        }
        proj = mat_mul(proj, mat_rot_x(0.6f));
    }
    if (getenv("HANDS_FAR")) {  // the same view, everything moved 20 m away from the origin (skinning check)
        for (int h = 0; h < 2; h++) {
            hs[h].grip = mat_mul(mat_translate(12, 3, -15), hs[h].grip);
            hs[h].aim = mat_mul(mat_translate(12, 3, -15), hs[h].aim);
        }
        proj = mat_mul(proj, mat_translate(-12, -3, 15));
    }
    if (getenv("HANDS_OPEN"))
        for (int h = 0; h < 2; h++) hs[h].open = 1;
    if (getenv("HANDS_TIP"))  // development: the ray starts at the index fingertip (pointing hand)
        for (int h = 0; h < 2; h++) {
            hs[h].trigger = 0;
            hs[h].squeeze = 1;
            float tip[3];
            if (vrglove::index_tip(h, hs[h], tip)) {
                for (int r = 0; r < 3; r++) hs[h].aim.m[r][3] = tip[r];
                hs[h].aim = mat_mul(hs[h].aim, mat_translate(0, 0, 0.05f));  // the ray begins 5 cm along -Z
            }
        }
    if (const char *pe = getenv("HANDS_PISTOL")) { hs[1].pistol = true; hs[1].kick = (float)atof(pe); }  // right hand holds the pistol (value = recoil)
    extern const PistolModel *g_preview_pistol;
    if (g_preview_pistol) set_pistol_model(g_preview_pistol);
    draw(proj, hs, 1, false);
    c->CopyResource(st, rt);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(c->Map(st, 0, D3D11_MAP_READ, 0, &m))) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint32_t size = 54 + W * H * 3, off = 54, ihs = 40, pb = 1 | (24 << 16);
    int w = W, hgt = H;
    uint8_t hdr[54] = {'B', 'M'};
    memcpy(hdr + 2, &size, 4); memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &ihs, 4);
    memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &hgt, 4); memcpy(hdr + 26, &pb, 4);
    fwrite(hdr, 1, 54, f);
    for (int y = H - 1; y >= 0; y--) {
        const uint8_t *s = (const uint8_t *)m.pData + y * m.RowPitch;
        for (int x = 0; x < W; x++) { uint8_t px[3] = {s[x * 4 + 2], s[x * 4 + 1], s[x * 4]}; fwrite(px, 1, 3, f); }
    }
    fclose(f);
    c->Unmap(st, 0);
    return true;
}

}  // namespace vrhands
