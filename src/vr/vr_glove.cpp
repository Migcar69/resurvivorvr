// Realistic VR hands: the shape and skeleton of the skinned hand models that ship with SteamVR
// (resources/rendermodels/vr_glove), loaded from the player's own SteamVR installation at start (nothing is copied
// into this project). Their sci-fi glove textures are not used: the hand is drawn as bare skin with the wizard's
// jacket sleeve (dark leather, shirt cuff) added round the wrist and forearm. The skeleton (wrist + 5 fingers, 3 phalanges
// each) is posed every frame from the controller (index finger <- trigger, the other fingers <- grip) and the mesh
// is skinned on the CPU.
#include "vr_glove.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <shlwapi.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <tuple>
#include <string>
#include <vector>

#include "../core/psx.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")

namespace vrglove {

namespace {

// ---------------------------------------------------------------- minimal JSON
struct Json {
    enum Type { NUL, NUM, STR, ARR, OBJ, BOOL } type = NUL;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;
    const Json &operator[](const char *k) const {
        static Json none;
        for (auto &kv : obj)
            if (kv.first == k) return kv.second;
        return none;
    }
    const Json &operator[](int i) const {
        static Json none;
        return i >= 0 && (size_t)i < arr.size() ? arr[(size_t)i] : none;
    }
    bool has(const char *k) const {
        for (auto &kv : obj)
            if (kv.first == k) return true;
        return false;
    }
    int i(int def = 0) const { return type == NUM ? (int)num : def; }
    double d(double def = 0) const { return type == NUM ? num : def; }
};

struct JsonParser {
    const char *p, *e;
    void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
    std::string string() {
        std::string s;
        p++;  // "
        while (p < e && *p != '"') {
            if (*p == '\\' && p + 1 < e) {
                p++;
                char c = *p++;
                if (c == 'n') s += '\n';
                else if (c == 't') s += '\t';
                else if (c == 'u') { p += 4; s += '?'; }
                else s += c;
            } else {
                s += *p++;
            }
        }
        p++;
        return s;
    }
    Json value() {
        ws();
        Json j;
        if (p >= e) return j;
        if (*p == '{') {
            j.type = Json::OBJ;
            p++;
            ws();
            if (*p == '}') { p++; return j; }
            while (p < e) {
                ws();
                std::string k = string();
                ws();
                p++;  // :
                j.obj.push_back({k, value()});
                ws();
                if (*p == ',') { p++; continue; }
                p++;  // }
                break;
            }
        } else if (*p == '[') {
            j.type = Json::ARR;
            p++;
            ws();
            if (*p == ']') { p++; return j; }
            while (p < e) {
                j.arr.push_back(value());
                ws();
                if (*p == ',') { p++; continue; }
                p++;  // ]
                break;
            }
        } else if (*p == '"') {
            j.type = Json::STR;
            j.str = string();
        } else if (*p == 't' || *p == 'f') {
            j.type = Json::BOOL;
            j.num = *p == 't';
            p += *p == 't' ? 4 : 5;
        } else if (*p == 'n') {
            p += 4;
        } else {
            j.type = Json::NUM;
            char *end;
            j.num = strtod(p, &end);
            p = end;
        }
        return j;
    }
};

// ---------------------------------------------------------------- glove model
struct Bone {
    std::string name;
    int parent = -1;
    float t[3], q[4];  // rest translation / rotation (local)
    int curl = -1;     // which finger drives it (0 thumb, 1 index, 2 middle, 3 ring, 4 pinky), -1 = none
    int seg = 0;       // 0 = proximal .. 2 = distal
};

struct GV {  // skinned vertex
    float p[3], n[3], t[4], uv[2];
};

struct Glove {
    bool ok = false, right = false;
    std::vector<Bone> bones;              // glTF nodes
    std::vector<int> joints;              // skin joint -> node
    std::vector<Mat4> ibm;                // inverse bind matrices
    std::vector<float> pos, nrm, tan, uv; // rest mesh
    std::vector<uint16_t> jnt;            // 4 per vertex
    std::vector<float> wgt;               // 4 per vertex
    std::vector<uint32_t> idx;
    ID3D11ShaderResourceView *color = nullptr, *normal = nullptr;
    ID3D11Buffer *vb = nullptr, *ib = nullptr;
    std::vector<GV> out;
};

ID3D11Device *dev;
ID3D11DeviceContext *ctx;
ID3D11VertexShader *vs;
ID3D11PixelShader *ps;
ID3D11InputLayout *layout;
ID3D11Buffer *cbuf;
ID3D11SamplerState *sampler;
ID3D11RasterizerState *raster;
ID3D11DepthStencilState *ds;
ID3D11BlendState *bs;
Glove glove[2];  // 0 = left, 1 = right

struct CB {
    Mat4 vp;
    uint32_t srgb, pad[3];
};

const char *src = R"(
cbuffer C : register(b0) { row_major float4x4 vp; uint srgb; uint3 pad; };
Texture2D albedo : register(t0);
Texture2D nmap : register(t1);
SamplerState smp : register(s0);
struct VI { float3 p : POSITION; float3 n : NORMAL; float4 t : TANGENT; float2 uv : TEXCOORD0; };
struct VO { float4 p : SV_Position; float3 n : NORMAL; float4 t : TANGENT; float2 uv : TEXCOORD0; };
VO vs_main(VI i) { VO o; o.p = mul(vp, float4(i.p, 1)); o.n = i.n; o.t = i.t; o.uv = i.uv; return o; }
float4 ps_main(VO i) : SV_Target {
    // material in uv.x: 0 skin, 1 robe, 2 gold trim (interpolated between them along the sleeve's edge)
    float m = i.uv.x < 0.5 ? 0 : i.uv.x < 1.5 ? 1 : 2;  // sharp edges between the materials
    // Resident Evil Survivor: Ark's dark leather jacket, the shirt cuff at the wrist
    float3 skin = float3(0.78, 0.56, 0.43), robe = float3(0.15, 0.13, 0.11), gold = float3(0.30, 0.29, 0.27);
    float3 c = m < 1 ? lerp(skin, robe, m) : lerp(robe, gold, m - 1);
    float3 n = normalize(i.n);
    float3 sun = normalize(float3(0.35, 0.85, 0.4));
    float d = dot(n, sun);
    float wrap = saturate((d + 0.4) / 1.4);
    float sky = 0.5 + 0.5 * n.y;
    // a dim night light (the game's streets are dark): cool moonlight from above, faint warm bounce
    float3 light = (float3(0.85, 0.88, 1.0) * wrap * 0.8 + lerp(float3(0.22, 0.2, 0.2), float3(0.30, 0.33, 0.42), sky)) * 0.62;
    float skin_amt = saturate(1 - m);
    // skin lets warm light through near the shadow line; gold shines
    float3 sss = float3(0.32, 0.10, 0.05) * saturate(1 - abs(d)) * skin_amt;
    float3 h = normalize(sun + float3(0, 0.2, 1));
    float spec = pow(saturate(dot(n, h)), m > 1.5 ? 40 : 16) * (m > 1.5 ? 0.08 : 0.05 * skin_amt) * saturate(d * 4);
    c = c * light + c * sss + spec;
    if (srgb) c = pow(saturate(c), 2.2);
    return float4(c, 1);
}
)";

// ---------------------------------------------------------------- loading
std::string steamvr_dir() {
    // %LOCALAPPDATA%\openvr\openvrpaths.vrpath -> "runtime" : [ "C:\\...\\SteamVR" ]
    char buf[MAX_PATH];
    if (GetEnvironmentVariableA("LOCALAPPDATA", buf, MAX_PATH)) {
        std::string fn = std::string(buf) + "\\openvr\\openvrpaths.vrpath";
        if (FILE *f = fopen(fn.c_str(), "rb")) {
            std::string s;
            char tmp[4096];
            size_t n;
            while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) s.append(tmp, n);
            fclose(f);
            JsonParser jp{s.data(), s.data() + s.size()};
            Json j = jp.value();
            const Json &rt = j["runtime"];
            if (rt.type == Json::ARR && !rt.arr.empty() && rt[0].type == Json::STR) return rt[0].str;
        }
    }
    return "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR";
}

