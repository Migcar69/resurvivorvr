// The weapons held in VR, from the player's own game. The program ships no part of the game: the weapons' 3D models
// (the ones turning on the "You've obtained" screen, also lying where they are found) are recognised in RAM while the
// game runs, rebuilt into the VR pistol frame (the same steps as tools/models/genweap.py) and kept in a local file,
// resvr_weapons.bin, next to the program; from then on that weapon is held with its own model. Until a weapon's model
// has been seen, the generic pistol is held.
//
// A model is an object record: 3, then pointers to its primitives (GPU textured polygons, 4-bit texture), its vertices
// (3 x s16 + pad) and its normals. It is recognised by numbers measured here (vertex count, size of the primitives, a
// CRC of the vertices); the rest (each weapon's real length, the fist's place, which way it faces) is VR's own.
#include "game.h"

#include "../core/psx.h"
#include "../vr/vr_hands.h"
#include "handgun_a_shape.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace psx;
namespace psx { bool cdrom_read_file(const std::string &path, std::vector<u8> &out); }

namespace game {

namespace {

struct Spec {
    int weapon;        // the equipped weapon's number (game::equipped_weapon)
    const char *name;
    int nv, prim_bytes;
    uint32_t vert_crc;
    float length_mm;   // the real weapon's length (the model is scaled to it)
    bool turn;         // the model has its barrel towards +x: turned half round
    float fist_x, fist_y;  // the fist on the grip, in the model's units (after the turn)
};
// measured on the American disc's saves (the Magnum on the European one: the same model)
const Spec SPECS[] = {
    {4, "Handgun D", 72, 1860, 0x05a30910u, 245, false, 105, 16},
    {2, "Handgun B", 62, 1392, 0x6739e0e4u, 211, false, 108, 40},
    {5, "Shotgun", 68, 1624, 0xd28105acu, 1041, true, 192, 12},
    {9, "Grenade gun", 100, 1880, 0xb0789e5cu, 737, true, 70, 85},
    {6, "Magnum", 56, 2092, 0x3b273ae4u, 252, false, 70, 38},
    {3, "Handgun C", 46, 1716, 0x33c1c11eu, 195, false, 106, 19},  // (on the shelf: snap_20261105_223748)
};

struct Built {
    int tw = 0, th = 0;
    std::vector<uint8_t> rgba;
    std::vector<float> mesh;  // x y z, nx ny nz, u v
    float muzzle_y = 0, muzzle_z = 0;
    vrhands::PistolModel pm{};
    void publish() {
        pm.tw = tw, pm.th = th, pm.rgba = rgba.data(), pm.nverts = (int)(mesh.size() / 8), pm.mesh = mesh.data();
        pm.muzzle_y = muzzle_y, pm.muzzle_z = muzzle_z;
    }
};

std::mutex m;
std::unique_ptr<Built> models[16];               // owned, never freed while running
std::atomic<const vrhands::PistolModel *> pub[16];  // what the VR thread reads
std::string cache_path = "resvr_weapons.bin";
bool loaded = false;

uint32_t crc32(const uint8_t *p, size_t n) {
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return ~c;
}
uint32_t rd32(const uint8_t *ram, uint32_t a) { uint32_t v; memcpy(&v, ram + (a & 0x1ffffc), 4); return v; }
int16_t rd16s(const uint8_t *ram, uint32_t a) { int16_t v; memcpy(&v, ram + (a & 0x1ffffe), 2); return v; }

// The model -> the VR pistol frame (metres: -Z barrel, +Y up, origin = the fist), with its texture from VRAM
std::unique_ptr<Built> build(const uint8_t *ram, const uint16_t *vram, uint32_t prim, uint32_t vert, int nv,
                             const Spec &s) {
    struct Face { int idx[4]; int uv[4][2]; int n; uint16_t clut, tpage; };
    std::vector<Face> faces;
    for (uint32_t a = prim; a < vert;) {
        uint32_t w = rd32(ram, a);
        uint32_t code = w >> 24;
        if ((code & 0xe4) != 0x24) { a += 4; continue; }  // a group header
        int n = (code & 0x08) ? 4 : 3, ncol = (code & 0x10) ? n : 1, nw = ncol + n + 1;
        if (a + 4 * nw > vert) break;
        uint32_t r[16];
        for (int i = 0; i < nw; i++) r[i] = rd32(ram, a + 4 * i);
        Face f{};
        f.n = n;
        for (int k = 0; k < n; k++) f.uv[k][0] = r[ncol + k] & 0xff, f.uv[k][1] = (r[ncol + k] >> 8) & 0xff;
        f.clut = (uint16_t)(r[ncol] >> 16);
        uint16_t tp = (uint16_t)(r[ncol + 1] >> 16);
        f.tpage = (uint16_t)((tp & 0x1f) | (tp & 0x180));
        uint32_t last = r[ncol + n];
        if (n == 4) f.idx[0] = r[ncol + 2] >> 16, f.idx[1] = r[ncol + 3] >> 16, f.idx[2] = last & 0xffff, f.idx[3] = last >> 16;
        else f.idx[0] = r[ncol + 2] >> 16, f.idx[1] = last & 0xffff, f.idx[2] = last >> 16;
        for (int k = 0; k < n; k++)
            if (f.idx[k] >= nv) return nullptr;
        faces.push_back(f);
        a += 4 * nw;
    }
    if (faces.empty()) return nullptr;
    // texture: the 4-bit page's rows used, one band per CLUT
    int tp = faces[0].tpage, px0 = (tp & 0xf) * 64, py0 = ((tp >> 4) & 1) * 256;
    int vmin = 255, vmax = 0;
    std::vector<uint16_t> cluts;
    for (auto &f : faces) {
        for (int k = 0; k < f.n; k++) vmin = std::min(vmin, f.uv[k][1]), vmax = std::max(vmax, f.uv[k][1]);
        if (std::find(cluts.begin(), cluts.end(), f.clut) == cluts.end()) cluts.push_back(f.clut);
    }
    std::sort(cluts.begin(), cluts.end());
    auto b = std::make_unique<Built>();
    int bh = vmax - vmin + 1;
    b->tw = 256, b->th = bh * (int)cluts.size();
    b->rgba.assign((size_t)b->tw * b->th * 4, 0);
    int opaque = 0;
    for (size_t ci = 0; ci < cluts.size(); ci++) {
        int cx = (cluts[ci] & 0x3f) * 16, cy = cluts[ci] >> 6;
        for (int v = vmin; v <= vmax; v++)
            for (int u = 0; u < 256; u++) {
                uint16_t word = vram[((py0 + v) & 511) * 1024 + ((px0 + u / 4) & 1023)];
                uint16_t c = vram[(cy & 511) * 1024 + ((cx + ((word >> ((u & 3) * 4)) & 15)) & 1023)];
                uint8_t *o = &b->rgba[(((ci * bh) + (v - vmin)) * 256 + u) * 4];
                o[0] = (uint8_t)((c & 31) << 3), o[1] = (uint8_t)(((c >> 5) & 31) << 3), o[2] = (uint8_t)(((c >> 10) & 31) << 3);
                o[3] = c == 0 ? 0 : 255;
                opaque += c != 0;
            }
    }
    // (the texture not in VRAM yet: nearly all of it empty)
    if (opaque < b->tw * b->th / 20) return nullptr;
    // vertices: model x along the gun (barrel at -x), y down, z across -> X = z, Y = -y, Z = x
    std::vector<float> V((size_t)nv * 3);
    float xmin = 1e9f, xmax = -1e9f;
    for (int i = 0; i < nv; i++) {
        float x = rd16s(ram, vert + 8 * i), y = rd16s(ram, vert + 8 * i + 2), z = rd16s(ram, vert + 8 * i + 4);
        if (s.turn) x = -x, z = -z;
        V[i * 3] = x, V[i * 3 + 1] = y, V[i * 3 + 2] = z;
        xmin = std::min(xmin, x), xmax = std::max(xmax, x);
    }
    if (xmax - xmin < 1) return nullptr;
    float S = s.length_mm / 1000.0f / (xmax - xmin), FX = s.fist_x, FY = s.fist_y;
    auto P = [&](int i, float out[3]) {
        out[0] = V[i * 3 + 2] * S, out[1] = -(V[i * 3 + 1] - FY) * S, out[2] = (V[i * 3] - FX) * S;
    };
    float bore = 0;
    int nb = 0;
    for (int i = 0; i < nv; i++)
        if (V[i * 3] <= xmin + 1) bore += V[i * 3 + 1], nb++;
    bore /= std::max(1, nb);
    b->muzzle_y = -(bore - FY) * S, b->muzzle_z = (xmin - FX) * S;
    for (auto &f : faces) {
        int ci = (int)(std::find(cluts.begin(), cluts.end(), f.clut) - cluts.begin());
        float pts[4][3];
        for (int k = 0; k < f.n; k++) P(f.idx[k], pts[k]);
        static const int TQ[2][3] = {{0, 1, 2}, {1, 3, 2}};
        for (int t = 0; t < (f.n == 4 ? 2 : 1); t++) {
            const int *T = TQ[t];
            float e1[3], e2[3], nn[3];
            for (int i = 0; i < 3; i++) e1[i] = pts[T[1]][i] - pts[T[0]][i], e2[i] = pts[T[2]][i] - pts[T[0]][i];
            nn[0] = e1[1] * e2[2] - e1[2] * e2[1], nn[1] = e1[2] * e2[0] - e1[0] * e2[2], nn[2] = e1[0] * e2[1] - e1[1] * e2[0];
            float l = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
            if (l > 0) for (float &x : nn) x /= l;
            else nn[0] = 0, nn[1] = 1, nn[2] = 0;
            for (int k = 0; k < 3; k++) {
                int q = T[k];
                float u = (f.uv[q][0] + 0.5f) / b->tw, v = (ci * bh + f.uv[q][1] - vmin + 0.5f) / b->th;
                float row[8] = {pts[q][0], pts[q][1], pts[q][2], nn[0], nn[1], nn[2], u, v};
                b->mesh.insert(b->mesh.end(), row, row + 8);
            }
        }
    }
    return b;
}

// The Custom Magnum: the Magnum with a 6" barrel (as tools/models/stretchbarrel.py -0.10 -0.175 55)
std::unique_ptr<Built> stretched(const Built &src) {
    auto b = std::make_unique<Built>(src);
    const float zf = -0.10f, zt = -0.175f, add = 0.055f, k = (zf - zt + add) / (zf - zt);
    auto fz = [&](float z) { return z >= zf ? z : z >= zt ? zf + (z - zf) * k : z - add; };
    for (size_t i = 2; i < b->mesh.size(); i += 8) b->mesh[i] = fz(b->mesh[i]);
    b->muzzle_z = fz(b->muzzle_z);
    return b;
}

void set_model(int weapon, std::unique_ptr<Built> b) {
    b->publish();
    pub[weapon].store(&b->pm);
    models[weapon] = std::move(b);  // (the old one, if any, is never replaced: weapons are harvested once)
}

void save_cache() {
    FILE *f = fopen((cache_path + ".tmp").c_str(), "wb");
    if (!f) return;
    fwrite("RVW1", 1, 4, f);
    for (int w = 0; w < 16; w++) {
        if (!models[w] || w == 7) continue;  // (the Custom Magnum is made from the Magnum when loading)
        const Built &b = *models[w];
        int32_t hdr[4] = {w, b.tw, b.th, (int32_t)b.mesh.size()};
        fwrite(hdr, sizeof hdr, 1, f);
        fwrite(&b.muzzle_y, 4, 1, f), fwrite(&b.muzzle_z, 4, 1, f);
        fwrite(b.rgba.data(), 1, b.rgba.size(), f);
        fwrite(b.mesh.data(), 4, b.mesh.size(), f);
    }
    fclose(f);
    remove(cache_path.c_str());
    rename((cache_path + ".tmp").c_str(), cache_path.c_str());
}

}  // namespace

void harvest_init(const char *path) {
    std::lock_guard<std::mutex> lk(m);
    if (loaded) return;
    loaded = true;
    if (path) cache_path = path;
    FILE *f = fopen(cache_path.c_str(), "rb");
    if (!f) return;
    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "RVW1", 4) != 0) { fclose(f); return; }
    int32_t hdr[4];
    while (fread(hdr, sizeof hdr, 1, f) == 1) {
        if (hdr[0] < 0 || hdr[0] >= 16 || hdr[1] <= 0 || hdr[1] > 1024 || hdr[2] <= 0 || hdr[2] > 1024 || hdr[3] <= 0 ||
            hdr[3] > 8 * 20000)
            break;
        auto b = std::make_unique<Built>();
        b->tw = hdr[1], b->th = hdr[2];
        b->rgba.resize((size_t)b->tw * b->th * 4);
        b->mesh.resize((size_t)hdr[3]);
        if (fread(&b->muzzle_y, 4, 1, f) != 1 || fread(&b->muzzle_z, 4, 1, f) != 1 ||
            fread(b->rgba.data(), 1, b->rgba.size(), f) != b->rgba.size() ||
            fread(b->mesh.data(), 4, b->mesh.size(), f) != b->mesh.size())
            break;
        int w = hdr[0];
        if (w == 6) set_model(7, stretched(*b));
        set_model(w, std::move(b));
        logf("weapon model %d loaded from %s", w, cache_path.c_str());
    }
    fclose(f);
}

