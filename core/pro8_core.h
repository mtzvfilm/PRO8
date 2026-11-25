// PRO8 core: one synth instance (voice engine + effects + edit buffer + settings) and the shared preset library.
// Used by the plugin (plugin/src/pro8_clap.mm); the panel (core/ui.html) talks to
// it through handle() (messages from the web view) and the js callback (calls into the web view).
#pragma once
#import <Foundation/Foundation.h>
#include <array>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include "../dsp/p800_engine.h"
#include "../dsp/p800_fx.h"

namespace pro8 {

using FxVals = std::array<uint16_t, p800fx::FX_N>;

class Core {
public:
    explicit Core(bool plugin);
    // web view bridge
    std::function<void(NSString*)> js;          // evaluate in the panel (any thread; the caller posts it to the main thread)
    void handle(NSDictionary* m);               // a message from the panel
    void ui_ready();                            // push the whole state to a freshly loaded panel
    // audio (render thread)
    void set_sample_rate(double sr);
    void render(float* L, float* R, int n);     // stereo out, engine + effects + volume
    void note(int n, int vel, bool on);
    void midi(uint8_t st, uint8_t d1, uint8_t d2);   // CCs follow the PRO-800 MIDI map, the rest goes to the engine
    void midi_clock(uint64_t host_time);        // 0xF8 from a DAW (standalone SYNC TO DAW)
    void midi_start();                          // 0xFA
    void set_host_tempo(double bpm, bool playing_started);   // plugin: the host transport
    // plugin state (patch, FX, program, settings, arpeggiator)
    NSData* get_state();
    bool set_state(NSData* d);
    std::mutex m;                               // guards engine / FX against the render thread
private:
    bool plugin;
    p800::Engine eng; p800fx::FX fx;
    std::vector<uint8_t> patch; int prog = 0; FxVals fxcur;
    double gain = 1.0, bpm = 120, ui_bpm = 120;
    bool daw_sync = false;
    struct Settings { double vint = 0.5, spread = 1.5, tune = 440; int voices = 8, uni = 8; bool polyuni = false, mono = false; } st;
    struct Arp { int mode = -1, oct = 1, div = 6, lfos = 0, lfod = 4; } arp;
    std::vector<float> mono;
    double clk_last = 0, clk_avg = 0, clk_win[48] = {}; int clk_n = 0, clk_wn = 0, clk_wi = 0, clk_jump = 0; double clk_shown = -1;
    void apply_engine(bool knob = false);
    void apply_fx();
    void apply_settings();
    void load_prog(int n);
    void push_patch();
    void push_fx();
    void push_settings();
    bool panel_cc(int cc, int val);
    void send_list();
};

// preset library: banks A..Z x 100 slots, one .syx per bank in ~/Library/Application Support/PRO8/Banks
void library_init(NSString* seed_dir);         // load; first start: copy the seed banks (A..Z.syx in seed_dir)

}  // namespace pro8