ID3D11ShaderResourceView *decode_image(const uint8_t *data, size_t size, int max_side) {
    static IWICImagingFactory *wic = nullptr;
    if (!wic) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return nullptr;
    }
    IStream *st = SHCreateMemStream(data, (UINT)size);
    if (!st) return nullptr;
    IWICBitmapDecoder *dec = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;
    IWICBitmapScaler *scaler = nullptr;
    IWICFormatConverter *conv = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    UINT w = 0, h = 0;
    if (SUCCEEDED(wic->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h))) {
        UINT tw = w, th = h;
        while ((int)tw > max_side || (int)th > max_side) { tw /= 2; th /= 2; }
        if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(frame, tw, th, WICBitmapInterpolationModeFant)) &&
            SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(scaler, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom))) {
            std::vector<uint8_t> px((size_t)tw * th * 4);
            if (SUCCEEDED(conv->CopyPixels(nullptr, tw * 4, (UINT)px.size(), px.data()))) {
                D3D11_TEXTURE2D_DESC td = {};
                td.Width = tw;
                td.Height = th;
                td.MipLevels = 0;  // full chain, generated below
                td.ArraySize = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                td.SampleDesc.Count = 1;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
                ID3D11Texture2D *tex = nullptr;
                if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &tex))) {
                    ctx->UpdateSubresource(tex, 0, nullptr, px.data(), tw * 4, 0);
                    dev->CreateShaderResourceView(tex, nullptr, &srv);
                    if (srv) ctx->GenerateMips(srv);
                    tex->Release();
                }
            }
        }
    }
    if (conv) conv->Release();
    if (scaler) scaler->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    st->Release();
    return srv;
}

