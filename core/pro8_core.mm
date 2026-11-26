// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 mtzvfilm. PRO8 - https://github.com/mtzvfilm/PRO8
// PRO8 core - see pro8_core.h
#import <Cocoa/Cocoa.h>
#include "pro8_core.h"
#include "basic_patch.h"
#include <map>
#include <set>
#include <tuple>
#include <cmath>
#include <mach/mach_time.h>

namespace pro8 {

// ---------------------------------------------------------------- patch fields (decoded SysEx offsets, docs/MIDI_MAP.md CCs)
struct Field { int off; bool cont; int cc; int ccf; };
static const std::map<std::string, Field> FIELDS = {
    {"freq_a", {5, 1, 8, 80}}, {"vol_a", {7, 1, 9, 81}}, {"pw_a", {9, 1, 10, 82}}, {"freq_b", {11, 1, 11, 83}},
    {"vol_b", {13, 1, 12, 84}}, {"pw_b", {15, 1, 13, 85}}, {"fine_b", {17, 1, 14, 86}}, {"cutoff", {19, 1, 15, 87}},
    {"res", {21, 1, 16, 88}}, {"fenv_amt", {23, 1, 17, 89}}, {"fe_r", {25, 1, 18, 90}}, {"fe_s", {27, 1, 19, 91}},
    {"fe_d", {29, 1, 20, 92}}, {"fe_a", {31, 1, 21, 93}}, {"ae_r", {33, 1, 22, 94}}, {"ae_s", {35, 1, 23, 95}},
    {"ae_d", {37, 1, 24, 100}}, {"ae_a", {39, 1, 25, 101}}, {"pm_env", {41, 1, 26, 102}}, {"pm_oscb", {43, 1, 27, 103}},
    {"lfo_freq", {45, 1, 28, 104}}, {"lfo_amt", {47, 1, 29, 105}}, {"glide", {49, 1, 30, 106}}, {"amp_vel", {51, 1, 31, 107}},
    {"filt_vel", {53, 1, 32, 108}}, {"mod_delay", {76, 1, 33, 109}}, {"vib_speed", {78, 1, -1, -1}}, {"vib_amt", {80, 1, -1, -1}},
    {"detune", {82, 1, -1, -1}}, {"noise", {142, 1, 37, -2}}, {"vca_at", {144, 1, -1, -1}}, {"vcf_at", {146, 1, -1, -1}},
    {"lfo_at", {166, 1, -1, -1}}, {"pitch_range", {171, 1, -1, -1}},
    {"saw_a", {55, 0, -1, -1}}, {"tri_a", {56, 0, -1, -1}}, {"sqr_a", {57, 0, -1, -1}}, {"saw_b", {58, 0, -1, -1}},
    {"tri_b", {59, 0, -1, -1}}, {"sqr_b", {60, 0, -1, -1}}, {"sync", {61, 0, -1, -1}}, {"pm_freq", {62, 0, -1, -1}},
    {"pm_filt", {63, 0, -1, -1}}, {"lfo_shape", {64, 0, -1, -1}}, {"lfo_range", {65, 0, -1, -1}}, {"lfo_target", {66, 0, -1, -1}},
    {"keytrk", {67, 0, -1, -1}}, {"fe_shape", {68, 0, -1, -1}}, {"fe_speed", {69, 0, -1, -1}}, {"ae_shape", {70, 0, -1, -1}},
    {"unison", {71, 0, -1, -1}}, {"bend_target", {72, 0, -1, -1}}, {"freqmode_a", {74, 0, -1, -1}}, {"freqmode_b", {75, 0, -1, -1}},
    {"mw_target", {84, 0, -1, -1}}, {"ae_speed", {148, 0, -1, -1}}, {"glide_mode", {170, 0, -1, -1}}};
static const std::map<std::string, std::pair<int, int>> STEP_CC = {   // stepped parameters with a panel CC: state = val * N / 128
    {"saw_a", {48, 2}}, {"tri_a", {49, 2}}, {"sqr_a", {50, 2}}, {"saw_b", {51, 2}}, {"tri_b", {52, 2}}, {"sqr_b", {53, 2}},
    {"sync", {54, 2}}, {"pm_freq", {55, 2}}, {"pm_filt", {56, 2}}, {"unison", {65, 2}}, {"keytrk", {60, 3}}, {"lfo_shape", {57, 6}}};
static const uint8_t HDR[] = {0xF0, 0x00, 0x20, 0x32, 0x00, 0x01, 0x24, 0x00};

// ---------------------------------------------------------------- SysEx helpers
static std::vector<uint8_t> enc78(const std::vector<uint8_t>& d) {
    std::vector<uint8_t> o;
    for (size_t i = 0; i < d.size(); i += 7) {
        uint8_t msb = 0; size_t n = std::min<size_t>(7, d.size() - i);
        for (size_t j = 0; j < n; ++j) msb |= ((d[i + j] >> 7) & 1) << j;
        o.push_back(msb); for (size_t j = 0; j < n; ++j) o.push_back(d[i + j] & 0x7F);
    }
    return o;
}
static std::vector<uint8_t> dec87(const uint8_t* d, size_t n) {
    std::vector<uint8_t> o;
    for (size_t i = 0; i < n; i += 8) { uint8_t msb = d[i]; for (size_t j = 1; j < 8 && i + j < n; ++j) o.push_back(d[i + j] | (((msb >> (j - 1)) & 1) << 7)); }
    return o;
}
// Patches from older editors/firmware (format 0x6E, or 0x6F written short): 16-byte name, LFO AT, the unit's defaults
static std::vector<uint8_t> normalize_patch(std::vector<uint8_t> d) {
    if (d.size() < 150) return d;
    bool ok = d.size() >= 173 && d[4] == 0x6F;
    for (int k = 150; ok && k < 166; ++k) if (d[k] != 0 && (d[k] < 32 || d[k] > 126)) ok = false;
    if (ok) { d.resize(173); return d; }
    size_t k = 150; while (k < d.size() && k < 166 && d[k] >= 32 && d[k] < 127) ++k;
    size_t j = (k < d.size() && d[k] == 0) ? k + 1 : k;
    int lfo_at = 0; if (j + 1 < d.size() && d[j] < 128 && d[j + 1] < 128 && (d[j] | d[j + 1]) && j + 1 < 168) lfo_at = d[j] | (d[j + 1] << 8);
    std::vector<uint8_t> o(d.begin(), d.begin() + 150);
    for (size_t q = 150; q < 166; ++q) o.push_back(q < k ? d[q] : 0);
    // a short 0x6F dump is zero-filled like the unit does; an old 0x6E dump gets the unit's defaults (reference C4, range 12)
    if (d[4] == 0x6F) { o.push_back(lfo_at & 255); o.push_back(lfo_at >> 8); for (int q = 0; q < 5; ++q) o.push_back(0); }
    else { o.push_back(lfo_at & 255); o.push_back(lfo_at >> 8); o.push_back(0); o.push_back(3); o.push_back(0); o.push_back(0x00); o.push_back(0x60); }
    // the unit plays an old-format LFO amount 2048 lower than the same value in 0x6F (measured 25.11)
    if (d[4] == 0x6E) { int la = o[47] | (o[48] << 8); la = std::max(0, la - 2048); o[47] = la & 255; o[48] = la >> 8; }
    o[4] = 0x6F; return o;
}
static std::string patch_name(const std::vector<uint8_t>& p) {
    std::string nm; for (int i = 150; i < 166 && i < (int)p.size() && p[i] >= 32 && p[i] < 127; ++i) nm += (char)p[i];
    while (!nm.empty() && nm.back() == ' ') nm.pop_back(); return nm;
}
static void set_name(std::vector<uint8_t>& p, NSString* name) {
    p.resize(173, 0); const char* c = [[name uppercaseString] cStringUsingEncoding:NSASCIIStringEncoding] ?: "";
    int L = (int)strlen(c); for (int i = 0; i < 16; ++i) p[150 + i] = i < L ? ((c[i] >= 32 && c[i] < 127) ? (uint8_t)c[i] : '?') : 0;
}
static NSString* js_str(const std::string& s) {   // a quoted JS string literal
    NSString* q = [NSString stringWithUTF8String:s.c_str()] ?: @"";
    q = [[q stringByReplacingOccurrencesOfString:@"\\" withString:@""] stringByReplacingOccurrencesOfString:@"'" withString:@"’"];
    return [NSString stringWithFormat:@"'%@'", q];
}

// ---------------------------------------------------------------- FX block in .syx: F0 7D 'P' '8' 'F' 'X' 01 lo hi <enc78 20 x 16-bit> F7
// 0x7D is the non-commercial manufacturer ID: the PRO-800 ignores it, the file still loads on the unit
static const uint8_t FXHDR[] = {0xF0, 0x7D, 'P', '8', 'F', 'X', 0x01};
static FxVals fx_default() { FxVals v; std::copy(p800fx::FX_DEFAULT, p800fx::FX_DEFAULT + p800fx::FX_N, v.begin()); return v; }
static std::vector<uint8_t> fx_msg(int n, const FxVals& v) {
    std::vector<uint8_t> d; for (auto x : v) { d.push_back(x & 255); d.push_back(x >> 8); }
    std::vector<uint8_t> m(FXHDR, FXHDR + 7); m.push_back(n & 127); m.push_back((n >> 7) & 127);
    auto e = enc78(d); m.insert(m.end(), e.begin(), e.end()); m.push_back(0xF7); return m;
}
static bool fx_parse(const uint8_t* m, size_t len, FxVals& v) {
    if (len < 12 || !std::equal(FXHDR, FXHDR + 7, m)) return false;
    auto d = dec87(m + 9, len - 10); if (d.size() < 2 * p800fx::FX_N) return false;
    for (int k = 0; k < p800fx::FX_N; ++k) v[k] = d[2 * k] | (d[2 * k + 1] << 8); return true;
}
static std::vector<uint8_t> patch_msg(int n, const std::vector<uint8_t>& p) {
    std::vector<uint8_t> m(HDR, HDR + 8); m.push_back(0x78); m.push_back(n & 127); m.push_back((n >> 7) & 127);
    auto e = enc78(p); m.insert(m.end(), e.begin(), e.end()); m.push_back(0xF7); return m;
}
static std::vector<std::vector<uint8_t>> parse_syx(NSData* data, std::vector<int>* nums, std::vector<FxVals>* fxs) {
    std::vector<std::vector<uint8_t>> out; if (!data) return out;
    const uint8_t* raw = (const uint8_t*)data.bytes; size_t N = data.length;
    for (size_t i = 0; i < N; ++i) if (raw[i] == 0xF0) { size_t j = i; while (j < N && raw[j] != 0xF7) ++j;
        FxVals fv; if (fxs && j < N && !out.empty() && fx_parse(raw + i, j - i + 1, fv)) { (*fxs)[out.size() - 1] = fv; i = j; continue; }
        if (j < N && j - i > 160 && std::equal(HDR, HDR + 8, raw + i) && raw[i + 8] == 0x78 && !(raw[i + 9] == 0x7E && raw[i + 10] == 0x03)) {
            if (nums) nums->push_back(raw[i + 9] | (raw[i + 10] << 7));
            out.push_back(normalize_patch(dec87(raw + i + 11, j - i - 11))); if (fxs) fxs->push_back(fx_default()); }
        i = j; }
    return out;
}
// .fxp / .fxb: the VST preset / bank container with an opaque chunk, plugin ID 'PrO8'
// program chunk: 'P8PS' 01 + 173 patch bytes + 40 FX bytes; bank chunk: 'P8BK' 01 count + count x (slot, 173 + 40)
static void be32(std::vector<uint8_t>& o, uint32_t v) { o.push_back(v >> 24); o.push_back(v >> 16); o.push_back(v >> 8); o.push_back(v); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put_prog(std::vector<uint8_t>& c, const std::vector<uint8_t>& p, const FxVals& f) {
    auto q = p; q.resize(173, 0); c.insert(c.end(), q.begin(), q.end()); for (auto x : f) { c.push_back(x & 255); c.push_back(x >> 8); }
}
static void finish_ccnk(std::vector<uint8_t>& o) { uint32_t sz = (uint32_t)o.size() - 8; o[4] = sz >> 24; o[5] = sz >> 16; o[6] = sz >> 8; o[7] = sz; }
static NSData* make_fxp(const std::vector<uint8_t>& p, const FxVals& f) {
    std::vector<uint8_t> c = {'P', '8', 'P', 'S', 1}; put_prog(c, p, f);
    std::vector<uint8_t> o = {'C', 'c', 'n', 'K'}; be32(o, 0); o.insert(o.end(), {'F', 'P', 'C', 'h'}); be32(o, 1); o.insert(o.end(), {'P', 'r', 'O', '8'});
    be32(o, 1); be32(o, 0); char nm[28] = {0}; std::string n = patch_name(p); memcpy(nm, n.c_str(), std::min<size_t>(27, n.size()));
    o.insert(o.end(), nm, nm + 28); be32(o, (uint32_t)c.size()); o.insert(o.end(), c.begin(), c.end()); finish_ccnk(o);
    return [NSData dataWithBytes:o.data() length:o.size()];
}

// ---------------------------------------------------------------- library (shared by all instances)
// function-local statics: the AU wrapper calls library_init from a global constructor, before this file's globals exist
static std::mutex& lib_mutex() { static std::mutex m; return m; }
static std::map<int, std::vector<uint8_t>>& lib_patches() { static std::map<int, std::vector<uint8_t>> m; return m; }   // bank * 100 + slot -> patch
static std::map<int, FxVals>& lib_fx() { static std::map<int, FxVals> m; return m; }
#define libm lib_mutex()
#define bankfile lib_patches()
#define bankfx lib_fx()
static NSString* lib_dir() {
    NSString* d = [[NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES) firstObject] stringByAppendingPathComponent:@"PRO8/Banks"];
    [[NSFileManager defaultManager] createDirectoryAtPath:d withIntermediateDirectories:YES attributes:nil error:nil]; return d;
}
static NSString* bank_path(int b) { return [lib_dir() stringByAppendingPathComponent:[NSString stringWithFormat:@"%c.syx", 'A' + b]]; }
static NSData* bank_data(int b, int numbase) {
    NSMutableData* d = [NSMutableData data];
    for (int i = 0; i < 100; ++i) { auto it = bankfile.find(b * 100 + i); if (it == bankfile.end()) continue;
        auto m = patch_msg(numbase + i, it->second); [d appendBytes:m.data() length:m.size()];
        auto f = bankfx.find(b * 100 + i); if (f != bankfx.end()) { auto fm = fx_msg(numbase + i, f->second); [d appendBytes:fm.data() length:fm.size()]; } }
    return d;
}
static void save_bank(int b) { NSData* d = bank_data(b, 0); if (d.length) [d writeToFile:bank_path(b) atomically:YES]; else [[NSFileManager defaultManager] removeItemAtPath:bank_path(b) error:nil]; }
static NSData* make_fxb(int b) {
    std::vector<uint8_t> c = {'P', '8', 'B', 'K', 1, 0}; int cnt = 0;
    for (int i = 0; i < 100; ++i) { auto it = bankfile.find(b * 100 + i); if (it == bankfile.end()) continue; ++cnt; c.push_back(i);
        auto f = bankfx.find(b * 100 + i); put_prog(c, it->second, f != bankfx.end() ? f->second : fx_default()); }
    c[5] = cnt;
    std::vector<uint8_t> o = {'C', 'c', 'n', 'K'}; be32(o, 0); o.insert(o.end(), {'F', 'B', 'C', 'h'}); be32(o, 2); o.insert(o.end(), {'P', 'r', 'O', '8'});
    be32(o, 1); be32(o, cnt); for (int i = 0; i < 4; ++i) be32(o, 0); o.insert(o.end(), 112, 0);   // current program + future[124]
    be32(o, (uint32_t)c.size()); o.insert(o.end(), c.begin(), c.end()); finish_ccnk(o);
    return [NSData dataWithBytes:o.data() length:o.size()];
}
static std::vector<std::tuple<int, std::vector<uint8_t>, FxVals>> read_fx_file(NSData* d) {
    std::vector<std::tuple<int, std::vector<uint8_t>, FxVals>> out; if (!d || d.length < 60) return out;
    const uint8_t* q = (const uint8_t*)d.bytes; size_t N = d.length;
    if (memcmp(q, "CcnK", 4) || memcmp(q + 16, "PrO8", 4)) return out;
    size_t off = !memcmp(q + 8, "FPCh", 4) ? 56 : !memcmp(q + 8, "FBCh", 4) ? 156 : 0; if (!off || off + 4 > N) return out;
    uint32_t cs = rd32(q + off); const uint8_t* c = q + off + 4; if (off + 4 + cs > N || cs < 5) return out;
    const size_t PL = 173 + 2 * p800fx::FX_N;
    auto prog = [&](const uint8_t* x) { std::vector<uint8_t> p(x, x + 173); FxVals f; for (int k = 0; k < p800fx::FX_N; ++k) f[k] = x[173 + 2 * k] | (x[174 + 2 * k] << 8); return std::make_pair(p, f); };
    if (!memcmp(c, "P8PS", 4) && cs >= 5 + PL) { auto [p, f] = prog(c + 5); out.push_back({-1, p, f}); }
    if (!memcmp(c, "P8BK", 4)) { int cnt = c[5]; for (int k = 0; k < cnt && 6 + k * (PL + 1) + PL + 1 <= cs; ++k) { const uint8_t* x = c + 6 + k * (PL + 1); auto [p, f] = prog(x + 1); out.push_back({x[0], p, f}); } }
    return out;
}
static int import_bank(NSData* data, int b) {
    std::vector<int> nums; std::vector<FxVals> fxs; auto ps = parse_syx(data, &nums, &fxs); if (ps.empty()) return 0;
    for (int i = 0; i < 100; ++i) { bankfile.erase(b * 100 + i); bankfx.erase(b * 100 + i); }
    std::set<int> u; for (int n : nums) u.insert(n % 100); bool byNum = u.size() == nums.size();
    for (size_t k = 0; k < ps.size() && k < 100; ++k) { auto p = ps[k]; p.resize(173, 0); int n = b * 100 + (byNum ? nums[k] % 100 : (int)k); bankfile[n] = p; bankfx[n] = fxs[k]; }
    save_bank(b); return (int)std::min<size_t>(ps.size(), 100);
}
void library_init(NSString* seed_dir) {
    std::lock_guard<std::mutex> l(libm);
    static bool done = false; if (done) return; done = true;
    NSFileManager* fm = [NSFileManager defaultManager]; bool any = false;
    for (int b = 0; b < 26; ++b) if ([fm fileExistsAtPath:bank_path(b)]) any = true;
    if (!any && seed_dir)   // first start: the seed banks shipped with the app / plugin
        for (int b = 0; b < 26; ++b) { NSString* s = [seed_dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%c.syx", 'A' + b]];
            if ([fm fileExistsAtPath:s]) [fm copyItemAtPath:s toPath:bank_path(b) error:nil]; }
    for (int b = 0; b < 26; ++b) if ([fm fileExistsAtPath:bank_path(b)]) {
        std::vector<int> nums; std::vector<FxVals> fxs; auto ps = parse_syx([NSData dataWithContentsOfFile:bank_path(b)], &nums, &fxs);
        for (size_t k = 0; k < ps.size(); ++k) { auto p = ps[k]; p.resize(173, 0); bankfile[b * 100 + nums[k] % 100] = p; bankfx[b * 100 + nums[k] % 100] = fxs[k]; } }
}
static NSString* pick_open(NSArray* types) {
    NSOpenPanel* o = [NSOpenPanel openPanel]; o.allowedFileTypes = types; o.allowsMultipleSelection = NO;
    return [o runModal] == NSModalResponseOK ? o.URL.path : nil;
}
static NSString* pick_save(NSString* name, NSArray* types) {
    NSSavePanel* s = [NSSavePanel savePanel]; s.allowedFileTypes = types; s.nameFieldStringValue = name; s.allowsOtherFileTypes = NO;
    return [s runModal] == NSModalResponseOK ? s.URL.path : nil;
}
static NSUserDefaults* prefs() { static NSUserDefaults* u = [[NSUserDefaults alloc] initWithSuiteName:@"com.mtzvfilm.pro8"]; return u; }

// ---------------------------------------------------------------- instance
Core::Core(bool plug) : plugin(plug), eng(48000), fx(48000) {
    patch.assign(PRO8_BASIC_PATCH, PRO8_BASIC_PATCH + 173); fxcur = fx_default();
    eng.set_calibrated_output(true);
    NSUserDefaults* u = prefs();   // new instances start from the last settings used
    if ([u objectForKey:@"vintage"]) st.vint = [u doubleForKey:@"vintage"];
    if ([u objectForKey:@"spread"]) st.spread = [u doubleForKey:@"spread"];
    if ([u objectForKey:@"tune"]) st.tune = [u doubleForKey:@"tune"];
    if ([u objectForKey:@"voices"]) st.voices = (int)[u integerForKey:@"voices"];
    if ([u objectForKey:@"univoices"]) st.uni = (int)[u integerForKey:@"univoices"];
    st.polyuni = [u boolForKey:@"polyuni"]; st.mono = [u boolForKey:@"mono"];
    daw_sync = plugin ? ([u objectForKey:@"hostsync"] ? [u boolForKey:@"hostsync"] : true) : [u boolForKey:@"dawsync"];
    apply_settings(); apply_engine(); apply_fx();
    std::lock_guard<std::mutex> l(libm); if (bankfile.count(0)) { patch = bankfile[0]; prog = 0; if (bankfx.count(0)) fxcur = bankfx[0]; }
    apply_engine(); apply_fx();
}
void Core::apply_engine(bool knob) { p800::Patch p; p.parse(patch.data(), patch.size()); std::lock_guard<std::mutex> l(m); eng.set_patch(p, knob); }
void Core::apply_fx() { std::lock_guard<std::mutex> l(m); for (int k = 0; k < p800fx::FX_N; ++k) fx.set(k, fxcur[k]); }
void Core::apply_settings() {
    // VINTAGE slider 0..1: 0..0.5 brings in the measured traits of the eight voices (0.5 = the instrument as measured);
    // above 0.5 the traits grow by up to 50 % plus random per-note detune / cutoff scatter / stronger drift.
    // OSC SPREAD 0..10: up to 4 = x the measured spread, above 4 it grows faster (10 -> ~21x).
    double e = std::max(0.0, 2 * st.vint - 1), sp = st.spread <= 4 ? st.spread : 4.0 * std::pow(st.spread / 4.0, 1.8);
    std::lock_guard<std::mutex> l(m);
    eng.set_character(std::min(1.0, 2 * st.vint) + 0.5 * e, e, sp); eng.set_master_tune(1200.0 * std::log2(st.tune / 440.0));
    eng.set_voices(st.voices); eng.set_unison_voices(st.uni); eng.set_poly_unison(st.polyuni); eng.set_mono(st.mono);
    eng.set_arp(arp.mode, arp.oct, arp.div); eng.set_lfo_sync(arp.lfos != 0, arp.lfod); eng.set_tempo(bpm); fx.set_tempo(bpm);
}
void Core::set_sample_rate(double sr) { std::lock_guard<std::mutex> l(m); eng.set_sample_rate(sr); fx.set_sample_rate(sr); }
void Core::render(float* L, float* R, int n) {
    if ((int)mono.size() < n) mono.resize(n);
    std::lock_guard<std::mutex> l(m);
    eng.render(mono.data(), n); fx.process(mono.data(), L, R, n);
    for (int i = 0; i < n; ++i) { L[i] = std::clamp((float)(L[i] * gain), -1.0f, 1.0f); R[i] = std::clamp((float)(R[i] * gain), -1.0f, 1.0f); }
}
void Core::note(int n, int v, bool on) { std::lock_guard<std::mutex> l(m); if (on) eng.note_on(n, v); else eng.note_off(n); }
bool Core::panel_cc(int cc, int val) {   // CCs of the PRO-800 MIDI map move the patch like its panel
    bool hit = false;
    for (auto& [k, f] : FIELDS) if (f.cc == cc) { int v = (int)std::lround(val * 65535.0 / 127.0); patch[f.off] = v & 255; patch[f.off + 1] = v >> 8; hit = true; }
    for (auto& [k, sc] : STEP_CC) if (sc.first == cc) { patch[FIELDS.at(k).off] = (uint8_t)std::min(sc.second - 1, val * sc.second / 128); hit = true; }
    if (cc == 74 || cc == 75 || cc == 76) { int bit = cc == 74 ? 1 : cc == 75 ? 2 : 4; uint8_t& t = patch[FIELDS.at("lfo_target").off]; t = val >= 64 ? (t | bit) : (t & ~bit); hit = true; }
    if (!hit) return false;
    apply_engine(true);
    static uint64_t last = 0; uint64_t now = mach_absolute_time(); if (now - last > 30000000ULL) { last = now; push_patch(); }   // ~30 ms UI refresh
    return true;
}
void Core::midi(uint8_t st_, uint8_t d1, uint8_t d2) {
    uint8_t s = st_ & 0xF0;
    if (s == 0x90 || s == 0x80) { note(d1, d2, s == 0x90 && d2 > 0); return; }
    if (s == 0xB0 && panel_cc(d1, d2)) return;
    std::lock_guard<std::mutex> l(m); eng.midi(s, d1, d2);
}
void Core::midi_start() { if (!daw_sync) return; clk_n = 0; clk_last = 0; std::lock_guard<std::mutex> l(m); eng.transport_start(); }
void Core::midi_clock(uint64_t ts) {   // tempo = mean tick interval over 2 beats; a step (3 ticks > 4 % off) restarts the window
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    double t = (double)(ts ? ts : mach_absolute_time()) * tb.numer / tb.denom * 1e-9;
    if (clk_n == 0) { clk_wn = 0; clk_wi = 0; clk_jump = 0; }
    if (clk_last > 0) {
        double d = t - clk_last;
        if (d > 0.25) clk_n = 0;
        else if (d > 0) {
            if (clk_wn >= 12 && std::fabs(d - clk_avg) > 0.04 * clk_avg) { if (++clk_jump >= 3) { clk_wn = 0; clk_wi = 0; clk_jump = 0; } } else clk_jump = 0;
            clk_win[clk_wi] = d; clk_wi = (clk_wi + 1) % 48; clk_wn = std::min(clk_wn + 1, 48); ++clk_n;
            double sum = 0; for (int k = 0; k < clk_wn; ++k) sum += clk_win[k]; clk_avg = sum / clk_wn;
            if (clk_wn >= 12 && clk_jump == 0 && daw_sync) {
                double b = 60.0 / (24.0 * clk_avg);
                if (std::fabs(b - bpm) > 0.02) { bpm = b; std::lock_guard<std::mutex> l(m); eng.set_tempo(b); fx.set_tempo(b); }
                if (std::fabs(b - clk_shown) > 0.6) { clk_shown = std::round(b); if (js) js([NSString stringWithFormat:@"setBpm(%.0f, 1)", clk_shown]); }
            }
        }
    }
    clk_last = t;
}
void Core::set_host_tempo(double b, bool started) {   // plugin: called from the audio thread with the host transport
    if (!daw_sync) return;   // SYNC TO DAW off: the panel's own BPM
    std::lock_guard<std::mutex> l(m);
    if (b > 0 && std::fabs(b - bpm) > 0.001) { bpm = b; eng.set_tempo(b); fx.set_tempo(b);
        if (std::fabs(b - clk_shown) > 0.6 && js) { clk_shown = std::round(b); js([NSString stringWithFormat:@"setBpm(%.0f, 1)", clk_shown]); } }
    if (started) eng.transport_start();
}
void Core::load_prog(int n) {
    std::lock_guard<std::mutex> l(libm);
    auto it = bankfile.find(n); if (it == bankfile.end()) return;
    patch = it->second; patch.resize(173, 0); prog = n;
    auto f = bankfx.find(n); fxcur = f != bankfx.end() ? f->second : fx_default();
    apply_engine(); apply_fx(); push_patch(); push_fx();
}
void Core::push_patch() {
    if (!js) return;
    NSMutableString* s = [NSMutableString stringWithString:@"setPatch({"];
    for (auto& [k, f] : FIELDS) [s appendFormat:@"%s:%d,", k.c_str(), f.cont ? (patch[f.off] | (patch[f.off + 1] << 8)) : patch[f.off]];
    [s appendFormat:@"}, %@, %d)", js_str(patch_name(patch)), prog]; js(s);
}
void Core::push_fx() {
    if (!js) return;
    NSMutableString* q = [NSMutableString stringWithString:@"setFx({"];
    for (int k = 0; k < p800fx::FX_N; ++k) [q appendFormat:@"%s:%d,", p800fx::FX_KEYS[k], fxcur[k]];
    [q appendString:@"})"]; js(q);
}
void Core::push_settings() {
    if (!js) return;
    js([NSString stringWithFormat:@"setSettings({vint:%d, spread:%d, poly:%d, uni:%d, polyuni:%d, tune:%d, mono:%d, dawsync:%d})",
        (int)lround(100 * st.vint), (int)lround(100 * st.spread), st.voices, st.uni, (int)st.polyuni, (int)lround(10 * st.tune), (int)st.mono, (int)daw_sync]);
    js([NSString stringWithFormat:@"setArp({mode:%d, oct:%d, div:%d, lfos:%d, lfod:%d, bpm:%d})", arp.mode, arp.oct, arp.div, arp.lfos, arp.lfod, (int)lround(ui_bpm)]);
    if (daw_sync) js(plugin ? [NSString stringWithFormat:@"setBpm(%.0f, 1)", bpm] : @"setBpm(null, 1)"); else js([NSString stringWithFormat:@"setBpm(%.0f, 0)", ui_bpm]);
}
void Core::send_list() {
    if (!js) return;
    NSMutableString* s = [NSMutableString stringWithString:@"setPresets(["];
    { std::lock_guard<std::mutex> l(libm); for (auto& [n, d] : bankfile) [s appendFormat:@"[%d,%@],", n, js_str(patch_name(d))]; }
    [s appendString:@"])"]; js(s);
}
void Core::ui_ready() {
    if (plugin && js) js(@"setHost('plugin')");
    send_list(); push_settings(); push_patch(); push_fx();
}
static void save_pref(id v, NSString* k) { [prefs() setObject:v forKey:k]; }
void Core::handle(NSDictionary* msg) {
    NSString* t = msg[@"t"];
    if ([t isEqualToString:@"param"]) {
        std::string k = [msg[@"k"] UTF8String]; int v = [msg[@"v"] intValue];
        if (k == "__vol") { gain = std::pow(10.0, (v / 65535.0 * 36.0 - 24.0) / 20.0); return; }   // 43690 = 0 dB
        if (k == "__mono") { st.mono = v != 0; save_pref(@(st.mono), @"mono"); apply_settings(); return; }
        if (k == "__polyuni") { st.polyuni = v != 0; save_pref(@(st.polyuni), @"polyuni"); apply_settings(); return; }
        auto it = FIELDS.find(k); if (it == FIELDS.end()) return; const Field& f = it->second;
        if (f.cont) { patch[f.off] = v & 255; patch[f.off + 1] = v >> 8; } else patch[f.off] = (uint8_t)v;
        if (k == "unison" && v) { bool allff = true; for (int i = 87; i < 94; ++i) allff &= patch[i] == 0xFF; if (allff) for (int i = 87; i < 94; ++i) patch[i] = 0; }
        apply_engine(true);
    } else if ([t isEqualToString:@"note"]) {
        note([msg[@"n"] intValue], [msg[@"v"] intValue], [msg[@"on"] intValue]);
    } else if ([t isEqualToString:@"prog"]) {   // arrows: filled slots only; +-100 = next / previous bank with presets
        int target = prog, dd = [msg[@"d"] intValue];
        { std::lock_guard<std::mutex> l(libm);
          if (!bankfile.empty()) {
            if (std::abs(dd) >= 100) {
                for (int k = 1; k <= 26; ++k) { int b = ((target / 100 + (dd > 0 ? k : -k)) % 26 + 26) % 26;
                    auto it = bankfile.lower_bound(b * 100); if (it != bankfile.end() && it->first < b * 100 + 100) { target = it->first; break; } }
            } else { auto it = bankfile.find(target); if (it == bankfile.end()) it = bankfile.lower_bound(target); if (it == bankfile.end()) it = bankfile.begin();
                else if (dd > 0) { ++it; if (it == bankfile.end()) it = bankfile.begin(); } else { if (it == bankfile.begin()) it = bankfile.end(); --it; }
                target = it->first; } } }
        if (js) js([NSString stringWithFormat:@"setProg(%d)", target]); load_prog(target);
    } else if ([t isEqualToString:@"progset"]) {
        int n = [msg[@"n"] intValue]; if (js) js([NSString stringWithFormat:@"setProg(%d)", n]); load_prog(n);
    } else if ([t isEqualToString:@"presets"]) {
        send_list();
    } else if ([t isEqualToString:@"init"] || [t isEqualToString:@"new"]) {
        patch.assign(PRO8_BASIC_PATCH, PRO8_BASIC_PATCH + 173); if ([t isEqualToString:@"new"]) { set_name(patch, @"INIT"); fxcur = fx_default(); apply_fx(); push_fx(); }
        apply_engine(); push_patch();
    } else if ([t isEqualToString:@"save"]) {
        int n = [msg[@"n"] intValue]; if (n < 0 || n >= 2600) return;
        set_name(patch, msg[@"name"] ?: @""); prog = n;
        { std::lock_guard<std::mutex> l(libm); bankfile[n] = patch; bankfx[n] = fxcur; save_bank(n / 100); }
        send_list(); push_patch();
    } else if ([t isEqualToString:@"delete"]) {
        int n = [msg[@"n"] intValue]; { std::lock_guard<std::mutex> l(libm); bankfile.erase(n); bankfx.erase(n); save_bank(n / 100); } send_list();
    } else if ([t isEqualToString:@"pimport"]) {   // .syx (first patch + its FX block) or .fxp into the edit buffer
        NSString* f = pick_open(@[@"syx", @"fxp"]); if (!f) return; NSData* d = [NSData dataWithContentsOfFile:f];
        if ([[f.pathExtension lowercaseString] isEqualToString:@"fxp"]) {
            auto r = read_fx_file(d); if (r.empty()) { if (js) js(@"toast('NOT A PRO8 PRESET')"); return; }
            patch = normalize_patch(std::get<1>(r[0])); fxcur = std::get<2>(r[0]);
        } else {
            std::vector<FxVals> fxs; auto ps = parse_syx(d, nullptr, &fxs); if (ps.empty()) { if (js) js(@"toast('NO PATCH IN FILE')"); return; }
            patch = ps[0]; fxcur = fxs[0];
        }
        patch.resize(173, 0); apply_engine(); apply_fx(); push_patch(); push_fx();
    } else if ([t isEqualToString:@"pexport"]) {
        std::string nm = patch_name(patch);
        NSString* f = pick_save([NSString stringWithFormat:@"%s.syx", nm.empty() ? "patch" : nm.c_str()], @[@"syx", @"fxp"]); if (!f) return;
        if ([[f.pathExtension lowercaseString] isEqualToString:@"fxp"]) { [make_fxp(patch, fxcur) writeToFile:f atomically:YES]; return; }
        int n = prog >= 0 && prog < 400 ? prog : 0; auto out = patch_msg(n, patch); auto fm = fx_msg(n, fxcur); out.insert(out.end(), fm.begin(), fm.end());
        [[NSData dataWithBytes:out.data() length:out.size()] writeToFile:f atomically:YES];
    } else if ([t isEqualToString:@"bankimport"]) {
        int b = [msg[@"b"] intValue]; NSString* f = pick_open(@[@"syx", @"fxb"]); if (!f) return; NSData* d = [NSData dataWithContentsOfFile:f]; int k = 0;
        { std::lock_guard<std::mutex> l(libm);
          if ([[f.pathExtension lowercaseString] isEqualToString:@"fxb"]) {
            auto r = read_fx_file(d);
            if (!r.empty()) { for (int i = 0; i < 100; ++i) { bankfile.erase(b * 100 + i); bankfx.erase(b * 100 + i); }
                for (auto& [slot, p, fv] : r) { int n = b * 100 + (slot < 0 ? k : slot % 100); auto q = normalize_patch(p); q.resize(173, 0); bankfile[n] = q; bankfx[n] = fv; ++k; }
                save_bank(b); }
          } else k = import_bank(d, b); }
        send_list(); if (js) js([NSString stringWithFormat:@"toast('BANK %c: %d PATCHES')", 'A' + b, k]);
    } else if ([t isEqualToString:@"bankexport"]) {
        int b = [msg[@"b"] intValue]; NSString* f = pick_save([NSString stringWithFormat:@"PRO8 bank %c.syx", 'A' + b], @[@"syx", @"fxb"]); if (!f) return;
        std::lock_guard<std::mutex> l(libm);
        if ([[f.pathExtension lowercaseString] isEqualToString:@"fxb"]) [make_fxb(b) writeToFile:f atomically:YES];
        else [bank_data(b, b < 4 ? b * 100 : 0) writeToFile:f atomically:YES];   // banks A-D keep the unit's numbers (the file loads on the unit)
    } else if ([t isEqualToString:@"fx"]) {
        int k = [msg[@"k"] intValue], v = [msg[@"v"] intValue]; if (k < 0 || k >= p800fx::FX_N) return;
        fxcur[k] = (uint16_t)std::clamp(v, 0, 65535); std::lock_guard<std::mutex> l(m); fx.set(k, fxcur[k]);
    } else if ([t isEqualToString:@"tempo"]) {
        ui_bpm = [msg[@"v"] doubleValue]; if (daw_sync) return;
        bpm = ui_bpm; std::lock_guard<std::mutex> l(m); eng.set_tempo(bpm); fx.set_tempo(bpm);
    } else if ([t isEqualToString:@"dawsync"]) {
        daw_sync = [msg[@"on"] intValue] != 0; clk_n = 0; clk_shown = -1; save_pref(@(daw_sync), plugin ? @"hostsync" : @"dawsync");
        if (!daw_sync) { bpm = ui_bpm; { std::lock_guard<std::mutex> l(m); eng.set_tempo(bpm); fx.set_tempo(bpm); } if (js) js([NSString stringWithFormat:@"setBpm(%.0f, 0)", ui_bpm]); }
        else if (js) js(@"setBpm(null, 1)");
    } else if ([t isEqualToString:@"arp"]) {
        arp.mode = [msg[@"mode"] intValue]; arp.oct = [msg[@"oct"] intValue]; arp.div = [msg[@"div"] intValue];
        std::lock_guard<std::mutex> l(m); eng.set_arp(arp.mode, arp.oct, arp.div);
    } else if ([t isEqualToString:@"lfosync"]) {
        arp.lfos = [msg[@"on"] intValue]; arp.lfod = [msg[@"div"] intValue]; std::lock_guard<std::mutex> l(m); eng.set_lfo_sync(arp.lfos != 0, arp.lfod);
    } else if ([t isEqualToString:@"spread"]) { st.spread = [msg[@"v"] doubleValue]; save_pref(@(st.spread), @"spread"); apply_settings();
    } else if ([t isEqualToString:@"vintage"]) { st.vint = [msg[@"v"] doubleValue]; save_pref(@(st.vint), @"vintage"); apply_settings();
    } else if ([t isEqualToString:@"tune"]) { st.tune = [msg[@"hz"] doubleValue]; save_pref(@(st.tune), @"tune"); apply_settings();
    } else if ([t isEqualToString:@"voices"]) { st.voices = [msg[@"n"] intValue]; save_pref(@(st.voices), @"voices"); apply_settings();
    } else if ([t isEqualToString:@"univoices"]) { st.uni = [msg[@"n"] intValue]; save_pref(@(st.uni), @"univoices"); apply_settings();
    } else if ([t isEqualToString:@"panic"]) { std::lock_guard<std::mutex> l(m); eng.all_notes_off();
    } else if ([t isEqualToString:@"ready"]) { ui_ready(); }
}
// ---------------------------------------------------------------- plugin state (JSON)
NSData* Core::get_state() {
    NSMutableArray* f = [NSMutableArray array]; for (auto x : fxcur) [f addObject:@(x)];
    NSDictionary* d = @{@"v": @1, @"prog": @(prog), @"patch": [[NSData dataWithBytes:patch.data() length:patch.size()] base64EncodedStringWithOptions:0], @"fx": f,
        @"vint": @(st.vint), @"spread": @(st.spread), @"tune": @(st.tune), @"voices": @(st.voices), @"uni": @(st.uni), @"polyuni": @(st.polyuni), @"mono": @(st.mono),
        @"arp": @[@(arp.mode), @(arp.oct), @(arp.div), @(arp.lfos), @(arp.lfod)], @"gain": @(gain), @"sync": @(daw_sync), @"bpm": @(ui_bpm)};
    return [NSJSONSerialization dataWithJSONObject:d options:0 error:nil];
}
bool Core::set_state(NSData* data) {
    NSDictionary* d = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil; if (![d isKindOfClass:[NSDictionary class]]) return false;
    NSData* p = [[NSData alloc] initWithBase64EncodedString:d[@"patch"] ?: @"" options:0]; if (p.length < 150) return false;
    patch.assign((const uint8_t*)p.bytes, (const uint8_t*)p.bytes + p.length); patch = normalize_patch(patch); patch.resize(173, 0);
    prog = [d[@"prog"] intValue]; NSArray* f = d[@"fx"]; for (int k = 0; k < p800fx::FX_N && k < (int)f.count; ++k) fxcur[k] = [f[k] intValue];
    st.vint = [d[@"vint"] doubleValue]; st.spread = [d[@"spread"] doubleValue]; st.tune = [d[@"tune"] doubleValue] ?: 440;
    st.voices = [d[@"voices"] intValue] ?: 8; st.uni = [d[@"uni"] intValue] ?: 8; st.polyuni = [d[@"polyuni"] boolValue]; st.mono = [d[@"mono"] boolValue];
    NSArray* a = d[@"arp"]; if (a.count == 5) { arp.mode = [a[0] intValue]; arp.oct = [a[1] intValue]; arp.div = [a[2] intValue]; arp.lfos = [a[3] intValue]; arp.lfod = [a[4] intValue]; }
    if (d[@"gain"]) gain = [d[@"gain"] doubleValue];
    if (d[@"sync"]) daw_sync = [d[@"sync"] boolValue];
    if (d[@"bpm"]) { ui_bpm = [d[@"bpm"] doubleValue]; if (!daw_sync) bpm = ui_bpm; }
    apply_engine(); apply_fx(); apply_settings(); ui_ready(); return true;
}

}  // namespace pro8
