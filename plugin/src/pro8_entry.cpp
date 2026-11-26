// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 mtzvfilm. PRO8 - https://github.com/mtzvfilm/PRO8
// exported clap_entry (recompiled into the CLAP / VST3 / AU wrappers by clap-wrapper's make_clapfirst_plugins)
#include <clap/clap.h>
extern bool pro8_entry_init(const char* plugin_path);
extern void pro8_entry_deinit(void);
extern const void* pro8_entry_get_factory(const char* factory_id);
extern "C" {
const CLAP_EXPORT struct clap_plugin_entry clap_entry = {CLAP_VERSION, pro8_entry_init, pro8_entry_deinit, pro8_entry_get_factory};
}
