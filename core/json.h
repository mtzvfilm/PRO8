// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 mtzvfilm. PRO8 - https://github.com/mtzvfilm/PRO8
// Minimal JSON value + parser + writer helpers: enough for the panel messages (flat objects) and the plugin state.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pro8 {

struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false; double n = 0; std::string s;
    std::vector<Json> a; std::map<std::string, Json> o;

    bool has(const std::string& k) const { return type == Obj && o.count(k) && o.at(k).type != Null; }
    const Json& operator[](const std::string& k) const { static const Json none; auto it = o.find(k); return type == Obj && it != o.end() ? it->second : none; }
    double num(double def = 0) const { return type == Num ? n : type == Bool ? (b ? 1 : 0) : type == Str ? std::atof(s.c_str()) : def; }
    int i(int def = 0) const { return type == Null ? def : (int)num(def); }
    bool truthy() const { return num() != 0; }
    const std::string& str() const { return s; }

    static Json parse(const std::string& t) { size_t p = 0; Json v; if (!value(t, p, v, 0)) return Json(); return v; }

private:
    static void ws(const std::string& t, size_t& p) { while (p < t.size() && (t[p] == ' ' || t[p] == '\t' || t[p] == '\n' || t[p] == '\r')) ++p; }
    static void utf8(std::string& o, uint32_t c) {
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63)); }
        else { o += (char)(0xF0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 63)); o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63)); }
    }
    static bool string(const std::string& t, size_t& p, std::string& out) {
        if (p >= t.size() || t[p] != '"') return false; ++p;
        while (p < t.size() && t[p] != '"') {
            char c = t[p++];
            if (c != '\\') { out += c; continue; }
            if (p >= t.size()) return false;
            char e = t[p++];
            switch (e) {
            case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break;
            case 'b': out += '\b'; break; case 'f': out += '\f'; break;
            case 'u': { if (p + 4 > t.size()) return false; uint32_t c1 = (uint32_t)std::strtoul(t.substr(p, 4).c_str(), nullptr, 16); p += 4;
                if (c1 >= 0xD800 && c1 < 0xDC00 && p + 6 <= t.size() && t[p] == '\\' && t[p + 1] == 'u') {
                    uint32_t c2 = (uint32_t)std::strtoul(t.substr(p + 2, 4).c_str(), nullptr, 16); p += 6; c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00); }
                utf8(out, c1); break; }
            default: out += e;
            }
        }
        if (p >= t.size()) return false; ++p; return true;
    }
    static bool value(const std::string& t, size_t& p, Json& v, int depth) {
        if (depth > 32) return false;
        ws(t, p); if (p >= t.size()) return false;
        char c = t[p];
        if (c == '{') {
            v.type = Obj; ++p; ws(t, p); if (p < t.size() && t[p] == '}') { ++p; return true; }
            for (;;) { ws(t, p); std::string k; if (!string(t, p, k)) return false; ws(t, p); if (p >= t.size() || t[p] != ':') return false; ++p;
                Json x; if (!value(t, p, x, depth + 1)) return false; v.o[k] = std::move(x); ws(t, p);
                if (p < t.size() && t[p] == ',') { ++p; continue; } if (p < t.size() && t[p] == '}') { ++p; return true; } return false; }
        }
        if (c == '[') {
            v.type = Arr; ++p; ws(t, p); if (p < t.size() && t[p] == ']') { ++p; return true; }
            for (;;) { Json x; if (!value(t, p, x, depth + 1)) return false; v.a.push_back(std::move(x)); ws(t, p);
                if (p < t.size() && t[p] == ',') { ++p; continue; } if (p < t.size() && t[p] == ']') { ++p; return true; } return false; }
        }
        if (c == '"') { v.type = Str; return string(t, p, v.s); }
        if (!t.compare(p, 4, "true")) { v.type = Bool; v.b = true; p += 4; return true; }
        if (!t.compare(p, 5, "false")) { v.type = Bool; v.b = false; p += 5; return true; }
        if (!t.compare(p, 4, "null")) { v.type = Null; p += 4; return true; }
        const char* s0 = t.c_str() + p; char* e = nullptr; double d = std::strtod(s0, &e); if (e == s0) return false;
        v.type = Num; v.n = d; p += (size_t)(e - s0); return true;
    }
};

inline std::string json_quote(const std::string& s) {   // a JSON / JS string literal
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}
inline std::string json_num(double v) { char b[32]; std::snprintf(b, sizeof b, "%.17g", v); return b; }

inline std::string base64_encode(const std::vector<uint8_t>& d) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; size_t i = 0;
    for (; i + 2 < d.size(); i += 3) { uint32_t v = d[i] << 16 | d[i + 1] << 8 | d[i + 2]; o += T[v >> 18]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += T[v & 63]; }
    if (i + 1 == d.size()) { uint32_t v = d[i] << 16; o += T[v >> 18]; o += T[(v >> 12) & 63]; o += "=="; }
    else if (i + 2 == d.size()) { uint32_t v = d[i] << 16 | d[i + 1] << 8; o += T[v >> 18]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += '='; }
    return o;
}
inline std::vector<uint8_t> base64_decode(const std::string& s) {
    std::vector<uint8_t> o; uint32_t v = 0; int bits = 0;
    for (char c : s) {
        int x = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (x < 0) continue; v = v << 6 | x; bits += 6; if (bits >= 8) { bits -= 8; o.push_back((v >> bits) & 255); }
    }
    return o;
}

}  // namespace pro8