float side_of(const Glove &g) { return g.right ? 1.0f : -1.0f; }

bool load(Glove &g, const std::string &fn) {
    FILE *f = fopen(fn.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> d;
    fseek(f, 0, SEEK_END);
    d.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    size_t got = fread(d.data(), 1, d.size(), f);
    fclose(f);
    if (got != d.size() || d.size() < 20 || memcmp(d.data(), "glTF", 4)) return false;
    uint32_t jlen;
    memcpy(&jlen, d.data() + 12, 4);
    JsonParser jp{(const char *)d.data() + 20, (const char *)d.data() + 20 + jlen};
    Json j = jp.value();
    size_t bin = 20 + jlen + 8;
    auto view = [&](int bvi, size_t &off, size_t &stride) {
        const Json &bv = j["bufferViews"][bvi];
        off = bin + (size_t)bv["byteOffset"].i();
        stride = (size_t)bv["byteStride"].i();
    };
    // read accessor as floats (count x n), converting the integer types
    auto accessor = [&](int ai, int &n) {
        const Json &a = j["accessors"][ai];
        const std::string &type = a["type"].str;
        n = type == "SCALAR" ? 1 : type == "VEC2" ? 2 : type == "VEC3" ? 3 : type == "VEC4" ? 4 : 16;
        int ct = a["componentType"].i(), count = a["count"].i();
        size_t off, stride;
        view(a["bufferView"].i(), off, stride);
        off += (size_t)a["byteOffset"].i();
        size_t cs = ct == 5126 || ct == 5125 ? 4 : ct == 5123 || ct == 5122 ? 2 : 1;
        if (!stride) stride = cs * n;
        std::vector<double> v((size_t)count * n);
        for (int k = 0; k < count; k++)
            for (int c = 0; c < n; c++) {
                const uint8_t *s = d.data() + off + k * stride + c * cs;
                double x = 0;
                if (ct == 5126) { float fv; memcpy(&fv, s, 4); x = fv; }
                else if (ct == 5125) { uint32_t u; memcpy(&u, s, 4); x = u; }
                else if (ct == 5123) { uint16_t u; memcpy(&u, s, 2); x = u; }
                else if (ct == 5122) { int16_t u; memcpy(&u, s, 2); x = u; }
                else x = *s;
                v[(size_t)k * n + c] = x;
            }
        return v;
    };
    // nodes
    const Json &nodes = j["nodes"];
    g.bones.resize(nodes.arr.size());
    for (size_t k = 0; k < nodes.arr.size(); k++) {
        const Json &nd = nodes[k];
        Bone &b = g.bones[k];
        b.name = nd["name"].str;
        for (int c = 0; c < 3; c++) b.t[c] = (float)nd["translation"][c].d(0);
        for (int c = 0; c < 4; c++) b.q[c] = (float)nd["rotation"][c].d(c == 3 ? 1 : 0);
        if (!nd.has("rotation")) { b.q[0] = b.q[1] = b.q[2] = 0; b.q[3] = 1; }
        for (auto &ch : nd["children"].arr) g.bones[(size_t)ch.i()].parent = (int)k;
        static const char *fingers[5] = {"thumb", "index", "middle", "ring", "pinky"};
        for (int fi = 0; fi < 5; fi++) {
            std::string pre = std::string("finger_") + fingers[fi] + "_";
            if (b.name.compare(0, pre.size(), pre) == 0 && b.name.size() > pre.size() && isdigit((unsigned char)b.name[pre.size()])) {
                b.curl = fi;
                b.seg = b.name[pre.size()] - '0';
            }
        }
    }
    // skin
    const Json &skin = j["skins"][0];
    for (auto &jn : skin["joints"].arr) g.joints.push_back(jn.i());
    int n;
    std::vector<double> ibm = accessor(skin["inverseBindMatrices"].i(), n);
    for (size_t k = 0; k < g.joints.size() && (k + 1) * 16 <= ibm.size(); k++) {
        Mat4 m;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) m.m[r][c] = (float)ibm[k * 16 + c * 4 + r];  // glTF is column-major
        g.ibm.push_back(m);
    }
    // mesh
    const Json &prim = j["meshes"][0]["primitives"][0];
    const Json &at = prim["attributes"];
    auto tofloat = [](const std::vector<double> &v) { return std::vector<float>(v.begin(), v.end()); };
    g.pos = tofloat(accessor(at["POSITION"].i(), n));
    g.nrm = tofloat(accessor(at["NORMAL"].i(), n));
    if (at.has("TANGENT")) g.tan = tofloat(accessor(at["TANGENT"].i(), n));
    g.uv = tofloat(accessor(at["TEXCOORD_0"].i(), n));
    std::vector<double> jv = accessor(at["JOINTS_0"].i(), n);
    g.jnt.assign(jv.begin(), jv.end());
    g.wgt = tofloat(accessor(at["WEIGHTS_0"].i(), n));
    // some vertices' weights add up to nearly 2: normalise, or those vertices are pulled away from the origin of the
    // space the hand is in (stretched polygons pointing one way)
    for (size_t k = 0; k + 3 < g.wgt.size(); k += 4) {
        float sum = g.wgt[k] + g.wgt[k + 1] + g.wgt[k + 2] + g.wgt[k + 3];
        if (sum > 1e-6f)
            for (int c = 0; c < 4; c++) g.wgt[k + c] /= sum;
    }
    std::vector<double> iv = accessor(prim["indices"].i(), n);
    g.idx.assign(iv.begin(), iv.end());
    size_t nv = g.pos.size() / 3;
    if (g.tan.size() != nv * 4) g.tan.assign(nv * 4, 0), [&] { for (size_t k = 0; k < nv; k++) g.tan[k * 4] = 1, g.tan[k * 4 + 3] = 1; }();
    // textures: base colour and normal map (downscaled to 1024)
    const Json &mat = j["materials"][0];
    auto image = [&](int tex) -> ID3D11ShaderResourceView * {
        const Json &img = j["images"][j["textures"][tex]["source"].i()];
        if (!img.has("bufferView")) return nullptr;
        size_t off, stride;
        view(img["bufferView"].i(), off, stride);
        return decode_image(d.data() + off, (size_t)j["bufferViews"][img["bufferView"].i()]["byteLength"].i(), 1024);
    };
    (void)mat;
    (void)image;
    // Keep only the hand itself: the model's other pieces (a strap and a tab at the wrist) and the glove's cuff
    // would cut through the sleeve and flicker. Pieces = triangles connected through shared (welded) positions.
    nv = g.pos.size() / 3;
    {
        std::map<std::tuple<long, long, long>, int> weld;
        std::vector<int> wid(nv), parent;
        for (size_t k = 0; k < nv; k++) {
            auto key = std::make_tuple(lroundf(g.pos[k * 3] * 1e5f), lroundf(g.pos[k * 3 + 1] * 1e5f), lroundf(g.pos[k * 3 + 2] * 1e5f));
            auto it = weld.find(key);
            if (it == weld.end()) { it = weld.emplace(key, (int)parent.size()).first; parent.push_back((int)parent.size()); }
            wid[k] = it->second;
        }
        std::function<int(int)> root = [&](int x) { while (parent[x] != x) x = parent[x] = parent[parent[x]]; return x; };
        for (size_t t = 0; t + 2 < g.idx.size(); t += 3) {
            int a = root(wid[g.idx[t]]), b = root(wid[g.idx[t + 1]]), c = root(wid[g.idx[t + 2]]);
            parent[a] = b;
            parent[root(b)] = root(c);
        }
        std::map<int, int> count;
        for (size_t t = 0; t + 2 < g.idx.size(); t += 3) count[root(wid[g.idx[t]])]++;
        int best = -1, best_n = 0;
        for (auto &kv : count) if (kv.second > best_n) best_n = kv.second, best = kv.first;
        std::vector<uint32_t> keep;
        for (size_t t = 0; t + 2 < g.idx.size(); t += 3) {
            if (root(wid[g.idx[t]]) != best) continue;
            float zmax = std::max({g.pos[g.idx[t] * 3 + 2], g.pos[g.idx[t + 1] * 3 + 2], g.pos[g.idx[t + 2] * 3 + 2]});
            if (zmax < 0.006f) continue;  // the glove's cuff: inside the sleeve
            keep.insert(keep.end(), {g.idx[t], g.idx[t + 1], g.idx[t + 2]});
        }
        g.idx.swap(keep);
    }
    for (size_t k = 0; k < nv; k++) { g.uv[k * 2] = 0; g.uv[k * 2 + 1] = 0; }  // all skin
    // the sleeve: an open tube of robe round the forearm, from the cuff towards the elbow, bound to the wrist
    {
        int wrist = 0;
        for (size_t k = 0; k < g.joints.size(); k++)
            if (g.bones[(size_t)g.joints[k]].name.compare(0, 6, "wrist_") == 0) wrist = (int)k;
        // rings: a gold band from 1.6 cm up the wrist (covering the hand's open edge) to 0.4 cm, then the robe
        const int S = 24, R = 9;
        static const float ring_z[R] = {0.016f, 0.004f, 0.0035f, -0.03f, -0.07f, -0.11f, -0.15f, -0.19f, -0.23f};
        uint32_t first = (uint32_t)nv;
        for (int r = 0; r < R; r++) {
            float z = ring_z[r], f = std::max(0.0f, -z) / 0.23f;
            float rx = 0.037f + f * 0.016f, ry = 0.045f + f * 0.014f;  // flatter across the palm than across
            for (int k = 0; k < S; k++) {
                float a = 6.2831853f * k / S, ca = cosf(a), sa = sinf(a);
                float px = ca * rx + (side_of(g) * 0.004f), py = sa * ry + 0.004f;
                g.pos.insert(g.pos.end(), {px, py, z});
                float nx = ca / rx, ny = sa / ry, l = sqrtf(nx * nx + ny * ny);
                g.nrm.insert(g.nrm.end(), {nx / l, ny / l, 0});
                g.tan.insert(g.tan.end(), {0, 0, 1, 1});
                g.uv.insert(g.uv.end(), {r < 2 ? 2.0f : 1.0f, 0});
                g.jnt.insert(g.jnt.end(), {(uint16_t)wrist, 0, 0, 0});
                g.wgt.insert(g.wgt.end(), {1, 0, 0, 0});
            }
        }
        for (int r = 0; r + 1 < R; r++)
            for (int k = 0; k < S; k++) {
                uint32_t a0 = first + r * S + k, a1 = first + r * S + (k + 1) % S, b0 = a0 + S, b1 = a1 + S;
                g.idx.insert(g.idx.end(), {a0, a1, b0, a1, b1, b0});
            }
        // inside of the sleeve (same rings, normals inwards, reversed faces, shadowed robe) and a dark disk across
        // it just under the hand, so looking into the sleeve shows cloth, not the sky through the arm
        size_t outer = first, ring_n = (size_t)R * S;
        uint32_t inner = (uint32_t)(g.pos.size() / 3);
        for (size_t k = 0; k < ring_n; k++) {
            size_t v = outer + k;
            g.pos.insert(g.pos.end(), {g.pos[v * 3] * 0.97f, g.pos[v * 3 + 1] * 0.97f, g.pos[v * 3 + 2]});
            g.nrm.insert(g.nrm.end(), {-g.nrm[v * 3], -g.nrm[v * 3 + 1], 0});
            g.tan.insert(g.tan.end(), {0, 0, 1, 1});
            g.uv.insert(g.uv.end(), {k < (size_t)2 * S ? 2.0f : 1.0f, 0});
            g.jnt.insert(g.jnt.end(), {(uint16_t)wrist, 0, 0, 0});
            g.wgt.insert(g.wgt.end(), {1, 0, 0, 0});
        }
        for (int r = 0; r + 1 < R; r++)
            for (int k = 0; k < S; k++) {
                uint32_t a0 = inner + r * S + k, a1 = inner + r * S + (k + 1) % S, b0 = a0 + S, b1 = a1 + S;
                g.idx.insert(g.idx.end(), {a0, b0, a1, a1, b0, b1});
            }
        uint32_t centre = (uint32_t)(g.pos.size() / 3);
        float cz = 0.002f;
        g.pos.insert(g.pos.end(), {side_of(g) * 0.004f, 0.004f, cz});
        g.nrm.insert(g.nrm.end(), {0, 0, 1});
        g.tan.insert(g.tan.end(), {1, 0, 0, 1});
        g.uv.insert(g.uv.end(), {1.0f, 0});
        g.jnt.insert(g.jnt.end(), {(uint16_t)wrist, 0, 0, 0});
        g.wgt.insert(g.wgt.end(), {1, 0, 0, 0});
        for (int k = 0; k < S; k++) {
            float a = 6.2831853f * k / S;
            g.pos.insert(g.pos.end(), {cosf(a) * 0.036f + side_of(g) * 0.004f, sinf(a) * 0.044f + 0.004f, cz});
            g.nrm.insert(g.nrm.end(), {0, 0, 1});
            g.tan.insert(g.tan.end(), {1, 0, 0, 1});
            g.uv.insert(g.uv.end(), {1.0f, 0});
            g.jnt.insert(g.jnt.end(), {(uint16_t)wrist, 0, 0, 0});
            g.wgt.insert(g.wgt.end(), {1, 0, 0, 0});
        }
        for (int k = 0; k < S; k++) {
            uint32_t a = centre + 1 + k, b = centre + 1 + (k + 1) % S;
            g.idx.insert(g.idx.end(), {centre, a, b, centre, b, a});  // both faces
        }
        nv = g.pos.size() / 3;
    }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = (UINT)(nv * sizeof(GV));
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev->CreateBuffer(&bd, nullptr, &g.vb);
    D3D11_BUFFER_DESC ibd = {};
    ibd.ByteWidth = (UINT)(g.idx.size() * 4);
    ibd.Usage = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd = {g.idx.data()};
    dev->CreateBuffer(&ibd, &sd, &g.ib);
    g.out.resize(nv);
    return g.vb && g.ib;
}

