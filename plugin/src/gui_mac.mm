// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 mtzvfilm. PRO8 - https://github.com/mtzvfilm/PRO8
// plugin/src/gui.h for macOS: the panel in a WKWebView
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include "gui.h"

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
    NSDictionary* m = msg.body; if (![m isKindOfClass:[NSDictionary class]]) return;
    if ([m[@"t"] isEqualToString:@"textfocus"]) { P8WebView* v = view; if (v) v->textFocus = [m[@"on"] intValue] != 0; return; }
    if (![NSJSONSerialization isValidJSONObject:m]) return;
    NSData* j = [NSJSONSerialization dataWithJSONObject:m options:0 error:nil];
    if (core && j) core->handle_json(std::string((const char*)j.bytes, j.length));
}
@end

namespace pro8gui {

struct View { pro8::Core* core; P8WebView* web; P8Bridge* bridge; };

const char* window_api() { return CLAP_WINDOW_API_COCOA; }
void post_main(std::function<void()> f) { dispatch_async(dispatch_get_main_queue(), ^{ f(); }); }

View* create(pro8::Core* core, const std::string& resources, uint32_t w, uint32_t h) {
    View* v = new View{core, nil, nil};
    WKWebViewConfiguration* cfg = [WKWebViewConfiguration new]; v->bridge = [P8Bridge new]; v->bridge->core = core;
    [cfg.userContentController addScriptMessageHandler:v->bridge name:@"p8"];
    v->web = [[P8WebView alloc] initWithFrame:NSMakeRect(0, 0, w, h) configuration:cfg]; v->bridge->view = v->web;
    v->web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [v->web setValue:@NO forKey:@"drawsBackground"];
    __weak P8WebView* weak = v->web;
    core->js = [weak](const std::string& s) {
        NSString* js = [NSString stringWithUTF8String:s.c_str()]; if (!js) return;
        dispatch_async(dispatch_get_main_queue(), ^{ WKWebView* wv = weak; if (wv) [wv evaluateJavaScript:js completionHandler:nil]; });
    };
    NSString* dir = [NSString stringWithUTF8String:resources.c_str()] ?: @"";
    NSURL* u = [NSURL fileURLWithPath:[dir stringByAppendingPathComponent:@"ui.html"]];
    [v->web loadFileURL:u allowingReadAccessToURL:[u URLByDeletingLastPathComponent]];
    return v;
}
void destroy(View* v) {
    if (!v) return;
    v->core->js = nullptr;
    [v->web.configuration.userContentController removeScriptMessageHandlerForName:@"p8"];
    [v->web removeFromSuperview]; v->web = nil; v->bridge = nil;
    delete v;
}
bool set_parent(View* v, const clap_window_t* win) {
    NSView* parent = (__bridge NSView*)win->cocoa; if (!parent || !v || !v->web) return false;
    NSRect f = v->web.frame; [v->web setFrame:NSMakeRect(0, 0, f.size.width, f.size.height)]; [parent addSubview:v->web]; return true;
}
void set_size(View* v, uint32_t w, uint32_t h) { if (v && v->web) [v->web setFrameSize:NSMakeSize(w, h)]; }
void set_visible(View* v, bool on) { if (v && v->web) [v->web setHidden:!on]; }

}  // namespace pro8gui
