// PRO8 effects: BBD chorus (I / II / I+II, rate, depth, mix), delay (tape / BBD / digital ping-pong, free or tempo-synced),
// reverb (plate / hall / cloud with shimmer). Runs after the voice engine (mono in, stereo out); the engine itself is untouched.
// Parameters are stored with each preset as 20 values 0..65535 (FX_KEYS order), see app/p800app.mm (SysEx block 0x7D, .fxp).
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <array>

namespace p800fx {

static const char* FX_KEYS[] = {"ch_on", "ch_mode", "ch_rate", "ch_depth", "ch_mix",
                                "dl_on", "dl_type", "dl_sync", "dl_time", "dl_div", "dl_fb", "dl_tone", "dl_mix",
                                "rv_on", "rv_type", "rv_size", "rv_pre", "rv_damp", "rv_mix", "rv_shim"};
constexpr int FX_N = 20;
// delay divisions in beats: 1/2, 1/4 dotted, 1/4, 1/8 dotted, 1/4 triplet, 1/8, 1/8 triplet, 1/16 dotted, 1/16, 1/16 triplet, 1/32
static const double DL_BEATS[11] = {2.0, 1.5, 1.0, 0.75, 2.0 / 3, 0.5, 1.0 / 3, 0.375, 0.25, 1.0 / 6, 0.125};
// defaults (0..65535): chorus off, mode I, rate/depth centre, mix 50 %; delay off, tape, synced 1/8 dotted, fb 35 %,
// tone centre, mix 25 %; reverb off, hall, size 55 %, pre 15 %, damp 40 %, mix 25 %, shimmer 0
// enums: index i is stored as i * 21846 (3 choices) or i * 5958 + 100 (11 delay divisions); switches 0 / 65535
static const uint16_t FX_DEFAULT[FX_N] = {0, 0, 32768, 32768, 32768,
                                          0, 0, 65535, 26000, 3 * 5958 + 100, 23000, 32768, 16384,
                                          0, 21846, 36000, 9830, 26214, 16384, 0};

static const double PI = 3.14159265358979323846;
struct OnePole { double a = 0, z = 0; void lp(double hz, double sr) { a = 1.0 - std::exp(-2 * PI * hz / sr); } double operator()(double x) { z += a * (x - z); return z; } };
struct Line {   // circular delay line with cubic (Hermite) fractional read
    std::vector<float> b; int w = 0, m = 0;
    void init(int n) { int s = 1; while (s < n + 4) s <<= 1; b.assign(s, 0.f); m = s - 1; w = 0; }
    void push(double x) { b[w] = (float)x; w = (w + 1) & m; }
    double tap(double d) const {   // d samples back (>= 1)
        double p = w - d; int i = (int)std::floor(p); double f = p - i;
        double y0 = b[(i - 1) & m], y1 = b[i & m], y2 = b[(i + 1) & m], y3 = b[(i + 2) & m];
        double c1 = 0.5 * (y2 - y0), c2 = y0 - 2.5 * y1 + 2 * y2 - 0.5 * y3, c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
        return ((c3 * f + c2) * f + c1) * f + y1;
    }
    double at(int d) const { return b[(w - d) & m]; }
};
struct Allpass { Line l; int d = 1; double g = 0.6; void init(int n, double gg) { l.init(n + 2); d = n; g = gg; }
    double operator()(double x) { double v = l.at(d); double u = x + g * v; l.push(u); return v - g * u; } };

class FX {
public:
    explicit FX(double sr = 48000) { set_sample_rate(sr); for (int k = 0; k < FX_N; ++k) p[k] = FX_DEFAULT[k]; }
    void set_sample_rate(double s) {
        sr = s; ch.init((int)(0.03 * sr)); dlL.init((int)(2.6 * sr)); dlR.init((int)(2.6 * sr)); pre.init((int)(0.25 * sr));
        static const double PL[8] = {0.0213, 0.0279, 0.0331, 0.0397, 0.0437, 0.0491, 0.0553, 0.0617};   // seconds, mutually prime-ish
        for (int i = 0; i < 8; ++i) { fdn[i].init((int)(PL[i] * 2.6 * sr) + 64); base[i] = PL[i]; }
        static const double DF[4] = {0.0047, 0.0036, 0.0127, 0.0093};
        for (int i = 0; i < 4; ++i) dif[i].init((int)(DF[i] * sr), 0.7);
        sh.init((int)(0.2 * sr)); tone_l.lp(9000, sr); tone_r.lp(9000, sr);
    }
    void set(int k, int v) { if (k >= 0 && k < FX_N) p[k] = std::clamp(v, 0, 65535); }
    int get(int k) const { return p[k]; }
    void set_tempo(double b) { bpm = std::max(20.0, b); }
    void reset() { for (auto& f : fdn) std::fill(f.b.begin(), f.b.end(), 0.f); std::fill(dlL.b.begin(), dlL.b.end(), 0.f); std::fill(dlR.b.begin(), dlR.b.end(), 0.f); }

