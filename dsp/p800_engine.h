// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 mtzvfilm. PRO8 - https://github.com/mtzvfilm/PRO8
// PRO-800 behavioural DSP engine (portable C++17, no dependencies). Model spec: docs/MODEL.md
#pragma once
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <random>
#include <vector>
#include "p800_patch.h"

namespace p800 {

constexpr int NV = 64;          // voice slots; nvoices of them play (8 = the unit; 16/32/64 repeat the measured 8 voices' traits)
constexpr int NV_HW = 8;

constexpr double ATT_START_LV = -0.045;   // = ATT_START in p800_tables.h

struct Env {
    enum Stage { Idle, Attack, Decay, Sustain, Release } st = Idle;
    double lv = 0, start = ATT_START_LV;   // VCA: measured start offset; filter env: 0
    double under = -1;                      // exp fall: target below the end level (<0: global VCA value)
    void gate_on(bool from_zero) { if (from_zero || lv <= 0) lv = start; st = Attack; }
    void gate_off() { if (st != Idle) st = Release; }
    // T* = full-scale segment times (s); exp: exponential segments
    double tick(double dt, double Ta, double Td, double S, double Tr, bool exp);
};

struct Osc {
    double ph = 0;          // 0..1, saw = 1-2ph (jumps up at wrap), recorded polarity
    double out = 0;
};

// exact memo: f(x) is recomputed only when x changes (bit-identical results, saves the transcendental calls on steady values)
struct Memo { double in = std::nan(""), out = 0; template <class F> double operator()(double x, F f) { if (x != in) { in = x; out = f(x); } return out; } };
struct Memo2 { double a = std::nan(""), b = 0, out = 0; template <class F> double operator()(double x, double y, F f) { if (x != a || y != b) { a = x; b = y; out = f(x, y); } return out; } };
struct Voice {
    int note = -1, vel = 0; bool gate = false; uint32_t age = 0;
    double pitch = 0, pitch_target = 0, glide_rate = 0;   // in semitones (MIDI note units)
    Osc a, b;
    Env aenv, fenv;
    double t_on = 0;                                    // time since note on (mod delay)
    double s[8] = {0, 0, 0, 0, 0, 0, 0, 0}, sfb = 0, spost = 0;   // filter states (N poles, feedback pole, post pole)
    double steal = 0;                                   // >0: fast fade before a stolen voice restarts
    int pend_note = -1, pend_vel = 0;
    double interval = 0;                                // unison chord interval (semitones)
    int uslot = -1; double pend_iv = 0;                 // unison slot (detune fan position), interval of a stolen voice's next note
    double rja = 0, rjb = 0, rjf = 1;
    Memo m_inca, m_incb, m_hfa, m_hfb, m_gl, m_gq, m_ks, m_vc; Memo2 m_warp, m_vint;
    Memo mc_ta, mc_td, mc_tr, mc_fta, mc_ftd, mc_ftr, mc_sus, mc_md, mc_lpp, mc_fca, mc_fcb, mc_ndb, mc_cfl, mc_rk, mc_tr0;   // patch-derived (exact memos)
    uint64_t rs = 1;                                    // own noise generator (xorshift64*): voices share no state -> render on several cores
    double rnd() { rs ^= rs >> 12; rs ^= rs << 25; rs ^= rs >> 27; return (double)((rs * 2685821657736338717ULL) >> 11) * (2.0 / 9007199254740992.0) - 1.0; }                   // VINTAGE > 50 %: random per-note detune of osc A/B (cents), cutoff factor
    double nz = 0;                                      // noise source low-pass state
    double hfa = 0, hfb = 0;                            // VCO high-pitch rolloff states
    double sync_res = 0;                                // decaying remainder of a sync reset jump
    double a_prev = 0;                                  // osc A one-sample delay (band-limited sync reset)
    double vca_sm = 0;                                  // smoothed VCA control
    double cv_a = 0, cv_b = 0, cv_f = 0, cv_fs = 0;     // grey-box CV path: S&H RC (723 Hz) on pitch A/B and filter CV, FSLEW state
    bool cv_ok = false;
    double mx_a = 0, mx_b = 0, cv_amp = 0;
    double drift_a = 0, drift_b = 0;
    double cv_as = 0;                                   // AMP CV slew (RC bridged by diodes)                    // slow VCO drift (cents)                          // mixer: 47 uF coupling states (one per VCO)                                 // CV states initialised (they persist between notes, like the S&H)
    double rbp = 0, rlp = 0;                            // VCA CV ring (band-pass) states
    double xprev = 0;                                   // filter input of the previous sample (2x oversampling)
    double sync_hold = 0;                               // A held at its start after a sync reset (s)
};

class Engine {
public:
    explicit Engine(double sr = 48000.0);
    ~Engine();
    void set_sample_rate(double sr);
    // smooth = a knob move: continuous values glide (~10 ms, like turning a pot), switches change at once; else (program
    // change) everything is instant
    void set_patch(const Patch& p, bool smooth = false) {
        if (!smooth) { P = p; Pt = p; glide_p = false; }
        else { Patch keep = P; P = p; double* d[64]; int n = 0; keep.each_cont([&](double& x) { d[n++] = &x; }); n = 0;
               P.each_cont([&](double& x) { x = *d[n++]; }); Pt = p; glide_p = true; }
        change_arp_mode(arp_ui_mode >= 0 ? arp_ui_mode : p.arp);
    }
    const Patch& patch() const { return P; }
    void set_voice_mask(uint8_t m) { mask = m; }        // Kill Voices (bit = voice enabled, voices 1-8)
    // the three below stop the notes only when the value really changes (the host calls them with every settings update)
    void set_voices(int n) { n = std::clamp(n, 1, NV); if (n == nvoices) return; all_notes_off(); nvoices = n; rr = 0; }   // polyphony 8/16/32/64
    void set_unison_voices(int n) { uni_n = std::clamp(n, 1, NV_HW); }                   // voices stacked in unison 2/4/6/8
    void set_poly_unison(bool on) { if (on == poly_uni) return; all_notes_off(); poly_uni = on; }                     // unison stacks per note, playable polyphonically
    void set_mono(bool on) { if (on == mono) return; all_notes_off(); mono = on; mono_held.clear(); }              // MONO: one voice, last-note priority
    int alloc_voice(bool oldest);
    int pool() const { return poly_uni && P.unison ? std::min(NV, std::max(nvoices, uni_n * 8)) : nvoices; }   // POLY UNISON: 8 notes x a full stack (multi-core)
    bool on(int i) const { return i < pool() && (i >= NV_HW || (mask >> i & 1)); }
    void set_calibrated_output(bool on) { cal = on; }   // level = Scarlett dBFS of the recordings
    void set_seed(uint32_t s) { rng.seed(s); for (int i = 0; i < NV; ++i) V[i].rs = 0x9E3779B97F4A7C15ULL * (uint64_t)(i + 1) ^ s; }
    void set_threads(int n);
    // user settings, per engine (several plugin instances): character amount, extra vintage, oscillator spread, master tune (cents)
    void set_character(double vint, double vint_x, double spread) { u_vint = vint; u_vx = vint_x; u_spread = spread; }
    void set_master_tune(double cents) { u_tune = cents; }   // worker threads for the voices (0 = all on the calling thread)
    void midi(uint8_t st, uint8_t d1, uint8_t d2);
    void note_on(int n, int v);
    void note_on_poly(int n, int v);
    void note_off(int n);
    void all_notes_off();
    void render(float* out, int n);                     // mono

private:
    double tick_voice(Voice& v, int idx, double lfo, double lfo_noise, double vib, double lfo_pw);
    struct Pool; Pool* pool_ = nullptr; int nthreads = -1;   // voice workers (multi-core)
    std::vector<double> sh_lfo, sh_ln, sh_vib, sh_pw, vbuf;   // per-block shared modulation and per-voice output
    double osc_sample(Osc& o, double inc, bool saw, bool tri, bool sqr, double duty, double sqr_amp, bool& wrapped, double& wrap_frac);
    void start_voice(int i, int note, int vel, double interval, bool from_zero);
    double sr, dt, out_sr = 48000, noise_c = 1, noise_norm = 1, vca_c = 1, pw_cv_c = 1, lfo_pw_sm = 0;
    static constexpr int DEC_TAPS = 95;
    double dec_h[DEC_TAPS] = {0}, dec_buf[DEC_TAPS] = {0}; int dec_pos = 0;
    double lp1_b = 1, lp1_a = 0, lp1_x = 0, lp1_y = 0, bq_b0 = 1, bq_b1 = 0, bq_b2 = 0, bq_a1 = 0, bq_a2 = 0, bq_z1 = 0, bq_z2 = 0;
    Patch P, Pt;                                      // current (gliding) and target patch
    bool glide_p = false;
    double last_pitch = -1;                          // last note played (glide start for a voice without history)
    Voice V[NV];
    uint8_t mask = 0xFF;
    double u_vint = 0, u_vx = 0, u_spread = 1, u_tune = 0;
    int nvoices = NV_HW, uni_n = NV_HW; bool poly_uni = false, mono = false; std::vector<std::pair<int, int>> mono_held;
    bool cal = true;
    int rr = 0; uint32_t clock = 0;
    double lfo_ph = 0, lfo_rand = 0, lfo_noise = 0, vib_ph = 0;
    double bend = 0, wheel = 0, pressure = 0;
    double hp_x = 0, hp_y = 0;
    bool sustain_pedal = false;
    bool keys[128] = {};                             // keys physically down (sustain: released voices of keys that are up)
    void sustain_release();
    std::vector<std::pair<int, int>> uni_held;      // UNISON (not poly): held keys, last-note priority like MONO
    void uni_play(int n, int vel);
    // arpeggiator. Patch byte 149: 1 up, 2 down, 3 up/down, 4 up then down, 5 random, 6 as played (PRO-800 manual + m_arp).
    // User settings (not in the patch, like the OB-6): mode override, 1-3 octaves, clock division of the tempo, gate.
    int arp_mode = 0; std::vector<int> arp_held; int arp_pos = 0, arp_note = -1, arp_vel = 100; double arp_t = 0, arp_off_t = -1;
    int arp_ui_mode = -1, arp_oct = 1, arp_div = 6; double arp_gate = 0.5, bpm = 120.0; long arp_stepno = 0;
    bool lfo_sync = false; int lfo_div = 4;
    void arp_step();
    double arp_step_len() const;
public:
    void set_tempo(double b) { bpm = b < 20 ? 20 : (b > 300 ? 300 : b); }
    void transport_start() { arp_t = 0; arp_pos = 0; arp_stepno = 0; lfo_ph = 0; }   // DAW start: arpeggio and synced LFO from the downbeat
    void set_arp(int mode, int oct, int div) { arp_ui_mode = mode; arp_oct = oct < 1 ? 1 : (oct > 3 ? 3 : oct); arp_div = div < 0 ? 0 : (div > 9 ? 9 : div); change_arp_mode(mode >= 0 ? mode : P.arp); }
    void change_arp_mode(int m);   // arpeggiator on <-> off while playing: no hanging notes, the keys still down carry on
    void set_lfo_sync(bool on, int div) { lfo_sync = on; lfo_div = div < 0 ? 0 : (div > 10 ? 10 : div); }
private:
    std::mt19937 rng{12345};
    std::uniform_real_distribution<double> uni{-1.0, 1.0};
    std::normal_distribution<double> gauss{0.0, 1.0};
};

}  // namespace p800

// C API (ctypes / plugin hosts)
extern "C" {
void* p800_create(double sr);
void p800_destroy(void* e);
int p800_set_patch(void* e, const uint8_t* decoded, int n);
void p800_set_tempo(void* e, double bpm);                    // tempo for the arpeggiator / synced LFO (app BPM or DAW host)
void p800_set_arp(void* e, int mode, int oct, int div);     // mode -1 = from the patch, 0 off, 1..6; oct 1-3; div 0..9 (HALF..32nd)
void p800_set_lfo_sync(void* e, int on, int div);          // div 0..10 (4 bars .. 32nd)
int p800_set_patch_smooth(void* e, const uint8_t* decoded, int n);   // knob move: continuous values glide
void p800_set_mask(void* e, int mask);
void p800_set_voices(void* e, int n);
void p800_set_unison_voices(void* e, int n);
void p800_set_poly_unison(void* e, int on);
void p800_set_mono(void* e, int on);
void p800_midi(void* e, int st, int d1, int d2);
void p800_render(void* e, float* out, int n);
void p800_set_seed(void* e, unsigned seed);
void p800_set_threads(void* e, int n);
int p800_set_cal(const char* name, double v);
}