// ---------------------------------------------------------------- posing
// The glove's own frame: fingers along +Z, thumb towards +Y, palm towards +X for the right hand (-X for the left,
// its mirror image), metres, wrist at the origin. OpenXR grip space: -Z along the handle from the little finger
// to the thumb (held naturally it points forward and up), +X to the right, +Y = Z x X; the palm faces -X (right
// hand) / +X (left). Holding a controller the handle crosses the palm diagonally: the fingers run forward along it
// and down round it, the palm faces the handle and the wrist is at the little-finger end, behind it.
Mat4 glove_to_grip(float side) {
    // glove axes in grip space
    // Both models share the axes (fingers +Z, thumb +Y); the left one is already the mirror image (palm towards -X),
    // so the same rotation serves both: glove +X -> grip -X puts the right palm towards -X and the left one towards +X.
    float F[3] = {0, -0.55f, -0.835f};               // glove +Z: fingers
    float X[3] = {-1, 0, 0};                         // glove +X
    float Y[3] = {F[1] * X[2] - F[2] * X[1], F[2] * X[0] - F[0] * X[2], F[0] * X[1] - F[1] * X[0]};  // Z x X
    Mat4 m = mat_identity();
    for (int r = 0; r < 3; r++) { m.m[r][0] = X[r]; m.m[r][1] = Y[r]; m.m[r][2] = F[r]; }
    // the handle (grip origin) passes through the hollow of the fist: 3 cm out of the palm, 7.5 cm up the hand
    float h[3] = {side * 0.032f, 0.012f, 0.075f};
    for (int r = 0; r < 3; r++) m.m[r][3] = -(m.m[r][0] * h[0] + m.m[r][1] * h[1] + m.m[r][2] * h[2]);
    return m;
}

