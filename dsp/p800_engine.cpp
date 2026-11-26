// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 mtzvfilm. PRO8 - https://github.com/mtzvfilm/PRO8
// PRO-800 behavioural DSP engine. Every law comes from docs/RESULTS.md; constants still to be calibrated are marked CAL.
#include "p800_engine.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <algorithm>
#include <cmath>
#include <string>
#include "p800_tables.h"
#include "p800_tables_gen.h"

namespace p800 {

static constexpr double PI = 3.14159265358979323846;
// exp attack = RC charge toward 1.2 cut at 1.0 (measured t10/t50/t90 = 0.15/0.67/1.92 s at 40960 slow)
static double EXP_ATT_TARGET = 1.2, EXP_ATT_TAU = 0.66;
// exp decay/release = RC fall toward 5 % below the end level, cut there (t-6/t-20 dB ratio 3.0 as measured)
static double EXP_FALL_UNDER = 0.05, EXP_FALL_TAU = 0.46;
// CAL: constants fitted by tools/cmp_*.py against the recordings
static double SYNC_DELTA = 0.14, SYNC_TIME = 0.0, SYNC_TAU = 0.0;   // phase A restarts at after a sync reset (negative of this)
static double OUT_LP_HZ = 17000.0, OUT_LP2_HZ = 41000.0, OUT_LP2_Q = 1.2;    // output rolloff (m_osc_full saw, 13 notes); 11 kHz with the black-box filter, 13.5 kHz with the grey-box V3320
static double A_DETUNE = 0.0;   // cents, osc A vs B (calibration aid)
static double PW_CV_HZ = 14.0;   // LFO -> PW depth falls with rate like a one-pole at ~14 Hz (m_mod_full PWM grid)
static double FENV_T = 1.07;     // filter env segments 7 % longer than the VCA's (pitch-exact poly-mod env takes, A/D fast+slow)
static double RES_SCALE = 1.0;   // overall resonance loop gain (calibration)
static double TUNE_C = 0.0;      // master tune in cents (SETTINGS: A = 440 Hz -> 0)
static double VINTAGE_X = 0.0;   // 0..1: VINTAGE above 50 % - random per-note detune (sigma 3 c), cutoff scatter (3 %), drift x3
static double VINTAGE = 0.0;     // 1: per-voice VCO tuning and scale compression as measured on the unit     // share of the resonance compensation gain seen by the input stage
       // in-loop odd-order softness (distortion growing with resonance)
static double OSC_HF_FC = 40000, OSC_HF_SLOPE = 0.0;   // off: it only compensated the VCF ceiling / res-0 errors fixed later   // VCO top-end loss above C5 (fc at C5, octaves of fc per octave of pitch)
static double TRI_DLY = 0.0, SQR_DLY = 0.0, EDGE_DLY = 4e-6;   // s, shaper delays vs the saw core
static double TRI_SKEW = 1.0;    // scale of the pitch-dependent triangle skew below
// triangle trough at 0.5+skew; the skew grows with pitch (fitted to the even harmonics of m_osc_full C1..C6)
static const Pt TRI_SKEW_T[] = {{24, 0.0014}, {36, 0.0020}, {48, 0.0030}, {60, 0.0031}, {72, 0.0035}, {84, 0.0030}, {96, 0.0030}};
static inline double tri_skew(double fhz) { return TRI_SKEW * interp(TRI_SKEW_T, 69.0 + 12.0 * std::log2(std::max(fhz, 1.0) / 440.0)); }
static const double TRI_AMP = 1.39, SQR_AMP = 0.82, NOISE_FULL = 0.365;
static double VCA_RING_F = 1.2, VCA_RING_Q = 0.5, VCA_RING_K = -0.07;   // fitted on 12 decay/release takes: tail level error 7.4 -> 3.5 dB
static double NOISE_LP_HZ = 40000.0;   // noise source: flat (its 11 kHz limit only made up for the black-box filter; grid cut 104: 3.4 -> 1.5 dB)
static double VOICE_NOISE = 2.2e-3;   // calibrated: gated hiss 2-20 kHz within 1 dB of the unit
static double CIRCUIT_NOISE = 3e-4;   // per-voice input noise; starts self-oscillation like the analog circuit

static inline double polyblep(double t, double dt) {
    if (t < dt) { double x = t / dt; return x + x - x * x - 1.0; }
    if (t > 1.0 - dt) { double x = (t - 1.0) / dt; return x * x + x + x + 1.0; }
    return 0.0;
}
static inline double wrap01(double x) { return x - std::floor(x); }
// cutoff knob -> fc: fitted table (MIDI 16..120), the old measured law outside, joined smoothly
// cutoff knob -> linear fc before the filter's high-frequency compression (filt_warp); anchored on the resonant peaks
// of m_filter_full (res 120, 21:41) and the self-oscillation of m_env_full (filter env sustain sweep)
static double GB_KNOB0 = 70;   // CAL table value at cutoff knob 0 (A-51 factory: -38.9 -> -25.8 dB, unit -24.5; Starsky A-17: -5 -> -1 dB)
static double cutoff_hz_tab(double v, const double* T) {
    double m = v * 127.0 / 65535.0;
    if (m <= 16 && GB_KNOB0 > 0) return std::exp(std::log(GB_KNOB0) + m / 16.0 * (std::log(T[0]) - std::log(GB_KNOB0)));   // bottom of the knob (CAL)
    if (m <= 16) return interp(CUTOFF_HZ, v, true) * (T[0] / interp(CUTOFF_HZ, 16 * 65535.0 / 127, true));
    if (m >= 120) return interp(CUTOFF_HZ, v, true) * (T[13] / interp(CUTOFF_HZ, 120 * 65535.0 / 127, true));
    int i = std::min(12, (int)((m - 16) / 8)); double f = (m - FILT_FC_MIDI[i]) / 8.0;
    return std::exp(std::log(T[i]) + f * (std::log(T[i + 1]) - std::log(T[i])));
}
// low resonance: the spectrum-fitted table (m_filter_full); high resonance: the peak-anchored table + HF compression
// grey-box V3320 constants (CAL marks values fitted on recordings; the rest come from the circuit)
static double GB_R = 1.0;              // OUT2 branch weight against OUT4 at DC: passband-neutral (QCOMP; unit: bass flat/+1 dB with
                                       // resonance); 92k/74k current ratio 1.243 x the level difference of the two stages between them
static double GB_RHF = 0.70;           // OUT2 weight above GB_RQ*wc: the OUT2 node (gain-cell output, cap node) is loaded by its 74k
static double GB_RQ = 0.30;            // shelf corner / wc   (absolute grid fit incl. bass level: 1.61 dB; constant weight 2.51 dB)
static double GB_C1_HZ = 1.6;          // C1 2.2 uF into the resonance input with 74k||92k + 3.6k
static double GB_WC_SCALE = 1.13;      // pole frequency / FILT_FC_PK (grid fit, +-3 % over cut 24..88); peak = 0.538 wc = 0.608 x table
static double GB_LEAK_HZ = 0.5;        // gain-cell output impedance (~1 Mohm, V3320 datasheet) leaks each pole capacitor: fixed in Hz, so the
                                       // loop loses gain at low cutoffs (unit: self-oscillation from CC 113 at 80 Hz, CC 100 at 1.4 kHz; fit 1.7 %)
static double GB_KSLOPE = 0.015;      // CAL loop gain ~ (wc / 1 kHz)^KSLOPE (gain-cell current gain vs control voltage); with leak 0.5 Hz: self-oscillation onset 80 Hz..12 kHz within 0.7 CC of the unit (was -7.5 at 80 Hz, -5 at 12 kHz)
// CV paths (Behringer main board: 74HC4051 -> 10 nF hold -> TL064 -> 2k2 / 100 nF; Maffez voice trace: FSLEW 100k / 150 nF
// bridged by antiparallel diodes D16 on the filter CV, then FSUM with poly-mod and ext CV)
static double CV_RC_HZ = 723.4;        // 1 / (2 pi 2.2k 100n)
static double FSLEW_TAU = 0.015;       // 100k x 150 nF
static double FSLEW_OCT = 1.0;         // CAL diode threshold in octaves of filter CV (m_env_full filt_*: 99 c rms without the slew, 94 c with it)
static double GB_WC_MIN = 39.4, GB_WC_MAX = 36000.0;   // filter CV range of the unit: self-oscillation 21.2 Hz .. 20.45 kHz (m_fenv)
// V3320 exponential converter error at high control current: pole frequency compresses above ~10 kHz, as a function of
// frequency (so knob and envelope bend alike; m_greybox peaks + m_fenv env amounts)
static double GB_WARP_F = 35000.0, GB_WARP_N = 3.0;   // search on m_fenv env amounts with the knob peaks held: rms ~20 c (was +370 c at amount 49152)
// VCO mixer (Maffez trace): each VCO's selected waves sum into 560 ohm, 47 uF coupling (~6 Hz), V13700 level OTA whose input
// sees tens of mV (saw 10 Vpp via 165k: +-17 mV against 2 VT = 52 mV) -> tanh(MIX_OTA_K x)/MIX_OTA_K per unit of saw amplitude
static double MIX_HP_HZ = 6.0, MIX_OTA_K = 0.1;   // OTA input practically linear: any visible tanh worsened the m_osc_full waves (0.48 -> 0.65 dB at 0.33)
// VCO floor: the 3340 goes far below audio (Starsky scan: oscillators at 16 and ~10 Hz at C1); the old 27.5 Hz clamp cut them
static double VCO_MIN_HZ = 5.0;
static double DRIFT_TAU = 90.0, DRIFT_C = 0.25;
static double OSC_SPREAD = 1.0;   // user control: x the measured per-oscillator tuning spread and drift (app: ROZRZUT OSC)
// AMP CV slew like FSLEW: RC (ASLEW_TAU) bridged by diodes (ASLEW_V of full CV): big moves pass, the last ASLEW_V creeps in/out
// (m_env_full fast decays: below -20 dB the unit falls ~20 dB per 70 ms, a plain RC-free CV dropped to zero)
static double AMP_ATT_START = -0.02;    // CAL amp attack start level (all amp env takes: 3.11 dB at -0.045, 3.07 at -0.02, 3.12 at 0)
static double ASLEW_TAU = 0.03, ASLEW_V = 0.06;   // m_env_full amp (all 170 takes): 3.77 -> 3.11 dB   // s, cents
static double GB_RES_VT = 4.5;         // CAL resonance OTA input scale (joint fit 25.11: nlgrid + test 4 + lock + sweeps + IMD, score 5.83 -> 5.44)
static double GB_IN_POL = -1.0;        // which side the IN1 cell cuts off (+1 positive peaks, -1 negative)
static double GB_IN_VS = 2.6;          // CAL IN1 cell IREF in signal units (m_greybox A+B fifth: 2nd and 3rd order IMD within +-2 dB at all levels, knee 0.3)
static double GB_STG2 = 16.0;          // CAL cell limit of stages 2-4 (joint fit; mild)
static double GB_STG_E = 0.3;           // knee of the one-sided stage cells
static double GB_STG_ONE = 0.0;         // stage cell limits one-sided (V3320 current-mode cells): 0 = symmetric tanh, +-1 = cut-off side
static double GB_STG_VS = 6.0;          // CAL stage-1 cell limit (m_lock: entrainment range by the 2nd harmonic 62/152/356 c vs 58/146/380 c on the unit)
static double GB_OUT = 1.0;            // output level of OUT4 relative to the old structure
// pole-frequency mapping (knob -> CV law): anchored on the 8 self-oscillation peaks of m_greybox (peak = 0.538 wc, wc = 1.13 x table)
static const double GB_FC_RAW[14] = {135.6, 197.6, 288.1, 422.1, 618.2, 906.0, 1326.8, 1942.1, 2861.4, 4234.0, 6291.0, 9473.6, 15644.4, 26018.3};   // peaks-anchored (with compression baked in)
static const Pt GB_RES_K[] = {{0, 0.21}, {8256, 0.54}, {16513, 0.88}, {24769, 1.12}, {33026, 1.45}, {41282, 1.74}, {49538, 1.99}, {57795, 2.26}, {65535, 2.51}};   // grid fit (cut 24, 56) x1.038 so the onset falls on the m_greybox sweep thresholds; slope continued
// V3320 gain cell, current mode: IOUT = (IREF - IIN) A; the cell cuts off as the input current approaches +IREF (one side only).
// Smooth one-sided limit in signal units: R = IREF, e = knee softness; small signals pass with gain 1.
static double GB_CELL_E = 0.4;
static inline double cell(double x, double R, double e = -1) {
    if (e < 0) e = GB_CELL_E;
    double z = 1.0 - x / R, e2 = e * e;
    return R * (0.5 * (1.0 + std::sqrt(1.0 + e2)) - 0.5 * (z + std::sqrt(z * z + e2)));
}
static inline double ftanh(double x) { if (x > 3) return 1; if (x < -3) return -1; double x2 = x * x; return x * (27 + x2) / (27 + 9 * x2); }
// pulse: low from the saw reset to the edge, high after it. High time goes monotonically from ~92 % (PW 0) through
// 50 % to ~7.5 % (PW max) (first waveform test); the harmonic fit (PW_DUTY) only gives |d - 0.5|.
static double PW_MONO = 1.0;   // high time falls monotonically with PW (m_osc_full: PW 30 -> 72 % high, PW 80 -> 39 % high)
static inline double pw_edge(double v) { (void)PW_MONO; return 1.0 - interp(PW_HIGH, v); }   // low until the edge, high after
// Poly-mod osc B source (measured with B slowed to 10.6 Hz, tools/m_pmod.py): saw or tri selected -> a DC part plus
// the B triangle core; pulse -> 0..max square. Values = octaves at amount max; amount law (v/65535)^1.25.
static double PMF_DC = 2.03, PMF_AC = 0.56, PMF_SQ = 0.69;   // -> cutoff
static double PMA_DC = 0.697, PMA_AC = 0.24, PMA_SQ = 0.241;   // -> freq A (pulse part not measured, assumed half)
static double PM_TRI = 1.03;   // triangle source slightly stronger than saw (m_mod_full)
static inline double pm_src(const Patch& P, double ph, double duty, double dc, double ac, double sq) {
    // selected B waves are AVERAGED (m_mod_full: saw+pulse gives less than saw alone)
    // weighted: saw/tri 1.5, pulse 1.0 (fits saw+pulse and tri+pulse on freq A and filter)
    double y = 0, n = 0; double core = ac * (ph < 0.5 ? 1.0 - 4.0 * ph : -3.0 + 4.0 * ph);
    if (P.saw_b) { y += 1.5 * (dc + core); n += 1.5; }
    if (P.tri_b) { y += 1.5 * PM_TRI * (dc + core); n += 1.5; }
    if (P.sqr_b) { y += sq * (ph < duty ? 0.0 : 2.0); n += 1.0; }
    return n > 0 ? y / n : 0.0;
}

double Env::tick(double dt, double Ta, double Td, double S, double Tr, bool exp) {
    switch (st) {
    case Idle: lv = 0; break;
    case Attack:
        if (exp) { lv += (EXP_ATT_TARGET - lv) * (1.0 - std::exp(-dt / (EXP_ATT_TAU * Ta))); if (lv >= 1.0) { lv = 1; st = Decay; } }
        else { lv += dt / Ta; if (lv >= 1) { lv = 1; st = Decay; } }
        break;
    case Decay:
        if (exp) { double un = under < 0 ? EXP_FALL_UNDER : under; double tg = S - un * (1.0 - S); lv = tg + (lv - tg) * std::exp(-dt / (EXP_FALL_TAU * Td)); if (lv <= S + 1e-4) { lv = S; st = Sustain; } }
        else { lv -= dt / Td; if (lv <= S) { lv = S; st = Sustain; } }
        break;
    case Sustain: lv = S; break;
    case Release:
        if (exp) { double un = under < 0 ? EXP_FALL_UNDER : under; double tg = -un; lv = tg + (lv - tg) * std::exp(-dt / (EXP_FALL_TAU * Tr)); if (lv <= 1e-4) { lv = 0; st = Idle; } }
        else { lv -= dt / Tr; if (lv <= 0) { lv = 0; st = Idle; } }
        break;
    }
    return lv;
}

Engine::Engine(double s) {
    u_vint = VINTAGE; u_vx = VINTAGE_X; u_spread = OSC_SPREAD; u_tune = TUNE_C;   // user settings start from the calibration defaults
    set_sample_rate(s);
    std::mt19937 rng_x(4321);   // voices 9..64 draw from their own generator: the 8-voice random sequence stays as it was
    for (int i = 0; i < NV; ++i) { Voice& v = V[i]; auto& g = i < NV_HW ? rng : rng_x; v.a.ph = (uni(g) + 1) / 2; v.b.ph = (uni(g) + 1) / 2; v.fenv.start = 0.0; v.fenv.under = 0.0; v.aenv.start = AMP_ATT_START; }   // filter env: no attack start offset (poly-mod env takes)
    for (int i = 0; i < NV; ++i) V[i].rs = 0x9E3779B97F4A7C15ULL * (uint64_t)(i + 1);
}
// ---- voice workers: the voices share no state inside a block, so they can run on several cores; the mix is summed in
// voice order afterwards (bit-identical to one thread). Blocks with an arpeggio step, a knob glide or a voice steal run serially.
struct Engine::Pool {
    std::vector<std::thread> th; std::mutex m; std::condition_variable cv; std::atomic<int> done{0}; int gen = 0, nw; bool quit = false;
    std::function<void(int)> job;
    explicit Pool(int n) : nw(n) {
        for (int k = 0; k < n; ++k) th.emplace_back([this, k] { int seen = 0;
            for (;;) { { std::unique_lock<std::mutex> l(m); cv.wait(l, [&] { return quit || gen != seen; }); if (quit) return; seen = gen; }
                       job(k + 1); done.fetch_add(1, std::memory_order_release); } });
    }
    void run(const std::function<void(int)>& f) {
        job = f; done.store(0); { std::lock_guard<std::mutex> l(m); ++gen; } cv.notify_all();
        job(0); while (done.load(std::memory_order_acquire) < nw) std::this_thread::yield();
    }
    ~Pool() { { std::lock_guard<std::mutex> l(m); quit = true; } cv.notify_all(); for (auto& t : th) t.join(); }
};
Engine::~Engine() { delete pool_; }
void Engine::set_threads(int n) { delete pool_; pool_ = nullptr; nthreads = n; }

void Engine::set_sample_rate(double s) {
    for (auto& v : V) { v.m_inca = v.m_incb = v.m_hfa = v.m_hfb = v.m_gl = v.m_gq = v.m_ks = v.m_vc = Memo{}; v.m_warp = v.m_vint = Memo2{}; }   // memos depend on dt
    out_sr = s; sr = 2.0 * s; dt = 1.0 / sr;   // everything runs at 2x, decimated by a linear-phase FIR at the output
    {   // decimation FIR: Kaiser-windowed sinc, flat to ~0.46 out_sr
        const int N = DEC_TAPS; double fc = 0.46 * out_sr / sr, beta = 8.0, sum = 0;
        auto i0 = [](double x) { double s1 = 1, t = 1; for (int k = 1; k < 30; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s1 += t; } return s1; };
        for (int k = 0; k < N; ++k) {
            double m = k - (N - 1) / 2.0, r = 2.0 * k / (N - 1) - 1.0;
            double sinc = m == 0 ? 2 * fc : std::sin(2 * PI * fc * m) / (PI * m);
            dec_h[k] = sinc * i0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0(beta); sum += dec_h[k];
        }
        for (int k = 0; k < N; ++k) dec_h[k] /= sum;
        for (auto& v : dec_buf) v = 0; dec_pos = 0;
    }
    {   // output stage of the unit (measured saw rolloff, every note): one-pole 14 kHz + two-pole 20 kHz Q 0.75 (bilinear)
        double w1 = std::tan(PI * OUT_LP_HZ / sr); lp1_b = w1 / (1 + w1); lp1_a = (w1 - 1) / (w1 + 1);
        double w = std::tan(PI * OUT_LP2_HZ / sr), q = OUT_LP2_Q, n0 = 1 + w / q + w * w;
        bq_b0 = w * w / n0; bq_b1 = 2 * bq_b0; bq_b2 = bq_b0; bq_a1 = 2 * (w * w - 1) / n0; bq_a2 = (1 - w / q + w * w) / n0;
    }
    vca_c = 1.0 - std::exp(-dt / 0.000665);   // firmware part; with the 723 Hz S&H RC in series the rise stays as measured (~2 ms 10-90 %)
    pw_cv_c = 1.0 - std::exp(-2 * PI * PW_CV_HZ * dt);
    noise_c = 1.0 - std::exp(-2 * PI * NOISE_LP_HZ * dt);
    { double c11 = 1.0 - std::exp(-2 * PI * 11000.0 * dt); noise_norm = 1.0 / std::sqrt(c11 / (2 - c11)); }   // level per Hz as calibrated (with the 11 kHz limit)
}

// knob -> pitch offset in cents for the oscillator frequency modes (0 Free, 1 Semi, 2 Oct, 3 FiHD -> Free)
static double FREE_OFFSET = 1.0;   // scale of the correction below
// cents added to the Free table (14:18 session), fitted on the sync survey (reset-to-edge times and harmonic fits, 246 takes)
static const Pt FREE_CORR[] = {{0, -14.0}, {65535, -14.0}};   // a knob-dependent fit (+8..-15 c) was worse on the 246 takes (3.66 vs 3.53 dB): error depends on note too -> refit from m_sync_fine
static double freq_cents(int mode, double v) {
    if (mode == 1) return (std::min(24.0, std::round(v / (65536.0 / 24.0))) - 12.0) * 100.0;
    if (mode == 2) {   // measured zone offsets relative to zone 0 (m_freq: -3445 floor, -2386, -1192, 0, +1192, +2384, +3575)
        static const double Z[7] = {-3600.0, -2400.0, -1200.0, 0.0, 1200.0, 2400.0, 3600.0};   // exact octaves (the unit: 1192 c)
        return Z[std::min(6, (int)(v * 7.0 / 65536.0))];
    }
    return interp(FREE_CENTS, v) + FREE_OFFSET * interp(FREE_CORR, v);
}

void Engine::start_voice(int i, int note, int vel, double interval, bool from_zero) {
    Voice& v = V[i];
    double target = note + interval;
    bool had = v.note >= 0;
    if (!had && last_pitch >= 0) { v.pitch = last_pitch; had = true; }   // a voice that never played glides from the last note played
    if (v.aenv.st == Env::Idle && !v.gate) {   // the 3340s run all the time: a silent voice's oscillators are somewhere in their cycle
        v.a.ph = (uni(rng) + 1) / 2; v.b.ph = (uni(rng) + 1) / 2;
    }
    v.note = note; v.vel = vel; v.gate = true; v.age = ++clock; v.interval = interval; v.t_on = 0;
    if (u_vx > 0) { v.rja = u_vx * 3.0 * gauss(rng); v.rjb = u_vx * 3.0 * gauss(rng); v.rjf = std::exp(u_vx * 0.03 * gauss(rng)); }
    else { v.rja = v.rjb = 0; v.rjf = 1; }
    v.pitch_target = target; last_pitch = target;
    double T = interp(GLIDE_T, P.glide);
    if (!had || T <= 0) { v.pitch = target; v.glide_rate = 0; }
    else if (P.glide_mode == 0) v.glide_rate = std::fabs(target - v.pitch) / T;
    else v.glide_rate = 12.0 / T;
    v.aenv.gate_on(from_zero); v.fenv.gate_on(from_zero);
}

// clock divisions in beats (quarter = 1), OB-6 style: HALF, QTR, 8th D, 8th, 8th S, 8th T, 16th, 16th S, 16th T, 32nd
static const double ARP_BEATS[10] = {2.0, 1.0, 0.75, 0.5, 0.5, 1.0 / 3, 0.25, 0.25, 1.0 / 6, 0.125};
static const bool ARP_SWING[10] = {false, false, false, false, true, false, false, true, false, false};
// LFO sync divisions: 4 bars, 2 bars, 1 bar, HALF, QTR, 8th D, 8th, 8th T, 16th, 16th T, 32nd (one LFO cycle)
static const double LFO_BEATS[11] = {16.0, 8.0, 4.0, 2.0, 1.0, 0.75, 0.5, 1.0 / 3, 0.25, 1.0 / 6, 0.125};
double Engine::arp_step_len() const {
    double b = ARP_BEATS[arp_div] * 60.0 / bpm;
    if (ARP_SWING[arp_div]) b *= (arp_stepno % 2 == 0) ? 4.0 / 3.0 : 2.0 / 3.0;   // swing 2:1 within each pair
    return b;
}
void Engine::arp_step() {
    if (arp_note >= 0) { int n = arp_note; arp_note = -1; for (auto& v : V) if (v.gate && v.note == n) { v.gate = false; v.aenv.gate_off(); v.fenv.gate_off(); } }
    if (arp_held.empty()) return;
    std::vector<int> base = arp_held;
    if (arp_mode != 6) std::sort(base.begin(), base.end());
    std::vector<int> order;
    for (int o = 0; o < arp_oct; ++o) for (int n : base) if (n + 12 * o <= 127) order.push_back(n + 12 * o);
    int N = (int)order.size(), idx = 0;
    if (arp_mode == 2) idx = N - 1 - (arp_pos % N);
    else if (arp_mode == 3) { int L = std::max(1, 2 * N - 2); int k = arp_pos % L; idx = k < N ? k : L - k; }        // up/down, ends once
    else if (arp_mode == 4) { int L = 2 * N; int k = arp_pos % L; idx = k < N ? k : L - 1 - k; }                    // up then down, ends twice
    else if (arp_mode == 5) idx = (int)((uni(rng) + 1) * 0.5 * N) % N;
    else idx = arp_pos % N;
    ++arp_pos; ++arp_stepno; arp_note = order[idx];
    int m = arp_mode; arp_mode = 0; note_on(arp_note, arp_vel); arp_mode = m;
}
void Engine::note_on(int n, int vel) {
    if (vel == 0) { note_off(n); return; }
    if (arp_mode) {
        if (arp_held.empty()) { arp_pos = 0; arp_t = 0; arp_stepno = 0; arp_vel = vel; arp_held.push_back(n); arp_step(); arp_off_t = arp_gate * arp_step_len(); }
        else arp_held.push_back(n);
        return;
    }
    if (P.unison && !poly_uni) {
        // all enabled voices play the note: voice slot 0 = root, slot j = pattern[j-1] semitones (0xFF = unused)
        int j = 0;
        for (int i = 0; i < NV; ++i) {
            if (!on(i)) continue;
            int iv = j >= uni_n || i >= NV_HW ? 0xFF : j == 0 ? 0 : (j - 1 < 7 ? P.unison_pattern[j - 1] : 0xFF);   // uni_n voices stack
            ++j;
            if (iv == 0xFF) { V[i].gate = false; V[i].aenv.gate_off(); V[i].fenv.gate_off(); continue; }
            V[i].uslot = i % NV_HW; start_voice(i, n, vel, iv, false);
        }
        return;
    }
    if (mono && !P.unison) {   // MONO: voice 1 only, last-note priority; releasing returns to the previous held note
        mono_held.erase(std::remove_if(mono_held.begin(), mono_held.end(), [&](auto& h) { return h.first == n; }), mono_held.end());
        mono_held.push_back({n, vel}); V[0].uslot = -1; start_voice(0, n, vel, 0, false); return;
    }
    if (P.unison && poly_uni) {   // POLY UNISON: every note gets its own stack of uni_n voices (pattern intervals, detune fan by slot)
        for (auto& v : V) if (v.gate && v.note == n) { v.gate = false; v.aenv.gate_off(); v.fenv.gate_off(); }
        for (int k = 0; k < uni_n; ++k) {
            int iv = k == 0 ? 0 : (k - 1 < 7 ? P.unison_pattern[k - 1] : 0xFF);
            if (iv == 0xFF) continue;
            int i = alloc_voice(true); if (i < 0) break;
            Voice& v = V[i]; v.uslot = k;
            if (v.gate) { v.steal = 0.005; v.pend_note = n; v.pend_vel = vel; v.pend_iv = iv; v.gate = true; v.note = n; v.age = ++clock; }
            else { rr = i; start_voice(i, n, vel, iv, false); }
        }
        return;
    }
    // the same note goes back to the voice that played it (held or releasing): it retriggers from its envelope level,
    // no second voice sounds the same pitch (m_final 'assign': the unit's repeated / overlapping notes are level-constant)
    int pick = -1;
    for (int i = 0; i < NV; ++i) if (on(i) && V[i].note == n && V[i].steal <= 0 && V[i].pend_note < 0) { pick = i; break; }
    if (pick >= 0) { start_voice(pick, n, vel, 0, false); return; }
    // round robin over voices that are not held; steal the newest held note if all are held
    for (int k = 1; k <= nvoices; ++k) {
        int i = (rr + k) % nvoices;
        if (on(i) && !V[i].gate) { pick = i; break; }
    }
    if (pick >= 0) { rr = pick; V[pick].uslot = -1; start_voice(pick, n, vel, 0, false); return; }
    uint32_t best = 0;
    for (int i = 0; i < NV; ++i) if (on(i) && V[i].gate && V[i].age >= best) { best = V[i].age; pick = i; }
    if (pick < 0) return;
    Voice& v = V[pick];
    v.steal = 0.005; v.pend_note = n; v.pend_vel = vel; v.pend_iv = 0; v.uslot = -1; v.gate = true; v.note = n; v.age = ++clock;
}
int Engine::alloc_voice(bool oldest) {   // a free voice (round robin), else steal the oldest (or newest) held one
    const int N = pool();
    for (int k = 1; k <= N; ++k) { int i = (rr + k) % N; if (on(i) && !V[i].gate) { rr = i; return i; } }
    int pick = -1; uint32_t best = oldest ? UINT32_MAX : 0;
    for (int i = 0; i < N; ++i) if (on(i) && V[i].gate && (oldest ? V[i].age < best : V[i].age >= best)) { best = V[i].age; pick = i; }
    return pick;
}

void Engine::note_off(int n) {
    if (arp_mode) {
        arp_held.erase(std::remove(arp_held.begin(), arp_held.end(), n), arp_held.end());
        if (arp_held.empty() && arp_note >= 0) { int k = arp_note; arp_note = -1; int m = arp_mode; arp_mode = 0; note_off(k); arp_mode = m; }
        return;
    }
    if (mono && !P.unison) {
        bool top = !mono_held.empty() && mono_held.back().first == n;
        mono_held.erase(std::remove_if(mono_held.begin(), mono_held.end(), [&](auto& h) { return h.first == n; }), mono_held.end());
        if (sustain_pedal) return;
        if (top && !mono_held.empty()) { start_voice(0, mono_held.back().first, mono_held.back().second, 0, false); return; }
        if (!mono_held.empty()) return;
    }
    if (sustain_pedal) return;
    for (auto& v : V)
        if (v.gate && (v.note == n || v.pend_note == n)) { v.gate = false; v.pend_note = -1; v.aenv.gate_off(); v.fenv.gate_off(); }
}
void Engine::all_notes_off() { for (auto& v : V) { v.gate = false; v.pend_note = -1; v.aenv.gate_off(); v.fenv.gate_off(); } }

void Engine::midi(uint8_t st, uint8_t d1, uint8_t d2) {
    switch (st & 0xF0) {
    case 0x90: note_on(d1, d2); break;
    case 0x80: note_off(d1); break;
    case 0xE0: bend = ((d2 << 7 | d1) - 8192) / 8192.0; break;
    case 0xD0: pressure = d1; break;
    case 0xB0:
        if (d1 == 1) wheel = d2 / 127.0;
        else if (d1 == 64) { sustain_pedal = d2 >= 64; if (!sustain_pedal) for (auto& v : V) if (v.gate && false) {} }
        else if (d1 == 123 || d1 == 120) all_notes_off();
        break;
    }
}

double Engine::osc_sample(Osc& o, double inc, bool saw, bool tri, bool sqr, double duty, double sqr_amp, bool& wrapped, double& wrap_frac) {
    double p = o.ph, y = 0;
    if (saw) y += (1.0 - 2.0 * p) + polyblep(p, inc);                                 // jump +2 at the wrap
    double fhz = inc / dt;   // wave shapers lag the core slightly (fixed time -> phase grows with pitch; m_osc_combo)
    if (tri) { double q = wrap01(p - TRI_DLY * fhz), r = 0.5 + tri_skew(fhz); y += TRI_AMP * (q < r ? 1.0 - 2.0 * q / r : -1.0 + 2.0 * (q - r) / (1.0 - r)); }
    if (sqr) { double q = wrap01(p - SQR_DLY * fhz), e = std::min(0.98, duty + EDGE_DLY * fhz); y += sqr_amp * ((q < e ? -1.0 : 1.0) - polyblep(q, inc) + polyblep(wrap01(q - e), inc)); }   // rising edge comes EDGE_DLY late (pulse narrows at high notes)
    o.out = y;
    o.ph += inc; wrapped = false;
    if (o.ph >= 1.0) { o.ph -= 1.0; wrapped = true; wrap_frac = o.ph / inc; }
    return y;
}

double Engine::tick_voice(Voice& v, int i, double lfo, double lnoise, double vib, double lfo_pw) {
    if (v.aenv.st == Env::Idle && !v.gate && v.steal <= 0 && std::fabs(v.rbp) + std::fabs(v.rlp) < 1e-6 && v.cv_as < 1e-6) {
        // silent voice: the filter envelope keeps running (a longer filter release must finish, as on the unit)
        if (v.fenv.st != Env::Idle) {
            double tr0 = v.mc_tr0(P.fe_r + 1e6 * P.fe_speed, [&](double) { return P.fe_speed ? interp(REL_T_SLOW, P.fe_r, true) : interp(REL_T_FAST, P.fe_r, true); });
            v.fenv.tick(dt, 1, 1, 0, FENV_T * tr0, P.fe_shape);
        }
        return 0.0;
    }
    // stolen voice: 5 ms fade, then restart from zero
    double steal_gain = 1.0;
    if (v.steal > 0) {
        v.steal -= dt; steal_gain = std::max(0.0, v.steal / 0.005);
        if (v.steal <= 0) { int n = v.pend_note, vel = v.pend_vel; v.pend_note = -1; v.steal = 0; if (n >= 0) start_voice(i, n, vel, v.pend_iv, true); else return 0; }
    }
    v.t_on += dt;
    // glide (linear in pitch)
    if (v.pitch != v.pitch_target) {
        double st = v.glide_rate * dt;
        if (v.glide_rate <= 0 || std::fabs(v.pitch_target - v.pitch) <= st) v.pitch = v.pitch_target;
        else v.pitch += v.pitch_target > v.pitch ? st : -st;
    }
    // envelopes
    // envelope times from the patch: the same for every voice, recomputed only when a knob moves (exact memos)
    auto TA = [&](Memo& m, double x, int speed) { return m(x + 1e6 * speed, [&](double) { return speed ? interp(ATT_T_SLOW, x, true) : interp(ATT_T_FAST, x, true); }); };
    auto TD = [&](Memo& m, double x, int speed) { return m(x + 1e6 * speed, [&](double) { return speed ? interp(DEC_T_SLOW, x, true) : interp(DEC_T_FAST, x, true); }); };
    auto TR = [&](Memo& m, double x, int speed) { return m(x + 1e6 * speed, [&](double) { return speed ? interp(REL_T_SLOW, x, true) : interp(REL_T_FAST, x, true); }); };
    double ae = std::max(0.0, v.aenv.tick(dt, TA(v.mc_ta, P.ae_a, P.ae_speed), TD(v.mc_td, P.ae_d, P.ae_speed), v.mc_sus(P.ae_s, [](double x) { return interp(SUSTAIN, x); }), TR(v.mc_tr, P.ae_r, P.ae_speed), P.ae_shape));
    v.vca_sm += (ae - v.vca_sm) * vca_c; v.cv_amp += (v.vca_sm - v.cv_amp) * (1.0 - std::exp(-2 * PI * CV_RC_HZ * dt)); ae = v.cv_amp;
    if (ASLEW_TAU > 0) { double d = ae - v.cv_as; v.cv_as += d * (1.0 - std::exp(-dt / ASLEW_TAU)); d = ae - v.cv_as;
        if (d > ASLEW_V) v.cv_as = ae - ASLEW_V; else if (d < -ASLEW_V) v.cv_as = ae + ASLEW_V; ae = std::max(0.0, v.cv_as); }   // + AMP CV S&H RC   // VCA control smoothing (HW attack 0: 10->90 % in ~2 ms, no clicks)
    if (VCA_RING_K != 0) {   // VCA CV rings slightly after fast moves (m_env_full: dip + bump -35..-50 dB after decay/release, ~0.7 s period); clamped at 0
        double g = std::tan(PI * VCA_RING_F * dt), R = 0.5 / VCA_RING_Q;
        double hp = (ae - (2 * R + g) * v.rbp - v.rlp) / (1 + 2 * R * g + g * g), b = g * hp + v.rbp, l = g * b + v.rlp;
        v.rbp = g * hp + b; v.rlp = g * b + l;
        if (std::fabs(v.rbp) + std::fabs(v.rlp - ae) < 1e-7) { v.rbp = 0; v.rlp = ae; }
        if (v.aenv.st != Env::Attack) ae = std::max(0.0, ae + VCA_RING_K * b);   // only after falls (where it was measured); on a slow attack it clamped the VCA shut for the first ~50 ms
        if (v.aenv.st == Env::Idle && !v.gate && std::fabs(v.rbp) < 1e-6) { v.rbp = 0; v.rlp = 0; }
    }
    double fe = std::max(0.0, v.fenv.tick(dt, FENV_T * TA(v.mc_fta, P.fe_a, P.fe_speed), FENV_T * TD(v.mc_ftd, P.fe_d, P.fe_speed), P.fe_s / 65535.0, FENV_T * TR(v.mc_ftr, P.fe_r, P.fe_speed), P.fe_shape));   // filter env sustain is linear (A-05 pitch check)
    // modulation depth with mod delay
    double md = v.mc_md(P.mod_delay, [](double x) { return interp(MOD_DELAY_S, x); }), mdf = 1.0;
    if (md > 0) mdf = v.t_on < md ? 0.0 : std::min(1.0, (v.t_on - md) / (0.43 * md));
    static const double MW_AMT[4] = {0.03, 0.125, 0.5, 1.0};   // wheel -> LFO amount ranges (measured 0, 1, 3; 2 assumed)
    double lamt = std::min(65535.0, P.lfo_amt + P.lfo_at * pressure / 127.0 + (P.mw_target == 0 ? wheel * MW_AMT[std::min<int>(P.mw_amt, 3)] * 65535.0 : 0.0));
    double lfo_c = 0.5 * v.mc_lpp(lamt, [](double x) { return interp(LFO_PP_CENTS, x); }) * (P.lfo_shape == 4 ? lnoise : lfo) * mdf;  // cents
    // pitch (cents relative to A440 grid)
    double common = (v.pitch - 69.0) * 100.0 + vib + u_tune;
    // base tuning: A440, exact octaves (the unit's own scale error - sharp bass, flat top - is calibration state, not emulated)
    if (P.bend_target == 1) common += bend * (P.pitch_range / 2048.0) * 100.0;
    if (P.unison) {
        double dm = P.detune * 127.0 / 65535.0;   // detune knob in MIDI steps
        int us = v.uslot >= 0 ? v.uslot : i % NV_HW;
        common += DETUNE_SLOPE[us] * dm + DETUNE_OFS[us] * std::min(1.0, dm / 8.0);
    }
    // PWM by LFO
    double pwl = (P.lfo_target & 4) ? lfo_pw * mdf * (lamt / 65535.0) * 0.42 : 0.0;   // measured: +-0.054 / 0.21 / 0.41 duty; shared PW CV smoothed (~14 Hz)
    double da = std::clamp(pw_edge(P.pw_a) + pwl, 0.02, 0.98), db = std::clamp(pw_edge(P.pw_b) + pwl, 0.02, 0.98);
    bool lf = P.lfo_target & 1, la = P.lfo_target & 8, lb = P.lfo_target & 16;
    if (lf && !la && !lb) la = lb = true;
    double ca = common + A_DETUNE + v.mc_fca(P.freq_a + 1e6 * P.freqmode_a, [&](double) { return freq_cents(P.freqmode_a, P.freq_a); }) + ((lf && la) ? lfo_c : 0);
    double cb = common + v.mc_fcb(P.freq_b + 1e6 * P.freqmode_b, [&](double) { return freq_cents(P.freqmode_b, P.freq_b); }) + ((lf && lb) ? lfo_c : 0);
    double fb = P.fine_b - 32768.0; cb += fb > 0 ? fb / 32767.0 * 25.5 : fb / 32768.0 * 22.0;
    if (u_vint > 0) {   // each oscillator's own tuning (measured) + slow drift (Ornstein-Uhlenbeck, ~0.25 c, minutes)
        auto dev = [&](int k) { const double* d = OSC_DEV[k][i % NV_HW]; double n = v.pitch;
            return n <= 69 ? d[0] + (d[1] - d[0]) * std::clamp((n - 33) / 36.0, 0.0, 1.0) : d[1] + (d[2] - d[1]) * std::clamp((n - 69) / 24.0, 0.0, 1.0); };
        double dx = 1.0 + 2.0 * u_vx;
        ca += u_vint * u_spread * (dev(0) + dx * v.drift_a) + v.rja; cb += u_vint * u_spread * (dev(1) + dx * v.drift_b) + v.rjb;
    }
    double fvel_pm = 1.0 - (P.filt_vel / 65535.0) * (1.0 - v.vel / 127.0);
    if (P.pm_freq) ca += (P.pm_env - 32768.0) / 1000.0 * 182.0 * fe * fvel_pm;          // filter env -> freq A: firmware (DAC CV)
    {   // pitch CVs leave the DAC through the S&H RC (723 Hz)
        const double kc = 1.0 - std::exp(-2 * PI * CV_RC_HZ * dt);
        if (!v.cv_ok) { v.cv_a = ca; v.cv_b = cb; }
        v.cv_a += kc * (ca - v.cv_a); v.cv_b += kc * (cb - v.cv_b); ca = v.cv_a; cb = v.cv_b;
    }
    if (P.pm_freq) ca += 1200.0 * pm_src(P, v.b.ph, db, PMA_DC, PMA_AC, PMA_SQ) * interp(PM_FREQ_AMT, P.pm_oscb);   // osc B -> FM: poly-mod OTA (audio)
    // VCO range floor (~27.5 Hz on the unit: poly-mod env / free mode cannot go lower)
    double inc_a = std::clamp(440.0 * v.m_inca(ca, [](double c) { return std::exp2(c / 1200.0); }), VCO_MIN_HZ, 0.45 * sr) * dt,
           inc_b = std::clamp(440.0 * v.m_incb(cb, [](double c) { return std::exp2(c / 1200.0); }), VCO_MIN_HZ, 0.45 * sr) * dt;
    bool wa, wb; double fa, fbw;
    double ob = osc_sample(v.b, inc_b, P.saw_b, P.tri_b, P.sqr_b, db, SQR_AMP, wb, fbw);
    // osc A naked waveform at an arbitrary phase (negative = just after a sync reset: the ramp overshoots)
    const double skA = tri_skew(inc_a / dt);
    auto wave_a = [&](double p) {
        double y = 0;
        if (p < 0) { if (P.saw_a) y += 1.0 - 2.0 * p; if (P.tri_a) y += TRI_AMP * (1.0 - 2.0 * p / (0.5 + skA)); if (P.sqr_a) y += -SQR_AMP; return y; }
        if (P.saw_a) y += 1.0 - 2.0 * p;
        if (P.tri_a) { double r = 0.5 + skA; y += TRI_AMP * (p < r ? 1.0 - 2.0 * p / r : -1.0 + 2.0 * (p - r) / (1.0 - r)); }
        if (P.sqr_a) y += SQR_AMP * (p < da ? -1.0 : 1.0);
        return y;
    };
    double oa;
    if (v.a.ph < 0) { oa = wave_a(v.a.ph); v.a.out = oa; v.a.ph += inc_a; }
    else oa = osc_sample(v.a, inc_a, P.saw_a, P.tri_a, P.sqr_a, da, SQR_AMP, wa, fa);
    // osc A goes out one sample late so a sync reset can be band-limited on both sides (polyBLEP residuals)
    double oa_out = v.a_prev;
    if (P.sync && wb) {
        double t = std::clamp(fbw, 0.0, 1.0);                        // samples since B wrapped
        v.a.ph = -(SYNC_TIME > 0 ? SYNC_TIME * inc_a / dt : SYNC_DELTA) + t * inc_a;
        double nv = wave_a(v.a.ph), J = nv - oa;                      // jump at the reset
        oa_out += J * t * t * 0.5;                                    // residual before the step
        v.a_prev = nv - J * (1 - t) * (1 - t) * 0.5;                  // residual after the step
        v.a.out = nv;
        if (SYNC_TAU > 0) v.sync_res -= J;                             // finite reset: the jump decays in like an RC discharge
    } else v.a_prev = oa;
    if (SYNC_TAU > 0) { oa_out += v.sync_res; v.sync_res *= std::exp(-dt / SYNC_TAU); }
    oa = oa_out;
    // VCO high-pitch rolloff: above ~C5 the oscillators lose their top end (m_osc_combo / m_osc_full C6-C7)
    if (OSC_HF_SLOPE > 0) {
        auto hf = [&](double inc) { double fc = OSC_HF_FC * std::exp2(-std::max(0.0, std::log2(inc / dt / 523.25)) * OSC_HF_SLOPE);
                                    return 1.0 - std::exp(-2 * PI * std::min(fc, 0.45 * sr) * dt); };
        double ga = v.m_hfa(inc_a, hf), gb = v.m_hfb(inc_b, hf);
        v.hfa += ga * (oa - v.hfa); v.hfb += gb * (ob - v.hfb); oa = v.hfa; ob = v.hfb;
    }
    {   // per-VCO coupling capacitor and level-OTA input
        const double kh = 1.0 - std::exp(-2 * PI * MIX_HP_HZ * dt);
        v.mx_a += kh * (oa - v.mx_a); oa -= v.mx_a; v.mx_b += kh * (ob - v.mx_b); ob -= v.mx_b;
        if (MIX_OTA_K > 0) { oa = ftanh(MIX_OTA_K * oa) / MIX_OTA_K; ob = ftanh(MIX_OTA_K * ob) / MIX_OTA_K; }
    }
    double x = mixer_gain(P.vol_a) * oa + mixer_gain(P.vol_b) * ob;
    if (P.noise) {   // band-limited white noise (HW: flat to 4 kHz, -4.5 dB at 16 kHz) -> one-pole at 11 kHz, rms kept
        v.nz += noise_c * (v.rnd() - v.nz);
        x += NOISE_FULL * v.mc_ndb(P.noise, [](double n) { return std::pow(10.0, interp(NOISE_DB, n) / 20.0); }) * v.nz * noise_norm;
    }
    x += CIRCUIT_NOISE * v.rnd();
    x += VOICE_NOISE * v.rnd();   // VCO/mixer hiss heard while the VCA is open (m_osc_full: ~-83 dB/bin at 2-12 kHz on the open triangle)
    x *= v.mc_cfl(P.cutoff, [](double c) { return std::pow(10.0, interp(CUTOFF_FLOOR_DB, c) / 20.0); });

    // cutoff
    double fvel = 1.0 - (P.filt_vel / 65535.0) * (1.0 - v.vel / 127.0);
    double oct = (P.fenv_amt - 32768.0) / 4096.0 * 1.38 * fe * fvel;
    static const double KT[3] = {0.0, 0.47, 0.945};
    oct += KT[std::min<int>(P.keytrk, 2)] * (v.pitch - (24 + 12 * P.kbd_ref)) / 12.0;
    // the VCOs are retuned to A440 (the unit's scale error PITCH_DEV removed); the keytracked filter moves with them so the
    // filter tone keeps its relation to the note's harmonics (self-oscillating patches lock like on the unit, A-59)
    oct -= KT[std::min<int>(P.keytrk, 2)] * interp(PITCH_DEV, v.pitch) / 1200.0;
    if (P.lfo_target & 2) oct += 4.5 * lfo_c / 1200.0;        // measured: filter swing = 4.3x the pitch swing (m_lfo_dest)
    if (P.bend_target == 2) oct += bend * (P.pitch_range / 2048.0) / 12.0 * 0.93;
    oct += 0.047 * pressure * (P.vcf_at / 32768.0);
    {   // filter CV (knob + env + keytrack + LFO + bend + AT, all from the DAC): S&H RC, then FSLEW (RC 15 ms; big steps pass
        // through the diodes, the output trails the input by the diode drop), then FSUM adds the audio-rate poly-mod
        double cvf = oct;   // the knob part is constant per patch; only the summed modulation needs the RC/slew
        const double kc = 1.0 - std::exp(-2 * PI * CV_RC_HZ * dt), ks = 1.0 - std::exp(-dt / FSLEW_TAU);
        if (!v.cv_ok) { v.cv_f = cvf; v.cv_fs = cvf; }
        v.cv_f += kc * (cvf - v.cv_f);
        double d = v.cv_f - v.cv_fs; v.cv_fs += ks * d;
        d = v.cv_f - v.cv_fs; if (d > FSLEW_OCT) v.cv_fs = v.cv_f - FSLEW_OCT; else if (d < -FSLEW_OCT) v.cv_fs = v.cv_f + FSLEW_OCT;
        oct = v.cv_fs;
        v.cv_ok = true;
    }
    if (P.pm_filt) oct += pm_src(P, v.b.ph, db, PMF_DC, PMF_AC, PMF_SQ) * std::pow(P.pm_oscb / 65535.0, 1.25);
    // ---- V3320 (grey-box, docs/GREYBOX_PLAN.md): 4 inverting OTA-C poles; resonance OTA fed from OUT4 (92k) against OUT2 (74k)
    // through C1 2.2 uF (QCOMP: the bass rises ~3 dB at the self-oscillation threshold instead of falling); oscillation at
    // 0.402 wc with loop gain 1.35. Mapping layer: wc from the measured peak table (peak = 0.402 wc), loop gain from the
    // resonance knob (linear, fit on m_filter_full), per-voice spreads as measured.
    // knob -> pole frequency as measured (GB_FC_RAW, compression included); the summed CV (env, keytrack, LFO ...) acts in the
    // uncompressed domain: unwarp the knob value, add the octaves, warp back
    double wk = GB_WC_SCALE * cutoff_hz_tab(P.cutoff, GB_FC_RAW);
    double wc;
    if (GB_WARP_F < 1e8) {
        // relative form: the knob value itself is never altered (also above the compression corner), only the modulation bends
        double r = std::min(0.95, std::pow(wk / GB_WARP_F, GB_WARP_N));
        double wl0 = wk / std::pow(1.0 - r, 1.0 / GB_WARP_N), wl = wl0 * std::exp2(oct);
        auto warp = [](double w) { return w / std::pow(1.0 + std::pow(w / GB_WARP_F, GB_WARP_N), 1.0 / GB_WARP_N); };
        wc = oct == 0.0 ? wk : wk * warp(wl) / warp(wl0);
    } else wc = wk * std::exp2(oct);
    if (u_vx > 0) wc *= v.rjf;
    if (u_vint > 0) wc *= v.m_vint(wc, u_vint, [&](double w, double vt) { return std::pow(VOICE_CUTOFF[i % NV_HW], vt) * std::exp2(vt * VOICE_CSLOPE[i % NV_HW] * std::log2(std::max(w, 1.0) / 1000.0)); });   // u_vint 0..1 scales the traits   // per-voice V/oct offset + scale
    wc = std::clamp(wc, GB_WC_MIN, std::min(GB_WC_MAX, 0.42 * sr));
    double gl = v.m_gl(wc, [&](double w) { return std::tan(PI * std::min(w + GB_LEAK_HZ, 0.45 * sr) * dt); }), Gl = gl / (1 + gl), H1 = 1 - Gl;
    const double G = Gl * wc / (wc + GB_LEAK_HZ);   // input gain of a leaky pole (DC gain wc/(wc+leak))
    const double ain = wc / (wc + GB_LEAK_HZ);
    double gh = std::tan(PI * GB_C1_HZ * dt), Gh = gh / (1 + gh);
    double vm = u_vint > 0 ? 1.0 + u_vint * (VOICE_RES_K[i % NV_HW] - 1.0) : 1.0;   // per-voice loop gain (m_voices_sweep)
    double K = v.mc_rk(P.res, [](double r) { return interp(GB_RES_K, r); }) * vm * RES_SCALE * (GB_KSLOPE != 0 ? v.m_ks(wc, [](double w) { return std::pow(w / 1000.0, GB_KSLOPE); }) : 1.0);
    double xin = GB_IN_POL * cell(GB_IN_POL * x, GB_IN_VS);                                   // IN1 gain cell (A+B summed here): one-sided cut-off
    double S1 = H1 * v.s[0], S2 = H1 * v.s[1], S3 = H1 * v.s[2], S4 = H1 * v.s[3];
    double gq = v.m_gq(wc, [&](double w) { return std::tan(PI * std::min(GB_RQ * w, 0.42 * sr) * dt); }), Gq = gq / (1 + gq), Sq = (1 - Gq) * v.s[4];
    double G2 = G * G, G4 = G2 * G2;
    // fb = OUT4 - [RHF OUT2 + (R - RHF) LP(OUT2)];  OUT2 = G2 u + (G S1 + S2),  OUT4 = G4 u + (G3 S1 + G2 S2 + G S3 + S4)
    double o2c = G * S1 + S2;
    double A = G4 - GB_RHF * G2 - (GB_R - GB_RHF) * Gq * G2;
    double B = (G * G2 * S1 + G2 * S2 + G * S3 + S4) - GB_RHF * o2c - (GB_R - GB_RHF) * (Gq * o2c + Sq);
    double c1 = 1 - Gh;                                                       // C1 high-pass (fb - its low-pass state)
    double u = 0, fbh = v.sfb;
    for (int it = 0; it < 2; ++it) {   // resonance OTA tanh: secant gain from the previous estimate, refined once
        double z = fbh / GB_RES_VT, ke = K * (std::fabs(z) < 1e-6 ? 1.0 : ftanh(z) / z);
        u = (xin - ke * c1 * (B - v.spost)) / (1 + ke * c1 * A);
        fbh = c1 * (A * u + B - v.spost);
    }
    u = GB_STG_ONE != 0 ? GB_STG_ONE * cell(GB_STG_ONE * u, GB_STG_VS, GB_STG_E) : GB_STG_VS * ftanh(u / GB_STG_VS);   // stage-1 cell input limit
    double y2 = 0, y = u;
    for (int st = 0; st < 4; ++st) {   // stages 2-4: each V3320 cell has its own input current limit (GB_STG2; 0 = off)
        if (st > 0 && GB_STG2 > 0) y = GB_STG_ONE != 0 ? GB_STG_ONE * cell(GB_STG_ONE * y, GB_STG2, GB_STG_E) : GB_STG2 * ftanh(y / GB_STG2);
        double vv = (ain * y - v.s[st]) * Gl; double o = vv + v.s[st]; v.s[st] = o + vv; y = o; if (st == 1) y2 = y; }
    double y2l; { double vv = (y2 - v.s[4]) * Gq; y2l = vv + v.s[4]; v.s[4] = y2l + vv; }
    { double fb = y - GB_RHF * y2 - (GB_R - GB_RHF) * y2l;
      double vv = (fb - v.spost) * Gh; double o = vv + v.spost; v.spost = o + vv; v.sfb = fb - o; }
    y *= GB_OUT;
    // VCA
    double avel = 1.0 - (P.amp_vel / 65535.0) * (1.0 - std::pow(v.vel / 127.0, 1.15));
    double vca = ae * avel * steal_gain;
    // lfo_target bit 64 ("VCA" in the community docs): no effect on the unit (m_lfo_dest)
    return y * vca;
}

void Engine::render(float* out, int n) {
    double lhz = P.lfo_range ? interp(LFO_HZ_FAST, P.lfo_freq, true) : interp(LFO_HZ_SLOW, P.lfo_freq, true);
    if (lfo_sync) lhz = bpm / 60.0 / LFO_BEATS[lfo_div];   // tempo-synced LFO: one cycle per division
    double vhz = interp(VIB_HZ, P.vib_speed, true);
    double nc = 1.0 - std::exp(-2 * PI * 40.0 * dt), nscale = 0.36 / 0.577 / std::sqrt(nc / (2 - nc));  // noise LFO: sd 0.36, ~40 Hz wide (m_mod_full)
    double hpa = 1.0 / (1.0 + 2 * PI * OUT_HP_HZ * dt);
    double gain = cal ? OUT_GAIN_CAL : 0.25;
    {   // VCO drift: per oscillator OU process, tau DRIFT_TAU, rms DRIFT_C (recordings/drift: 0.2-0.3 c over hours after warm-up)
        double T = 2.0 * n * dt, a = std::exp(-T / DRIFT_TAU), b = DRIFT_C * std::sqrt(1 - a * a);
        for (int i = 0, N = std::max(nvoices, pool()); i < N; ++i) { Voice& v = V[i]; v.drift_a = a * v.drift_a + b * gauss(rng); v.drift_b = a * v.drift_b + b * gauss(rng); }
    }
    auto shared = [&](int s, double& lfo, double& vib) {   // per-sample global modulation (LFO, vibrato, arpeggio, knob glide)
        if (glide_p && (s & 15) == 0) {   // knob glide, every 16 samples: one-pole ~10 ms
            const double kg = 1.0 - std::exp(-16.0 * dt / 0.010); double* tv[64]; int m = 0; bool more = false;
            Pt.each_cont([&](double& x) { tv[m++] = &x; }); m = 0;
            P.each_cont([&](double& x) { double t = *tv[m++]; x += kg * (t - x); if (std::fabs(t - x) > 0.5) more = true; else x = t; });
            glide_p = more;
        }
        double p = lfo_ph;
        switch (P.lfo_shape) {
        case 0: lfo = p < 0.5 ? 1.0 : -1.0; break;
        case 1: lfo = p < 0.5 ? 4 * p - 1 : 3 - 4 * p; break;
        case 2: lfo = lfo_rand; break;
        case 3: lfo = std::sin(2 * PI * p); break;
        case 5: lfo = 2 * p - 1; break;
        default: lfo = 0; break;
        }
        double half = std::floor(lfo_ph * 2);
        lfo_ph += lhz * dt; if (lfo_ph >= 1) lfo_ph -= 1;
        if (std::floor(lfo_ph * 2) != half) lfo_rand = uni(rng);          // S&H: new value twice per period
        lfo_noise += nc * (uni(rng) * nscale - lfo_noise);
        double vt = vib_ph < 0.5 ? 4 * vib_ph - 1 : 3 - 4 * vib_ph; vib_ph += vhz * dt; if (vib_ph >= 1) vib_ph -= 1;
        vib = vt * 700.0 * (P.vib_amt / 65535.0) * (P.mw_target == 1 ? wheel : 1.0);   // measured 1400 c p-p at max
        if (arp_mode && !arp_held.empty()) {
            arp_t += dt; double L = arp_step_len();
            if (arp_off_t >= 0 && arp_t >= arp_off_t && arp_note >= 0) {   // gate: the step's note ends at arp_gate of the step
                int k = arp_note; arp_note = -1; arp_off_t = -1; for (auto& v : V) if (v.gate && v.note == k) { v.gate = false; v.aenv.gate_off(); v.fenv.gate_off(); } }
            if (arp_t >= L) { arp_t -= L; arp_step(); arp_off_t = arp_gate * arp_step_len(); }
        }
        lfo_pw_sm += pw_cv_c * ((P.lfo_shape == 4 ? lfo_noise : lfo) - lfo_pw_sm);   // PW CV is shared and smoothed on the unit
    };
    auto output = [&](int s, double mix) {   // DC block, output filters, 2x decimation
        double y = hpa * (hp_y + mix - hp_x); hp_x = mix; hp_y = y;
        { double o = lp1_b * y + lp1_b * lp1_x - lp1_a * lp1_y; lp1_x = y; lp1_y = o; y = o; }
        { double o = bq_b0 * y + bq_z1; bq_z1 = bq_b1 * y - bq_a1 * o + bq_z2; bq_z2 = bq_b2 * y - bq_a2 * o; y = o; }
        dec_buf[dec_pos] = y; dec_pos = (dec_pos + 1) % DEC_TAPS;
        if (s & 1) {
            double acc = 0; int j = dec_pos;
            for (int k = 0; k < DEC_TAPS; ++k) { acc += dec_h[k] * dec_buf[j]; j = (j + 1) % DEC_TAPS; }
            out[s >> 1] = (float)(acc * gain);
        }
    };
    const int N = std::max(nvoices, pool()), S = 2 * n;
    int act[NV], na = 0;
    for (int i = 0; i < N; ++i) if (i < NV_HW ? (mask >> i & 1) : true) act[na++] = i;
    bool serial = glide_p || (arp_mode && !arp_held.empty());
    int busy = 0; for (int k = 0; k < na; ++k) { const Voice& v = V[act[k]]; if (v.steal > 0) serial = true; if (v.gate || v.aenv.st != Env::Idle) ++busy; }
    if (nthreads < 0) nthreads = std::max(0, std::min(3, (int)std::thread::hardware_concurrency() - 1));
    if (serial || nthreads == 0 || busy < 12) {
        for (int s = 0; s < S; ++s) {
            double lfo, vib; shared(s, lfo, vib);
            double mix = 0;
            for (int k = 0; k < na; ++k) mix += tick_voice(V[act[k]], act[k], lfo, lfo_noise, vib, lfo_pw_sm);
            output(s, mix);
        }
        return;
    }
    if ((int)sh_lfo.size() < S) { sh_lfo.resize(S); sh_ln.resize(S); sh_vib.resize(S); sh_pw.resize(S); }
    if ((int)vbuf.size() < NV * S) vbuf.resize(NV * S);
    for (int s = 0; s < S; ++s) { double lfo, vib; shared(s, lfo, vib); sh_lfo[s] = lfo; sh_ln[s] = lfo_noise; sh_vib[s] = vib; sh_pw[s] = lfo_pw_sm; }
    if (!pool_) pool_ = new Pool(nthreads);
    const int nw = pool_->nw + 1;
    pool_->run([&](int w) {
        for (int k = w; k < na; k += nw) { double* b = &vbuf[(size_t)k * S]; Voice& v = V[act[k]];
            for (int s = 0; s < S; ++s) b[s] = tick_voice(v, act[k], sh_lfo[s], sh_ln[s], sh_vib[s], sh_pw[s]); }
    });
    for (int s = 0; s < S; ++s) { double mix = 0; for (int k = 0; k < na; ++k) mix += vbuf[(size_t)k * S + s]; output(s, mix); }
}

}  // namespace p800