    // mono in -> stereo out (in place safe for L == in)
    void process(const float* in, float* L, float* R, int n) {
        auto u = [&](int k) { return p[k] / 65535.0; };
        const bool chOn = p[0] > 32767, dlOn = p[5] > 32767, rvOn = p[13] > 32767;
        const int chMode = std::min(2, p[1] * 3 / 65536), dlType = std::min(2, p[6] * 3 / 65536), rvType = std::min(2, p[14] * 3 / 65536);
        // chorus: BBD, two taps modulated in anti-phase (L / R); I slow and shallow, II faster and deeper, I+II both LFOs
        const double rmul = std::pow(4.0, 2 * u(2) - 1), dmul = 2 * u(3), chMix = u(4);
        const double r1 = (chMode == 1 ? 0.83 : 0.5) * rmul, r2 = 0.83 * rmul, dep = (chMode == 1 ? 2.2e-3 : 1.6e-3) * dmul * sr, base_d = 3.6e-3 * sr;
        // delay
        double dsec = p[7] > 32767 ? DL_BEATS[std::min(10, p[9] * 11 / 65536)] * 60.0 / bpm : 0.01 * std::pow(200.0, u(8));
        dsec = std::min(dsec, 2.5); const double dtarget = dsec * sr, fb = 0.95 * u(10), dlMix = u(12);
        double toneHz = dlType == 0 ? 1500 * std::pow(6.0, u(11)) : dlType == 1 ? 900 * std::pow(5.0, u(11)) : 2500 * std::pow(7.0, u(11));
        tone_l.lp(toneHz, sr); tone_r.lp(toneHz, sr); hp_a = 1.0 - std::exp(-2 * PI * 70 / sr);
        // reverb
        const double size = u(15), damp = u(17), rvMix = u(18), shim = rvType == 2 ? u(19) : 0.0;
        const double rt60 = rvType == 0 ? 0.6 + 3.4 * size : rvType == 1 ? 1.2 + 7.0 * size : 3.0 + 17.0 * size;
        const double scale = rvType == 0 ? 0.55 + 0.25 * size : rvType == 1 ? 0.9 + 0.6 * size : 1.4 + 1.2 * size;
        const double dampHz = (rvType == 0 ? 12000 : rvType == 1 ? 8000 : 6000) * std::pow(0.12, damp);
        const double modDepth = (rvType == 2 ? 0.0024 : rvType == 1 ? 0.0006 : 0.0003) * sr;
        const int preN = (int)(u(16) * 0.2 * sr);
        double g[8], dl[8]; for (int i = 0; i < 8; ++i) { dl[i] = base[i] * scale * sr; g[i] = std::pow(10.0, -3.0 * dl[i] / (rt60 * sr)) * (1.0 - 0.12 * shim); }   // shimmer adds loop gain
        const double da = 1.0 - std::exp(-2 * PI * dampHz / sr);
        for (int i = 0; i < 4; ++i) dif[i].g = rvType == 0 ? 0.75 : 0.65;

        for (int i = 0; i < n; ++i) {
            double x = in[i], l = x, r = x;
            if (chOn) {
                ph1 += r1 / sr; if (ph1 >= 1) ph1 -= 1; ph2 += r2 / sr; if (ph2 >= 1) ph2 -= 1;
                auto tri = [](double ph) { return ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph; };
                double m = chMode == 2 ? 0.6 * tri(ph1) + 0.4 * tri(ph2) : tri(ph1);
                ch.push(x);
                double wl = bbd_l(ch.tap(base_d + dep * (1 + m) * 0.5 + 1)), wr = bbd_r(ch.tap(base_d + dep * (1 - m) * 0.5 + 1));
                l = x + chMix * (wl - x * 0.0); r = x + chMix * (wr - x * 0.0);
                l *= 1.0 / (1 + 0.5 * chMix); r *= 1.0 / (1 + 0.5 * chMix);
            }
            if (dlOn) {
                dcur += (dtarget - dcur) * (dlType == 0 ? 0.00008 : 0.0004);   // tape glides (pitch bend), others move faster
                double wob = 0;
                if (dlType == 0) { wph += 0.55 / sr; fph += 5.8 / sr; if (wph >= 1) wph -= 1; if (fph >= 1) fph -= 1; wob = (0.0011 * std::sin(2 * PI * wph) + 0.00018 * std::sin(2 * PI * fph)) * sr; }
                if (dlType == 1) { wph += 0.3 / sr; if (wph >= 1) wph -= 1; wob = 0.0007 * std::sin(2 * PI * wph) * sr; }
                double d = std::max(2.0, dcur + wob), yl, yr;
                if (dlType == 2) {   // digital ping-pong: L -> R -> L
                    yl = dlL.tap(d); yr = dlR.tap(d);
                    double fl = tone_l(yl), fr = tone_r(yr);
                    dlL.push(0.5 * (l + r) + fb * fr); dlR.push(fb * fl);
                    yl = fl; yr = fr;
                } else {             // tape / BBD: one line, saturated, darker on every pass; R reads a little later for width
                    yl = dlL.tap(d); yr = dlL.tap(d + 0.011 * sr);
                    double f = tone_l(yl); hp_z += hp_a * (f - hp_z); f -= hp_z;
                    double drive = dlType == 0 ? 1.6 : 1.2; f = std::tanh(drive * f) / drive;
                    dlL.push(0.5 * (l + r) + fb * f);
                    yl = tone_r(yl); yr = yr * 0.95;
                }
                l += dlMix * yl; r += dlMix * yr;
            }
            if (rvOn) {
                pre.push(0.5 * (l + r)); double s = preN > 0 ? pre.at(preN) : 0.5 * (l + r);
                for (auto& a : dif) s = a(s);
                double o[8];
                mph += (rvType == 2 ? 0.35 : 0.8) / sr; if (mph >= 1) mph -= 1;
                for (int k = 0; k < 8; ++k) {
                    double md = modDepth * std::sin(2 * PI * (mph + k * 0.125));
                    o[k] = fdn[k].tap(std::max(4.0, dl[k] + md)); dz[k] += da * (o[k] - dz[k]); o[k] = dz[k];
                }
                double sum = 0; for (int k = 0; k < 8; ++k) sum += o[k]; sum *= 0.25;   // Householder: o - 2/N sum
                double shv = 0;
                if (shim > 0) {   // octave up: two crossfaded grains reading at double speed
                    sh.push(0.5 * (o[0] + o[3])); const double G = 0.1 * sr;
                    gph += 1.0 / G; if (gph >= 1) gph -= 1;
                    auto grain = [&](double ph) { double d = G * (1 - ph) + 2; return sh.tap(d) * std::sin(PI * ph); };
                    shv = shim * 0.18 * (grain(gph) + grain(std::fmod(gph + 0.5, 1.0)));
                }
                for (int k = 0; k < 8; ++k) fdn[k].push(g[k] * (o[k] - sum) + (k & 1 ? -s : s) * 0.5 + (k < 2 ? shv * g[k] : 0));
                double wl = o[0] - o[2] + o[4] - o[6], wr = o[1] - o[3] + o[5] - o[7];
                l += rvMix * 0.7 * wl; r += rvMix * 0.7 * wr;
            }
            L[i] = (float)l; R[i] = (float)r;
        }
    }
private:
    double sr = 48000, bpm = 120;
    int p[FX_N];
    Line ch, dlL, dlR, pre, sh; std::array<Line, 8> fdn; std::array<Allpass, 4> dif; double base[8] = {};
    double ph1 = 0, ph2 = 0.25, wph = 0, fph = 0, mph = 0, gph = 0, dcur = 12000, hp_a = 0, hp_z = 0, dz[8] = {};
    OnePole tone_l, tone_r;
    double bl1 = 0, bl2 = 0, br1 = 0, br2 = 0;
    double bbd_l(double x) { const double a = 1.0 - std::exp(-2 * PI * 9000 / sr); bl1 += a * (x - bl1); bl2 += a * (bl1 - bl2); return bl2; }   // BBD anti-alias / reconstruction
    double bbd_r(double x) { const double a = 1.0 - std::exp(-2 * PI * 9000 / sr); br1 += a * (x - br1); br2 += a * (br1 - br2); return br2; }
};

}  // namespace p800fx
