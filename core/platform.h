// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 mtzvfilm. PRO8 - https://github.com/mtzvfilm/PRO8
// The few things the core needs from the operating system. macOS: core/platform_mac.mm, Windows: core/platform_win.cpp.
// Paths are UTF-8 strings.
#pragma once
#include <string>
#include <vector>

namespace pro8::platform {

// per-user folder for the preset library: macOS ~/Library/Application Support/PRO8/Banks, Windows %APPDATA%\PRO8\Banks
std::string banks_dir();
// file dialogs (called on the UI thread); exts without the dot, e.g. {"syx", "fxp"}; "" when cancelled
std::string pick_open(const std::vector<std::string>& exts);
std::string pick_save(const std::string& name, const std::vector<std::string>& exts);
// last-used settings shared by new instances (macOS: NSUserDefaults suite com.mtzvfilm.pro8, Windows: %APPDATA%\PRO8\settings.json)
bool pref_get(const char* key, double& v);
void pref_set(const char* key, double v);

}  // namespace pro8::platform
