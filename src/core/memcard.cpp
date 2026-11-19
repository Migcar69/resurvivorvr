// Memory card: standard 128KB raw image (.mcd, emulator-compatible) and the BIOS "bu00:" file API.
#include "psx.h"
#include "state.h"
#include <cstdio>

namespace psx {

static u8 card[128 * 1024];
static std::string card_path;

constexpr int FRAME = 128, BLOCK = 8192;

static void save_card() {
    FILE *f = fopen(card_path.c_str(), "wb");
    if (!f) return;
    fwrite(card, 1, sizeof card, f);
    fclose(f);
}

static void frame_checksum(u8 *fr) {
    u8 x = 0;
    for (int i = 0; i < 127; i++) x ^= fr[i];
    fr[127] = x;
}

void mc_format() {
    memset(card, 0, sizeof card);
    card[0] = 'M';
    card[1] = 'C';
    frame_checksum(card);
    for (int i = 1; i < 16; i++) {
        u8 *d = card + i * FRAME;
        d[0] = 0xa0;
        d[8] = 0xff;
        d[9] = 0xff;
        frame_checksum(d);
    }
    for (int i = 16; i < 36; i++) {
        u8 *d = card + i * FRAME;
        memset(d, 0, FRAME);
        d[0] = d[1] = d[2] = d[3] = 0xff;
        d[8] = d[9] = 0xff;
        frame_checksum(d);
    }
    memcpy(card + 63 * FRAME, card, FRAME);
    save_card();
}

void memcard_init(const std::string &path) {
    card_path = path;
    FILE *f = fopen(path.c_str(), "rb");
    if (f) {
        size_t n = fread(card, 1, sizeof card, f);
        fclose(f);
        if (n == sizeof card && card[0] == 'M' && card[1] == 'C') return;
    }
    mc_format();
}

void mc_raw_read(int sector, u32 dst) {
    if (sector < 0 || sector >= 1024) return;
    for (int i = 0; i < FRAME; i++) write8(dst + i, card[sector * FRAME + i]);
}

void mc_raw_write(int sector, u32 src) {
    if (sector < 0 || sector >= 1024) return;
    for (int i = 0; i < FRAME; i++) card[sector * FRAME + i] = read8(src + i);
    save_card();
}

// ---- directory helpers
static u8 *dir(int blk) { return card + blk * FRAME; }  // blk 1..15

static std::string strip_dev(const std::string &name) {
    size_t c = name.find(':');
    return c == std::string::npos ? name : name.substr(c + 1);
}

static int find_file(const std::string &fname) {
    for (int b = 1; b < 16; b++) {
        u8 *d = dir(b);
        if (d[0] != 0x51) continue;
        if (strncmp((const char *)d + 10, fname.c_str(), 20) == 0) return b;
    }
    return -1;
}

static std::vector<int> file_blocks(int first) {
    std::vector<int> v;
    int b = first;
    while (b >= 1 && b < 16 && v.size() < 15) {
        v.push_back(b);
        u16 next = (u16)(dir(b)[8] | (dir(b)[9] << 8));
        if (next == 0xffff) break;
        b = next + 1;
    }
    return v;
}

struct Fd {
    bool used = false;
    int first = 0;
    u32 pos = 0, size = 0;
    bool async = false;
};
static Fd fds[16];

static bool wildcard(const char *p, const char *s) {
    if (!*p) return !*s;
    if (*p == '*') return wildcard(p + 1, s) || (*s && wildcard(p, s + 1));
    if (*p == '?') return *s && wildcard(p + 1, s + 1);
    return *p == *s && wildcard(p + 1, s + 1);
}

int mc_open(const std::string &name, u32 mode) {
    std::string fname = strip_dev(name);
    if (name.rfind("bu", 0) != 0) {
        logf("open(%s) non-card device", name.c_str());
        return -1;
    }
    int first = find_file(fname);
    if ((mode & 0x200) && first < 0) {  // create
        int blocks = (int)(mode >> 16);
        if (blocks <= 0) blocks = 1;
        std::vector<int> freeb;
        for (int b = 1; b < 16 && (int)freeb.size() < blocks; b++)
            if ((dir(b)[0] & 0xf0) == 0xa0) freeb.push_back(b);
        if ((int)freeb.size() < blocks) return -1;
        for (size_t i = 0; i < freeb.size(); i++) {
            u8 *d = dir(freeb[i]);
            memset(d, 0, FRAME);
            d[0] = freeb.size() == 1 ? 0x51 : (i == 0 ? 0x51 : (i + 1 == freeb.size() ? 0x53 : 0x52));
            if (i == 0) {
                u32 sz = (u32)blocks * BLOCK;
                memcpy(d + 4, &sz, 4);
                strncpy((char *)d + 10, fname.c_str(), 20);
            }
            u16 next = (i + 1 < freeb.size()) ? (u16)(freeb[i + 1] - 1) : 0xffff;
            d[8] = (u8)next;
            d[9] = (u8)(next >> 8);
            frame_checksum(d);
            memset(card + freeb[i] * BLOCK, 0, BLOCK);
        }
        save_card();
        first = freeb[0];
    } else if (mode & 0x200) {
        return -1;  // already exists
    }
    if (first < 0) return -1;
    for (int i = 2; i < 16; i++) {
        if (!fds[i].used) {
            fds[i].used = true;
            fds[i].first = first;
            fds[i].pos = 0;
            memcpy(&fds[i].size, dir(first) + 4, 4);
            fds[i].async = (mode & 0x8000) != 0;
            return i;
        }
    }
    return -1;
}

static u8 *file_byte(const Fd &f, u32 pos) {
    auto bl = file_blocks(f.first);
    u32 bi = pos / BLOCK;
    if (bi >= bl.size()) return nullptr;
    return card + bl[bi] * BLOCK + (pos % BLOCK);
}

int mc_read(int fd, u32 dst, u32 len) {
    if (fd < 0 || fd >= 16 || !fds[fd].used) return -1;
    Fd &f = fds[fd];
    for (u32 i = 0; i < len; i++) {
        u8 *p = file_byte(f, f.pos);
        if (!p) return (int)i;
        write8(dst + i, *p);
        f.pos++;
    }
    return (int)len;
}

int mc_write(int fd, u32 src, u32 len) {
    if (fd < 0 || fd >= 16 || !fds[fd].used) return -1;
    Fd &f = fds[fd];
    for (u32 i = 0; i < len; i++) {
        u8 *p = file_byte(f, f.pos);
        if (!p) { save_card(); return (int)i; }
        *p = read8(src + i);
        f.pos++;
    }
    save_card();
    return (int)len;
}

int mc_seek(int fd, s32 off, int whence) {
    if (fd < 0 || fd >= 16 || !fds[fd].used) return -1;
    Fd &f = fds[fd];
    if (whence == 0) f.pos = (u32)off;
    else if (whence == 1) f.pos += (u32)off;
    else f.pos = f.size + (u32)off;
    return (int)f.pos;
}

int mc_close(int fd) {
    if (fd < 0 || fd >= 16 || !fds[fd].used) return -1;
    fds[fd].used = false;
    return fd;
}

bool mc_is_async(int fd) { return fd >= 0 && fd < 16 && fds[fd].used && fds[fd].async; }

static std::string find_pattern;
static int find_next = 1;

static u32 fill_dirent(int b, u32 de) {
    u8 *d = dir(b);
    for (int i = 0; i < 20; i++) write8(de + i, d[10 + i]);
    write32(de + 20, 0x50);
    u32 sz;
    memcpy(&sz, d + 4, 4);
    write32(de + 24, sz);
    write32(de + 28, 0);
    write32(de + 32, (u32)b);
    write32(de + 36, 0);
    return de;
}

static u32 find_from(u32 de) {
    for (; find_next < 16; find_next++) {
        u8 *d = dir(find_next);
        if (d[0] != 0x51) continue;
        char nm[21] = {0};
        memcpy(nm, d + 10, 20);
        if (wildcard(find_pattern.c_str(), nm)) {
            int b = find_next++;
            return fill_dirent(b, de);
        }
    }
    return 0;
}

u32 mc_firstfile(const std::string &pattern, u32 de) {
    find_pattern = strip_dev(pattern);
    if (find_pattern.empty()) find_pattern = "*";
    find_next = 1;
    return find_from(de);
}

u32 mc_nextfile(u32 de) { return find_from(de); }

bool mc_erase(const std::string &name) {
    int first = find_file(strip_dev(name));
    if (first < 0) return false;
    for (int b : file_blocks(first)) {
        u8 *d = dir(b);
        d[0] = (u8)(0xa0 | (d[0] & 0x0f));
        frame_checksum(d);
    }
    save_card();
    return true;
}

bool mc_rename(const std::string &a, const std::string &b) {
    int first = find_file(strip_dev(a));
    if (first < 0) return false;
    u8 *d = dir(first);
    memset(d + 10, 0, 21);
    strncpy((char *)d + 10, strip_dev(b).c_str(), 20);
    frame_checksum(d);
    save_card();
    return true;
}


void st_memcard(StateIO &s) { s.pod(fds); s.pod(find_next); }

}  // namespace psx
