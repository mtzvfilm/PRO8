// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 mtzvfilm. PRO8 - https://github.com/mtzvfilm/PRO8
// PRO8 as a CLAP plugin (VST3 / AU through clap-wrapper): instrument, stereo out, notes (CLAP + MIDI), host tempo,
// state (patch, FX, settings), GUI = the same panel (ui.html in the bundle's Resources) in a WKWebView.
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include <clap/clap.h>
#include <string>
#include <vector>
#include <cstring>
#include "../../core/pro8_core.h"

static NSString* g_resources = nil;   // <bundle>/Contents/Resources

// ---------------------------------------------------------------- GUI glue
// the web view hands key presses to the host (Logic's Musical Typing, transport keys, ...) unless the user is typing
// into a text field of the panel (preset name, search): otherwise the host saw the key-down but never the key-up
@interface P8WebView : WKWebView { @public bool textFocus; } @end
@implementation P8WebView
- (void)keyDown:(NSEvent*)e { if (textFocus) [super keyDown:e]; else [self.nextResponder keyDown:e]; }
- (void)keyUp:(NSEvent*)e { if (textFocus) [super keyUp:e]; else [self.nextResponder keyUp:e]; }
- (void)flagsChanged:(NSEvent*)e { if (textFocus) [super flagsChanged:e]; else [self.nextResponder flagsChanged:e]; }
- (BOOL)performKeyEquivalent:(NSEvent*)e { return textFocus ? [super performKeyEquivalent:e] : NO; }
@end
@interface P8Bridge : NSObject <WKScriptMessageHandler> { @public pro8::Core* core; P8WebView* __weak view; } @end
@implementation P8Bridge
- (void)userContentController:(WKUserContentController*)uc didReceiveScriptMessage:(WKScriptMessage*)msg {
    NSDictionary* m = msg.body;
    if ([m[@"t"] isEqualToString:@"textfocus"]) { P8WebView* v = view; if (v) v->textFocus = [m[@"on"] intValue] != 0; return; }
    if (core) core->handle(m);
}
@end

struct Pro8Plugin {
    clap_plugin_t plugin;
    const clap_host_t* host;
    pro8::Core* core = nullptr;
    P8WebView* web = nil; P8Bridge* bridge = nil;
    uint32_t w = 1280, h = 784;
    bool was_playing = false;
};
static Pro8Plugin* P(const clap_plugin_t* p) { return (Pro8Plugin*)p->plugin_data; }

// ---------------------------------------------------------------- descriptor
static const char* FEATURES[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_STEREO, nullptr};
static const clap_plugin_descriptor_t DESC = {CLAP_VERSION, "com.mtzvfilm.pro8", "PRO8", "mtzvfilm", "https://instagram.com/mtzv_film", "", "",
                                              "1.0.1", "8-voice analog polysynth", FEATURES};

// ---------------------------------------------------------------- audio / note ports
static uint32_t ap_count(const clap_plugin_t*, bool is_input) { return is_input ? 0 : 1; }
static bool ap_get(const clap_plugin_t*, uint32_t index, bool is_input, clap_audio_port_info_t* info) {
    if (is_input || index) return false;
    info->id = 0; strncpy(info->name, "Output", sizeof(info->name)); info->channel_count = 2; info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->port_type = CLAP_PORT_STEREO; info->in_place_pair = CLAP_INVALID_ID; return true;
}
static const clap_plugin_audio_ports_t AUDIO_PORTS = {ap_count, ap_get};
static uint32_t np_count(const clap_plugin_t*, bool is_input) { return is_input ? 1 : 0; }
static bool np_get(const clap_plugin_t*, uint32_t index, bool is_input, clap_note_port_info_t* info) {
    if (!is_input || index) return false;
    info->id = 0; info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI; info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    strncpy(info->name, "Notes", sizeof(info->name)); return true;
}
static const clap_plugin_note_ports_t NOTE_PORTS = {np_count, np_get};

// ---------------------------------------------------------------- state
static bool st_save(const clap_plugin_t* p, const clap_ostream_t* s) {
    NSData* d = P(p)->core->get_state(); const uint8_t* b = (const uint8_t*)d.bytes; int64_t left = d.length;
    while (left > 0) { int64_t k = s->write(s, b, left); if (k <= 0) return false; b += k; left -= k; }
    return true;
}
static bool st_load(const clap_plugin_t* p, const clap_istream_t* s) {
    NSMutableData* d = [NSMutableData data]; uint8_t buf[4096];
    for (;;) { int64_t k = s->read(s, buf, sizeof buf); if (k < 0) return false; if (k == 0) break; [d appendBytes:buf length:(NSUInteger)k]; }
    return P(p)->core->set_state(d);
}
static const clap_plugin_state_t STATE = {st_save, st_load};

