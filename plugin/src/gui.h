// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 mtzvfilm. PRO8 - https://github.com/mtzvfilm/PRO8
// The panel window: core/ui.html in the system web view. macOS: gui_mac.mm (WKWebView), Windows: gui_win.cpp (WebView2).
// All calls on the host's main (UI) thread.
#pragma once
#include <clap/clap.h>
#include <cstdint>
#include <functional>
#include <string>
#include "../../core/pro8_core.h"

namespace pro8gui {

struct View;
const char* window_api();                       // CLAP_WINDOW_API_COCOA / CLAP_WINDOW_API_WIN32
// creates the web view and sets core->js; resources = the folder with ui.html (macOS; Windows uses the embedded copy)
View* create(pro8::Core* core, const std::string& resources, uint32_t w, uint32_t h);
void destroy(View* v);                          // clears core->js
bool set_parent(View* v, const clap_window_t* win);
void set_size(View* v, uint32_t w, uint32_t h);
void set_visible(View* v, bool on);
#ifdef __APPLE__
void post_main(std::function<void()> f);        // run on the main thread (macOS: GCD main queue)
#endif

}  // namespace pro8gui
