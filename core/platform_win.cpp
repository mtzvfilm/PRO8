// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 mtzvfilm. PRO8 - https://github.com/mtzvfilm/PRO8
// core/platform.h for Windows: %APPDATA%\PRO8\Banks, common file dialogs, settings in %APPDATA%\PRO8\settings.json
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include "platform.h"
#include "json.h"

namespace fs = std::filesystem;

namespace pro8::platform {

static std::wstring wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n); return w;
}
static std::string utf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr); return s;
}
static fs::path app_dir() {   // %APPDATA%\PRO8
    PWSTR p = nullptr; fs::path d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) d = fs::path(p) / L"PRO8";
    CoTaskMemFree(p);
    std::error_code ec; fs::create_directories(d, ec); return d;
}
std::string banks_dir() { return utf8((app_dir() / L"Banks").wstring()); }

// filter string: "PRO8 files (*.syx;*.fxp)\0*.syx;*.fxp\0SYX (*.syx)\0*.syx\0...\0\0"
static std::wstring filter(const std::vector<std::string>& exts) {
    std::wstring all, f;
    for (auto& e : exts) { if (!all.empty()) all += L";"; all += L"*." + wide(e); }
    f += L"PRO8 files (" + all + L")"; f.push_back(0); f += all; f.push_back(0);
    for (auto& e : exts) { std::wstring x = L"*." + wide(e); f += x; f.push_back(0); f += x; f.push_back(0); }
    f.push_back(0); return f;
}
std::string pick_open(const std::vector<std::string>& exts) {
    wchar_t buf[MAX_PATH * 4] = {0}; std::wstring flt = filter(exts);
    OPENFILENAMEW o = {}; o.lStructSize = sizeof o; o.hwndOwner = GetActiveWindow(); o.lpstrFilter = flt.c_str();
    o.lpstrFile = buf; o.nMaxFile = (DWORD)(sizeof buf / sizeof buf[0]); o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    return GetOpenFileNameW(&o) ? utf8(buf) : "";
}
std::string pick_save(const std::string& name, const std::vector<std::string>& exts) {
    wchar_t buf[MAX_PATH * 4] = {0}; std::wstring n = wide(name); wcsncpy_s(buf, n.c_str(), _TRUNCATE);
    std::wstring flt = filter(exts), def = exts.empty() ? L"" : wide(exts[0]);
    OPENFILENAMEW o = {}; o.lStructSize = sizeof o; o.hwndOwner = GetActiveWindow(); o.lpstrFilter = flt.c_str();
    o.nFilterIndex = 1; o.lpstrDefExt = def.c_str();
    o.lpstrFile = buf; o.nMaxFile = (DWORD)(sizeof buf / sizeof buf[0]); o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    return GetSaveFileNameW(&o) ? utf8(buf) : "";
}

// settings: a flat JSON object of numbers
static std::mutex& pm() { static std::mutex m; return m; }
static std::map<std::string, double>& pmap() {
    static std::map<std::string, double> m; static bool loaded = false;
    if (!loaded) { loaded = true; std::ifstream f(app_dir() / L"settings.json"); std::stringstream ss; ss << f.rdbuf();
        Json j = Json::parse(ss.str()); for (auto& [k, v] : j.o) if (v.type == Json::Num || v.type == Json::Bool) m[k] = v.num(); }
    return m;
}
bool pref_get(const char* key, double& v) {
    std::lock_guard<std::mutex> l(pm()); auto& m = pmap(); auto it = m.find(key); if (it == m.end()) return false; v = it->second; return true;
}
void pref_set(const char* key, double v) {
    std::lock_guard<std::mutex> l(pm()); auto& m = pmap(); m[key] = v;
    std::string s = "{"; bool first = true;
    for (auto& [k, x] : m) { if (!first) s += ","; first = false; s += json_quote(k) + ":" + json_num(x); }
    s += "}";
    fs::path dst = app_dir() / L"settings.json", tmp = dst; tmp += L".tmp";
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); f << s; }
    std::error_code ec; fs::rename(tmp, dst, ec);
}

}  // namespace pro8::platform