// Global (model space) matrices of every bone for the hand's current finger state
std::vector<Mat4> pose_bones(const Glove &g, const vrhands::HandState &h) {
    // finger curl per joint (radians about the bone's local Z, which bends it towards the palm)
    float curl[5];
    curl[0] = 0.15f + 0.35f * h.squeeze;             // thumb rests on top
    curl[1] = h.point ? 0.02f : 0.12f + 1.0f * h.squeeze;  // index: with the others, or pointing
    curl[2] = curl[3] = curl[4] = 0.15f + 1.15f * h.squeeze;
    if (h.pistol) {  // round the pistol's grip: three fingers closed, the thumb along the side, index on the trigger
        curl[0] = 0.5f;
        curl[1] = 0.45f + 0.45f * h.trigger;
        curl[2] = curl[3] = curl[4] = 1.25f;
    }
    // a spell leaving the hand flings it open (fingers straight, a little bent back)
    for (int f = 0; f < 5; f++) curl[f] += (-0.12f - curl[f]) * std::min(1.0f, std::max(0.0f, h.open));
    static const float seg_k[3] = {0.85f, 1.15f, 0.8f};
    std::vector<Mat4> global(g.bones.size());
    std::vector<bool> done(g.bones.size(), false);
    std::function<const Mat4 &(int)> get = [&](int k) -> const Mat4 & {
        if (done[k]) return global[k];
        const Bone &b = g.bones[k];
        Mat4 local = mat_pose(b.q[0], b.q[1], b.q[2], b.q[3], b.t[0], b.t[1], b.t[2]);
        if (b.curl >= 0) {
            float a = curl[b.curl] * seg_k[std::min(b.seg, 2)];
            if (b.curl == 0 && b.seg == 0) a *= 0.4f;  // thumb base: little
            local = mat_mul(local, mat_rot_z(a));
        }
        global[k] = b.parent >= 0 ? mat_mul(get(b.parent), local) : local;
        done[k] = true;
        return global[k];
    };
    for (size_t k = 0; k < g.bones.size(); k++) get((int)k);
    return global;
}