extern "C" {
void* p800_create(double sr) { return new p800::Engine(sr); }
void p800_destroy(void* e) { delete (p800::Engine*)e; }
int p800_set_patch(void* e, const uint8_t* d, int n) { p800::Patch p; if (!p.parse(d, n)) return 0; ((p800::Engine*)e)->set_patch(p); return 1; }
int p800_set_patch_smooth(void* e, const uint8_t* d, int n) { p800::Patch p; if (!p.parse(d, n)) return 0; ((p800::Engine*)e)->set_patch(p, true); return 1; }
void p800_set_mask(void* e, int m) { ((p800::Engine*)e)->set_voice_mask((uint8_t)m); }
void p800_set_voices(void* e, int n) { ((p800::Engine*)e)->set_voices(n); }
void p800_set_unison_voices(void* e, int n) { ((p800::Engine*)e)->set_unison_voices(n); }
void p800_set_poly_unison(void* e, int on) { ((p800::Engine*)e)->set_poly_unison(on != 0); }
void p800_set_mono(void* e, int on) { ((p800::Engine*)e)->set_mono(on != 0); }
void p800_midi(void* e, int st, int d1, int d2) { ((p800::Engine*)e)->midi((uint8_t)st, (uint8_t)d1, (uint8_t)d2); }
void p800_render(void* e, float* out, int n) { ((p800::Engine*)e)->render(out, n); }
void p800_set_tempo(void* e, double bpm) { ((p800::Engine*)e)->set_tempo(bpm); }
void p800_set_arp(void* e, int mode, int oct, int div) { ((p800::Engine*)e)->set_arp(mode, oct, div); }
void p800_set_lfo_sync(void* e, int on, int div) { ((p800::Engine*)e)->set_lfo_sync(on != 0, div); }
void p800_set_seed(void* e, unsigned s) { ((p800::Engine*)e)->set_seed(s); }
void p800_set_threads(void* e, int n) { ((p800::Engine*)e)->set_threads(n); }
// calibration constants (tools/cmp_*.py); returns 0 for an unknown name
int p800_set_cal(const char* name, double v) {
    using namespace p800;
    static const struct { const char* n; double* p; } T[] = {{"PMF_DC", &PMF_DC}, {"PMF_AC", &PMF_AC}, {"PMF_SQ", &PMF_SQ}, {"PMA_DC", &PMA_DC}, {"PMA_AC", &PMA_AC}, {"PMA_SQ", &PMA_SQ}, {"TRI_SKEW", &TRI_SKEW}, {"VINTAGE", &VINTAGE}, {"VINTAGE_X", &VINTAGE_X}, {"TUNE_C", &TUNE_C}, {"RES_SCALE", &RES_SCALE}, {"FENV_T", &FENV_T}, {"EXP_FALL_TAU", &EXP_FALL_TAU}, {"EXP_FALL_UNDER", &EXP_FALL_UNDER}, {"PW_CV_HZ", &PW_CV_HZ}, {"PM_TRI", &PM_TRI}, {"VCA_RING_F", &VCA_RING_F}, {"VCA_RING_Q", &VCA_RING_Q}, {"VCA_RING_K", &VCA_RING_K}, {"FREE_OFFSET", &FREE_OFFSET}, {"VOICE_NOISE", &VOICE_NOISE}, {"OSC_HF_FC", &OSC_HF_FC}, {"OSC_HF_SLOPE", &OSC_HF_SLOPE}, {"TRI_DLY", &TRI_DLY}, {"SQR_DLY", &SQR_DLY}, {"EDGE_DLY", &EDGE_DLY}, {"A_DETUNE", &A_DETUNE}, {"CIRCUIT_NOISE", &CIRCUIT_NOISE}, {"SYNC_DELTA", &SYNC_DELTA}, {"SYNC_TIME", &SYNC_TIME}, {"SYNC_TAU", &SYNC_TAU}, {"PW_MONO", &PW_MONO}, {"EXP_ATT_TAU", &EXP_ATT_TAU}, {"EXP_ATT_TARGET", &EXP_ATT_TARGET}, {"OUT_LP_HZ", &OUT_LP_HZ}, {"OUT_LP2_HZ", &OUT_LP2_HZ}, {"OUT_LP2_Q", &OUT_LP2_Q}, {"NOISE_LP_HZ", &NOISE_LP_HZ}, {"CV_RC_HZ", &CV_RC_HZ}, {"FSLEW_TAU", &FSLEW_TAU}, {"FSLEW_OCT", &FSLEW_OCT}, {"GB_WARP_F", &GB_WARP_F}, {"GB_WARP_N", &GB_WARP_N}, {"GB_WC_MAX", &GB_WC_MAX}, {"MIX_HP_HZ", &MIX_HP_HZ}, {"MIX_OTA_K", &MIX_OTA_K}, {"DRIFT_TAU", &DRIFT_TAU}, {"DRIFT_C", &DRIFT_C}, {"ASLEW_TAU", &ASLEW_TAU}, {"ASLEW_V", &ASLEW_V}, {"GB_KNOB0", &GB_KNOB0}, {"AMP_ATT_START", &AMP_ATT_START}, {"OSC_SPREAD", &OSC_SPREAD}, {"GB_R", &GB_R}, {"GB_RHF", &GB_RHF}, {"GB_RQ", &GB_RQ}, {"GB_C1_HZ", &GB_C1_HZ}, {"GB_WC_SCALE", &GB_WC_SCALE}, {"GB_RES_VT", &GB_RES_VT}, {"GB_LEAK_HZ", &GB_LEAK_HZ}, {"GB_KSLOPE", &GB_KSLOPE}, {"GB_IN_VS", &GB_IN_VS}, {"GB_IN_POL", &GB_IN_POL}, {"GB_CELL_E", &GB_CELL_E}, {"GB_STG_VS", &GB_STG_VS}, {"GB_STG2", &GB_STG2}, {"GB_STG_ONE", &GB_STG_ONE}, {"GB_STG_E", &GB_STG_E}, {"GB_OUT", &GB_OUT}};
    for (auto& t : T) if (std::string(t.n) == name) { *t.p = v; return 1; }
    return 0;
}
}
