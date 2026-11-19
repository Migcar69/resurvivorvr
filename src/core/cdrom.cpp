// CD-ROM controller (register level, as driven by PsyQ libcd) with XA-ADPCM and CD-DA audio.
#include "psx.h"
#include "state.h"
#include <algorithm>
#include <cstdio>
#include <deque>
#include <mutex>

namespace psx {

// ------------------------------------------------------------- disc image
struct Track {
    int number;
    bool audio;
    u32 start;  // absolute disc sector (MSF incl. 2s offset) of INDEX 01
};
// The disc image may be one BIN or one BIN per track (multi-FILE cue): the files are laid out
// back to back as a single sector space.
struct BinFile {
    FILE *f;
    u32 first;    // first sector (0-based, without the 2s lead-in) covered by this file
    u32 sectors;
};
static std::vector<BinFile> bins;
static u32 bin_sectors = 0;
static std::vector<Track> tracks;

static int bcd2i(u8 b) { return (b >> 4) * 10 + (b & 15); }
static u8 i2bcd(int i) { return (u8)(((i / 10) << 4) | (i % 10)); }

bool cdrom_open(const std::string &cue_path) {
    FILE *f = fopen(cue_path.c_str(), "r");
    if (!f) return false;
    std::string dir = cue_path.substr(0, cue_path.find_last_of("/\\") + 1);
    char line[512];
    int cur = 0;
    bool cur_audio = false;
    bool ok = true;
    while (fgets(line, sizeof line, f)) {
        char a[256];
        int n, mm, ss, ff;
        if (sscanf(line, " FILE \"%255[^\"]\"", a) == 1) {
            FILE *bf = fopen((dir + a).c_str(), "rb");
            if (!bf) { ok = false; break; }
            _fseeki64(bf, 0, SEEK_END);
            u32 n_sec = (u32)(_ftelli64(bf) / 2352);
            bins.push_back({bf, bin_sectors, n_sec});
            bin_sectors += n_sec;
        } else if (sscanf(line, " TRACK %d %255s", &n, a) == 2) {
            cur = n;
            cur_audio = strcmp(a, "AUDIO") == 0;
        } else if (sscanf(line, " INDEX %d %d:%d:%d", &n, &mm, &ss, &ff) == 4 && n == 1 && !bins.empty()) {
            // INDEX times are relative to the start of the current FILE
            tracks.push_back({cur, cur_audio, bins.back().first + (u32)((mm * 60 + ss) * 75 + ff + 150)});
        }
    }
    fclose(f);
    return ok && !bins.empty();
}

static bool read_raw(u32 disc_sector, u8 *out) {
    if (disc_sector < 150 || disc_sector - 150 >= bin_sectors) {
        memset(out, 0, 2352);
        return false;
    }
    u32 s = disc_sector - 150;
    for (auto &b : bins)
        if (s >= b.first && s < b.first + b.sectors) {
            _fseeki64(b.f, (long long)(s - b.first) * 2352, SEEK_SET);
            return fread(out, 1, 2352, b.f) == 2352;
        }
    memset(out, 0, 2352);
    return false;
}

// Read a file from the ISO9660 filesystem on the data track (path like "SLPS_007.42" or "DATA/AVATAR.")
bool cdrom_read_file(const std::string &path, std::vector<u8> &outv) {
    u8 raw[2352];
    auto rd = [&](u32 lba, u8 *dst) {
        read_raw(lba + 150, raw);
        memcpy(dst, raw + 24, 2048);
    };
    u8 sec[2048];
    rd(16, sec);
    u32 dir_lba, dir_size;
    memcpy(&dir_lba, sec + 156 + 2, 4);
    memcpy(&dir_size, sec + 156 + 10, 4);
    std::string rest = path;
    while (!rest.empty()) {
        size_t sl = rest.find_first_of("/\\");
        std::string part = rest.substr(0, sl);
        rest = sl == std::string::npos ? "" : rest.substr(sl + 1);
        bool found = false;
        for (u32 s = 0; s < (dir_size + 2047) / 2048 && !found; s++) {
            rd(dir_lba + s, sec);
            for (u32 p = 0; p < 2048 && sec[p];) {
                u8 len = sec[p];
                std::string nm((const char *)sec + p + 33, sec[p + 32]);
                nm = nm.substr(0, nm.find(';'));
                if (_stricmp(nm.c_str(), part.c_str()) == 0) {
                    memcpy(&dir_lba, sec + p + 2, 4);
                    memcpy(&dir_size, sec + p + 10, 4);
                    found = true;
                    break;
                }
                p += len;
            }
        }
        if (!found) return false;
    }
    outv.resize(dir_size);
    for (u32 o = 0; o < dir_size; o += 2048) {
        rd(dir_lba + o / 2048, sec);
        memcpy(outv.data() + o, sec, std::min<u32>(2048, dir_size - o));
    }
    return true;
}

static int track_of(u32 sector) {
    int t = 0;
    for (size_t i = 0; i < tracks.size(); i++)
        if (sector >= tracks[i].start - (i ? 150 : 0)) t = (int)i;
    return t;
}

// ------------------------------------------------------------- controller state
static u8 index_reg = 0;
static std::deque<u8> params;
static u8 int_enable = 0, int_flag = 0;
static std::deque<u8> response;
static std::vector<u8> data_fifo;
static size_t data_pos = 0;
static u8 mode = 0;
static u8 stat = 0x02;  // motor on
static u32 setloc = 0, pos = 0;
static bool setloc_pending = false;
static u8 filter_file = 0, filter_chan = 0;
static bool muted = false;
static u8 last_header[8];
static u8 sector_buf[2352];
static bool sector_ready = false;

enum State { IDLE, READING, PLAYING };
static State state = IDLE;
static u64 read_due = ~0ull;

struct Pending {
    u8 irq;
    std::vector<u8> resp;
    u64 due;
};

static std::deque<Pending> pending;  // queued interrupts/responses
static u64 cmd_due = ~0ull;

static void push_irq(u8 irq, std::vector<u8> r, u64 delay) {
    u64 due = M.cycles + delay;
    if (!pending.empty()) due = std::max(due, pending.back().due + 1);
    pending.push_back({irq, std::move(r), due});
}

static void deliver_pending() {
    if (pending.empty() || int_flag & 7) return;
    if (M.cycles < pending.front().due) return;
    Pending p = pending.front();
    pending.pop_front();
    response.assign(p.resp.begin(), p.resp.end());
    int_flag = (int_flag & ~7) | p.irq;
    if (int_flag & int_enable & 0x1f) M.irq(IRQ_CDROM);
}

// ------------------------------------------------------------- audio (XA / CD-DA)
bool g_log_cd = false;
static std::mutex audio_mtx;
static std::deque<s16> audio_q;  // interleaved stereo @44100
static u8 vol_ll = 0x80, vol_lr = 0, vol_rl = 0, vol_rr = 0x80;
static u8 pv_ll = 0x80, pv_lr = 0, pv_rl = 0, pv_rr = 0x80;

static void audio_push_resampled(const s16 *st, int frames, int rate) {
    // linear resample stereo frames to 44100
    static double phase = 0;
    static s16 last_l = 0, last_r = 0;
    std::lock_guard<std::mutex> lk(audio_mtx);
    double step = (double)rate / 44100.0;
    double p = phase;
    while (p < frames) {
        int i = (int)p;
        double fr = p - i;
        s16 l0 = i == 0 ? last_l : st[(i - 1) * 2], r0 = i == 0 ? last_r : st[(i - 1) * 2 + 1];
        s16 l1 = st[i * 2], r1 = st[i * 2 + 1];
        int l = (int)(l0 + (l1 - l0) * fr), r = (int)(r0 + (r1 - r0) * fr);
        int ol = (l * vol_ll + r * vol_rl) >> 7, orr = (r * vol_rr + l * vol_lr) >> 7;
        audio_q.push_back((s16)std::clamp(ol, -32768, 32767));
        audio_q.push_back((s16)std::clamp(orr, -32768, 32767));
        p += step;
    }
    phase = p - frames;
    last_l = st[(frames - 1) * 2];
    last_r = st[(frames - 1) * 2 + 1];
    while (audio_q.size() > 44100 * 2) audio_q.pop_front();  // bound latency
}

void cdrom_get_audio(s16 *out, int frames) {
    std::lock_guard<std::mutex> lk(audio_mtx);
    for (int i = 0; i < frames * 2; i++) {
        if (audio_q.empty()) out[i] = 0;
        else { out[i] = audio_q.front(); audio_q.pop_front(); }
    }
}

static const int xa_k0[4] = {0, 60, 115, 98}, xa_k1[4] = {0, 0, -52, -55};
static s32 xa_prev[2][2];

static void xa_decode(const u8 *sec) {
    const u8 *sub = sec + 16;
    u8 ci = sub[3];
    bool stereo = (ci & 3) == 1;
    int rate = (ci & 0x0c) ? 18900 : 37800;
    bool bits8 = (ci & 0x30) == 0x10;
    std::vector<s16> out;
    out.reserve(4032 * 2);
    std::vector<s16> mono;
    const u8 *data = sec + 24;
    for (int grp = 0; grp < 18; grp++) {
        const u8 *g = data + grp * 128;
        int units = bits8 ? 4 : 8;
        for (int u = 0; u < units; u++) {
            u8 hdr = g[4 + u];
            int range = hdr & 15;
            if (range > 12) range = 9;  // invalid ranges behave like 9
            int filt = (hdr >> 4) & 3;
            int ch = stereo ? (u & 1) : 0;
            s32 &p1 = xa_prev[ch][0], &p2 = xa_prev[ch][1];
            for (int s = 0; s < 28; s++) {
                s32 v;
                if (bits8) v = (s32)(s16)(u16)(g[16 + u + s * 4] << 8);
                else {
                    u8 b = g[16 + (u >> 1) + s * 4];
                    v = (s32)(s16)(u16)((((u & 1) ? (b >> 4) : (b & 15))) << 12);
                }
                v >>= range;
                s32 smp = v + ((p1 * xa_k0[filt] + p2 * xa_k1[filt] + 32) >> 6);
                smp = std::clamp(smp, -32768, 32767);
                p2 = p1;
                p1 = smp;
                if (stereo) {
                    // stereo: even units L, odd units R; store in interleaved position
                    size_t idx = (size_t)(grp * (units / 2) + (u >> 1)) * 28 + s;
                    if (out.size() < (idx + 1) * 2) out.resize((idx + 1) * 2);
                    out[idx * 2 + ch] = (s16)smp;
                } else {
                    mono.push_back((s16)smp);
                }
            }
        }
    }
    if (!stereo) {
        out.resize(mono.size() * 2);
        for (size_t i = 0; i < mono.size(); i++) out[i * 2] = out[i * 2 + 1] = mono[i];
    }
    if (!muted) audio_push_resampled(out.data(), (int)(out.size() / 2), rate);
}

// ------------------------------------------------------------- reading
static u64 sector_cycles() { return (mode & 0x80) ? CPU_HZ / 150 : CPU_HZ / 75; }

static void start_read() {
    if (setloc_pending) { pos = setloc; setloc_pending = false; }
    state = READING;
    stat = (stat & ~0xe0) | 0x20;
    read_due = M.cycles + sector_cycles() + 20000;
}

static void do_read_sector() {
    u8 raw[2352];
    read_raw(pos, raw);
    u32 cur = pos++;
    (void)cur;
    memcpy(last_header, raw + 12, 8);
    u8 submode = raw[18];
    bool is_audio_sector = (raw[15] == 2) && (submode & 0x04);
    if ((mode & 0x40) && is_audio_sector) {
        bool match = !(mode & 0x08) || (raw[16] == filter_file && raw[17] == filter_chan);
        if (match) xa_decode(raw);
        return;  // XA audio sectors are not delivered to the CPU
    }
    // The XA filter only selects which audio sectors are played: data sectors always reach the CPU (as on hardware;
    // Resident Evil Survivor reads its data files with XA playback and a filter on another file number).
    memcpy(sector_buf, raw, 2352);
    sector_ready = true;
    push_irq(1, {stat}, 0);
}

static void do_play_sector() {
    static int cnt = 0;
    if (g_log_cd && ++cnt % 75 == 0) logf("CDDA pos %u queue %zu frame %llu", pos, audio_q.size() / 2, (unsigned long long)(M.cycles / CYCLES_PER_FRAME));
    u8 raw[2352];
    read_raw(pos++, raw);
    if (!muted) audio_push_resampled((const s16 *)raw, 588, 44100);
    int t = track_of(pos);
    if ((mode & 0x02) && t + 1 < (int)tracks.size() && pos >= tracks[t + 1].start - 150) {  // autopause
        state = IDLE;
        stat &= ~0x80;
        push_irq(4, {stat}, 0);
    }
}

// ------------------------------------------------------------- commands
static void command(u8 cmd) {
    std::vector<u8> p(params.begin(), params.end());
    if (g_log_cd) logf("CD cmd %02x params=%d [%02x %02x %02x] frame %llu pc %08x ra %08x", cmd, (int)p.size(), p.size() > 0 ? p[0] : 0, p.size() > 1 ? p[1] : 0, p.size() > 2 ? p[2] : 0, (unsigned long long)(M.cycles / CYCLES_PER_FRAME), M.cpu.pc, M.cpu.r[31]);
    params.clear();
    const u64 ACK = 25000;
    auto ack = [&]() { push_irq(3, {stat}, ACK); };
    switch (cmd) {
    case 0x01: ack(); break;  // Getstat
    case 0x02:  // Setloc
        if (p.size() >= 3) {
            setloc = (u32)((bcd2i(p[0]) * 60 + bcd2i(p[1])) * 75 + bcd2i(p[2]));
            setloc_pending = true;
        }
        ack();
        break;
    case 0x03: {  // Play
        if (!p.empty() && p[0]) {
            int t = bcd2i(p[0]);
            for (auto &tr : tracks) if (tr.number == t) { pos = tr.start; }
            setloc_pending = false;
        } else if (setloc_pending) { pos = setloc; setloc_pending = false; }
        state = PLAYING;
        stat = (stat & ~0xe0) | 0x80;
        read_due = M.cycles + CPU_HZ / 75;
        ack();
        break;
    }
    case 0x04: case 0x05: ack(); break;  // Forward/Backward
    case 0x06: case 0x1b: ack(); start_read(); break;  // ReadN/ReadS
    case 0x07: stat |= 2; ack(); push_irq(2, {stat}, 100000); break;  // MotorOn
    case 0x08:  // Stop
        state = IDLE; stat &= ~0xe0; read_due = ~0ull;
        ack(); push_irq(2, {stat}, 100000); stat &= ~2;
        break;
    case 0x09:  // Pause
        ack();
        state = IDLE; read_due = ~0ull;
        stat &= ~0xe0;
        push_irq(2, {stat}, 70000);
        break;
    case 0x0a:  // Init
        mode = 0; stat = 0x02; state = IDLE; read_due = ~0ull;
        ack(); push_irq(2, {stat}, 70000);
        break;
    case 0x0b: muted = true; ack(); break;
    case 0x0c: muted = false; ack(); break;
    case 0x0d: if (p.size() >= 2) { filter_file = p[0]; filter_chan = p[1]; } ack(); break;
    case 0x0e: if (!p.empty()) mode = p[0]; ack(); break;
    case 0x0f: push_irq(3, {stat, mode, 0, filter_file, filter_chan}, ACK); break;
    case 0x10: push_irq(3, std::vector<u8>(last_header, last_header + 8), ACK); break;
    case 0x11: {  // GetlocP
        int t = track_of(pos);
        u32 rel = pos >= tracks[t].start ? pos - tracks[t].start : tracks[t].start - pos;
        u32 abs = pos;
        push_irq(3, {i2bcd(tracks[t].number), 1, i2bcd(rel / 75 / 60), i2bcd(rel / 75 % 60), i2bcd(rel % 75),
                     i2bcd(abs / 75 / 60), i2bcd(abs / 75 % 60), i2bcd(abs % 75)}, ACK);
        break;
    }
    case 0x12: ack(); push_irq(2, {stat}, 100000); break;  // SetSession
    case 0x13: push_irq(3, {stat, 1, i2bcd(tracks.back().number)}, ACK); break;
    case 0x14: {  // GetTD
        int t = p.empty() ? 0 : bcd2i(p[0]);
        u32 s = bin_sectors + 150;
        for (auto &tr : tracks) if (tr.number == t) s = tr.start;
        push_irq(3, {stat, i2bcd(s / 75 / 60), i2bcd(s / 75 % 60)}, ACK);
        break;
    }
    case 0x15: case 0x16:  // SeekL/SeekP
        if (setloc_pending) { pos = setloc; setloc_pending = false; }
        state = IDLE; read_due = ~0ull;
        stat = (stat & ~0xe0);
        ack(); push_irq(2, {stat}, 60000);
        break;
    case 0x19:  // Test
        if (!p.empty() && p[0] == 0x20) push_irq(3, {0x94, 0x09, 0x19, 0xc0}, ACK);
        else push_irq(3, {stat}, ACK);
        break;
    case 0x1a:  // GetID
        ack();
        push_irq(2, {0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'I'}, 50000);
        break;
    case 0x1c: mode = 0; state = IDLE; read_due = ~0ull; ack(); break;
    case 0x1e: ack(); push_irq(2, {stat}, 500000); break;  // ReadTOC
    default:
        logf("CD: unknown command %02x", cmd);
        push_irq(5, {(u8)(stat | 1), 0x40}, ACK);
        break;
    }
}

void cdrom_init() {
    params.clear(); response.clear(); pending.clear();
    data_fifo.clear(); data_pos = 0;
    int_flag = 0;
    int_enable = 0x1f;  // left enabled by the BIOS after booting from disc
    state = IDLE; read_due = ~0ull;
}

u8 cdrom_read(u32 reg) {
    switch (reg) {
    case 0: {
        u8 s = index_reg;
        s |= params.empty() ? 0x08 : 0;
        s |= params.size() < 16 ? 0x10 : 0;
        s |= response.empty() ? 0 : 0x20;
        s |= data_pos < data_fifo.size() ? 0x40 : 0;
        return s;
    }
    case 1: {
        if (response.empty()) return 0;
        u8 v = response.front();
        response.pop_front();
        return v;
    }
    case 2: return data_pos < data_fifo.size() ? data_fifo[data_pos++] : 0;
    case 3: return (index_reg & 1) ? (u8)(int_flag | 0xe0) : (u8)(int_enable | 0xe0);
    }
    return 0;
}

void cdrom_write(u32 reg, u8 v) {
    switch (reg * 4 + index_reg) {
    case 0: case 1: case 2: case 3: index_reg = v & 3; return;
    case 4: command(v); break;                       // 1801.0
    case 8: if (params.size() < 16) params.push_back(v); break;  // 1802.0
    case 12:                                         // 1803.0 request
        if (v & 0x80) {
            if (sector_ready) {
                size_t off = (mode & 0x20) ? 12 : 24;
                size_t len = (mode & 0x20) ? 2340 : 2048;
                data_fifo.assign(sector_buf + off, sector_buf + off + len);
                data_pos = 0;
                sector_ready = false;
            }
        } else {
            data_fifo.clear();
            data_pos = 0;
        }
        break;
    case 9: int_enable = v & 0x1f; break;            // 1802.1
    case 13:                                         // 1803.1 ack
        int_flag &= ~(v & 0x1f);
        if (v & 0x40) params.clear();
        // Acknowledging does not empty the response FIFO: some libcd versions ack first and read the result
        // after (Magic Carpet's CdGetToc). The next interrupt replaces it, but not right away, as on hardware.
        if (!(int_flag & 7) && !pending.empty()) pending.front().due = std::max(pending.front().due, M.cycles + 2000);
        break;
    case 10: pv_ll = v; break;                       // 1802.2
    case 14: pv_lr = v; break;                       // 1803.2
    case 7: pv_rr = v; break;                        // 1801.3
    case 11: pv_rl = v; break;                       // 1802.3
    case 15: if (v & 0x20) { vol_ll = pv_ll; vol_lr = pv_lr; vol_rl = pv_rl; vol_rr = pv_rr; } break;
    default: break;
    }
    schedule_update();
}

u32 cdrom_dma_read() {
    u32 v = 0;
    for (int i = 0; i < 4; i++) {
        u8 b = data_pos < data_fifo.size() ? data_fifo[data_pos++] : 0;
        v |= (u32)b << (i * 8);
    }
    return v;
}

u64 cdrom_next_event() {
    u64 n = read_due;
    if (!pending.empty() && !(int_flag & 7)) n = std::min(n, pending.front().due);
    return n;
}

void cdrom_event() {
    if (M.cycles >= read_due) {
        if (state == READING) {
            // Only deliver a new data sector if the previous INT1 was acknowledged
            do_read_sector();
            read_due += sector_cycles();
        } else if (state == PLAYING) {
            do_play_sector();
            read_due += CPU_HZ / 75;
        } else {
            read_due = ~0ull;
        }
        if (read_due <= M.cycles) read_due = M.cycles + sector_cycles();
    }
    deliver_pending();
    // Avoid an unbounded backlog of INT1 if the game is slow to acknowledge
    while (pending.size() > 8) pending.pop_front();
}


void st_cdrom(StateIO &s) {
    s.pod(index_reg); s.deq(params); s.pod(int_enable); s.pod(int_flag); s.deq(response); s.vec(data_fifo);
    s.pod(data_pos); s.pod(mode); s.pod(stat); s.pod(setloc); s.pod(pos); s.pod(setloc_pending); s.pod(filter_file);
    s.pod(filter_chan); s.pod(muted); s.pod(last_header); s.pod(sector_buf); s.pod(sector_ready); s.pod(state);
    s.pod(read_due); s.pod(cmd_due);
    u32 n = (u32)pending.size();
    s.pod(n);
    if (!s.saving) pending.resize(n);
    for (auto &p : pending) { s.pod(p.irq); s.vec(p.resp); s.pod(p.due); }
    s.pod(vol_ll); s.pod(vol_lr); s.pod(vol_rl); s.pod(vol_rr); s.pod(pv_ll); s.pod(pv_lr); s.pod(pv_rl); s.pod(pv_rr);
    s.pod(xa_prev);
    if (!s.saving) { std::lock_guard<std::mutex> lk(audio_mtx); audio_q.clear(); }
}

}  // namespace psx