void pose_and_skin(Glove &g, const vrhands::HandState &h, float side) {
    std::vector<Mat4> global = pose_bones(g, h);
    auto get = [&](int k) -> const Mat4 & { return global[k]; };
    Mat4 world = mat_mul(h.grip, glove_to_grip(side));
    std::vector<Mat4> jm(g.joints.size());
    for (size_t k = 0; k < g.joints.size() && k < g.ibm.size(); k++) jm[k] = mat_mul(world, mat_mul(get(g.joints[k]), g.ibm[k]));
    size_t nv = g.pos.size() / 3;
    for (size_t v = 0; v < nv; v++) {
        float M[3][4] = {};
        for (int i = 0; i < 4; i++) {
            float w = g.wgt[v * 4 + i];
            if (w <= 0) continue;
            const Mat4 &J = jm[std::min<size_t>(g.jnt[v * 4 + i], jm.size() - 1)];
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 4; c++) M[r][c] += w * J.m[r][c];
        }
        GV &o = g.out[v];
        const float *p = &g.pos[v * 3], *n = &g.nrm[v * 3], *t = &g.tan[v * 4];
        for (int r = 0; r < 3; r++) {
            o.p[r] = M[r][0] * p[0] + M[r][1] * p[1] + M[r][2] * p[2] + M[r][3];
            o.n[r] = M[r][0] * n[0] + M[r][1] * n[1] + M[r][2] * n[2];
            o.t[r] = M[r][0] * t[0] + M[r][1] * t[1] + M[r][2] * t[2];
        }
        o.t[3] = t[3];
        o.uv[0] = g.uv[v * 2];
        o.uv[1] = g.uv[v * 2 + 1];
    }
}

}  // namespace

