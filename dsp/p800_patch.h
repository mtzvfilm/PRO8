// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 mtzvfilm. PRO8 - https://github.com/mtzvfilm/PRO8
// PRO-800 patch: decoded SysEx data (format 0x6F, firmware 1.4.6). Offsets: inputs/repos/samstaton_pro800/pro800syx.md
#pragma once
#include <cstdint>
#include <cstring>

namespace p800 {

struct Patch {
    // continuous (0..65535); double so knob moves can glide smoothly (Engine::set_patch(p, true))
    double freq_a = 32768, vol_a = 32768, pw_a = 32768, freq_b = 32768, vol_b = 0, pw_b = 32768, fine_b = 32768;
    double cutoff = 65535, res = 0, fenv_amt = 32768;
    double fe_a = 0, fe_d = 0, fe_s = 65535, fe_r = 0, ae_a = 0, ae_d = 0, ae_s = 65535, ae_r = 0;
    double pm_env = 32768, pm_oscb = 0, lfo_freq = 32768, lfo_amt = 0, glide = 0, amp_vel = 0, filt_vel = 0;
    double mod_delay = 0, vib_speed = 32768, vib_amt = 0, detune = 0, noise = 0, vca_at = 0, vcf_at = 0, lfo_at = 0;
    double pitch_range = 0x6000;
    // stepped
    uint8_t saw_a = 1, tri_a = 0, sqr_a = 0, saw_b = 0, tri_b = 0, sqr_b = 0, sync = 0, pm_freq = 0, pm_filt = 0;
    uint8_t lfo_shape = 1, lfo_range = 1, lfo_target = 0, keytrk = 0, fe_shape = 0, fe_speed = 1, ae_shape = 0;
    uint8_t unison = 0, bend_target = 1, mw_amt = 0, freqmode_a = 2, freqmode_b = 2, mw_target = 0, ae_speed = 1;
    uint8_t voice_spread = 0, kbd_ref = 3, glide_mode = 0, arp = 0;   // arp = byte 149 (clocked arpeggiator mode)
    uint8_t unison_pattern[7] = {0, 0, 0, 0, 0, 0, 0};
    char name[17] = {0};

    template <class F> void each_cont(F f) {   // every continuous field
        double* v[] = {&freq_a, &vol_a, &pw_a, &freq_b, &vol_b, &pw_b, &fine_b, &cutoff, &res, &fenv_amt, &fe_a, &fe_d, &fe_s, &fe_r, &ae_a, &ae_d, &ae_s, &ae_r,
                       &pm_env, &pm_oscb, &lfo_freq, &lfo_amt, &glide, &amp_vel, &filt_vel, &mod_delay, &vib_speed, &vib_amt, &detune, &noise, &vca_at, &vcf_at, &lfo_at, &pitch_range};
        for (double* x : v) f(*x);
    }
    static uint16_t u16(const uint8_t* d, int o) { return (uint16_t)(d[o] | (d[o + 1] << 8)); }

    // d = decoded patch data (>= 173 bytes for format 0x6F)
    bool parse(const uint8_t* d, size_t n) {
        if (n < 150) return false;
        freq_a = u16(d, 5); vol_a = u16(d, 7); pw_a = u16(d, 9); freq_b = u16(d, 11); vol_b = u16(d, 13); pw_b = u16(d, 15);
        fine_b = u16(d, 17); cutoff = u16(d, 19); res = u16(d, 21); fenv_amt = u16(d, 23);
        fe_r = u16(d, 25); fe_s = u16(d, 27); fe_d = u16(d, 29); fe_a = u16(d, 31);
        ae_r = u16(d, 33); ae_s = u16(d, 35); ae_d = u16(d, 37); ae_a = u16(d, 39);
        pm_env = u16(d, 41); pm_oscb = u16(d, 43); lfo_freq = u16(d, 45); lfo_amt = u16(d, 47); glide = u16(d, 49);
        amp_vel = u16(d, 51); filt_vel = u16(d, 53);
        saw_a = d[55]; tri_a = d[56]; sqr_a = d[57]; saw_b = d[58]; tri_b = d[59]; sqr_b = d[60]; sync = d[61];
        pm_freq = d[62]; pm_filt = d[63]; lfo_shape = d[64]; lfo_range = d[65]; lfo_target = d[66]; keytrk = d[67];
        fe_shape = d[68]; fe_speed = d[69]; ae_shape = d[70]; unison = d[71]; bend_target = d[72]; mw_amt = d[73];
        freqmode_a = d[74]; freqmode_b = d[75];
        mod_delay = u16(d, 76); vib_speed = u16(d, 78); vib_amt = u16(d, 80); detune = u16(d, 82); mw_target = d[84];
        for (int i = 0; i < 7; ++i) unison_pattern[i] = d[87 + i];
        arp = d[149];
        noise = u16(d, 142); vca_at = u16(d, 144); vcf_at = u16(d, 146); ae_speed = d[148];
        std::memcpy(name, d + 150, 16); name[16] = 0;
        if (n >= 173) {
            lfo_at = u16(d, 166); voice_spread = d[168]; kbd_ref = d[169]; glide_mode = d[170]; pitch_range = u16(d, 171);
        }
        return true;
    }
};

}  // namespace p800