// ---------------------------------------------------------------- GUI (cocoa, WKWebView)
static bool gui_supported(const clap_plugin_t*, const char* api, bool floating) { return !floating && !strcmp(api, CLAP_WINDOW_API_COCOA); }
static bool gui_preferred(const clap_plugin_t*, const char** api, bool* floating) { *api = CLAP_WINDOW_API_COCOA; *floating = false; return true; }
static bool gui_create(const clap_plugin_t* p, const char* api, bool floating) {
    if (floating || strcmp(api, CLAP_WINDOW_API_COCOA)) return false;
    Pro8Plugin* pl = P(p);
    WKWebViewConfiguration* cfg = [WKWebViewConfiguration new]; pl->bridge = [P8Bridge new]; pl->bridge->core = pl->core;
    [cfg.userContentController addScriptMessageHandler:pl->bridge name:@"p8"];
    pl->web = [[P8WebView alloc] initWithFrame:NSMakeRect(0, 0, pl->w, pl->h) configuration:cfg]; pl->bridge->view = pl->web;
    pl->web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [pl->web setValue:@NO forKey:@"drawsBackground"];
    __weak P8WebView* weak = pl->web;
    pl->core->js = [weak](NSString* s) { dispatch_async(dispatch_get_main_queue(), ^{ WKWebView* v = weak; if (v) [v evaluateJavaScript:s completionHandler:nil]; }); };
    NSURL* u = [NSURL fileURLWithPath:[g_resources stringByAppendingPathComponent:@"ui.html"]];
    [pl->web loadFileURL:u allowingReadAccessToURL:[u URLByDeletingLastPathComponent]];
    return true;
}
static void gui_destroy(const clap_plugin_t* p) {
    Pro8Plugin* pl = P(p); pl->core->js = nullptr;
    [pl->web.configuration.userContentController removeScriptMessageHandlerForName:@"p8"];
    [pl->web removeFromSuperview]; pl->web = nil; pl->bridge = nil;
}
static bool gui_set_scale(const clap_plugin_t*, double) { return false; }
static bool gui_get_size(const clap_plugin_t* p, uint32_t* w, uint32_t* h) { *w = P(p)->w; *h = P(p)->h; return true; }
static bool gui_can_resize(const clap_plugin_t*) { return true; }
static bool gui_get_resize_hints(const clap_plugin_t*, clap_gui_resize_hints_t* r) {
    r->can_resize_horizontally = r->can_resize_vertically = true; r->preserve_aspect_ratio = true; r->aspect_ratio_width = 1600; r->aspect_ratio_height = 980; return true;
}
static bool gui_adjust_size(const clap_plugin_t*, uint32_t* w, uint32_t* h) { if (*w < 800) *w = 800; *h = (uint32_t)(*w * 980.0 / 1600.0); return true; }
static bool gui_set_size(const clap_plugin_t* p, uint32_t w, uint32_t h) { Pro8Plugin* pl = P(p); pl->w = w; pl->h = h; if (pl->web) [pl->web setFrameSize:NSMakeSize(w, h)]; return true; }
static bool gui_set_parent(const clap_plugin_t* p, const clap_window_t* win) {
    Pro8Plugin* pl = P(p); NSView* parent = (__bridge NSView*)win->cocoa; if (!parent || !pl->web) return false;
    [pl->web setFrame:NSMakeRect(0, 0, pl->w, pl->h)]; [parent addSubview:pl->web]; return true;
}
static bool gui_set_transient(const clap_plugin_t*, const clap_window_t*) { return false; }
static void gui_suggest_title(const clap_plugin_t*, const char*) {}
static bool gui_show(const clap_plugin_t* p) { [P(p)->web setHidden:NO]; return true; }
static bool gui_hide(const clap_plugin_t* p) { [P(p)->web setHidden:YES]; return true; }
static const clap_plugin_gui_t GUI = {gui_supported, gui_preferred, gui_create, gui_destroy, gui_set_scale, gui_get_size, gui_can_resize,
                                      gui_get_resize_hints, gui_adjust_size, gui_set_size, gui_set_parent, gui_set_transient, gui_suggest_title, gui_show, gui_hide};