// Handgun A: the game has no 3D model of it, only its item picture (B\BIGTIM.BIN: 8-bit TIMs, 112 x 72, the first is
// Handgun A's). VR's own shape (handgun_a_shape.h: the Glock 17's measures) is coloured with the part of that picture it
// was made from, read from the player's disc once at start; recognised by the CRC of the pixels its mask covers.
static void handgun_a_from_disc() {
    std::vector<u8> d;
    if (!cdrom_read_file("B/BIGTIM.BIN", d) || d.size() < 64) return;
    for (size_t p = 0; p + 40 <= d.size();) {
        uint32_t id, fl;
        memcpy(&id, &d[p], 4), memcpy(&fl, &d[p + 4], 4);
        if (id != 0x10 || fl != 9) { p += 4; continue; }  // (an 8-bit TIM with its CLUT)
        size_t q = p + 8;
        uint32_t cl_len, im_len;
        uint16_t cw, ch, iw, ih;
        memcpy(&cl_len, &d[q], 4), memcpy(&cw, &d[q + 8], 2), memcpy(&ch, &d[q + 10], 2);
        const size_t clut_at = q + 12;
        q += cl_len;
        if (q + 12 > d.size()) break;
        memcpy(&im_len, &d[q], 4), memcpy(&iw, &d[q + 8], 2), memcpy(&ih, &d[q + 10], 2);
        const size_t img_at = q + 12;
        const int W = iw * 2, H = ih;
        if (cw < 256 || img_at + (size_t)W * H > d.size() || W < HGA_PIC_X + HGA_TEX_W || H < HGA_PIC_Y + HGA_TEX_H) {
            p = q + im_len;
            continue;
        }
        auto colour = [&](int x, int y) {
            uint8_t ix = d[img_at + (size_t)(HGA_PIC_Y + y) * W + HGA_PIC_X + x];
            uint16_t c;
            memcpy(&c, &d[clut_at + 2 * ix], 2);
            return (uint16_t)(c & 0x7fff);
        };
        std::vector<uint16_t> px;
        for (int y = 0; y < HGA_TEX_H; y++)
            for (int x = 0; x < HGA_TEX_W; x++) {
                int i = y * HGA_TEX_W + x;
                if (HGA_MASK[i >> 3] & (1 << (i & 7))) px.push_back(colour(x, y));
            }
        if (crc32((const uint8_t *)px.data(), px.size() * 2) == HGA_CRC) {
            auto b = std::make_unique<Built>();
            b->tw = HGA_TEX_W, b->th = HGA_TEX_H;
            b->rgba.assign((size_t)b->tw * b->th * 4, 0);
            for (int y = 0; y < HGA_TEX_H; y++)
                for (int x = 0; x < HGA_TEX_W; x++) {
                    int i = y * HGA_TEX_W + x;
                    if (!(HGA_MASK[i >> 3] & (1 << (i & 7)))) continue;
                    uint16_t c = colour(x, y);
                    uint8_t *o = &b->rgba[(size_t)i * 4];
                    o[0] = (uint8_t)((c & 31) << 3), o[1] = (uint8_t)(((c >> 5) & 31) << 3), o[2] = (uint8_t)(((c >> 10) & 31) << 3);
                    o[3] = 255;
                }
            b->mesh.assign(HGA_MESH, HGA_MESH + HGA_VERTS * 8);
            b->muzzle_y = 0.066f, b->muzzle_z = -0.152f;  // (the pistol's default muzzle)
            std::lock_guard<std::mutex> lk(m);
            set_model(1, std::move(b));
            logf("weapon model: Handgun A coloured with its item picture from the disc");
            return;
        }
        p = q + im_len;
    }
    logf("weapon model: Handgun A's picture not found on the disc (the plain Glock is held)");
}

