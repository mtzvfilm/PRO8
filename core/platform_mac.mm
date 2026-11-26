// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 mtzvfilm. PRO8 - https://github.com/mtzvfilm/PRO8
// core/platform.h for macOS
#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include "platform.h"

namespace pro8::platform {

std::string banks_dir() {
    NSString* d = [[NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES) firstObject] stringByAppendingPathComponent:@"PRO8/Banks"];
    return d.UTF8String ?: "";
}
static NSArray<UTType*>* types(const std::vector<std::string>& exts) {
    NSMutableArray<UTType*>* a = [NSMutableArray array];
    for (auto& e : exts) { UTType* t = [UTType typeWithFilenameExtension:[NSString stringWithUTF8String:e.c_str()]]; if (t) [a addObject:t]; }
    return a;
}
std::string pick_open(const std::vector<std::string>& exts) {
    NSOpenPanel* o = [NSOpenPanel openPanel]; o.allowedContentTypes = types(exts); o.allowsMultipleSelection = NO;
    return [o runModal] == NSModalResponseOK ? (o.URL.path.UTF8String ?: "") : "";
}
std::string pick_save(const std::string& name, const std::vector<std::string>& exts) {
    NSSavePanel* s = [NSSavePanel savePanel]; s.allowedContentTypes = types(exts); s.allowsOtherFileTypes = NO;
    s.nameFieldStringValue = [NSString stringWithUTF8String:name.c_str()] ?: @"";
    return [s runModal] == NSModalResponseOK ? (s.URL.path.UTF8String ?: "") : "";
}
static NSUserDefaults* prefs() { static NSUserDefaults* u = [[NSUserDefaults alloc] initWithSuiteName:@"com.mtzvfilm.pro8"]; return u; }
bool pref_get(const char* key, double& v) {
    NSString* k = [NSString stringWithUTF8String:key]; if (![prefs() objectForKey:k]) return false;
    v = [prefs() doubleForKey:k]; return true;
}
void pref_set(const char* key, double v) { [prefs() setDouble:v forKey:[NSString stringWithUTF8String:key]]; }

}  // namespace pro8::platform