bool init(ID3D11Device *d, ID3D11DeviceContext *c) {
    dev = d;
    ctx = c;
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(src, strlen(src), "glove", nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(src, strlen(src), "glove", nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &psb, &err))) {
        if (err) psx::logf("glove shader: %s", (const char *)err->GetBufferPointer());
        return false;
    }
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
    D3D11_INPUT_ELEMENT_DESC el[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev->CreateInputLayout(el, 4, vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout);
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(CB);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dev->CreateBuffer(&bd, nullptr, &cbuf);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.MaxAnisotropy = 8;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &sampler);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;  // the model has layered shells (pads over the knuckles): hide their insides
    rd.FrontCounterClockwise = TRUE;  // glTF winding
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &raster);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    dev->CreateDepthStencilState(&dd, &ds);
    D3D11_BLEND_DESC bld = {};
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bld, &bs);
    std::string dir = steamvr_dir() + "\\resources\\rendermodels\\vr_glove\\";
    glove[1].right = true;
    bool ok = load(glove[0], dir + "vr_glove_left_model.glb") && load(glove[1], dir + "vr_glove_right_model.glb");
    glove[0].ok = glove[1].ok = ok;
    psx::logf("VR gloves: %s (%s)", ok ? "loaded" : "not found, simple hands", dir.c_str());
    return ok;
}