// ---------------------------------------------------------------- plugin
static bool pl_init(const clap_plugin_t* p) { P(p)->core = new pro8::Core(true); return true; }
static void pl_destroy(const clap_plugin_t* p) { Pro8Plugin* pl = P(p); delete pl->core; delete pl; }
static bool pl_activate(const clap_plugin_t* p, double sr, uint32_t, uint32_t) { P(p)->core->set_sample_rate(sr); return true; }
static void pl_deactivate(const clap_plugin_t*) {}
static bool pl_start(const clap_plugin_t*) { return true; }
static void pl_stop(const clap_plugin_t*) {}
static void pl_reset(const clap_plugin_t* p) { NSDictionary* m = @{@"t": @"panic"}; P(p)->core->handle(m); }
static void handle_event(Pro8Plugin* pl, const clap_event_header_t* e) {
    if (e->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
    switch (e->type) {
    case CLAP_EVENT_NOTE_ON: { auto* n = (const clap_event_note_t*)e; pl->core->note(n->key, std::max(1, (int)std::lround(n->velocity * 127)), true); break; }
    case CLAP_EVENT_NOTE_OFF: { auto* n = (const clap_event_note_t*)e; pl->core->note(n->key, 0, false); break; }
    case CLAP_EVENT_MIDI: { auto* m = (const clap_event_midi_t*)e; uint8_t st = m->data[0];
        if ((st & 0xF0) == 0xB0) { uint8_t d1 = m->data[1], d2 = m->data[2]; pro8::Core* c = pl->core; dispatch_async(dispatch_get_main_queue(), ^{ c->midi(st, d1, d2); }); }
        else pl->core->midi(st, m->data[1], m->data[2]); break; }
    }
}
static clap_process_status pl_process(const clap_plugin_t* p, const clap_process_t* pr) {
    Pro8Plugin* pl = P(p);
    if (pr->transport && (pr->transport->flags & CLAP_TRANSPORT_HAS_TEMPO)) {
        bool playing = pr->transport->flags & CLAP_TRANSPORT_IS_PLAYING;
        pl->core->set_host_tempo(pr->transport->tempo, playing && !pl->was_playing); pl->was_playing = playing;
    }
    float* L = pr->audio_outputs[0].data32[0]; float* R = pr->audio_outputs[0].data32[1];
    const uint32_t N = pr->frames_count, ne = pr->in_events->size(pr->in_events); uint32_t pos = 0, ei = 0;
    while (pos < N) {   // render up to the next event, then apply it (sample-accurate notes)
        uint32_t next = N;
        while (ei < ne) { const clap_event_header_t* e = pr->in_events->get(pr->in_events, ei); if (e->time > pos) { next = std::min(N, e->time); break; } handle_event(pl, e); ++ei; }
        if (ei >= ne) next = N;
        if (next > pos) { pl->core->render(L + pos, R + pos, (int)(next - pos)); pos = next; }
    }
    return CLAP_PROCESS_CONTINUE;
}
static const void* pl_get_extension(const clap_plugin_t*, const char* id) {
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &AUDIO_PORTS;
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &NOTE_PORTS;
    if (!strcmp(id, CLAP_EXT_STATE)) return &STATE;
    if (!strcmp(id, CLAP_EXT_GUI)) return &GUI;
    return nullptr;
}
static void pl_on_main_thread(const clap_plugin_t*) {}

static const clap_plugin_t* create(const clap_host_t* host) {
    Pro8Plugin* pl = new Pro8Plugin(); pl->host = host;
    pl->plugin = {&DESC, pl, pl_init, pl_destroy, pl_activate, pl_deactivate, pl_start, pl_stop, pl_reset, pl_process, pl_get_extension, pl_on_main_thread};
    return &pl->plugin;
}
// ---------------------------------------------------------------- factory / entry
static uint32_t f_count(const clap_plugin_factory_t*) { return 1; }
static const clap_plugin_descriptor_t* f_desc(const clap_plugin_factory_t*, uint32_t i) { return i == 0 ? &DESC : nullptr; }
static const clap_plugin_t* f_create(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
    return !strcmp(id, DESC.id) && clap_version_is_compatible(host->clap_version) ? create(host) : nullptr;
}
static const clap_plugin_factory_t FACTORY = {f_count, f_desc, f_create};
bool pro8_entry_init(const char* plugin_path) {
    // plugin_path: the .clap / .vst3 / .component bundle; resources (ui.html, seed banks) in Contents/Resources
    NSString* b = [NSString stringWithUTF8String:plugin_path ?: ""];
    while (b.length && ![b.pathExtension isEqualToString:@"clap"] && ![b.pathExtension isEqualToString:@"vst3"] && ![b.pathExtension isEqualToString:@"component"]) b = [b stringByDeletingLastPathComponent];
    g_resources = [[b stringByAppendingPathComponent:@"Contents"] stringByAppendingPathComponent:@"Resources"];
    pro8::library_init([g_resources stringByAppendingPathComponent:@"banks"]);
    return true;
}
void pro8_entry_deinit(void) {}
const void* pro8_entry_get_factory(const char* id) { return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &FACTORY : nullptr; }
