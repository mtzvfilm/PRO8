// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 mtzvfilm. PRO8 - https://github.com/mtzvfilm/PRO8
// PRO-800 measured laws (docs/RESULTS.md). All knob values are 0..65535 as in the patch (SysEx) data.
#pragma once
#include <cmath>
#include <cstddef>

namespace p800 {

struct Pt { double x, y; };

// piecewise-linear interpolation (optionally in log domain of y)
template <size_t N>
inline double interp(const Pt (&t)[N], double x, bool logy = false) {
    if (x <= t[0].x) return t[0].y;
    if (x >= t[N - 1].x) return t[N - 1].y;
    size_t i = 1;
    while (t[i].x < x) ++i;
    double f = (x - t[i - 1].x) / (t[i].x - t[i - 1].x);
    if (logy) return std::exp(std::log(t[i - 1].y) + f * (std::log(t[i].y) - std::log(t[i - 1].y)));
    return t[i - 1].y + f * (t[i].y - t[i - 1].y);
}

// ---- oscillators -------------------------------------------------------------------------------
// mixer: amplitude linear with a dead zone up to ~5240
inline double mixer_gain(double v) { double g = (v - 5240.0) / (65535.0 - 5240.0); return g < 0 ? 0 : g; }
// pulse high time vs knob (m_osc_full: 27 values, C3, raw waveform orientation + harmonic fit)
static const Pt PW_HIGH[] = {{0, 0.9192}, {2580, 0.8862}, {5160, 0.8528}, {7740, 0.8174}, {10320, 0.7859}, {12901, 0.7520}, {15481, 0.7186}, {18061, 0.6846}, {20641, 0.6512}, {23221, 0.6183}, {25801, 0.5843}, {28381, 0.5504}, {30961, 0.5165}, {33542, 0.4855}, {36122, 0.4526}, {38702, 0.4202}, {41282, 0.3862}, {43862, 0.3533}, {46442, 0.3209}, {49022, 0.2874}, {51602, 0.2545}, {54182, 0.2211}, {56763, 0.1876}, {59343, 0.1552}, {61923, 0.1213}, {64503, 0.0878}, {65535, 0.0734}};
// pulse width: duty vs knob (symmetric around 32768) - old harmonic fit, kept for reference
static const Pt PW_DUTY[] = {{0, .081}, {4096, .134}, {8192, .187}, {16384, .294}, {24576, .400}, {32768, .494},
                             {40960, .400}, {49152, .294}, {57344, .187}, {61440, .132}, {65535, .075}};
// noise level (amplitude relative to max) vs knob
static const Pt NOISE_DB[] = {{0, -120}, {4096, -40}, {8192, -26.8}, {16384, -14.9}, {32768, -5.8}, {49152, -2.7}, {65535, 0}};

// ---- filter ------------------------------------------------------------------------------------
// cutoff knob -> fc (Hz, the -12 dB point of 4 equal poles), voice 1; below 10239 extended by the A3 saw level test
// (16:58): the old noise fit there (floor ~110 Hz + passband loss) was an artefact
static const Pt CUTOFF_HZ[] = {{0, 30}, {2048, 48}, {4096, 71}, {6143, 102}, {8191, 123}, {10239, 158.5}, {12287, 195.2}, {14335, 240.5},
                               {16383, 286.9}, {18431, 347.8}, {20479, 428.5}, {22527, 519.5}, {24575, 609.9},
                               {26623, 716.0}, {28671, 882.1}, {30719, 1069.4}, {32767, 1296.5}, {34815, 1571.7},
                               {36863, 1967.6}, {38911, 2310.0}, {40959, 2891.8}, {43007, 3505.8}, {45055, 4459.7},
                               {47103, 5494.0}, {49151, 6989.0}, {51199, 8472.9}, {53247, 10606.9}, {55295, 15841.4},
                               {57343, 20809.0}, {59391, 25635.3}, {61439, 34772.3}, {63487, 50292.3}, {65535, 60000}};
// passband gain (dB) at the bottom of the cutoff range (filter floors and attenuates)
static const Pt CUTOFF_FLOOR_DB[] = {{0, 0}, {65535, 0}};   // no passband floor: the low end is a plain exponential fc law
// resonance knob -> feedback k (4 poles at fc + one feedback pole at 0.437 fc; k_crit = 3.232)
static const double FB_POLE_RATIO = 0.437;

// ---- envelopes ---------------------------------------------------------------------------------
// full-scale linear segment time T (s). Attack from the 10->90 % slope, decay/release from the -20 dB point (0.9 T);
// then x1.046 / x1.015 so that m_env.py on the engine reproduces the hardware numbers (same 1 ms RMS method).
static const Pt ATT_T_FAST[] = {{0, 0.002615}, {8192, 0.0065375}, {16384, 0.022228}, {24576, 0.06276}, {32768, 0.12029}, {40960, 0.29942}, {49152, 0.63675}, {57344, 1.0957}, {65535, 1.5952}};
static const Pt ATT_T_SLOW[] = {{0, 0.0039225}, {8192, 0.035303}, {16384, 0.14775}, {24576, 0.4354}, {32768, 0.84072}, {40960, 2.0962}, {49152, 4.469}, {57344, 7.6818}, {65535, 11.15}};
static const Pt DEC_T_FAST[] = {{0, 0.0011165}, {8192, 0.004466}, {16384, 0.0203}, {24576, 0.0609}, {32768, 0.11957}, {40960, 0.29658}, {49152, 0.63833}, {57344, 1.0973}, {65535, 1.5992}};
static const Pt DEC_T_SLOW[] = {{0, 0.004466}, {8192, 0.036134}, {16384, 0.14657}, {24576, 0.43422}, {32768, 0.84133}, {40960, 2.1021}, {49152, 4.4863}, {57344, 7.713}, {65535, 11.246}};
// release: own law (measured -20 dB points / 0.9, x1.015): never shorter than ~17 ms (fast) / ~11 ms (slow)
static const Pt REL_T_FAST[] = {{0, .0169}, {8192, .0193}, {16384, .0284}, {24576, .069}, {32768, .128}, {40960, .308},
                                {49152, .647}, {57344, 1.107}, {65535, 1.61}};
static const Pt REL_T_SLOW[] = {{0, .0113}, {8192, .0429}, {16384, .156}, {24576, .444}, {32768, .849}, {40960, 2.11},
                                {49152, 4.5}, {57344, 7.72}, {65535, 11.2}};
// attack from zero starts at -4.5 % (measured start delay = 4.5 % of T on every setting)
static const double ATT_START = -0.045;
// sustain knob -> level (amplitude)
static const Pt SUSTAIN[] = {{0, 0}, {5240, 0.002}, {8192, .088}, {16384, .217}, {24576, .348}, {32768, .479},
                             {40960, .610}, {49152, .740}, {57344, .870}, {65535, 1.0}};
static const double EXP_TAU = 0.39;  // exp shape: time constant = 0.39 T

// ---- LFO / vibrato / delay / glide -------------------------------------------------------------
static const Pt LFO_HZ_FAST[] = {{0, 0.1424}, {3612, 0.2034}, {6708, 0.2705}, {10320, 0.3574}, {13933, 0.4702}, {17029, 0.5966}, {20641, 0.7891}, {24253, 1.039}, {27349, 1.322}, {30961, 1.743}, {34574, 2.296}, {38186, 3.024}, {41282, 3.841}, {44894, 5.053}, {48506, 6.656}, {51602, 8.496}, {55215, 11.09}, {58827, 14.79}, {61923, 18.15}, {65535, 23.49}};   // m_mod_full PWM takes, 20 rates
static const Pt LFO_HZ_SLOW[] = {{0, .0391}, {8192, .0448}, {16384, .0643}, {24576, .1336}, {32768, .251},
                                 {40960, .439}, {49152, .882}, {57344, 1.655}, {65535, 3.097}};
// LFO amount -> peak-to-peak cents on pitch (x1.126: raw track of the shape test, the first sweep was smoothed)
static const Pt LFO_PP_CENTS[] = {{0, 0}, {4096, 367}, {8192, 747}, {16384, 1491}, {32768, 2924}, {49152, 4408}, {65535, 5720}};   // p-p cents; scaled by the unit/engine ratios of m_mod_full (tracker-clipping safe)
// mod delay: time until the modulation starts; it reaches full depth at 1.43x that time
static const Pt MOD_DELAY_S[] = {{0, 0}, {15480, .15}, {30960, .92}, {46440, 2.85}, {65535, 10.0}};   // m_mod_full: MIDI 30 / 60 / 90
// vibrato speed
static const Pt VIB_HZ[] = {{0, 0.33}, {16384, 1.14}, {32768, 3.99}, {49152, 12.48}, {65535, 33.3}};   // measured (m_all)
// glide: linear in pitch; full time (s) for the slide (constant-time mode) or per octave (constant-rate mode)
static const Pt GLIDE_T[] = {{0, 0}, {8192, .01}, {16384, .067}, {32768, .378}, {40960, .944}, {49152, 2.0},
                             {57344, 3.43}, {65535, 5.0}};

// ---- unison detune: linear fan per voice (m_filter_full unidet_*, detune MIDI 8..48, residual <= 2 c): cents per detune step
// (MIDI) and a small offset of the down-going voices; the old 5-point table below was wrong for voices 5-8 (tracker wrapped)
static const double DETUNE_SLOPE[8] = {0.8097, -0.8038, 1.6204, -1.6051, 2.3761, -2.3487, 3.1469, -3.0885};
static const double DETUNE_OFS[8] = {-0.22, 4.21, 0.14, 3.62, 0.32, 3.32, 0.90, 3.60};
// (old) cents per voice vs detune knob (measured, unison pattern 00) ---------------

// poly-mod osc B -> freq A: amount -> fraction of the maximum shift (m_mod_full, carrier line vs amount, MIDI x 516)
static const Pt PM_FREQ_AMT[] = {{0, 0}, {8256, .044}, {16512, .181}, {24768, .324}, {33024, .468}, {41280, .609},
                                 {49536, .750}, {57792, .887}, {65535, 1.013}};

// ---- per-voice character (measured): cutoff factor and resonance peak at res 49152 --------------
// per-voice V3320 control scale (m_voices_sweep 25.11, self-oscillation at 330 Hz and 2.8 kHz): offset at 1 kHz, octaves per octave
static double VOICE_CUTOFF[8] = {0.99987, 0.99949, 0.99338, 0.99849, 0.99125, 0.99477, 0.99500, 0.98807};
static double VOICE_CSLOPE[8] = {0.00142, 0.00264, 0.00433, 0.00603, 0.00819, 0.01033, 0.01338, 0.01794};

// ---- the unit's scale (voice 1 osc A saw, m_osc_full 24.11 19:00, A4 offset removed): cents vs MIDI note
static const Pt PITCH_DEV[] = {{0, 300}, {24, 136.2}, {31, 52.2}, {36, 20.2}, {43, 16.2}, {48, 14.2}, {55, 9.2}, {60, 6.2},
                               {67, 1.2}, {72, -1.8}, {79, -6.8}, {84, -10.8}, {91, -15.8}, {96, -19.8}, {127, -40}};
// ---- per-oscillator tuning around A440 (recordings/drift: 34 probes over 20 h, mean of the 16 oscillators removed = the
// unit's scale error, which is not emulated): cents at MIDI 33 / 69 / 93, osc A then osc B of voices 1..8
static const double OSC_DEV[2][8][3] = {
    {{-1.25, 0.35, 1.40}, {4.00, -0.05, -2.73}, {2.00, -0.84, -2.90}, {-1.98, -0.30, 1.03}, {1.92, 0.31, -1.24}, {0.97, -0.17, -1.03}, {-1.39, -0.16, 0.33}, {-1.47, -0.62, 0.38}},
    {{-3.43, 1.11, 3.02}, {4.13, 0.49, -1.60}, {5.80, -0.21, -3.99}, {-0.47, 0.96, 1.89}, {-4.04, -0.59, 1.59}, {-0.99, -0.05, 0.58}, {-0.08, -0.04, 0.36}, {-3.72, -0.20, 2.89}}};
// ---- per-voice VCO tuning (optional "vintage" mode): cents at A4 and at A1, osc A, after a warm TUNE ----------

// ---- output --------------------------------------------------------------------------------------
// internal scale: saw at mixer max has peak 1.0; that equals -26.4 dBFS rms at the Scarlett input
static const double OUT_GAIN_CAL = 0.0827;
static const double OUT_HP_HZ = 9.9;

}  // namespace p800