void harvest_tick() {
    static int tick = 0;
    if (++tick % 30) return;  // twice a second is plenty
    if (!loaded) harvest_init(nullptr), handgun_a_from_disc();
    bool missing = false;
    for (const Spec &s : SPECS) missing = missing || !pub[s.weapon].load();
    if (!missing) return;
    const uint8_t *ram = M.ram;
    const uint16_t *vram = gpu_vram();
    for (uint32_t o = 0x10000; o + 16 <= 0x200000; o += 4) {
        if (rd32(ram, o) != 3) continue;
        uint32_t p = rd32(ram, o + 4), v = rd32(ram, o + 8), n = rd32(ram, o + 12);
        if ((p >> 21) != (0x80000000u >> 21) || (v >> 21) != (0x80000000u >> 21) || (n >> 21) != (0x80000000u >> 21))
            continue;
        if (!(p < v && v < n) || ((n - v) & 7)) continue;
        int nv = (int)((n - v) / 8), pb = (int)(v - p);
        for (const Spec &s : SPECS) {
            if (s.nv != nv || s.prim_bytes != pb || pub[s.weapon].load()) continue;
            if (crc32(ram + (v & 0x1fffff), (size_t)nv * 8) != s.vert_crc) continue;
            auto b = build(ram, vram, p & 0x1fffff, v & 0x1fffff, nv, s);
            if (!b) continue;
            std::lock_guard<std::mutex> lk(m);
            if (s.weapon == 6) set_model(7, stretched(*b));
            set_model(s.weapon, std::move(b));
            save_cache();
            logf("weapon model: %s found in RAM (record %08x), kept in %s", s.name, 0x80000000u | o, cache_path.c_str());
        }
    }
}

const void *harvested_model(int weapon) {
    if (weapon < 0 || weapon >= 16) return nullptr;
    return pub[weapon].load();
}

}  // namespace game
