// Save states of the whole emulated machine (development tool: headless tests start from a saved point in the game
// instead of replaying the boot every time). Each core module serializes its own state with st_*(StateIO&).
#pragma once
#include "psx.h"
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace psx {

struct StateIO {
    FILE *f = nullptr;
    bool saving = true;
    bool ok = true;
    void raw(void *p, size_t n) {
        if (!ok || !n) return;
        size_t r = saving ? fwrite(p, 1, n, f) : fread(p, 1, n, f);
        if (r != n) ok = false;
    }
    template <class T> void pod(T &v) { raw(&v, sizeof v); }
    template <class T> void vec(std::vector<T> &v) {
        u32 n = (u32)v.size();
        pod(n);
        if (!saving) v.resize(n);
        if (n) raw(v.data(), n * sizeof(T));
    }
    template <class T> void deq(std::deque<T> &d) {
        std::vector<T> v(d.begin(), d.end());
        vec(v);
        if (!saving) d.assign(v.begin(), v.end());
    }
    template <class K, class V> void map(std::map<K, V> &m) {
        std::vector<std::pair<K, V>> v(m.begin(), m.end());
        vec(v);
        if (!saving) { m.clear(); for (auto &kv : v) m.insert(kv); }
    }
};

void st_bios(StateIO &s);
void st_cdrom(StateIO &s);
void st_hw(StateIO &s);
void st_gpu(StateIO &s);
void st_spu(StateIO &s);
void st_mdec(StateIO &s);
void st_pad(StateIO &s);
void st_memcard(StateIO &s);
void st_cpu(StateIO &s);
bool state_save(const char *path);
bool state_load(const char *path);

}  // namespace psx