bool ready() { return glove[0].ok && glove[1].ok; }

bool index_tip(int hand, const vrhands::HandState &h, float out[3]) {
    if (!ready() || hand < 0 || hand > 1 || !h.valid) return false;
    const Glove &g = glove[hand];
    int tip = -1;
    for (size_t k = 0; k < g.bones.size(); k++)
        if (g.bones[k].name.find("finger_index_") == 0 && g.bones[k].name.find("_end") != std::string::npos) tip = (int)k;
    if (tip < 0) return false;
    std::vector<Mat4> global = pose_bones(g, h);
    Mat4 m = mat_mul(mat_mul(h.grip, glove_to_grip(hand ? 1.0f : -1.0f)), global[(size_t)tip]);
    // the end bone sits inside the finger pad: half a centimetre further along the finger
    float dir[3] = {m.m[0][0], m.m[1][0], m.m[2][0]};  // bone x (towards the tip is -x on the right, +x on the left)
    float sgn = hand ? -1.0f : 1.0f;
    for (int c = 0; c < 3; c++) out[c] = m.m[c][3] + dir[c] * sgn * 0.005f;
    return true;
}

bool index_ray(int hand, const vrhands::HandState &h, float origin[3], float dir[3]) {
    if (!ready() || hand < 0 || hand > 1 || !h.valid) return false;
    const Glove &g = glove[hand];
    int base = -1;
    for (size_t k = 0; k < g.bones.size(); k++)
        if (g.bones[k].name.find("finger_index_0") == 0) base = (int)k;
    if (base < 0) return false;
    vrhands::HandState p = h;
    p.point = true;
    p.squeeze = 1;
    p.open = 0;
    float tip[3];
    if (!index_tip(hand, p, tip)) return false;
    std::vector<Mat4> global = pose_bones(g, p);
    Mat4 m = mat_mul(mat_mul(p.grip, glove_to_grip(hand ? 1.0f : -1.0f)), global[(size_t)base]);
    float len = 0;
    for (int c = 0; c < 3; c++) { dir[c] = tip[c] - m.m[c][3]; len += dir[c] * dir[c]; }
    len = sqrtf(len);
    if (len < 1e-4f) return false;
    for (int c = 0; c < 3; c++) { dir[c] /= len; origin[c] = tip[c]; }
    return true;
}

void draw(const Mat4 &view_proj, const vrhands::HandState hands[2], bool srgb) {
    if (!ready()) return;
    CB cb = {view_proj, srgb ? 1u : 0u, {0, 0, 0}};
    ctx->UpdateSubresource(cbuf, 0, nullptr, &cb, 0, 0);
    ctx->IASetInputLayout(layout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &cbuf);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &cbuf);
    ctx->PSSetSamplers(0, 1, &sampler);
    ctx->RSSetState(raster);
    ctx->OMSetBlendState(bs, nullptr, 0xffffffff);
    ctx->OMSetDepthStencilState(ds, 0);
    for (int h = 0; h < 2; h++) {
        if (!hands[h].valid) continue;
        Glove &g = glove[h];
        pose_and_skin(g, hands[h], h ? 1.0f : -1.0f);
        D3D11_MAPPED_SUBRESOURCE ms;
        if (FAILED(ctx->Map(g.vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) continue;
        memcpy(ms.pData, g.out.data(), g.out.size() * sizeof(GV));
        ctx->Unmap(g.vb, 0);
        UINT stride = sizeof(GV), off = 0;
        ctx->IASetVertexBuffers(0, 1, &g.vb, &stride, &off);
        ctx->IASetIndexBuffer(g.ib, DXGI_FORMAT_R32_UINT, 0);
        ctx->DrawIndexed((UINT)g.idx.size(), 0, 0);
    }
}

}  // namespace vrglove
